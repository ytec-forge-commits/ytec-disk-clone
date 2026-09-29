#include "vm_clone_execution_service.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/inventory_formatter.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This executable must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YTEC-VM-ONLY-PHASE3-BIOS-BCDBOOT";
constexpr std::uint64_t kTargetDiskBytes =
    56ULL * 1024U * 1024U * 1024U;

struct Arguments final {
  bool plan{};
  bool execute{};
  std::optional<std::uint32_t> target_number;
  std::wstring windows_root;
  std::wstring system_root;
  std::wstring authorization;
  std::wstring confirmation;
};

struct VolumeLocation final {
  std::uint32_t disk_number{};
  std::uint64_t starting_offset{};
  std::wstring file_system;
};

struct Selection final {
  ytec::diskmodel::DiskInfo target;
  ytec::clonecore::StableDiskIdentity identity;
  ytec::diskmodel::PartitionInfo windows_partition;
  ytec::diskmodel::PartitionInfo active_system_partition;
};

void print_error(const ytec::clonecore::Error& error) {
  std::cerr << "YDC_ERROR code=" << static_cast<unsigned int>(error.code)
            << " native=" << error.native_code
            << " operation=" << ytec::diskmodel::to_utf8(error.operation)
            << " message=" << ytec::diskmodel::to_utf8(error.message) << '\n';
}

ytec::clonecore::Error make_error(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

std::optional<std::uint32_t> parse_disk_number(const std::wstring& text) {
  if (text.empty() || text.size() > 10) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const wchar_t character : text) {
    if (character < L'0' || character > L'9') {
      return std::nullopt;
    }
    value = value * 10U + static_cast<std::uint64_t>(character - L'0');
    if (value > (std::numeric_limits<std::uint32_t>::max)()) {
      return std::nullopt;
    }
  }
  return static_cast<std::uint32_t>(value);
}

bool is_drive_root(const std::wstring& value) {
  return value.size() == 3 && std::iswalpha(value[0]) != 0 &&
         value[1] == L':' && (value[2] == L'\\' || value[2] == L'/');
}

std::wstring normalized_root(std::wstring value) {
  if (is_drive_root(value)) {
    value[0] = static_cast<wchar_t>(std::towupper(value[0]));
    value[2] = L'\\';
  }
  return value;
}

std::optional<Arguments> parse_arguments(const int argc, wchar_t* argv[]) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--plan") {
      arguments.plan = true;
    } else if (argument == L"--execute") {
      arguments.execute = true;
    } else if (argument == L"--target" && index + 1 < argc) {
      arguments.target_number = parse_disk_number(argv[++index]);
    } else if (argument == L"--windows-root" && index + 1 < argc) {
      arguments.windows_root = normalized_root(argv[++index]);
    } else if (argument == L"--system-root" && index + 1 < argc) {
      arguments.system_root = normalized_root(argv[++index]);
    } else if (argument == L"--authorization" && index + 1 < argc) {
      arguments.authorization = argv[++index];
    } else if (argument == L"--confirmation" && index + 1 < argc) {
      arguments.confirmation = argv[++index];
    } else {
      return std::nullopt;
    }
  }
  if (arguments.plan == arguments.execute ||
      !arguments.target_number.has_value() ||
      !is_drive_root(arguments.windows_root) ||
      !is_drive_root(arguments.system_root)) {
    return std::nullopt;
  }
  return arguments;
}

bool equals_case_insensitive(
    const std::wstring& left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
         _wcsicmp(left.c_str(), right.data()) == 0;
}

ytec::clonecore::Result<VolumeLocation> query_volume_location(
    const std::wstring& root) {
  const std::wstring device = L"\\\\.\\" + root.substr(0, 2);
  ytec::clonecore::UniqueHandle volume(CreateFileW(
      device.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    return ytec::clonecore::Result<VolumeLocation>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"BIOS BCDBoot対象ボリュームを読取り専用で開く",
            GetLastError()));
  }

  std::array<std::byte, 1024> buffer{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          volume.get(),
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &bytes_returned,
          nullptr)) {
    return ytec::clonecore::Result<VolumeLocation>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"BIOS BCDBoot対象ボリュームのディスク対応取得",
            GetLastError()));
  }
  constexpr std::size_t required =
      offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT);
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (bytes_returned < required || extents->NumberOfDiskExtents != 1 ||
      extents->Extents[0].StartingOffset.QuadPart < 0) {
    return ytec::clonecore::Result<VolumeLocation>::failure(make_error(
        ytec::clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"BIOS BCDBoot対象ボリュームのディスク対応検証",
        L"単一物理ディスク上の通常パーティションだけを使用できます"));
  }

  std::array<wchar_t, MAX_PATH> file_system{};
  if (!GetVolumeInformationW(
          root.c_str(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return ytec::clonecore::Result<VolumeLocation>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"BIOS BCDBoot対象ファイルシステム取得",
            GetLastError()));
  }
  return ytec::clonecore::Result<VolumeLocation>::success(VolumeLocation{
      .disk_number = extents->Extents[0].DiskNumber,
      .starting_offset = static_cast<std::uint64_t>(
          extents->Extents[0].StartingOffset.QuadPart),
      .file_system = file_system.data(),
  });
}

ytec::clonecore::Result<Selection> select_target(
    const std::uint32_t target_number,
    const std::wstring& windows_root,
    const std::wstring& system_root) {
  auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
  const auto inventory = provider->enumerate();
  if (!inventory) {
    return ytec::clonecore::Result<Selection>::failure(inventory.error());
  }
  if (!inventory.value().issues.empty()) {
    return ytec::clonecore::Result<Selection>::failure(make_error(
        ytec::clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"BIOS BCDBoot VM専用ディスク列挙",
        L"未解決の列挙診断があるため実行できません"));
  }
  const auto target = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [&](const auto& disk) { return disk.disk_number == target_number; });
  if (target == inventory.value().disks.end()) {
    return ytec::clonecore::Result<Selection>::failure(make_error(
        ytec::clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"BIOS BCDBoot VM専用コピー先検索",
        L"指定されたコピー先ディスクがありません"));
  }
  std::wstring model = target->model;
  std::transform(
      model.begin(), model.end(), model.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towupper(character));
      });
  if (model.find(L"VBOX") == std::wstring::npos ||
      target->size_bytes != kTargetDiskBytes ||
      target->logical_sector_size != 512 || target->is_system_disk ||
      target->partition_style != ytec::diskmodel::PartitionStyle::mbr ||
      target->read_only.value_or(true) || target->removable.value_or(true) ||
      target->serial_suffix.size() < 4) {
    return ytec::clonecore::Result<Selection>::failure(make_error(
        ytec::clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"BIOS BCDBoot VM専用コピー先制限",
        L"固定56GiBの非システムVirtualBox MBRコピー先だけを使用できます"));
  }

  const auto windows_location = query_volume_location(windows_root);
  const auto system_location = query_volume_location(system_root);
  if (!windows_location) {
    return ytec::clonecore::Result<Selection>::failure(
        windows_location.error());
  }
  if (!system_location) {
    return ytec::clonecore::Result<Selection>::failure(
        system_location.error());
  }
  if (windows_location.value().disk_number != target_number ||
      system_location.value().disk_number != target_number ||
      !equals_case_insensitive(
          windows_location.value().file_system, L"NTFS") ||
      !equals_case_insensitive(
          system_location.value().file_system, L"NTFS")) {
    return ytec::clonecore::Result<Selection>::failure(make_error(
        ytec::clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"BIOS BCDBoot対象ドライブの物理対応検証",
        L"Windows/システムドライブが指定コピー先のNTFSではありません"));
  }

  const auto windows_partition = std::find_if(
      target->partitions.begin(),
      target->partitions.end(),
      [&](const auto& partition) {
        return partition.offset_bytes ==
                   windows_location.value().starting_offset &&
               partition.style == ytec::diskmodel::PartitionStyle::mbr;
      });
  const auto active_system_partition = std::find_if(
      target->partitions.begin(),
      target->partitions.end(),
      [&](const auto& partition) {
        return partition.offset_bytes ==
                   system_location.value().starting_offset &&
               partition.style == ytec::diskmodel::PartitionStyle::mbr &&
               partition.bootable;
      });
  const auto active_count = std::count_if(
      target->partitions.begin(),
      target->partitions.end(),
      [](const auto& partition) { return partition.bootable; });
  if (windows_partition == target->partitions.end() ||
      active_system_partition == target->partitions.end() ||
      active_count != 1) {
    return ytec::clonecore::Result<Selection>::failure(make_error(
        ytec::clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"BIOS BCDBoot Activeパーティション検証",
        L"Windows/システムドライブと一意なActive MBRパーティションが一致しません"));
  }

  const std::wstring winload = windows_root + L"Windows\\System32\\winload.exe";
  const DWORD attributes = GetFileAttributesW(winload.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return ytec::clonecore::Result<Selection>::failure(make_error(
        ytec::clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"コピー先BIOS Windowsブートローダー確認",
        L"通常ファイルのwinload.exeをコピー先Windowsで確認できません"));
  }

  auto identity = ytec::diskmodel::make_stable_disk_identity(*target, false);
  if (!identity) {
    return ytec::clonecore::Result<Selection>::failure(identity.error());
  }
  return ytec::clonecore::Result<Selection>::success(Selection{
      .target = *target,
      .identity = identity.take_value(),
      .windows_partition = *windows_partition,
      .active_system_partition = *active_system_partition,
  });
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  const auto arguments = parse_arguments(argc, argv);
  if (!arguments.has_value()) {
    std::wcerr << L"Usage: --plan|--execute --target N --windows-root W:\\ "
                  L"--system-root S:\\ "
                  L"[--authorization TOKEN --confirmation TEXT]\n";
    return 64;
  }
  if (!ytec::vmtest::is_virtualbox_guest()) {
    std::wcerr << L"ERROR: VirtualBoxゲスト以外では実行できません\n";
    return 65;
  }
  const auto selection = select_target(
      arguments->target_number.value(),
      arguments->windows_root,
      arguments->system_root);
  if (!selection) {
    print_error(selection.error());
    return 1;
  }
  const std::wstring token = ytec::clonecore::make_target_confirmation_token(
      selection.value().identity);
  if (arguments->plan) {
    std::wcout << L"YDC_VM_BIOS_BCDBOOT_PLAN_V1\n"
               << L"targetDisk=" << selection.value().target.disk_number
               << L" targetBytes=" << selection.value().target.size_bytes
               << L" targetSerialSuffix="
               << std::wstring(
                      selection.value().target.serial_suffix.begin(),
                      selection.value().target.serial_suffix.end())
               << L"\nwindowsPartition="
               << selection.value().windows_partition.number
               << L" windowsRoot=" << arguments->windows_root
               << L"\nactiveSystemPartition="
               << selection.value().active_system_partition.number
               << L" systemRoot=" << arguments->system_root
               << L"\nconfirmation=" << token << L'\n';
    return 0;
  }
  if (!ytec::vmtest::is_administrator() ||
      arguments->authorization != kAuthorization ||
      arguments->confirmation != token) {
    std::wcerr << L"ERROR: 管理者権限、VM専用許可語、または確認文字列が一致しません\n";
    return 66;
  }

  const auto report = ytec::bootrepair::execute_bcdboot_with_windows_apis(
      ytec::bootrepair::BcdBootRequest{
          .target_windows_directory = arguments->windows_root + L"Windows",
          .target_system_partition_root = arguments->system_root,
          .firmware = ytec::bootrepair::BcdBootFirmware::bios,
      });
  if (!report) {
    print_error(report.error());
    return 1;
  }
  std::wcout << L"YDC_VM_BIOS_BCDBOOT_PASS\n"
             << L"targetDisk=" << selection.value().target.disk_number << L'\n'
             << L"windowsPartition="
             << selection.value().windows_partition.number << L'\n'
             << L"activeSystemPartition="
             << selection.value().active_system_partition.number << L'\n'
             << L"microsoftSignatureVerified="
             << (report.value().microsoft_signature_verified ? L"true"
                                                              : L"false")
             << L'\n'
             << L"exitCode=" << report.value().exit_code << L'\n';
  std::cout << "bcdbootStdoutBegin\n"
            << report.value().standard_output
            << "\nbcdbootStdoutEnd\n"
            << "bcdbootStderrBegin\n"
            << report.value().standard_error
            << "\nbcdbootStderrEnd\n";
  return 0;
}

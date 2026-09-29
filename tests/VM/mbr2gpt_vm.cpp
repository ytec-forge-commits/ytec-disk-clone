#include "vm_clone_execution_service.h"

#include "ytec/bootrepair/mbr2gpt.h"
#include "ytec/bootrepair/offline_windows.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/diskmodel/inventory_formatter.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    L"YTEC-VM-ONLY-PHASE4-MBR2GPT";
constexpr std::uint64_t kTargetDiskBytes =
    56ULL * 1024U * 1024U * 1024U;

struct Arguments final {
  bool plan{};
  bool execute{};
  std::optional<std::uint32_t> target_number;
  std::wstring windows_root;
  std::wstring authorization;
  std::wstring confirmation;
};

struct VolumeInspection final {
  std::uint32_t disk_number{};
  std::wstring file_system;
  bool bitlocker_signature{};
};

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

void print_error(const ytec::clonecore::Error& error) {
  std::cerr << "YDC_ERROR code=" << static_cast<unsigned int>(error.code)
            << " native=" << error.native_code
            << " operation=" << ytec::diskmodel::to_utf8(error.operation)
            << " message=" << ytec::diskmodel::to_utf8(error.message) << '\n';
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
      arguments.windows_root = argv[++index];
      if (is_drive_root(arguments.windows_root)) {
        arguments.windows_root[0] = static_cast<wchar_t>(
            std::towupper(arguments.windows_root[0]));
        arguments.windows_root[2] = L'\\';
      }
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
      !is_drive_root(arguments.windows_root)) {
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

ytec::clonecore::Result<VolumeInspection> inspect_windows_volume(
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
    return ytec::clonecore::Result<VolumeInspection>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"MBR2GPT VM対象Windowsボリューム読取り",
            GetLastError()));
  }

  std::array<std::byte, 1024> extent_buffer{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          volume.get(),
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0,
          extent_buffer.data(),
          static_cast<DWORD>(extent_buffer.size()),
          &bytes_returned,
          nullptr)) {
    return ytec::clonecore::Result<VolumeInspection>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"MBR2GPT VM対象Windowsボリューム対応取得",
            GetLastError()));
  }
  constexpr std::size_t kExtentBytes =
      offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DISK_EXTENT);
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extent_buffer.data());
  if (bytes_returned < kExtentBytes || extents->NumberOfDiskExtents != 1) {
    return ytec::clonecore::Result<VolumeInspection>::failure(make_error(
        ytec::clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"MBR2GPT VM対象Windowsボリューム対応検証",
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
    return ytec::clonecore::Result<VolumeInspection>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"MBR2GPT VM対象ファイルシステム取得",
            GetLastError()));
  }

  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(volume.get(), beginning, nullptr, FILE_BEGIN)) {
    return ytec::clonecore::Result<VolumeInspection>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"MBR2GPT VM対象ブートセクター位置設定",
            GetLastError()));
  }
  std::array<std::byte, 512> boot_sector{};
  DWORD bytes_read = 0;
  if (!ReadFile(
          volume.get(),
          boot_sector.data(),
          static_cast<DWORD>(boot_sector.size()),
          &bytes_read,
          nullptr) ||
      bytes_read != boot_sector.size()) {
    return ytec::clonecore::Result<VolumeInspection>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"MBR2GPT VM対象ブートセクター読取り",
            GetLastError()));
  }
  constexpr char kBitLockerSignature[] = "-FVE-FS-";
  const bool bitlocker = std::memcmp(
      boot_sector.data() + 3, kBitLockerSignature, 8) == 0;
  return ytec::clonecore::Result<VolumeInspection>::success(
      VolumeInspection{
          .disk_number = extents->Extents[0].DiskNumber,
          .file_system = file_system.data(),
          .bitlocker_signature = bitlocker,
      });
}

class VmMbr2GptTargetObserver final
    : public ytec::bootrepair::IMbr2GptTargetObserver {
 public:
  explicit VmMbr2GptTargetObserver(std::wstring windows_root)
      : windows_root_(std::move(windows_root)) {}

  ytec::clonecore::Result<ytec::clonecore::StableDiskIdentity> observe_target(
      const std::uint32_t candidate_disk_number) override {
    auto provider = ytec::diskmodel::make_windows_disk_inventory_provider();
    const auto inventory = provider->enumerate();
    if (!inventory) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(inventory.error());
    }
    if (!inventory.value().issues.empty()) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(make_error(
          ytec::clonecore::ErrorCode::query_failed,
          ERROR_INVALID_DATA,
          L"MBR2GPT VM専用ディスク列挙",
          L"未解決の列挙診断があるため実行できません"));
    }
    const auto target = std::find_if(
        inventory.value().disks.begin(),
        inventory.value().disks.end(),
        [&](const auto& disk) {
          return disk.disk_number == candidate_disk_number;
        });
    if (target == inventory.value().disks.end()) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(make_error(
          ytec::clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"MBR2GPT VM専用変換先検索",
          L"指定された変換先ディスクがありません"));
    }

    std::wstring model = target->model;
    std::transform(
        model.begin(), model.end(), model.begin(), [](const wchar_t character) {
          return static_cast<wchar_t>(std::towupper(character));
        });
    const auto active_count = std::count_if(
        target->partitions.begin(),
        target->partitions.end(),
        [](const auto& partition) { return partition.bootable; });
    const bool known_partition_types = std::all_of(
        target->partitions.begin(),
        target->partitions.end(),
        [](const auto& partition) {
          return partition.style == ytec::diskmodel::PartitionStyle::mbr &&
                 (partition.type == L"0x07" || partition.type == L"0x27");
        });
    if (model.find(L"VBOX") == std::wstring::npos ||
        target->size_bytes != kTargetDiskBytes ||
        target->logical_sector_size != 512 || target->is_system_disk ||
        target->partition_style != ytec::diskmodel::PartitionStyle::mbr ||
        target->offline.value_or(true) || target->read_only.value_or(true) ||
        target->removable.value_or(true) || target->serial_suffix.size() < 4 ||
        target->partitions.empty() || target->partitions.size() > 3 ||
        active_count != 1 || !known_partition_types) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(make_error(
          ytec::clonecore::ErrorCode::unsupported_layout,
          ERROR_ACCESS_DENIED,
          L"MBR2GPT VM専用変換先制限",
          L"固定56GiBのonline非システムVirtualBox基本MBRディスクだけを使用できます"));
    }

    const auto volume = inspect_windows_volume(windows_root_);
    if (!volume) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(volume.error());
    }
    const std::wstring winload =
        windows_root_ + L"Windows\\System32\\winload.exe";
    const DWORD attributes = GetFileAttributesW(winload.c_str());
    if (volume.value().disk_number != candidate_disk_number ||
        !equals_case_insensitive(volume.value().file_system, L"NTFS") ||
        volume.value().bitlocker_signature ||
        attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
                       FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(make_error(
          ytec::clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_DATA,
          L"MBR2GPT VM対象Windows検証",
          L"変換先上の非BitLocker NTFS Windows 10/11を確認できません"));
    }
    const auto architecture =
        ytec::bootrepair::verify_offline_windows_amd64(windows_root_);
    if (!architecture) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(
          architecture.error());
    }
    return ytec::diskmodel::make_stable_disk_identity(*target, false);
  }

 private:
  std::wstring windows_root_;
};

std::optional<std::wstring> system_directory() {
  std::array<wchar_t, MAX_PATH> buffer{};
  const UINT length = GetSystemDirectoryW(
      buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return std::nullopt;
  }
  return std::wstring(buffer.data(), length);
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  const auto arguments = parse_arguments(argc, argv);
  if (!arguments.has_value()) {
    std::wcerr << L"Usage: --plan|--execute --target N --windows-root W:\\ "
                  L"[--authorization TOKEN --confirmation TEXT]\n";
    return 64;
  }
  if (!ytec::vmtest::is_virtualbox_guest()) {
    std::wcerr << L"ERROR: VirtualBoxゲスト以外では実行できません\n";
    return 65;
  }

  VmMbr2GptTargetObserver observer(arguments->windows_root);
  const auto initial = observer.observe_target(arguments->target_number.value());
  if (!initial) {
    print_error(initial.error());
    return 1;
  }
  const std::wstring token =
      ytec::clonecore::make_target_confirmation_token(initial.value());
  if (arguments->plan) {
    std::wcout << L"YDC_VM_MBR2GPT_PLAN_V1\n"
               << L"targetDisk=" << arguments->target_number.value() << L'\n'
               << L"targetBytes=" << initial.value().size_bytes << L'\n'
               << L"windowsRoot=" << arguments->windows_root << L'\n'
               << L"confirmation=" << token << L'\n';
    return 0;
  }
  if (!ytec::vmtest::is_administrator() ||
      arguments->authorization != kAuthorization ||
      arguments->confirmation != token) {
    std::wcerr << L"ERROR: 管理者権限、VM専用許可語、または確認文字列が一致しません\n";
    return 66;
  }
  const auto system32 = system_directory();
  if (!system32.has_value()) {
    std::wcerr << L"ERROR: WinPE System32を取得できません\n";
    return 1;
  }

  auto verifier = ytec::bootrepair::make_windows_authenticode_verifier();
  auto runner = ytec::bootrepair::make_windows_process_runner();
  const auto report = ytec::bootrepair::execute_mbr2gpt_conversion(
      ytec::bootrepair::Mbr2GptConversionRequest{
          .candidate_disk_number = arguments->target_number.value(),
          .expected_target = initial.value(),
          .confirmation = ytec::clonecore::TargetConfirmation{
              .first_step_acknowledged = true,
              .typed_token = arguments->confirmation,
          },
      },
      system32.value(),
      observer,
      *verifier,
      *runner);
  if (!report) {
    print_error(report.error());
    return 1;
  }
  std::wcout << L"YDC_VM_MBR2GPT_PASS\n"
             << L"targetDisk=" << report.value().disk_number << L'\n'
             << L"microsoftSignatureVerified="
             << (report.value().microsoft_signature_verified ? L"true"
                                                              : L"false")
             << L'\n'
             << L"targetReidentifiedBeforeConversion="
             << (report.value().target_reidentified_before_conversion
                     ? L"true"
                     : L"false")
             << L'\n'
             << L"validateExitCode=" << report.value().validation.exit_code
             << L'\n'
             << L"convertExitCode=" << report.value().conversion.exit_code
             << L'\n';
  std::cout << "validateStdoutBegin\n"
            << report.value().validation.standard_output
            << "\nvalidateStdoutEnd\n"
            << "validateStderrBegin\n"
            << report.value().validation.standard_error
            << "\nvalidateStderrEnd\n"
            << "convertStdoutBegin\n"
            << report.value().conversion.standard_output
            << "\nconvertStdoutEnd\n"
            << "convertStderrBegin\n"
            << report.value().conversion.standard_error
            << "\nconvertStderrEnd\n";
  return 0;
}

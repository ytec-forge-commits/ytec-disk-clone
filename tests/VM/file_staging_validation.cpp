#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/windows_file_staging.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef YTEC_VM_DESTRUCTIVE_TEST_ONLY
#error This validation harness must never be built as a product target.
#endif

namespace {

constexpr std::wstring_view kAuthorization =
    L"YDC_PHASE5_FILE_STAGING_VM_ONLY";
constexpr std::wstring_view kDestinationRoot = L"T:\\";
constexpr std::wstring_view kFinalPath =
    L"T:\\YDC-Phase5-File-Staging\\synthetic.dcimg";
constexpr std::uint64_t kSourceDiskBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDestinationDiskBytes =
    512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kChunkOffset = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kChunkLength = 1ULL * 1024ULL * 1024ULL;

ytec::clonecore::Error harness_error(
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

template <typename T>
ytec::clonecore::Result<T> fail_result(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Result<T>::failure(harness_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool is_administrator() {
  SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
  PSID administrators = nullptr;
  if (!AllocateAndInitializeSid(
          &authority,
          2,
          SECURITY_BUILTIN_DOMAIN_RID,
          DOMAIN_ALIAS_RID_ADMINS,
          0,
          0,
          0,
          0,
          0,
          0,
          &administrators)) {
    return false;
  }
  BOOL member = FALSE;
  const BOOL checked =
      CheckTokenMembership(nullptr, administrators, &member);
  FreeSid(administrators);
  return checked != FALSE && member != FALSE;
}

bool contains_case_insensitive(
    const std::wstring_view text,
    const std::wstring_view needle) {
  if (needle.empty() || text.size() < needle.size()) {
    return false;
  }
  return std::search(
             text.begin(),
             text.end(),
             needle.begin(),
             needle.end(),
             [](const wchar_t left, const wchar_t right) {
               return std::towlower(left) == std::towlower(right);
             }) != text.end();
}

bool registry_key_exists(const wchar_t* sub_key) {
  HKEY key = nullptr;
  const LSTATUS result = RegOpenKeyExW(
      HKEY_LOCAL_MACHINE,
      sub_key,
      0,
      KEY_READ,
      &key);
  if (result != ERROR_SUCCESS) {
    return false;
  }
  RegCloseKey(key);
  return true;
}

ytec::clonecore::Result<std::wstring> read_bios_registry_value(
    const wchar_t* value_name) {
  std::array<wchar_t, 256> buffer{};
  DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  const LSTATUS result = RegGetValueW(
      HKEY_LOCAL_MACHINE,
      L"HARDWARE\\DESCRIPTION\\System\\BIOS",
      value_name,
      RRF_RT_REG_SZ,
      nullptr,
      buffer.data(),
      &bytes);
  if (result != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
    return fail_result<std::wstring>(
        ytec::clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"VirtualBox BIOS識別",
        L"BIOSレジストリ値を取得できませんでした");
  }
  return ytec::clonecore::Result<std::wstring>::success(buffer.data());
}

bool is_virtualbox_guest() {
  const auto manufacturer =
      read_bios_registry_value(L"SystemManufacturer");
  const auto product = read_bios_registry_value(L"SystemProductName");
  return (manufacturer &&
          (contains_case_insensitive(
               manufacturer.value(),
               L"VirtualBox") ||
           contains_case_insensitive(
               manufacturer.value(),
               L"innotek"))) ||
      (product &&
       contains_case_insensitive(product.value(), L"VirtualBox")) ||
      registry_key_exists(L"HARDWARE\\ACPI\\DSDT\\VBOX__");
}

class DeterministicReader final
    : public ytec::clonecore::ISourceDiskReader {
 public:
  DeterministicReader(
      const std::uint64_t size_bytes,
      const std::uint32_t logical_sector_size) noexcept
      : size_bytes_(size_bytes),
        logical_sector_size_(logical_sector_size) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return size_bytes_;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return logical_sector_size_;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > size_bytes_ ||
        length > size_bytes_ - offset) {
      return fail_result<std::vector<std::byte>>(
          ytec::clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"VM合成Reader範囲",
          L"要求範囲が固定合成ソース容量外です");
    }
    std::vector<std::byte> bytes(length);
    for (std::size_t index = 0; index < length; ++index) {
      const std::uint64_t position = offset + index;
      bytes[index] = static_cast<std::byte>(
          (position * 131ULL + 17ULL) & 0xFFULL);
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

 private:
  std::uint64_t size_bytes_{};
  std::uint32_t logical_sector_size_{};
};

ytec::clonecore::Result<std::uint32_t> disk_number_for_volume(
    const std::wstring_view root_path) {
  std::array<wchar_t, 64> volume_name{};
  if (!GetVolumeNameForVolumeMountPointW(
          std::wstring(root_path).c_str(),
          volume_name.data(),
          static_cast<DWORD>(volume_name.size()))) {
    return fail_result<std::uint32_t>(
        ytec::clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"VM保存先Volume GUID取得",
        L"固定T:のVolume GUIDを取得できませんでした");
  }
  std::wstring device_path(volume_name.data());
  if (device_path.ends_with(L'\\')) {
    device_path.pop_back();
  }
  ytec::clonecore::UniqueHandle volume(CreateFileW(
      device_path.c_str(),
      0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!volume) {
    return fail_result<std::uint32_t>(
        ytec::clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"VM保存先Volume対応付け",
        L"固定T:を対応付け用に開けませんでした");
  }

  std::array<std::byte, 4096> buffer{};
  DWORD returned = 0;
  if (!DeviceIoControl(
          volume.get(),
          IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
          nullptr,
          0,
          buffer.data(),
          static_cast<DWORD>(buffer.size()),
          &returned,
          nullptr) ||
      returned < offsetof(VOLUME_DISK_EXTENTS, Extents) +
          sizeof(DISK_EXTENT)) {
    return fail_result<std::uint32_t>(
        ytec::clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"VM保存先Volume Extent取得",
        L"固定T:の物理ディスク対応を取得できませんでした");
  }
  const auto* extents =
      reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
  if (extents->NumberOfDiskExtents != 1) {
    return fail_result<std::uint32_t>(
        ytec::clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"VM保存先Volume単一ディスク確認",
        L"固定T:が単一物理ディスクではありません");
  }
  return ytec::clonecore::Result<std::uint32_t>::success(
      extents->Extents[0].DiskNumber);
}

ytec::clonecore::Result<std::vector<std::byte>> read_complete_file(
    const std::wstring_view path,
    const std::uint64_t expected_length) {
  if (expected_length == 0 ||
      expected_length > 4ULL * 1024ULL * 1024ULL ||
      expected_length > std::numeric_limits<std::size_t>::max()) {
    return fail_result<std::vector<std::byte>>(
        ytec::clonecore::ErrorCode::invalid_argument,
        ERROR_FILE_TOO_LARGE,
        L"VM完成dcimgサイズ上限",
        L"完成dcimgが固定4MiB上限外です");
  }
  ytec::clonecore::UniqueHandle file(CreateFileW(
      std::wstring(path).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!file) {
    return fail_result<std::vector<std::byte>>(
        ytec::clonecore::ErrorCode::io_failed,
        GetLastError(),
        L"VM完成dcimg読取り",
        L"確定後dcimgを読み取り専用で開けませんでした");
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) != expected_length) {
    return fail_result<std::vector<std::byte>>(
        ytec::clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"VM完成dcimg長さ確認",
        L"確定後dcimg長が書込み報告と一致しません");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(expected_length));
  DWORD read = 0;
  if (!ReadFile(
          file.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &read,
          nullptr) ||
      read != bytes.size()) {
    return fail_result<std::vector<std::byte>>(
        ytec::clonecore::ErrorCode::io_failed,
        GetLastError(),
        L"VM完成dcimg全件読取り",
        L"確定後dcimgを全件読み取れませんでした");
  }
  return ytec::clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

void write_utf8_handle(
    const DWORD standard_handle,
    const std::wstring_view text) {
  if (text.empty() ||
      text.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return;
  }
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      text.data(),
      static_cast<int>(text.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) {
    return;
  }
  std::string utf8(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          text.data(),
          static_cast<int>(text.size()),
          utf8.data(),
          required,
          nullptr,
          nullptr) != required) {
    return;
  }
  const HANDLE handle = GetStdHandle(standard_handle);
  DWORD written = 0;
  WriteFile(
      handle,
      utf8.data(),
      static_cast<DWORD>(utf8.size()),
      &written,
      nullptr);
}

void print_error(const ytec::clonecore::Error& error) {
  write_utf8_handle(
      STD_ERROR_HANDLE,
      L"YDC_PHASE5_FILE_STAGING_VM_FAIL operation=" +
          error.operation + L" native=" +
          std::to_wstring(error.native_code) + L" message=" +
          error.message + L"\r\n");
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  if (argc != 3 || std::wstring_view(argv[1]) != L"--authorize" ||
      std::wstring_view(argv[2]) != kAuthorization) {
    std::wcerr
        << L"VM専用です。--authorize "
        << kAuthorization << L" が必要です\n";
    return 64;
  }
  if (!is_administrator()) {
    std::wcerr
        << L"YDC_PHASE5_FILE_STAGING_VM_FAIL 管理者権限が必要です\n";
    return 5;
  }
  if (!is_virtualbox_guest()) {
    std::wcerr
        << L"YDC_PHASE5_FILE_STAGING_VM_FAIL "
        << L"VirtualBox専用VMではありません\n";
    return 5;
  }

  wchar_t file_system[MAX_PATH]{};
  if (!GetVolumeInformationW(
          kDestinationRoot.data(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          file_system,
          MAX_PATH) ||
      _wcsicmp(file_system, L"NTFS") != 0) {
    std::wcerr
        << L"YDC_PHASE5_FILE_STAGING_VM_FAIL 固定T:がNTFSではありません\n";
    return 1;
  }

  auto inventory = ytec::diskmodel::make_windows_disk_inventory_provider();
  const auto report = inventory->enumerate();
  if (!report) {
    print_error(report.error());
    return 1;
  }
  if (!report.value().issues.empty() || report.value().disks.size() != 3) {
    std::wcerr
        << L"YDC_PHASE5_FILE_STAGING_VM_FAIL "
        << L"列挙問題があるか固定3台構成ではありません\n";
    return 1;
  }

  const auto destination_number =
      disk_number_for_volume(kDestinationRoot);
  if (!destination_number) {
    print_error(destination_number.error());
    return 1;
  }

  const ytec::diskmodel::DiskInfo* source = nullptr;
  const ytec::diskmodel::DiskInfo* destination = nullptr;
  std::size_t system_disk_count = 0;
  for (const auto& disk : report.value().disks) {
    if (disk.is_system_disk) {
      ++system_disk_count;
    }
    if (!disk.is_system_disk &&
        disk.size_bytes == kSourceDiskBytes &&
        disk.partitions.empty() &&
        contains_case_insensitive(disk.model, L"VBOX")) {
      if (source != nullptr) {
        std::wcerr
            << L"YDC_PHASE5_FILE_STAGING_VM_FAIL "
            << L"固定128MiBソース候補が重複しています\n";
        return 1;
      }
      source = &disk;
    }
    if (disk.disk_number == destination_number.value()) {
      destination = &disk;
    }
  }
  if (system_disk_count != 1 || source == nullptr ||
      destination == nullptr || destination->is_system_disk ||
      destination->size_bytes != kDestinationDiskBytes ||
      !contains_case_insensitive(destination->model, L"VBOX") ||
      destination->disk_number == source->disk_number) {
    std::wcerr
        << L"YDC_PHASE5_FILE_STAGING_VM_FAIL "
        << L"固定合成ディスク構成が一致しません\n";
    return 1;
  }

  const auto source_identity =
      ytec::diskmodel::make_stable_disk_identity(*source, false);
  if (!source_identity) {
    print_error(source_identity.error());
    return 1;
  }

  const std::string manifest_text =
      "{\"schemaVersion\":1,\"kind\":\"phase5-file-staging-vm\","
      "\"source\":\"deterministic-synthetic-reader\"}";
  std::vector<std::byte> manifest(manifest_text.size());
  std::transform(
      manifest_text.begin(),
      manifest_text.end(),
      manifest.begin(),
      [](const char value) {
        return static_cast<std::byte>(
            static_cast<unsigned char>(value));
      });
  ytec::imageformat::PartitionSnapshot partition_snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = source->size_bytes,
      .logical_sector_size = source->logical_sector_size,
      .regions = {
          ytec::imageformat::PartitionTableRegion{
              .disk_offset = 0,
              .data = std::vector<std::byte>(
                  source->logical_sector_size,
                  std::byte{0}),
          },
      },
  };
  const auto encoded_snapshot =
      ytec::imageformat::build_partition_snapshot_v1(partition_snapshot);
  if (!encoded_snapshot) {
    print_error(encoded_snapshot.error());
    return 1;
  }

  const auto staging =
      ytec::imageformat::make_windows_file_staging_target(
          ytec::imageformat::WindowsFileStagingRequest{
              .final_path = std::wstring(kFinalPath),
              .expected_source_disk = source_identity.value(),
              .expected_clone_target_disk = std::nullopt,
          });
  if (!staging) {
    print_error(staging.error());
    return 1;
  }

  DeterministicReader reader(
      source->size_bytes,
      source->logical_sector_size);
  const auto image = ytec::imageformat::write_verified_uncompressed_dcimg_v1(
      ytec::imageformat::DcimgStreamBuildRequest{
          .source_disk_size = source->size_bytes,
          .logical_sector_size = source->logical_sector_size,
          .physical_sector_size = source->physical_sector_size,
          .chunk_size = ytec::imageformat::kDcimgChunkSize16MiB,
          .verification_block_bytes = 64U * 1024U,
          .manifest = std::move(manifest),
          .partition_table_snapshot = encoded_snapshot.value(),
          .chunks = {
              ytec::imageformat::DcimgStreamChunk{
                  .logical_offset = kChunkOffset,
                  .logical_length = kChunkLength,
                  .source_offset = kChunkOffset,
                  .zero_filled = false,
                  .source = &reader,
              },
          },
      },
      *staging.value());
  if (!image) {
    print_error(image.error());
    return 1;
  }
  if (!image.value().committed ||
      !image.value().all_chunks_read_back_verified ||
      !image.value().global_hash_read_back_verified) {
    std::wcerr
        << L"YDC_PHASE5_FILE_STAGING_VM_FAIL "
        << L"ストリーム書込み報告が完全成功ではありません\n";
    return 1;
  }

  const auto completed =
      read_complete_file(kFinalPath, image.value().image_length);
  if (!completed) {
    print_error(completed.error());
    return 1;
  }
  const auto inspection =
      ytec::imageformat::inspect_dcimg_v1(completed.value());
  if (!inspection || !inspection.value().manifest_hash_verified ||
      !inspection.value().chunk_hashes_verified ||
      !inspection.value().global_hash_verified) {
    if (!inspection) {
      print_error(inspection.error());
    } else {
      std::wcerr
          << L"YDC_PHASE5_FILE_STAGING_VM_FAIL "
          << L"確定後dcimgの全ハッシュ検証が完了していません\n";
    }
    return 1;
  }

  std::wcout
      << L"YDC_PHASE5_FILE_STAGING_VM_PASS"
      << L" sourceDisk=" << source->disk_number
      << L" destinationDisk=" << destination->disk_number
      << L" imageBytes=" << image.value().image_length
      << L" storedBytes=" << image.value().stored_data_bytes
      << L" chunks=" << image.value().chunk_count << L'\n';
  return 0;
}

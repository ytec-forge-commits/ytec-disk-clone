#include "ytec/clonecore/log.h"
#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/vssrequester/snapshot_reader.h"
#include "ytec/vssrequester/windows_backend.h"
#include "ytec/vssrequester/workflow.h"

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
    L"YDC_PHASE5_VSS_VM_ONLY";
constexpr std::wstring_view kSentinelRelativePath =
    L"YDC-VSS-Validation\\sentinel.txt";
constexpr std::string_view kExpectedSentinel =
    "YDC_PHASE5_VM_SENTINEL_20260730\r\n";

struct VolumeGeometry final {
  std::uint64_t size_bytes{};
  std::uint32_t logical_sector_size{};
};

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

ytec::clonecore::Status fail(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Status::failure(harness_error(
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
    return ytec::clonecore::Result<std::wstring>::failure(harness_error(
        ytec::clonecore::ErrorCode::query_failed,
        static_cast<DWORD>(result),
        L"VirtualBox BIOS識別",
        L"BIOSレジストリ値を取得できませんでした"));
  }
  return ytec::clonecore::Result<std::wstring>::success(buffer.data());
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

ytec::clonecore::Status require_virtualbox_guest() {
  const auto manufacturer =
      read_bios_registry_value(L"SystemManufacturer");
  const auto product = read_bios_registry_value(L"SystemProductName");
  const bool virtualbox =
      (manufacturer &&
       (contains_case_insensitive(
            manufacturer.value(),
            L"VirtualBox") ||
        contains_case_insensitive(
            manufacturer.value(),
            L"innotek"))) ||
      (product &&
       contains_case_insensitive(product.value(), L"VirtualBox")) ||
      registry_key_exists(L"HARDWARE\\ACPI\\DSDT\\VBOX__");
  if (!virtualbox) {
    return fail(
        ytec::clonecore::ErrorCode::access_denied,
        ERROR_NOT_SUPPORTED,
        L"VirtualBoxゲスト確認",
        L"このハーネスはVirtualBox専用VM以外では実行できません");
  }
  return ytec::clonecore::success_status();
}

ytec::clonecore::Result<std::wstring> system_volume_guid_path() {
  std::array<wchar_t, 50> path{};
  if (!GetVolumeNameForVolumeMountPointW(
          L"C:\\",
          path.data(),
          static_cast<DWORD>(path.size()))) {
    return ytec::clonecore::Result<std::wstring>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"固定C: Volume GUID取得",
            GetLastError()));
  }
  return ytec::clonecore::Result<std::wstring>::success(path.data());
}

ytec::clonecore::Result<VolumeGeometry> query_volume_geometry(
    const std::wstring& volume_guid_path) {
  std::wstring path = volume_guid_path;
  if (path.ends_with(L'\\')) {
    path.pop_back();
  }
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!handle) {
    return ytec::clonecore::Result<VolumeGeometry>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"固定C: Volume読取り専用オープン",
            GetLastError()));
  }

  GET_LENGTH_INFORMATION length{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle.get(),
          IOCTL_DISK_GET_LENGTH_INFO,
          nullptr,
          0,
          &length,
          sizeof(length),
          &bytes_returned,
          nullptr) ||
      bytes_returned < sizeof(length) || length.Length.QuadPart <= 0) {
    return ytec::clonecore::Result<VolumeGeometry>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"固定C: Volume容量取得",
            GetLastError()));
  }

  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageAccessAlignmentProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
  bytes_returned = 0;
  if (!DeviceIoControl(
          handle.get(),
          IOCTL_STORAGE_QUERY_PROPERTY,
          &query,
          sizeof(query),
          &alignment,
          sizeof(alignment),
          &bytes_returned,
          nullptr) ||
      bytes_returned < sizeof(alignment) ||
      alignment.BytesPerLogicalSector == 0) {
    return ytec::clonecore::Result<VolumeGeometry>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::query_failed,
            L"固定C: Volume論理セクター取得",
            GetLastError()));
  }
  return ytec::clonecore::Result<VolumeGeometry>::success(VolumeGeometry{
      .size_bytes = static_cast<std::uint64_t>(length.Length.QuadPart),
      .logical_sector_size = alignment.BytesPerLogicalSector,
  });
}

ytec::clonecore::Result<std::vector<std::byte>> read_small_file(
    const std::wstring& path) {
  ytec::clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!handle) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VSS合成Sentinel読取り",
            GetLastError()));
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart <= 0 ||
      size.QuadPart > 1024) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        harness_error(
            ytec::clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"VSS合成Sentinelサイズ確認",
            L"Sentinelが空または1KiB上限を超えています"));
  }
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(size.QuadPart));
  DWORD bytes_read = 0;
  if (!ReadFile(
          handle.get(),
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &bytes_read,
          nullptr) ||
      bytes_read != bytes.size()) {
    return ytec::clonecore::Result<std::vector<std::byte>>::failure(
        ytec::clonecore::make_win32_error(
            ytec::clonecore::ErrorCode::io_failed,
            L"VSS合成Sentinel完全読取り",
            GetLastError()));
  }
  return ytec::clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

ytec::clonecore::Status verify_sentinel(
    const std::wstring& path) {
  const auto bytes = read_small_file(path);
  if (!bytes) {
    return ytec::clonecore::Status::failure(bytes.error());
  }
  const auto expected =
      std::as_bytes(std::span(kExpectedSentinel.data(), kExpectedSentinel.size()));
  if (!std::equal(
          bytes.value().begin(),
          bytes.value().end(),
          expected.begin(),
          expected.end())) {
    return fail(
        ytec::clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"VSS合成Sentinel内容確認",
        L"Snapshot内Sentinelが固定合成データと一致しません");
  }
  return ytec::clonecore::success_status();
}

void write_ascii_handle(
    const DWORD standard_handle,
    const std::string& text) {
  const HANDLE handle = GetStdHandle(standard_handle);
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
      text.empty() ||
      text.size() > std::numeric_limits<DWORD>::max()) {
    return;
  }
  DWORD bytes_written = 0;
  static_cast<void>(WriteFile(
      handle,
      text.data(),
      static_cast<DWORD>(text.size()),
      &bytes_written,
      nullptr));
}

void print_error(const ytec::clonecore::Error& error) {
  std::string operation_tag = "other";
  if (error.operation.find(L"Snapshot容量") != std::wstring::npos) {
    operation_tag = "snapshot-length";
  } else if (
      error.operation.find(L"Snapshot論理セクター") !=
      std::wstring::npos) {
    operation_tag = "snapshot-storage-sector";
  } else if (
      error.operation.find(L"Snapshotファイルシステムセクター") !=
      std::wstring::npos) {
    operation_tag = "snapshot-filesystem-sector";
  } else if (
      error.operation.find(L"Bitmap") != std::wstring::npos) {
    operation_tag = "snapshot-bitmap";
  } else if (
      error.operation.find(L"Writer") != std::wstring::npos) {
    operation_tag = "writer";
  }
  write_ascii_handle(
      STD_ERROR_HANDLE,
      "YDC_PHASE5_VSS_VM_FAIL operation=" + operation_tag +
          " native=" +
          std::to_string(error.native_code) + "\r\n");
  std::wcerr << L"YDC_PHASE5_VSS_VM_FAIL operation=" << error.operation
             << L" native=" << error.native_code
             << L" message=" << error.message << L'\n';
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
  if (argc != 3 || std::wstring_view(argv[1]) != L"--authorize" ||
      std::wstring_view(argv[2]) != kAuthorization) {
    std::wcerr
        << L"VM専用です。--authorize YDC_PHASE5_VSS_VM_ONLY が必要です\n";
    return 64;
  }
  if (!is_administrator()) {
    std::wcerr << L"YDC_PHASE5_VSS_VM_FAIL 管理者権限が必要です\n";
    return 5;
  }
  const auto virtualbox = require_virtualbox_guest();
  if (!virtualbox) {
    print_error(virtualbox.error());
    return 5;
  }

  const auto volume = system_volume_guid_path();
  if (!volume) {
    print_error(volume.error());
    return 1;
  }
  wchar_t file_system[MAX_PATH]{};
  if (!GetVolumeInformationW(
          volume.value().c_str(),
          nullptr,
          0,
          nullptr,
          nullptr,
          nullptr,
          file_system,
          MAX_PATH) ||
      _wcsicmp(file_system, L"NTFS") != 0) {
    std::wcerr << L"YDC_PHASE5_VSS_VM_FAIL 固定C:がNTFSではありません\n";
    return 1;
  }

  const std::wstring live_sentinel =
      std::wstring(L"C:\\") + std::wstring(kSentinelRelativePath);
  const auto live_sentinel_status = verify_sentinel(live_sentinel);
  if (!live_sentinel_status) {
    print_error(live_sentinel_status.error());
    return 1;
  }
  const auto geometry = query_volume_geometry(volume.value());
  if (!geometry) {
    print_error(geometry.error());
    return 1;
  }

  ytec::clonecore::Logger logger(
      [](const ytec::clonecore::LogRecord& record) {
        std::wcout << L"["
                   << ytec::clonecore::log_level_name(record.level)
                   << L"] " << record.message << L'\n';
      });
  std::size_t used_range_count = 0;
  std::uint64_t used_bytes = 0;
  ytec::vssrequester::WindowsVssBackend backend(
      ytec::vssrequester::WindowsVssBackendOptions{
          .async_wait =
              ytec::vssrequester::AsyncWaitOptions{
                  .timeout_ms = 180'000,
                  .poll_interval_ms = 250,
              },
          .copy_snapshot_data =
              [&](const ytec::vssrequester::SnapshotCopyContext& context) {
                if (context.snapshot_set_id.empty() ||
                    context.mappings.size() != 1U ||
                    context.mappings.front().snapshot_id.empty()) {
                  return fail(
                      ytec::clonecore::ErrorCode::identity_mismatch,
                      ERROR_INVALID_DATA,
                      L"VSS VM Snapshot件数確認",
                      L"固定C:に対してSnapshotが1件ではありません");
                }
                const auto& snapshot_path =
                    context.mappings.front().snapshot_device_path;
                const std::wstring snapshot_sentinel =
                    snapshot_path + L"\\" +
                    std::wstring(kSentinelRelativePath);
                const auto sentinel = verify_sentinel(snapshot_sentinel);
                if (!sentinel) {
                  return sentinel;
                }

                auto reader = ytec::vssrequester::
                    open_snapshot_volume_reader_with_windows_apis(
                        ytec::vssrequester::SnapshotVolumeOpenRequest{
                            .snapshot_device_path =
                                snapshot_path,
                            .expected_size_bytes =
                                geometry.value().size_bytes,
                            .logical_sector_size =
                                geometry.value().logical_sector_size,
                        });
                if (!reader) {
                  return ytec::clonecore::Status::failure(
                      reader.error());
                }
                const auto boot_sector = reader.value()->read(
                    0,
                    geometry.value().logical_sector_size);
                if (!boot_sector) {
                  return ytec::clonecore::Status::failure(
                      boot_sector.error());
                }
                const auto ntfs = ytec::clonecore::parse_ntfs_geometry(
                    boot_sector.value(),
                    geometry.value().logical_sector_size,
                    geometry.value().size_bytes);
                if (!ntfs) {
                  return ytec::clonecore::Status::failure(ntfs.error());
                }

                ytec::clonecore::WindowsSnapshotVolumeBitmapProvider
                    bitmap_provider({
                        ytec::clonecore::SnapshotVolumeBitmapBinding{
                            .partition_entry_index = 0,
                            .snapshot_device_path = snapshot_path,
                        },
                    });
                const auto ranges =
                    bitmap_provider.query_used_ranges(0, ntfs.value());
                if (!ranges) {
                  return ytec::clonecore::Status::failure(
                      ranges.error());
                }
                used_range_count = ranges.value().size();
                used_bytes = 0;
                for (const auto& range : ranges.value()) {
                  if (range.length >
                      std::numeric_limits<std::uint64_t>::max() -
                          used_bytes) {
                    return fail(
                        ytec::clonecore::ErrorCode::invalid_data,
                        ERROR_ARITHMETIC_OVERFLOW,
                        L"VSS VM使用範囲合計",
                        L"使用範囲合計がオーバーフローしました");
                  }
                  used_bytes += range.length;
                }
                return ytec::clonecore::success_status();
              },
          .logger = &logger,
      });

  const auto report = ytec::vssrequester::execute_backup_workflow(
      ytec::vssrequester::WorkflowRequest{
          .administrator = true,
          .volumes = {
              ytec::vssrequester::VolumeRequest{
                  .volume_guid_path = volume.value(),
                  .file_system = L"NTFS",
              },
          },
      },
      backend);
  if (!report) {
    print_error(report.error());
    return 1;
  }
  write_ascii_handle(
      STD_OUTPUT_HANDLE,
      "YDC_PHASE5_VSS_VM_PASS writers=" +
          std::to_string(report.value().writer_count) +
          " usedRanges=" + std::to_string(used_range_count) +
          " usedBytes=" + std::to_string(used_bytes) + "\r\n");
  std::wcout << L"YDC_PHASE5_VSS_VM_PASS"
             << L" snapshotSet=" << report.value().snapshot_set_id
             << L" writers=" << report.value().writer_count
             << L" usedRanges=" << used_range_count
             << L" usedBytes=" << used_bytes << L'\n';
  return 0;
}

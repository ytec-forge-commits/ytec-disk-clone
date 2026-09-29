#include "ytec/vssrequester/snapshot_reader.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::vssrequester {
namespace {

clonecore::Error snapshot_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool is_snapshot_device_path(const std::wstring_view path) {
  constexpr std::wstring_view prefix =
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy";
  if (!path.starts_with(prefix) || path.size() <= prefix.size()) {
    return false;
  }
  std::wstring_view suffix = path.substr(prefix.size());
  if (suffix.ends_with(L'\\')) {
    suffix.remove_suffix(1);
  }
  return !suffix.empty() &&
         std::all_of(
             suffix.begin(),
             suffix.end(),
             [](const wchar_t value) {
               return value >= L'0' && value <= L'9';
             });
}

clonecore::Status validate_read_range(
    const std::uint64_t size_bytes,
    const std::uint32_t logical_sector_size,
    const std::uint64_t offset,
    const std::size_t length) {
  if (offset > size_bytes || length > size_bytes - offset ||
      offset > static_cast<std::uint64_t>(
                   std::numeric_limits<LONGLONG>::max()) ||
      length > std::numeric_limits<DWORD>::max() ||
      offset % logical_sector_size != 0 ||
      length % logical_sector_size != 0) {
    return clonecore::Status::failure(snapshot_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS Snapshot読取り範囲確認",
        L"読取り範囲がSnapshot境界外、Windows API上限外、または論理セクター非整列です"));
  }
  return clonecore::success_status();
}

struct ObservedSnapshotGeometry final {
  std::uint64_t size_bytes{};
  std::uint32_t logical_sector_size{};
};

clonecore::Result<ObservedSnapshotGeometry> query_snapshot_geometry(
    const HANDLE handle,
    const std::wstring_view snapshot_device_path) {
  GET_LENGTH_INFORMATION length{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_DISK_GET_LENGTH_INFO,
          nullptr,
          0,
          &length,
          sizeof(length),
          &bytes_returned,
          nullptr) ||
      bytes_returned < sizeof(length) || length.Length.QuadPart <= 0) {
    return clonecore::Result<ObservedSnapshotGeometry>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"VSS Snapshot容量の再確認",
            GetLastError()));
  }

  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageAccessAlignmentProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
  bytes_returned = 0;
  const BOOL alignment_queried = DeviceIoControl(
          handle,
          IOCTL_STORAGE_QUERY_PROPERTY,
          &query,
          sizeof(query),
          &alignment,
          sizeof(alignment),
          &bytes_returned,
          nullptr);
  std::uint32_t logical_sector_size = 0;
  if (alignment_queried &&
      bytes_returned >= sizeof(alignment) &&
      alignment.BytesPerLogicalSector != 0) {
    logical_sector_size = alignment.BytesPerLogicalSector;
  } else {
    const DWORD alignment_error =
        alignment_queried ? ERROR_INVALID_DATA : GetLastError();
    if (alignment_error != ERROR_INVALID_FUNCTION &&
        alignment_error != ERROR_NOT_SUPPORTED) {
      return clonecore::Result<ObservedSnapshotGeometry>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"VSS Snapshot論理セクターの再確認",
              alignment_error));
    }

    std::wstring snapshot_root(snapshot_device_path);
    if (!snapshot_root.ends_with(L'\\')) {
      snapshot_root.push_back(L'\\');
    }
    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;
    if (!GetDiskFreeSpaceW(
            snapshot_root.c_str(),
            &sectors_per_cluster,
            &bytes_per_sector,
            &free_clusters,
            &total_clusters) ||
        bytes_per_sector == 0) {
      return clonecore::Result<ObservedSnapshotGeometry>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"VSS Snapshotファイルシステムセクターの再確認",
              GetLastError()));
    }
    logical_sector_size = bytes_per_sector;
  }
  return clonecore::Result<ObservedSnapshotGeometry>::success(
      ObservedSnapshotGeometry{
          .size_bytes =
              static_cast<std::uint64_t>(length.Length.QuadPart),
          .logical_sector_size = logical_sector_size,
      });
}

class WindowsSnapshotVolumeReader final
    : public clonecore::ISourceDiskReader {
 public:
  WindowsSnapshotVolumeReader(
      clonecore::UniqueHandle handle,
      const std::uint64_t size_bytes,
      const std::uint32_t logical_sector_size)
      : handle_(std::move(handle)),
        size_bytes_(size_bytes),
        logical_sector_size_(logical_sector_size) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return size_bytes_;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return logical_sector_size_;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    const auto range = validate_read_range(
        size_bytes_,
        logical_sector_size_,
        offset,
        length);
    if (!range) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          range.error());
    }
    if (length == 0) {
      return clonecore::Result<std::vector<std::byte>>::success({});
    }

    const std::scoped_lock lock(mutex_);
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"VSS Snapshot読取り位置設定",
              GetLastError()));
    }

    std::vector<std::byte> bytes(length);
    DWORD bytes_read = 0;
    if (!ReadFile(
            handle_.get(),
            bytes.data(),
            static_cast<DWORD>(bytes.size()),
            &bytes_read,
            nullptr) ||
        bytes_read != bytes.size()) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<std::vector<std::byte>>::failure(
          snapshot_error(
              clonecore::ErrorCode::io_failed,
              native_code == ERROR_SUCCESS ? ERROR_HANDLE_EOF
                                           : native_code,
              L"VSS Snapshot読取り",
              L"Snapshotデバイスから要求バイト数を完全に読み取れませんでした"));
    }
    return clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

 private:
  clonecore::UniqueHandle handle_;
  std::uint64_t size_bytes_{};
  std::uint32_t logical_sector_size_{};
  mutable std::mutex mutex_;
};

class WindowsSnapshotVolumeBackend final
    : public ISnapshotVolumeBackend {
 public:
  clonecore::Result<std::unique_ptr<clonecore::ISourceDiskReader>>
  open_read_only(const SnapshotVolumeOpenRequest& request) override {
    std::wstring path = request.snapshot_device_path;
    if (path.ends_with(L'\\')) {
      path.pop_back();
    }
    clonecore::UniqueHandle handle(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      const DWORD native_code = GetLastError();
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          clonecore::make_win32_error(
              native_code == ERROR_ACCESS_DENIED
                  ? clonecore::ErrorCode::access_denied
                  : clonecore::ErrorCode::io_failed,
              L"VSS Snapshotデバイスの読み取り専用オープン",
              native_code));
    }

    const auto geometry = query_snapshot_geometry(
        handle.get(),
        request.snapshot_device_path);
    if (!geometry) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          geometry.error());
    }
    if (geometry.value().size_bytes != request.expected_size_bytes ||
        geometry.value().logical_sector_size !=
            request.logical_sector_size) {
      return clonecore::Result<
          std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
          snapshot_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"VSS SnapshotデバイスGeometry確認",
              L"オープン後の容量または論理セクターが固定済み要求と一致しません"));
    }

    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::success(
        std::make_unique<WindowsSnapshotVolumeReader>(
            std::move(handle),
            request.expected_size_bytes,
            request.logical_sector_size));
  }
};

}  // namespace

clonecore::Status validate_snapshot_volume_open_request(
    const SnapshotVolumeOpenRequest& request) {
  if (!is_snapshot_device_path(request.snapshot_device_path) ||
      request.expected_size_bytes == 0 ||
      (request.logical_sector_size != 512 &&
       request.logical_sector_size != 4096) ||
      request.expected_size_bytes % request.logical_sector_size != 0) {
    return clonecore::Status::failure(snapshot_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS Snapshotデバイス要求検証",
        L"正規Snapshotパス、セクター整列容量、512/4096バイト論理セクターが必要です"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::unique_ptr<clonecore::ISourceDiskReader>>
open_snapshot_volume_reader(
    const SnapshotVolumeOpenRequest& request,
    ISnapshotVolumeBackend& backend) {
  const auto validated =
      validate_snapshot_volume_open_request(request);
  if (!validated) {
    return clonecore::Result<
        std::unique_ptr<clonecore::ISourceDiskReader>>::failure(
        validated.error());
  }
  return backend.open_read_only(request);
}

clonecore::Result<std::unique_ptr<clonecore::ISourceDiskReader>>
open_snapshot_volume_reader_with_windows_apis(
    const SnapshotVolumeOpenRequest& request) {
  WindowsSnapshotVolumeBackend backend;
  return open_snapshot_volume_reader(request, backend);
}

}  // namespace ytec::vssrequester

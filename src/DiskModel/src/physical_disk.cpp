#include "ytec/diskmodel/physical_disk.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::diskmodel {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::ISourceDiskReader;
using clonecore::ITargetDiskWriter;
using clonecore::Result;
using clonecore::StableDiskIdentity;
using clonecore::Status;
using clonecore::UniqueHandle;

Error physical_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool physical_path_matches(const DiskInfo& disk) {
  const std::wstring expected =
      L"\\\\.\\PhysicalDrive" + std::to_wstring(disk.disk_number);
  return _wcsicmp(disk.device_path.c_str(), expected.c_str()) == 0;
}

struct RawDiskGeometry final {
  std::uint32_t disk_number{};
  std::uint64_t size_bytes{};
  std::uint32_t logical_sector_size{};
};

Result<RawDiskGeometry> query_raw_disk_geometry(const HANDLE handle) {
  STORAGE_DEVICE_NUMBER device_number{};
  DWORD bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_GET_DEVICE_NUMBER,
          nullptr,
          0,
          &device_number,
          sizeof(device_number),
          &bytes_returned,
          nullptr) ||
      bytes_returned < sizeof(device_number) ||
      device_number.DeviceType != FILE_DEVICE_DISK) {
    return Result<RawDiskGeometry>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"生ディスク番号の再確認",
        GetLastError()));
  }

  GET_LENGTH_INFORMATION length{};
  bytes_returned = 0;
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
    return Result<RawDiskGeometry>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"生ディスク容量の再確認",
        GetLastError()));
  }

  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageAccessAlignmentProperty;
  query.QueryType = PropertyStandardQuery;
  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
  bytes_returned = 0;
  if (!DeviceIoControl(
          handle,
          IOCTL_STORAGE_QUERY_PROPERTY,
          &query,
          sizeof(query),
          &alignment,
          sizeof(alignment),
          &bytes_returned,
          nullptr) ||
      bytes_returned < sizeof(alignment) ||
      alignment.BytesPerLogicalSector == 0) {
    return Result<RawDiskGeometry>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"生ディスクセクターサイズの再確認",
        GetLastError()));
  }

  return Result<RawDiskGeometry>::success(RawDiskGeometry{
      .disk_number = device_number.DeviceNumber,
      .size_bytes = static_cast<std::uint64_t>(length.Length.QuadPart),
      .logical_sector_size = alignment.BytesPerLogicalSector,
  });
}

Status validate_opened_geometry(
    const DiskInfo& expected,
    const RawDiskGeometry& observed,
    const std::wstring_view operation) {
  if (expected.disk_number != observed.disk_number ||
      expected.size_bytes != observed.size_bytes ||
      expected.logical_sector_size != observed.logical_sector_size) {
    return Status::failure(physical_error(
        ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        std::wstring(operation),
        L"オープン後のディスク番号、容量、またはセクターサイズが再識別結果と一致しません"));
  }
  return clonecore::success_status();
}

Status validate_io_range(
    const std::uint64_t disk_size,
    const std::uint64_t offset,
    const std::size_t length,
    const std::wstring_view operation) {
  if (offset > disk_size || length > disk_size - offset ||
      offset > static_cast<std::uint64_t>(
                   std::numeric_limits<LONGLONG>::max()) ||
      length > std::numeric_limits<DWORD>::max()) {
    return Status::failure(physical_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        std::wstring(operation),
        L"生ディスクI/O範囲が境界外またはWindows API上限外です"));
  }
  return clonecore::success_status();
}

Result<std::vector<std::byte>> read_disk_exact(
    const HANDLE handle,
    const std::uint64_t disk_size,
    const std::uint64_t offset,
    const std::size_t length,
    const std::wstring_view operation) {
  const Status range_status =
      validate_io_range(disk_size, offset, length, operation);
  if (!range_status) {
    return Result<std::vector<std::byte>>::failure(range_status.error());
  }
  if (length == 0) {
    return Result<std::vector<std::byte>>::success({});
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(ErrorCode::io_failed, operation, GetLastError()));
  }
  std::vector<std::byte> bytes(length);
  DWORD bytes_read = 0;
  if (!ReadFile(
          handle,
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &bytes_read,
          nullptr) ||
      bytes_read != bytes.size()) {
    const DWORD native_code = GetLastError();
    return Result<std::vector<std::byte>>::failure(physical_error(
        ErrorCode::io_failed,
        native_code == ERROR_SUCCESS ? ERROR_HANDLE_EOF : native_code,
        std::wstring(operation),
        L"生ディスクから要求したバイト数を完全に読み取れませんでした"));
  }
  return Result<std::vector<std::byte>>::success(std::move(bytes));
}

Status write_disk_exact(
    const HANDLE handle,
    const std::uint64_t disk_size,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes) {
  const Status range_status =
      validate_io_range(disk_size, offset, bytes.size(), L"コピー先生ディスク書込み");
  if (!range_status) {
    return range_status;
  }
  if (bytes.empty()) {
    return clonecore::success_status();
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::io_failed, L"コピー先書込み位置設定", GetLastError()));
  }
  DWORD bytes_written = 0;
  if (!WriteFile(
          handle,
          bytes.data(),
          static_cast<DWORD>(bytes.size()),
          &bytes_written,
          nullptr) ||
      bytes_written != bytes.size()) {
    const DWORD native_code = GetLastError();
    return Status::failure(physical_error(
        ErrorCode::io_failed,
        native_code == ERROR_SUCCESS ? ERROR_WRITE_FAULT : native_code,
        L"コピー先生ディスク書込み",
        L"コピー先へ要求したバイト数を完全に書き込めませんでした"));
  }
  return clonecore::success_status();
}

class WindowsSourceDiskReader final : public ISourceDiskReader {
 public:
  WindowsSourceDiskReader(
      UniqueHandle handle,
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

  [[nodiscard]] Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::scoped_lock lock(mutex_);
    return read_disk_exact(
        handle_.get(), size_bytes_, offset, length, L"コピー元生ディスク読取り");
  }

 private:
  UniqueHandle handle_;
  std::uint64_t size_bytes_{};
  std::uint32_t logical_sector_size_{};
  mutable std::mutex mutex_;
};

class WindowsTargetDiskWriter final : public ITargetDiskWriter {
 public:
  WindowsTargetDiskWriter(
      UniqueHandle handle,
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

  [[nodiscard]] Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    std::scoped_lock lock(mutex_);
    return write_disk_exact(handle_.get(), size_bytes_, offset, bytes);
  }

  [[nodiscard]] Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::scoped_lock lock(mutex_);
    return read_disk_exact(
        handle_.get(), size_bytes_, offset, length, L"コピー先生ディスク読戻し");
  }

  [[nodiscard]] Status flush_target() override {
    std::scoped_lock lock(mutex_);
    if (!FlushFileBuffers(handle_.get())) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed, L"コピー先生ディスクflush", GetLastError()));
    }
    return clonecore::success_status();
  }

 private:
  UniqueHandle handle_;
  std::uint64_t size_bytes_{};
  std::uint32_t logical_sector_size_{};
  mutable std::mutex mutex_;
};

Result<UniqueHandle> open_raw_disk(
    const DiskInfo& disk,
    const DWORD desired_access,
    const DWORD flags,
    const std::wstring_view operation) {
  if (!physical_path_matches(disk)) {
    return Result<UniqueHandle>::failure(physical_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"列挙済みPhysicalDrive絶対パスだけを使用できます"));
  }
  UniqueHandle handle(CreateFileW(
      disk.device_path.c_str(),
      desired_access,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      flags,
      nullptr));
  if (!handle) {
    const DWORD native_code = GetLastError();
    return Result<UniqueHandle>::failure(clonecore::make_win32_error(
        native_code == ERROR_ACCESS_DENIED ? ErrorCode::access_denied
                                           : ErrorCode::io_failed,
        operation,
        native_code));
  }
  const auto geometry = query_raw_disk_geometry(handle.get());
  if (!geometry) {
    return Result<UniqueHandle>::failure(geometry.error());
  }
  const Status geometry_status =
      validate_opened_geometry(disk, geometry.value(), operation);
  if (!geometry_status) {
    return Result<UniqueHandle>::failure(geometry_status.error());
  }
  return Result<UniqueHandle>::success(std::move(handle));
}

class WindowsPhysicalDiskBackend final : public IWindowsPhysicalDiskBackend {
 public:
  Result<std::unique_ptr<ISourceDiskReader>> open_source(
      const DiskInfo& disk) override {
    auto handle = open_raw_disk(
        disk,
        GENERIC_READ,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        L"コピー元物理ディスクの読取り専用オープン");
    if (!handle) {
      return Result<std::unique_ptr<ISourceDiskReader>>::failure(handle.error());
    }
    return Result<std::unique_ptr<ISourceDiskReader>>::success(
        std::make_unique<WindowsSourceDiskReader>(
            handle.take_value(), disk.size_bytes, disk.logical_sector_size));
  }

  Result<std::unique_ptr<ITargetDiskWriter>> open_offline_target(
      const DiskInfo& disk) override {
    if (!disk.offline.value_or(false) || disk.read_only.value_or(true)) {
      return Result<std::unique_ptr<ITargetDiskWriter>>::failure(physical_error(
          ErrorCode::access_denied,
          ERROR_ACCESS_DENIED,
          L"コピー先物理ディスクの書込みオープン",
          L"コピー先がofflineかつ書込み可能と確認できません"));
    }
    auto handle = open_raw_disk(
        disk,
        GENERIC_READ | GENERIC_WRITE,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        L"コピー先物理ディスクの書込みオープン");
    if (!handle) {
      return Result<std::unique_ptr<ITargetDiskWriter>>::failure(handle.error());
    }
    return Result<std::unique_ptr<ITargetDiskWriter>>::success(
        std::make_unique<WindowsTargetDiskWriter>(
            handle.take_value(), disk.size_bytes, disk.logical_sector_size));
  }

  Status set_target_offline(
      const DiskInfo& disk,
      const bool offline) override {
    auto handle = open_raw_disk(
        disk,
        GENERIC_READ | GENERIC_WRITE,
        FILE_ATTRIBUTE_NORMAL,
        offline ? L"コピー先物理ディスクのoffline化"
                : L"コピー先物理ディスクのonline化");
    if (!handle) {
      return Status::failure(handle.error());
    }
    SET_DISK_ATTRIBUTES attributes{};
    attributes.Version = sizeof(attributes);
    attributes.Persist = FALSE;
    attributes.Attributes = offline ? DISK_ATTRIBUTE_OFFLINE : 0;
    attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE;
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(
            handle.value().get(),
            IOCTL_DISK_SET_DISK_ATTRIBUTES,
            &attributes,
            sizeof(attributes),
            nullptr,
            0,
            &bytes_returned,
            nullptr)) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          offline ? L"コピー先物理ディスクのoffline化"
                  : L"コピー先物理ディスクのonline化",
          GetLastError()));
    }
    return clonecore::success_status();
  }

  Status set_source_read_only(
      const DiskInfo& disk,
      const bool read_only) override {
    auto handle = open_raw_disk(
        disk,
        GENERIC_READ | GENERIC_WRITE,
        FILE_ATTRIBUTE_NORMAL,
        read_only ? L"コピー元物理ディスクの一時read-only化"
                  : L"コピー元物理ディスクの一時read-only解除");
    if (!handle) {
      return Status::failure(handle.error());
    }
    SET_DISK_ATTRIBUTES attributes{};
    attributes.Version = sizeof(attributes);
    attributes.Persist = FALSE;
    attributes.Attributes = read_only ? DISK_ATTRIBUTE_READ_ONLY : 0;
    attributes.AttributesMask = DISK_ATTRIBUTE_READ_ONLY;
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(
            handle.value().get(),
            IOCTL_DISK_SET_DISK_ATTRIBUTES,
            &attributes,
            sizeof(attributes),
            nullptr,
            0,
            &bytes_returned,
            nullptr)) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          read_only ? L"コピー元物理ディスクの一時read-only化"
                    : L"コピー元物理ディスクの一時read-only解除",
          GetLastError()));
    }
    return clonecore::success_status();
  }
};

Result<DiskInfo> find_reidentified_disk(
    const StableDiskIdentity& expected,
    const InventoryReport& report,
    const std::wstring_view role) {
  std::vector<DiskInfo> matches;
  for (const auto& disk : report.disks) {
    const auto identity = make_stable_disk_identity(disk, disk.is_system_disk);
    if (identity && clonecore::validate_stable_identity(
                        expected, identity.value(), role)) {
      matches.push_back(disk);
    }
  }
  if (matches.size() != 1) {
    return Result<DiskInfo>::failure(physical_error(
        ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        std::wstring(role) + L"の物理ディスク再列挙",
        L"安定識別情報に一致する物理ディスクを一意に特定できません"));
  }
  return Result<DiskInfo>::success(std::move(matches.front()));
}

}  // namespace

Result<ReidentifiedReadOnlyDisk> reidentify_read_only_physical_disk(
    const StableDiskIdentity& expected,
    IDiskInventoryProvider& inventory) {
  const auto report = inventory.enumerate();
  if (!report) {
    return Result<ReidentifiedReadOnlyDisk>::failure(report.error());
  }
  if (!report.value().issues.empty()) {
    return Result<ReidentifiedReadOnlyDisk>::failure(physical_error(
        ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"読取り専用操作前の全ディスク再列挙",
        L"ディスク列挙に未解決の診断があるため読取り元を確定できません"));
  }
  auto observed =
      find_reidentified_disk(expected, report.value(), L"読取り元");
  if (!observed) {
    return Result<ReidentifiedReadOnlyDisk>::failure(observed.error());
  }
  auto identity = make_stable_disk_identity(
      observed.value(), observed.value().is_system_disk);
  if (!identity) {
    return Result<ReidentifiedReadOnlyDisk>::failure(identity.error());
  }
  const auto valid = clonecore::validate_stable_identity(
      expected, identity.value(), L"読取り元");
  if (!valid) {
    return Result<ReidentifiedReadOnlyDisk>::failure(valid.error());
  }
  return Result<ReidentifiedReadOnlyDisk>::success(
      ReidentifiedReadOnlyDisk{
          .observed = observed.take_value(),
          .identity = identity.take_value(),
      });
}

Result<ReadOnlyPhysicalDiskHandle>
open_verified_read_only_physical_disk(
    const StableDiskIdentity& expected,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend) {
  auto observed =
      reidentify_read_only_physical_disk(expected, inventory);
  if (!observed) {
    return Result<ReadOnlyPhysicalDiskHandle>::failure(observed.error());
  }
  auto reader = backend.open_source(observed.value().observed);
  if (!reader) {
    return Result<ReadOnlyPhysicalDiskHandle>::failure(reader.error());
  }
  if (!reader.value() ||
      reader.value()->size_bytes() !=
          observed.value().observed.size_bytes ||
      reader.value()->logical_sector_size() !=
          observed.value().observed.logical_sector_size) {
    return Result<ReadOnlyPhysicalDiskHandle>::failure(physical_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"読取り専用物理ディスク再確認",
        L"オープンしたReaderの容量または論理セクターが再列挙結果と一致しません"));
  }
  return Result<ReadOnlyPhysicalDiskHandle>::success(
      ReadOnlyPhysicalDiskHandle{
          .observed = observed.take_value(),
          .reader = reader.take_value(),
      });
}

Status set_verified_source_read_only(
    const StableDiskIdentity& expected_source,
    const bool read_only,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend) {
  auto observed =
      reidentify_read_only_physical_disk(expected_source, inventory);
  if (!observed) {
    return Status::failure(observed.error());
  }
  if (!observed.value().observed.read_only.has_value()) {
    return Status::failure(physical_error(
        ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"コピー元read-only属性の確認",
        L"コピー元のread-only状態を確認できません"));
  }
  if (observed.value().observed.read_only.value() != read_only) {
    const auto transition = backend.set_source_read_only(
        observed.value().observed, read_only);
    if (!transition) {
      return transition;
    }
  }
  auto verified =
      reidentify_read_only_physical_disk(expected_source, inventory);
  if (!verified) {
    return Status::failure(verified.error());
  }
  if (!verified.value().observed.read_only.has_value() ||
      verified.value().observed.read_only.value() != read_only) {
    return Status::failure(physical_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        read_only ? L"コピー元read-only化の検証"
                  : L"コピー元read-only解除の検証",
        L"状態変更後の再列挙結果が要求状態と一致しません"));
  }
  return clonecore::success_status();
}

Result<ReidentifiedPhysicalClone> reidentify_physical_clone_selection(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& expected_target,
    IDiskInventoryProvider& inventory,
    const bool require_target_same_or_larger) {
  const auto report = inventory.enumerate();
  if (!report) {
    return Result<ReidentifiedPhysicalClone>::failure(report.error());
  }
  if (!report.value().issues.empty()) {
    return Result<ReidentifiedPhysicalClone>::failure(physical_error(
        ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"破壊的操作前の全ディスク再列挙",
        L"ディスク列挙に未解決の診断があるため書込みを開始できません"));
  }
  auto source = find_reidentified_disk(
      expected_source, report.value(), L"コピー元");
  if (!source) {
    return Result<ReidentifiedPhysicalClone>::failure(source.error());
  }
  auto target = find_reidentified_disk(
      expected_target, report.value(), L"コピー先");
  if (!target) {
    return Result<ReidentifiedPhysicalClone>::failure(target.error());
  }
  auto source_identity =
      make_stable_disk_identity(source.value(), source.value().is_system_disk);
  auto target_identity =
      make_stable_disk_identity(target.value(), target.value().is_system_disk);
  if (!source_identity) {
    return Result<ReidentifiedPhysicalClone>::failure(source_identity.error());
  }
  if (!target_identity) {
    return Result<ReidentifiedPhysicalClone>::failure(target_identity.error());
  }
  const Status identity_status = clonecore::validate_clone_selection(
      expected_source,
      source_identity.value(),
      expected_target,
      target_identity.value(),
      require_target_same_or_larger);
  if (!identity_status) {
    return Result<ReidentifiedPhysicalClone>::failure(identity_status.error());
  }
  return Result<ReidentifiedPhysicalClone>::success(
      ReidentifiedPhysicalClone{
          .source = source.take_value(),
          .target = target.take_value(),
          .source_identity = source_identity.take_value(),
          .target_identity = target_identity.take_value(),
      });
}

Result<ReidentifiedPhysicalClone> reidentify_physical_clone(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory,
    const bool require_target_same_or_larger) {
  auto observed = reidentify_physical_clone_selection(
      expected_source,
      expected_target,
      inventory,
      require_target_same_or_larger);
  if (!observed) {
    return observed;
  }
  const Status confirmation_status = clonecore::validate_clone_identities(
      expected_source,
      observed.value().source_identity,
      expected_target,
      observed.value().target_identity,
      confirmation,
      require_target_same_or_larger);
  if (!confirmation_status) {
    return Result<ReidentifiedPhysicalClone>::failure(
        confirmation_status.error());
  }
  return observed;
}

Status set_verified_target_offline(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    const bool offline,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend) {
  auto observed = reidentify_physical_clone(
      expected_source, expected_target, confirmation, inventory);
  if (!observed) {
    return Status::failure(observed.error());
  }
  if (!observed.value().target.offline.has_value() ||
      !observed.value().target.read_only.has_value()) {
    return Status::failure(physical_error(
        ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"コピー先ディスク属性の確認",
        L"コピー先のoffline/read-only状態を確認できません"));
  }
  if (observed.value().target.read_only.value()) {
    return Status::failure(physical_error(
        ErrorCode::access_denied,
        ERROR_WRITE_PROTECT,
        L"コピー先ディスク属性の確認",
        L"コピー先が読み取り専用です"));
  }
  if (observed.value().target.offline.value() != offline) {
    const Status transition =
        backend.set_target_offline(observed.value().target, offline);
    if (!transition) {
      return transition;
    }
  }
  auto verified = reidentify_physical_clone(
      expected_source, expected_target, confirmation, inventory);
  if (!verified) {
    return Status::failure(verified.error());
  }
  if (!verified.value().target.offline.has_value() ||
      verified.value().target.offline.value() != offline) {
    return Status::failure(physical_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        offline ? L"コピー先offline化の検証" : L"コピー先online化の検証",
        L"状態変更後の再列挙結果が要求状態と一致しません"));
  }
  return clonecore::success_status();
}

Result<PhysicalCloneHandles> open_verified_physical_clone(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend) {
  auto observed = reidentify_physical_clone(
      expected_source, expected_target, confirmation, inventory);
  if (!observed) {
    return Result<PhysicalCloneHandles>::failure(observed.error());
  }
  if (observed.value().source.is_system_disk) {
    return Result<PhysicalCloneHandles>::failure(physical_error(
        ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"Phase 1コピー元の実行環境確認",
        L"実行中WindowsのシステムディスクはPhase 1のオフラインコピー元にできません"));
  }
  if (!observed.value().target.offline.value_or(false) ||
      observed.value().target.read_only.value_or(true)) {
    return Result<PhysicalCloneHandles>::failure(physical_error(
        ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"コピー先書込み前の属性確認",
        L"コピー先がofflineかつ書込み可能と確認できません"));
  }
  auto source = backend.open_source(observed.value().source);
  if (!source) {
    return Result<PhysicalCloneHandles>::failure(source.error());
  }
  auto target = backend.open_offline_target(observed.value().target);
  if (!target) {
    return Result<PhysicalCloneHandles>::failure(target.error());
  }
  return Result<PhysicalCloneHandles>::success(PhysicalCloneHandles{
      .observed = observed.take_value(),
      .source = source.take_value(),
      .target = target.take_value(),
  });
}

Result<ReidentifiedPhysicalTarget> reidentify_physical_target(
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory) {
  const auto report = inventory.enumerate();
  if (!report) {
    return Result<ReidentifiedPhysicalTarget>::failure(report.error());
  }
  if (!report.value().issues.empty()) {
    return Result<ReidentifiedPhysicalTarget>::failure(physical_error(
        ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"復元書込み前の全ディスク再列挙",
        L"ディスク列挙に未解決の診断があるため復元を開始できません"));
  }
  auto target = find_reidentified_disk(
      expected_target, report.value(), L"復元先");
  if (!target) {
    return Result<ReidentifiedPhysicalTarget>::failure(target.error());
  }
  auto target_identity = make_stable_disk_identity(
      target.value(), target.value().is_system_disk);
  if (!target_identity) {
    return Result<ReidentifiedPhysicalTarget>::failure(
        target_identity.error());
  }
  const auto identity_status = clonecore::validate_stable_identity(
      expected_target, target_identity.value(), L"復元先");
  if (!identity_status) {
    return Result<ReidentifiedPhysicalTarget>::failure(
        identity_status.error());
  }
  if (target.value().is_system_disk) {
    return Result<ReidentifiedPhysicalTarget>::failure(physical_error(
        ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"復元先のシステムディスク保護",
        L"実行中システムのディスクは復元先にできません"));
  }
  if (!target.value().offline.has_value() ||
      !target.value().read_only.has_value() ||
      !target.value().removable.has_value()) {
    return Result<ReidentifiedPhysicalTarget>::failure(physical_error(
        ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"復元先ディスク属性の確認",
        L"offline、read-only、removable属性をすべて確認できません"));
  }
  if (target.value().read_only.value()) {
    return Result<ReidentifiedPhysicalTarget>::failure(physical_error(
        ErrorCode::access_denied,
        ERROR_WRITE_PROTECT,
        L"復元先ディスク属性の確認",
        L"復元先が読み取り専用です"));
  }
  if (target.value().removable.value()) {
    return Result<ReidentifiedPhysicalTarget>::failure(physical_error(
        ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"復元先ディスク属性の確認",
        L"removableと報告されたディスクは復元先にできません"));
  }
  if (target.value().logical_sector_size != 512) {
    return Result<ReidentifiedPhysicalTarget>::failure(physical_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"復元先の4Kn有効化ゲート",
        L"実機相当検証までは512バイト論理セクターだけを対象にします"));
  }
  if (!confirmation.first_step_acknowledged ||
      confirmation.typed_token !=
          clonecore::make_target_confirmation_token(
              target_identity.value())) {
    return Result<ReidentifiedPhysicalTarget>::failure(physical_error(
        ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"復元先消去の二段階確認",
        L"復元先消去への同意または確認語が一致しません"));
  }
  return Result<ReidentifiedPhysicalTarget>::success(
      ReidentifiedPhysicalTarget{
          .target = target.take_value(),
          .target_identity = target_identity.take_value(),
      });
}

Status set_verified_physical_target_offline(
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    const bool offline,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend) {
  auto observed = reidentify_physical_target(
      expected_target, confirmation, inventory);
  if (!observed) {
    return Status::failure(observed.error());
  }
  if (observed.value().target.offline.value() != offline) {
    const auto transition =
        backend.set_target_offline(observed.value().target, offline);
    if (!transition) {
      return transition;
    }
  }
  auto verified = reidentify_physical_target(
      expected_target, confirmation, inventory);
  if (!verified) {
    return Status::failure(verified.error());
  }
  if (verified.value().target.offline.value() != offline) {
    return Status::failure(physical_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        offline ? L"復元先offline化の検証"
                : L"復元先online化の検証",
        L"状態変更後の再列挙結果が要求状態と一致しません"));
  }
  return clonecore::success_status();
}

Result<PhysicalTargetHandle> open_verified_physical_target(
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend) {
  auto observed = reidentify_physical_target(
      expected_target, confirmation, inventory);
  if (!observed) {
    return Result<PhysicalTargetHandle>::failure(observed.error());
  }
  if (!observed.value().target.offline.value()) {
    return Result<PhysicalTargetHandle>::failure(physical_error(
        ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"復元先書込み前の属性確認",
        L"復元先がofflineと確認できません"));
  }
  auto target = backend.open_offline_target(
      observed.value().target);
  if (!target) {
    return Result<PhysicalTargetHandle>::failure(target.error());
  }
  if (!target.value() ||
      target.value()->size_bytes() !=
          observed.value().target.size_bytes ||
      target.value()->logical_sector_size() !=
          observed.value().target.logical_sector_size) {
    return Result<PhysicalTargetHandle>::failure(physical_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"復元先物理ディスク再確認",
        L"オープンしたWriterの容量または論理セクターが再列挙結果と一致しません"));
  }
  return Result<PhysicalTargetHandle>::success(
      PhysicalTargetHandle{
          .observed = observed.take_value(),
          .target = target.take_value(),
      });
}

Status set_verified_target_offline_with_windows_apis(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    const bool offline) {
  auto inventory = make_windows_disk_inventory_provider();
  WindowsPhysicalDiskBackend backend;
  return set_verified_target_offline(
      expected_source,
      expected_target,
      confirmation,
      offline,
      *inventory,
      backend);
}

Result<PhysicalCloneHandles> open_verified_physical_clone_with_windows_apis(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation) {
  auto inventory = make_windows_disk_inventory_provider();
  WindowsPhysicalDiskBackend backend;
  return open_verified_physical_clone(
      expected_source,
      expected_target,
      confirmation,
      *inventory,
      backend);
}

Status set_verified_physical_target_offline_with_windows_apis(
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    const bool offline) {
  auto inventory = make_windows_disk_inventory_provider();
  WindowsPhysicalDiskBackend backend;
  return set_verified_physical_target_offline(
      expected_target,
      confirmation,
      offline,
      *inventory,
      backend);
}

Result<PhysicalTargetHandle>
open_verified_physical_target_with_windows_apis(
    const StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation) {
  auto inventory = make_windows_disk_inventory_provider();
  WindowsPhysicalDiskBackend backend;
  return open_verified_physical_target(
      expected_target, confirmation, *inventory, backend);
}

Result<ReadOnlyPhysicalDiskHandle>
open_verified_read_only_physical_disk_with_windows_apis(
    const StableDiskIdentity& expected) {
  auto inventory = make_windows_disk_inventory_provider();
  WindowsPhysicalDiskBackend backend;
  return open_verified_read_only_physical_disk(
      expected, *inventory, backend);
}

Status set_verified_source_read_only_with_windows_apis(
    const StableDiskIdentity& expected_source,
    const bool read_only) {
  auto inventory = make_windows_disk_inventory_provider();
  WindowsPhysicalDiskBackend backend;
  return set_verified_source_read_only(
      expected_source, read_only, *inventory, backend);
}

}  // namespace ytec::diskmodel

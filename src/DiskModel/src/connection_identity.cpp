#include "ytec/diskmodel/connection_identity.h"

#include <Windows.h>

#include <algorithm>
#include <cwchar>
#include <limits>
#include <string_view>
#include <utility>

namespace ytec::diskmodel {
namespace {

clonecore::Error identity_error(
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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(identity_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      right.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool is_usb(const DiskInfo& disk) noexcept {
  return equals_ordinal_ignore_case(disk.bus_type, L"USB");
}

bool partition_less(
    const ConnectionBoundPartitionIdentity& left,
    const ConnectionBoundPartitionIdentity& right) noexcept {
  if (left.offset_bytes != right.offset_bytes) {
    return left.offset_bytes < right.offset_bytes;
  }
  return left.number < right.number;
}

bool is_well_formed_connection_identity(
    const ConnectionBoundDiskIdentity& identity) {
  if (identity.device_interface_path.empty() ||
      identity.connection_location_path.empty() || identity.model.empty() ||
      identity.model == L"未取得" || identity.size_bytes == 0U ||
      identity.logical_sector_size == 0U ||
      identity.physical_sector_size == 0U ||
      identity.size_bytes % identity.logical_sector_size != 0U ||
      identity.physical_sector_size < identity.logical_sector_size ||
      identity.physical_sector_size % identity.logical_sector_size != 0U ||
      identity.partition_style == PartitionStyle::unknown ||
      identity.disk_identifier.empty()) {
    return false;
  }

  if ((identity.partition_style == PartitionStyle::raw) !=
      identity.partitions.empty()) {
    return false;
  }

  for (std::size_t index = 0U; index < identity.partitions.size(); ++index) {
    const auto& partition = identity.partitions[index];
    if (partition.number == 0U || partition.size_bytes == 0U ||
        partition.style != identity.partition_style ||
        partition.offset_bytes % identity.logical_sector_size != 0U ||
        partition.size_bytes % identity.logical_sector_size != 0U ||
        partition.offset_bytes > identity.size_bytes ||
        partition.size_bytes >
            identity.size_bytes - partition.offset_bytes) {
      return false;
    }

    const std::uint64_t partition_end =
        partition.offset_bytes + partition.size_bytes;
    for (std::size_t other_index = index + 1U;
         other_index < identity.partitions.size();
         ++other_index) {
      const auto& other = identity.partitions[other_index];
      if (partition.number == other.number) {
        return false;
      }
      if (other.offset_bytes > identity.size_bytes ||
          other.size_bytes > identity.size_bytes - other.offset_bytes) {
        return false;
      }
      const std::uint64_t other_end =
          other.offset_bytes + other.size_bytes;
      if (partition.offset_bytes < other_end &&
          other.offset_bytes < partition_end) {
        return false;
      }
    }
  }
  return true;
}

bool same_partition(
    const ConnectionBoundPartitionIdentity& left,
    const ConnectionBoundPartitionIdentity& right) noexcept {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes && left.style == right.style &&
      equals_ordinal_ignore_case(left.type, right.type) &&
      equals_ordinal_ignore_case(left.identifier, right.identifier) &&
      equals_ordinal_ignore_case(left.name, right.name) &&
      left.bootable == right.bootable;
}

bool same_connection_identity(
    const ConnectionBoundDiskIdentity& left,
    const ConnectionBoundDiskIdentity& right) noexcept {
  return equals_ordinal_ignore_case(
             left.device_interface_path, right.device_interface_path) &&
      equals_ordinal_ignore_case(
          left.connection_location_path, right.connection_location_path) &&
      equals_ordinal_ignore_case(left.model, right.model) &&
      left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.physical_sector_size == right.physical_sector_size &&
      left.partition_style == right.partition_style &&
      equals_ordinal_ignore_case(
          left.disk_identifier, right.disk_identifier) &&
      left.partitions.size() == right.partitions.size() &&
      std::all_of(
          left.partitions.begin(),
          left.partitions.end(),
          [&right](const ConnectionBoundPartitionIdentity& partition) {
            return std::any_of(
                right.partitions.begin(),
                right.partitions.end(),
                [&partition](
                    const ConnectionBoundPartitionIdentity& candidate) {
                  return same_partition(partition, candidate);
                });
          });
}

clonecore::Error reselection_error() {
  return identity_error(
      clonecore::ErrorCode::confirmation_required,
      ERROR_CANCELLED,
      L"USB同一接続の再選択",
      L"接続の中断または識別情報の変化を検出しました。対象を再選択し、大文字OKを再入力してください");
}

}  // namespace

clonecore::Result<ConnectionBoundDiskIdentity>
make_connection_bound_disk_identity(const DiskInfo& disk) {
  if (!is_usb(disk) || !disk.removable.has_value() ||
      disk.removable.value()) {
    return failure<ConnectionBoundDiskIdentity>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"USB同一接続識別の対象分類",
        L"書込み対象にできる固定USB HDD/SSDだけが同一接続識別を利用できます");
  }
  if (!disk.serial_suffix.empty()) {
    return failure<ConnectionBoundDiskIdentity>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"USB同一接続識別の選択",
        L"一意シリアルを利用できるディスクは永続識別経路を使用してください");
  }
  if (disk.device_interface_path.empty() ||
      disk.connection_location_path.empty() || disk.model.empty() ||
      disk.model == L"未取得" || disk.size_bytes == 0U ||
      disk.logical_sector_size == 0U ||
      disk.physical_sector_size == 0U ||
      disk.partition_style == PartitionStyle::unknown ||
      disk.disk_identifier.empty()) {
    return failure<ConnectionBoundDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"USB同一接続識別の作成",
        L"interface path、port、model、capacity、sector、disk ID、layoutの必須証拠が不足しています");
  }

  ConnectionBoundDiskIdentity result{
      .device_interface_path = disk.device_interface_path,
      .connection_location_path = disk.connection_location_path,
      .model = disk.model,
      .size_bytes = disk.size_bytes,
      .logical_sector_size = disk.logical_sector_size,
      .physical_sector_size = disk.physical_sector_size,
      .partition_style = disk.partition_style,
      .disk_identifier = disk.disk_identifier,
  };
  result.partitions.reserve(disk.partitions.size());
  for (const auto& partition : disk.partitions) {
    result.partitions.push_back(ConnectionBoundPartitionIdentity{
        .number = partition.number,
        .offset_bytes = partition.offset_bytes,
        .size_bytes = partition.size_bytes,
        .style = partition.style,
        .type = partition.type,
        .identifier = partition.identifier,
        .name = partition.name,
        .bootable = partition.bootable,
    });
  }
  std::sort(result.partitions.begin(), result.partitions.end(), partition_less);
  if (!is_well_formed_connection_identity(result)) {
    return failure<ConnectionBoundDiskIdentity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"USB同一接続layoutの正規化",
        L"セクターサイズ、partition番号、範囲、形式、または重なりを安全に識別できません");
  }
  return clonecore::Result<ConnectionBoundDiskIdentity>::success(
      std::move(result));
}

clonecore::Status validate_connection_bound_disk_identity(
    const ConnectionBoundDiskIdentity& expected,
    const ConnectionBoundDiskIdentity& observed) {
  if (!is_well_formed_connection_identity(expected) ||
      !is_well_formed_connection_identity(observed)) {
    return clonecore::Status::failure(identity_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"USB同一接続layoutの検証",
        L"同一接続の再照合に必要な識別情報が完全ではありません"));
  }
  if (!same_connection_identity(expected, observed)) {
    return clonecore::Status::failure(identity_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"USB同一接続識別の再照合",
        L"path、port、model、capacity、sector、disk ID、またはpartition layoutが選択時と一致しません"));
  }
  return clonecore::success_status();
}

DiskSelectionIdentity::DiskSelectionIdentity(
    const DiskIdentityBindingKind kind,
    std::optional<clonecore::StableDiskIdentity> persistent,
    std::optional<ConnectionBoundDiskIdentity> connection) noexcept
    : kind_(kind),
      persistent_(std::move(persistent)),
      connection_(std::move(connection)) {}

clonecore::Result<DiskSelectionIdentity> DiskSelectionIdentity::create(
    const DiskInfo& selected) {
  if (is_usb(selected) &&
      (!selected.removable.has_value() || selected.removable.value())) {
    return failure<DiskSelectionIdentity>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"USB対象の安全分類",
        L"USBメモリまたはremovable属性を確定できない媒体は対象にできません");
  }

  if (is_usb(selected) && selected.serial_suffix.empty()) {
    auto connection = make_connection_bound_disk_identity(selected);
    if (!connection) {
      return clonecore::Result<DiskSelectionIdentity>::failure(
          connection.error());
    }
    return clonecore::Result<DiskSelectionIdentity>::success(
        DiskSelectionIdentity(
            DiskIdentityBindingKind::same_connection,
            std::nullopt,
            connection.take_value()));
  }

  auto persistent =
      make_stable_disk_identity(selected, selected.is_system_disk);
  if (!persistent) {
    return clonecore::Result<DiskSelectionIdentity>::failure(
        persistent.error());
  }
  return clonecore::Result<DiskSelectionIdentity>::success(
      DiskSelectionIdentity(
          DiskIdentityBindingKind::persistent_identifier,
          persistent.take_value(),
          std::nullopt));
}

DiskIdentityBindingKind DiskSelectionIdentity::kind() const noexcept {
  return kind_;
}

bool DiskSelectionIdentity::reselection_required() const noexcept {
  return reselection_required_;
}

void DiskSelectionIdentity::invalidate_connection() noexcept {
  if (kind_ == DiskIdentityBindingKind::same_connection) {
    reselection_required_ = true;
  }
}

clonecore::Result<DiskInfo> DiskSelectionIdentity::reidentify(
    const InventoryReport& inventory) {
  if (kind_ == DiskIdentityBindingKind::same_connection &&
      reselection_required_) {
    return clonecore::Result<DiskInfo>::failure(reselection_error());
  }

  // Incomplete enumeration cannot prove that exactly one target still owns
  // the reviewed port. Consume the one-session approval rather than allowing
  // it to revive after a later clean enumeration.
  if (kind_ == DiskIdentityBindingKind::same_connection &&
      !inventory.issues.empty()) {
    reselection_required_ = true;
    return clonecore::Result<DiskInfo>::failure(reselection_error());
  }

  const DiskInfo* match = nullptr;
  std::size_t match_count = 0U;
  for (const auto& candidate : inventory.disks) {
    bool matches = false;
    if (kind_ == DiskIdentityBindingKind::persistent_identifier) {
      const auto observed =
          make_stable_disk_identity(candidate, candidate.is_system_disk);
      matches = observed.has_value() && persistent_.has_value() &&
          clonecore::validate_stable_identity(
              persistent_.value(), observed.value(), L"選択ディスク")
              .has_value();
    } else {
      const auto observed = make_connection_bound_disk_identity(candidate);
      matches = observed.has_value() && connection_.has_value() &&
          validate_connection_bound_disk_identity(
              connection_.value(), observed.value())
              .has_value();
    }
    if (matches) {
      match = &candidate;
      ++match_count;
    }
  }

  if (match_count != 1U || match == nullptr) {
    if (kind_ == DiskIdentityBindingKind::same_connection) {
      reselection_required_ = true;
      return clonecore::Result<DiskInfo>::failure(reselection_error());
    }
    return failure<DiskInfo>(
        clonecore::ErrorCode::identity_mismatch,
        match_count == 0U ? ERROR_DEVICE_NOT_CONNECTED : ERROR_DUP_NAME,
        L"永続ディスク識別の再照合",
        match_count == 0U
            ? L"選択したディスクを一意に再識別できません"
            : L"同じ安定識別情報へ複数ディスクが一致したため停止しました");
  }
  return clonecore::Result<DiskInfo>::success(*match);
}

}  // namespace ytec::diskmodel

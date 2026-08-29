#include "ytec/diskmodel/disk_inventory.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace ytec::diskmodel {
namespace {

constexpr std::wstring_view kGptBasicData =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
constexpr std::wstring_view kGptEfiSystem =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kGptMicrosoftReserved =
    L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}";
constexpr std::wstring_view kGptWindowsRecovery =
    L"{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}";

bool same_type(
    const std::wstring& value,
    const std::wstring_view expected) noexcept {
  return value.size() == expected.size() &&
      _wcsnicmp(value.c_str(), expected.data(), expected.size()) == 0;
}

bool is_gpt_windows_partition(const PartitionInfo& partition) noexcept {
  return partition.style == PartitionStyle::gpt &&
      same_type(partition.type, kGptBasicData);
}

bool is_gpt_required_system_partition(
    const PartitionInfo& partition) noexcept {
  return partition.style == PartitionStyle::gpt &&
      (same_type(partition.type, kGptEfiSystem) ||
       same_type(partition.type, kGptMicrosoftReserved) ||
       same_type(partition.type, kGptWindowsRecovery));
}

bool is_mbr_windows_partition(const PartitionInfo& partition) noexcept {
  return partition.style == PartitionStyle::mbr &&
      (partition.bootable || _wcsicmp(partition.type.c_str(), L"0x07") == 0);
}

bool is_mbr_required_system_partition(
    const PartitionInfo& partition) noexcept {
  return partition.style == PartitionStyle::mbr &&
      (partition.bootable || _wcsicmp(partition.type.c_str(), L"0x27") == 0);
}

clonecore::Error selection_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"Tsumugiイメージのパーティション選択",
      .message = std::move(message),
  };
}

}  // namespace

clonecore::Result<clonecore::StableDiskIdentity> make_stable_disk_identity(
    const DiskInfo& disk,
    const bool is_system_disk) {
  if (disk.model.empty() || disk.model == L"未取得" || disk.size_bytes == 0 ||
      disk.logical_sector_size == 0 ||
      (disk.serial_suffix.empty() && disk.device_instance_id.empty())) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        clonecore::Error{
            .code = clonecore::ErrorCode::invalid_data,
            .native_code = ERROR_INVALID_DATA,
            .operation = L"安定ディスク識別情報の作成",
            .message =
                L"モデル、容量、セクターサイズ、シリアル末尾またはデバイス識別子が不足しています",
        });
  }
  return clonecore::Result<clonecore::StableDiskIdentity>::success(
      clonecore::StableDiskIdentity{
          .disk_number = disk.disk_number,
          .model = disk.model,
          .size_bytes = disk.size_bytes,
          .logical_sector_size = disk.logical_sector_size,
          .serial_suffix = disk.serial_suffix,
          .device_instance_id = disk.device_instance_id,
          .is_system_disk = is_system_disk,
      });
}

clonecore::Result<ImagePartitionSelection>
normalize_image_partition_selection(
    const DiskInfo& reviewed_source,
    const std::span<const std::uint32_t> requested_partition_numbers) {
  if ((reviewed_source.partition_style != PartitionStyle::gpt &&
       reviewed_source.partition_style != PartitionStyle::mbr) ||
      reviewed_source.partitions.empty() ||
      reviewed_source.partitions.size() > 128U ||
      reviewed_source.logical_sector_size == 0U ||
      reviewed_source.size_bytes == 0U) {
    return clonecore::Result<ImagePartitionSelection>::failure(
        selection_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"GPT/MBRの1～128領域を持つレビュー済みSourceが必要です"));
  }

  std::set<std::uint32_t> known_numbers;
  std::vector<const PartitionInfo*> ordered_partitions;
  ordered_partitions.reserve(reviewed_source.partitions.size());
  for (const auto& partition : reviewed_source.partitions) {
    ordered_partitions.push_back(&partition);
  }
  std::sort(
      ordered_partitions.begin(),
      ordered_partitions.end(),
      [](const auto* left, const auto* right) {
        return left->offset_bytes < right->offset_bytes;
      });
  std::uint64_t previous_end = 0U;
  for (const auto* partition : ordered_partitions) {
    if (partition->number == 0U || partition->number > 128U ||
        partition->size_bytes == 0U ||
        partition->offset_bytes >
            (std::numeric_limits<std::uint64_t>::max)() -
                partition->size_bytes ||
        partition->offset_bytes + partition->size_bytes >
            reviewed_source.size_bytes ||
        partition->offset_bytes < previous_end ||
        !known_numbers.insert(partition->number).second) {
      return clonecore::Result<ImagePartitionSelection>::failure(
          selection_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Sourceの番号、範囲、順序、または一意性が不正です"));
    }
    previous_end = partition->offset_bytes + partition->size_bytes;
  }

  std::set<std::uint32_t> selected;
  if (requested_partition_numbers.empty()) {
    selected = known_numbers;
  } else {
    for (const auto number : requested_partition_numbers) {
      if (!known_numbers.contains(number) || !selected.insert(number).second) {
        return clonecore::Result<ImagePartitionSelection>::failure(
            selection_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"選択番号が未知、重複、または現在のSourceと不一致です"));
      }
    }
  }

  const bool recognizable_windows_layout = reviewed_source.is_system_disk ||
      std::any_of(
          reviewed_source.partitions.begin(),
          reviewed_source.partitions.end(),
          [](const PartitionInfo& partition) {
            return is_gpt_required_system_partition(partition) ||
                (partition.style == PartitionStyle::mbr &&
                 partition.bootable);
          });
  // Preserve the legacy whole-disk classification exactly.  In particular,
  // a bootable data disk was not previously promoted to a Windows image.
  // Explicit partition selection may use the stronger partition topology to
  // force the required boot/recovery set.
  bool contains_windows = requested_partition_numbers.empty()
      ? reviewed_source.is_system_disk
      : false;
  if (!requested_partition_numbers.empty() && recognizable_windows_layout) {
    contains_windows = std::any_of(
        reviewed_source.partitions.begin(),
        reviewed_source.partitions.end(),
        [&](const PartitionInfo& partition) {
          return selected.contains(partition.number) &&
              (is_gpt_windows_partition(partition) ||
               is_mbr_windows_partition(partition));
        });
  }

  std::set<std::uint32_t> required;
  if (contains_windows) {
    for (const auto& partition : reviewed_source.partitions) {
      if (is_gpt_required_system_partition(partition) ||
          is_mbr_required_system_partition(partition) ||
          (selected.contains(partition.number) &&
           (is_gpt_windows_partition(partition) ||
            is_mbr_windows_partition(partition)))) {
        required.insert(partition.number);
        selected.insert(partition.number);
      }
    }
  }

  ImagePartitionSelection result{
      .whole_disk = selected.size() == known_numbers.size(),
      .contains_windows = contains_windows,
  };
  result.selected_partition_numbers.reserve(selected.size());
  result.required_partition_numbers.reserve(required.size());
  for (const auto* partition : ordered_partitions) {
    if (!selected.contains(partition->number)) {
      continue;
    }
    if (partition->size_bytes >
        (std::numeric_limits<std::uint64_t>::max)() - result.selected_bytes) {
      return clonecore::Result<ImagePartitionSelection>::failure(
          selection_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"選択領域の合計容量が64bit上限を超えます"));
    }
    result.selected_partition_numbers.push_back(partition->number);
    result.selected_bytes += partition->size_bytes;
    if (required.contains(partition->number)) {
      result.required_partition_numbers.push_back(partition->number);
    }
  }
  if (result.selected_partition_numbers.empty() ||
      result.selected_bytes == 0U) {
    return clonecore::Result<ImagePartitionSelection>::failure(
        selection_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"少なくとも1つの内容領域を選択してください"));
  }
  return clonecore::Result<ImagePartitionSelection>::success(
      std::move(result));
}

bool image_partition_selection_contains(
    const ImagePartitionSelection& selection,
    const std::uint32_t partition_number) noexcept {
  return std::find(
             selection.selected_partition_numbers.begin(),
             selection.selected_partition_numbers.end(),
             partition_number) !=
      selection.selected_partition_numbers.end();
}

}  // namespace ytec::diskmodel

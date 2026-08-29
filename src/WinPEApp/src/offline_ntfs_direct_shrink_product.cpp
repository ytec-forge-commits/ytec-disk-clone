#include "ytec/winpeapp/offline_ntfs_direct_shrink_product.h"

#include "ytec/bootrepair/offline_windows.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/directshrink/windows_target_platform.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

clonecore::Error product_error(
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
  return clonecore::Result<T>::failure(product_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(product_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool align_up(
    const std::uint64_t value,
    const std::uint64_t alignment,
    std::uint64_t& result) noexcept {
  if (alignment == 0U ||
      value > (std::numeric_limits<std::uint64_t>::max)() -
          (alignment - 1U)) {
    return false;
  }
  result = ((value + alignment - 1U) / alignment) * alignment;
  return true;
}

bool equal_path(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
      right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool is_hex(const wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') ||
      (value >= L'a' && value <= L'f') ||
      (value >= L'A' && value <= L'F');
}

bool is_canonical_volume_guid_root(const std::wstring_view path) noexcept {
  constexpr std::wstring_view kPrefix = L"\\\\?\\Volume{";
  if (path.size() != 49U ||
      !equal_path(path.substr(0U, kPrefix.size()), kPrefix) ||
      path[47] != L'}' || path[48] != L'\\') {
    return false;
  }
  for (std::size_t index = kPrefix.size(); index < 47U; ++index) {
    const std::size_t guid_index = index - kPrefix.size();
    const bool hyphen = guid_index == 8U || guid_index == 13U ||
        guid_index == 18U || guid_index == 23U;
    if ((hyphen && path[index] != L'-') ||
        (!hyphen && !is_hex(path[index]))) {
      return false;
    }
  }
  return true;
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_string(
    std::vector<std::byte>& bytes,
    const std::string_view value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

void append_wstring(
    std::vector<std::byte>& bytes,
    const std::wstring_view value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t character : value) {
    const auto code_unit = static_cast<std::uint16_t>(character);
    bytes.push_back(static_cast<std::byte>(code_unit & 0xFFU));
    bytes.push_back(static_cast<std::byte>((code_unit >> 8U) & 0xFFU));
  }
}

bool stable_identity_exactly_matches(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  return left.model == right.model &&
      left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.serial_suffix == right.serial_suffix &&
      left.device_instance_id == right.device_instance_id;
}

const windowsshrink::AnalyzedShrinkPartition* find_analysis_partition(
    const windowsshrink::ShrinkSourceAnalysis& analysis,
    const std::uint32_t source_table_index) noexcept {
  const windowsshrink::AnalyzedShrinkPartition* found = nullptr;
  for (const auto& partition : analysis.partitions) {
    if (partition.source_table_index != source_table_index) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &partition;
  }
  return found;
}

const windowsshrink::AnalyzedShrinkVolume* find_analysis_volume(
    const windowsshrink::ShrinkSourceAnalysis& analysis,
    const std::uint32_t source_table_index) noexcept {
  const windowsshrink::AnalyzedShrinkVolume* found = nullptr;
  for (const auto& volume : analysis.content_volumes) {
    if (volume.source_table_index != source_table_index) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &volume;
  }
  return found;
}

const diskmodel::PartitionInfo* find_reviewed_partition(
    const diskmodel::DiskInfo& source,
    const std::uint32_t source_table_index) noexcept {
  const diskmodel::PartitionInfo* found = nullptr;
  for (const auto& partition : source.partitions) {
    if (partition.number != source_table_index) {
      continue;
    }
    if (found != nullptr) {
      return nullptr;
    }
    found = &partition;
  }
  return found;
}

bool required_for_offline_windows(
    const bool source_contains_windows,
    const migrationcore::MigrationPartitionRole role) noexcept {
  return source_contains_windows &&
      (role == migrationcore::MigrationPartitionRole::windows ||
       role == migrationcore::MigrationPartitionRole::efi_system ||
       role == migrationcore::MigrationPartitionRole::microsoft_reserved ||
       role == migrationcore::MigrationPartitionRole::recovery);
}

clonecore::Result<WinPeOfflineNtfsSourceEpochEvidence> make_source_epoch(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis,
    const std::span<const std::uint32_t> retained_indexes) {
  auto source = diskmodel::make_stable_disk_identity(
      request.reviewed_source, false);
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_source);
  auto analysis_hash = hash_winpe_offline_ntfs_source_analysis_v1(analysis);
  auto snapshot_hash = analysis.partition_snapshot.empty()
      ? clonecore::Result<imageformat::Sha256Digest>::failure(product_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"WinPE offline source partition snapshot",
            L"GPT sourceのraw partition snapshotがありません"))
      : imageformat::sha256(
            std::span<const std::byte>(analysis.partition_snapshot));
  if (!source || !source_layout || !analysis_hash || !snapshot_hash) {
    return clonecore::Result<WinPeOfflineNtfsSourceEpochEvidence>::failure(
        !source
            ? source.error()
            : !source_layout
                ? source_layout.error()
                : !analysis_hash ? analysis_hash.error()
                                 : snapshot_hash.error());
  }

  std::set<std::uint32_t> retained(
      retained_indexes.begin(), retained_indexes.end());
  WinPeOfflineNtfsSourceEpochEvidence epoch{
      .observed_source = source.take_value(),
      .source_layout_hash = source_layout.take_value(),
      .source_partition_snapshot_hash = snapshot_hash.take_value(),
      .source_analysis_hash = analysis_hash.take_value(),
      .logical_sector_size = request.reviewed_source.logical_sector_size,
      .stable_identity_reidentified = true,
      .source_os_read_only = request.reviewed_source.read_only.value_or(false),
      .physical_handle_read_only = true,
      .gpt_source = analysis.partition_style ==
          migrationcore::MigrationPartitionStyle::gpt,
      .whole_disk_analysis =
          analysis.partitions.size() == request.reviewed_source.partitions.size(),
      .bitlocker_fully_decrypted = analysis.bitlocker_fully_decrypted,
  };
  epoch.ntfs_volumes.reserve(analysis.content_volumes.size());
  for (const auto& volume : analysis.content_volumes) {
    if (!retained.empty() && !retained.contains(volume.source_table_index)) {
      continue;
    }
    const auto* partition = find_analysis_partition(
        analysis, volume.source_table_index);
    if (partition == nullptr ||
        partition->file_system != migrationcore::MigrationFileSystem::ntfs) {
      return failure<WinPeOfflineNtfsSourceEpochEvidence>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"WinPE offline source epoch Volume対応",
          L"NTFS Volume GUIDを解析済みsource extentへ一意に対応できません");
    }
    epoch.ntfs_volumes.push_back(WinPeOfflineNtfsVolumeBinding{
        .source_table_index = volume.source_table_index,
        .source_offset_bytes = partition->source_offset_bytes,
        .source_size_bytes = partition->source_size_bytes,
        .volume_guid_path = volume.volume_guid_path,
    });
  }
  auto epoch_hash = hash_winpe_offline_ntfs_source_epoch_v1(epoch);
  if (!epoch_hash) {
    return clonecore::Result<WinPeOfflineNtfsSourceEpochEvidence>::failure(
        epoch_hash.error());
  }
  epoch.canonical_epoch_hash = epoch_hash.take_value();
  return clonecore::Result<WinPeOfflineNtfsSourceEpochEvidence>::success(
      std::move(epoch));
}

clonecore::Result<imageformat::Sha256Digest> review_binding_hash(
    const WinPeOfflineNtfsPartitionReviewBinding& binding) {
  constexpr std::string_view kDomain =
      "YTEC-WINPE-OFFLINE-NTFS-PARTITION-REVIEW-V1";
  std::vector<std::byte> bytes;
  bytes.reserve(320U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_wstring(bytes, binding.source.model);
  append_u64(bytes, binding.source.size_bytes);
  append_u32(bytes, binding.source.logical_sector_size);
  append_string(bytes, binding.source.serial_suffix);
  append_wstring(bytes, binding.source.device_instance_id);
  append_wstring(bytes, binding.target.model);
  append_u64(bytes, binding.target.size_bytes);
  append_u32(bytes, binding.target.logical_sector_size);
  append_string(bytes, binding.target.serial_suffix);
  append_wstring(bytes, binding.target.device_instance_id);
  append_array(bytes, binding.source_layout_hash);
  append_array(bytes, binding.target_layout_hash);
  append_array(bytes, binding.source_partition_snapshot_hash);
  append_array(bytes, binding.source_analysis_hash);
  append_array(bytes, binding.source_epoch_hash);
  append_u8(bytes, binding.source_contains_windows ? 1U : 0U);
  return imageformat::sha256(bytes);
}

bool review_bindings_equal(
    const WinPeOfflineNtfsPartitionReviewBinding& left,
    const WinPeOfflineNtfsPartitionReviewBinding& right) noexcept {
  return stable_identity_exactly_matches(left.source, right.source) &&
      stable_identity_exactly_matches(left.target, right.target) &&
      left.source_layout_hash == right.source_layout_hash &&
      left.target_layout_hash == right.target_layout_hash &&
      left.source_partition_snapshot_hash ==
          right.source_partition_snapshot_hash &&
      left.source_analysis_hash == right.source_analysis_hash &&
      left.source_epoch_hash == right.source_epoch_hash &&
      left.binding_hash == right.binding_hash &&
      left.source_contains_windows == right.source_contains_windows;
}

clonecore::Result<imageformat::Sha256Digest> final_layout_hash(
    const migrationcore::DirectClonePlan& direct_plan,
    const directshrink::TargetOwnedStagingPlan& staging,
    const std::span<const directshrink::PartitionTask> tasks) {
  constexpr std::string_view kDomain =
      "YTEC-WINPE-OFFLINE-NTFS-FINAL-GPT-V1";
  std::vector<std::byte> bytes;
  bytes.reserve(160U + tasks.size() * 96U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_u8(bytes, static_cast<std::uint8_t>(direct_plan.source_style()));
  append_u8(bytes, static_cast<std::uint8_t>(direct_plan.target_style()));
  append_u64(bytes, direct_plan.target_size_bytes());
  append_u32(bytes, direct_plan.target_logical_sector_size());
  append_u8(bytes, static_cast<std::uint8_t>(direct_plan.surplus_allocation()));
  append_u8(
      bytes,
      direct_plan.surplus_target_source_table_index().has_value() ? 1U : 0U);
  append_u32(
      bytes, direct_plan.surplus_target_source_table_index().value_or(0U));
  append_u64(bytes, direct_plan.unallocated_tail_bytes());
  append_u64(bytes, staging.offset_bytes);
  append_u64(bytes, staging.length_bytes);
  append_u8(
      bytes, staging.final_growth_owner_target_number.has_value() ? 1U : 0U);
  append_u32(
      bytes, staging.final_growth_owner_target_number.value_or(0U));
  append_u32(bytes, static_cast<std::uint32_t>(tasks.size()));
  for (const auto& task : tasks) {
    append_u8(bytes, static_cast<std::uint8_t>(task.kind));
    append_u32(bytes, task.target_number);
    append_u8(bytes, task.source_table_index.has_value() ? 1U : 0U);
    append_u32(bytes, task.source_table_index.value_or(0U));
    append_u8(bytes, static_cast<std::uint8_t>(task.role));
    append_u8(bytes, task.active ? 1U : 0U);
    append_u64(bytes, task.target_offset_bytes);
    append_u64(bytes, task.construction_size_bytes);
    append_u64(bytes, task.target_size_bytes);
    append_u64(bytes, task.source_size_bytes);
    append_u64(bytes, task.source_used_bytes);
    append_array(bytes, task.source_partition_type);
  }
  return imageformat::sha256(bytes);
}

clonecore::Result<imageformat::Sha256Digest> target_payload_hash(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const WinPeOfflineNtfsSourceEpochEvidence& epoch,
    const migrationcore::DirectClonePlan& direct_plan,
    const directshrink::TargetPlanData& data) {
  constexpr std::string_view kDomain =
      "YTEC-WINPE-OFFLINE-NTFS-TARGET-PLAN-V2";
  std::vector<std::byte> bytes;
  bytes.reserve(384U + data.tasks.size() * 160U +
                data.source_partition_mappings.size() * 24U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_array(bytes, data.expected_source_layout_hash);
  append_array(bytes, data.expected_target_layout_hash);
  append_array(bytes, data.source_partition_snapshot_hash);
  append_array(bytes, epoch.source_analysis_hash);
  append_array(bytes, epoch.canonical_epoch_hash);
  append_array(bytes, data.final_layout_hash);
  append_string(bytes, request.analysis_created_utc);
  append_string(bytes, request.app_version);
  append_u8(bytes, static_cast<std::uint8_t>(direct_plan.surplus_allocation()));
  append_u8(
      bytes,
      direct_plan.surplus_target_source_table_index().has_value() ? 1U : 0U);
  append_u32(
      bytes, direct_plan.surplus_target_source_table_index().value_or(0U));
  append_u64(bytes, data.checkpoint_offset_bytes);
  append_u64(bytes, directshrink::kCheckpointRecordBytes);
  append_u64(bytes, data.staging.offset_bytes);
  append_u64(bytes, data.staging.length_bytes);
  append_u64(bytes, data.staging.control_reserve_bytes);
  append_u64(bytes, data.staging.archive_offset_bytes);
  append_u64(bytes, data.staging.archive_capacity_bytes);
  append_u32(bytes, static_cast<std::uint32_t>(data.tasks.size()));
  for (const auto& task : data.tasks) {
    append_u8(bytes, static_cast<std::uint8_t>(task.kind));
    append_u32(bytes, task.target_number);
    append_u8(bytes, task.source_table_index.has_value() ? 1U : 0U);
    append_u32(bytes, task.source_table_index.value_or(0U));
    append_u8(bytes, static_cast<std::uint8_t>(task.role));
    append_u64(bytes, task.source_offset_bytes);
    append_u64(bytes, task.target_offset_bytes);
    append_u64(bytes, task.construction_size_bytes);
    append_u64(bytes, task.target_size_bytes);
    append_u64(bytes, task.source_size_bytes);
    append_u64(bytes, task.source_used_bytes);
    append_array(bytes, task.source_partition_type);
    append_u64(bytes, task.archive_upper_bound_bytes);
    append_wstring(bytes, task.original_volume_guid_path);
  }
  append_u32(
      bytes,
      static_cast<std::uint32_t>(data.source_partition_mappings.size()));
  for (const auto& mapping : data.source_partition_mappings) {
    append_u32(bytes, mapping.source_table_index);
    append_u8(bytes, static_cast<std::uint8_t>(mapping.role));
    append_u8(bytes, static_cast<std::uint8_t>(mapping.disposition));
    append_u8(bytes, mapping.target_number.has_value() ? 1U : 0U);
    append_u32(bytes, mapping.target_number.value_or(0U));
    append_u8(bytes, mapping.requested ? 1U : 0U);
    append_u8(bytes, mapping.selected ? 1U : 0U);
    append_u8(bytes, mapping.required ? 1U : 0U);
  }
  return imageformat::sha256(bytes);
}

clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan> build_target_plan(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis,
    const WinPeOfflineNtfsPartitionInspection& inspection) {
  const bool source_contains_windows =
      inspection.binding.source_contains_windows;
  std::set<std::uint32_t> selected_indexes;
  for (const auto index : request.selected_source_table_indexes) {
    if (index == 0U || !selected_indexes.emplace(index).second) {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"WinPE offline partition選択",
          L"source table indexは1以上かつ重複なしでなければなりません");
    }
  }

  migrationcore::DirectClonePlanningRequest direct_request{
      .mode_choice = migrationcore::DirectCloneModeChoice::shrink,
      .partition_style_choice =
          migrationcore::DirectClonePartitionStyleChoice::preserve,
      .source_style = migrationcore::MigrationPartitionStyle::gpt,
      .source_size_bytes = request.reviewed_source.size_bytes,
      .source_logical_sector_size =
          request.reviewed_source.logical_sector_size,
      .target_size_bytes = request.reviewed_target.size_bytes,
      .target_logical_sector_size =
          request.reviewed_target.logical_sector_size,
      .source_is_windows_system = source_contains_windows,
      .windows_is_amd64 = !source_contains_windows ||
          (analysis.windows_version.has_value() &&
           analysis.windows_version->architecture == "AMD64"),
      .bitlocker_fully_decrypted = analysis.bitlocker_fully_decrypted,
      .surplus_allocation = request.surplus_allocation,
      .surplus_target_source_table_index =
          request.surplus_target_source_table_index,
  };
  direct_request.source_partitions.reserve(analysis.partitions.size());
  for (const auto& partition : analysis.partitions) {
    const bool required = required_for_offline_windows(
        source_contains_windows, partition.role);
    const bool selected = required ||
        request.selected_source_table_indexes.empty() ||
        selected_indexes.contains(partition.source_table_index);
    direct_request.source_partitions.push_back(
        migrationcore::DirectCloneSourcePartition{
            .partition = migrationcore::ShrinkSourcePartition{
                .source_table_index = partition.source_table_index,
                .role = partition.role,
                .file_system = partition.role ==
                        migrationcore::MigrationPartitionRole::efi_system
                    ? migrationcore::MigrationFileSystem::fat32
                    : partition.file_system,
                .source_size_bytes = partition.source_size_bytes,
                .used_bytes = partition.used_bytes,
                .minimum_target_bytes = 0U,
                .cluster_size = partition.cluster_size,
                .label = partition.label,
                .active = partition.active,
            },
            .selected = selected,
            .required_for_windows = required && partition.role ==
                migrationcore::MigrationPartitionRole::recovery,
        });
  }
  auto direct_plan = migrationcore::plan_direct_clone(direct_request);
  if (!direct_plan) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        direct_plan.error());
  }
  if (direct_plan.value().mode() != migrationcore::DirectCloneMode::shrink ||
      direct_plan.value().source_style() !=
          migrationcore::MigrationPartitionStyle::gpt ||
      direct_plan.value().target_style() !=
          migrationcore::MigrationPartitionStyle::gpt ||
      !direct_plan.value().source_remains_unchanged()) {
    return failure<WinPeOfflineNtfsDirectShrinkPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPE offline normalized decision",
        L"shared plannerがGPT preserve/source unchangedのshrink decisionを返しませんでした");
  }

  std::vector<std::uint32_t> retained_ntfs_indexes;
  for (const auto& partition : direct_plan.value().target_partitions()) {
    if (partition.source_table_index.has_value() &&
        partition.transfer ==
            migrationcore::DirectClonePartitionTransfer::
                file_system_content &&
        partition.source_used_bytes != 0U) {
      retained_ntfs_indexes.push_back(*partition.source_table_index);
    }
  }
  auto epoch = make_source_epoch(request, analysis, retained_ntfs_indexes);
  if (!epoch) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        epoch.error());
  }

  directshrink::TargetPlanData data{
      .expected_source = inspection.binding.source,
      .expected_target = inspection.binding.target,
      .expected_source_layout_hash = inspection.binding.source_layout_hash,
      .expected_target_layout_hash = inspection.binding.target_layout_hash,
      .source_partition_snapshot_hash =
          inspection.binding.source_partition_snapshot_hash,
      .source_partition_style = migrationcore::MigrationPartitionStyle::gpt,
      .partition_style = migrationcore::MigrationPartitionStyle::gpt,
      .partition_style_choice =
          migrationcore::DirectClonePartitionStyleChoice::preserve,
      .surplus_allocation = direct_plan.value().surplus_allocation(),
      .surplus_target_source_table_index =
          direct_plan.value().surplus_target_source_table_index(),
      .checkpoint_offset_bytes = directshrink::kCheckpointOffsetBytes,
      .boot_finalization_required =
          direct_plan.value().boot_finalization_required(),
      .target_is_active_rescue_media = false,
  };
  data.tasks.reserve(direct_plan.value().target_partitions().size());
  std::uint64_t expected_work_bytes{};
  std::uint64_t final_end{};
  std::size_t windows_count{};
  std::size_t efi_count{};
  std::size_t msr_count{};
  std::size_t recovery_count{};
  for (const auto& partition : direct_plan.value().target_partitions()) {
    const auto* analyzed = partition.source_table_index.has_value()
        ? find_analysis_partition(analysis, *partition.source_table_index)
        : nullptr;
    const auto* reviewed = partition.source_table_index.has_value()
        ? find_reviewed_partition(
              request.reviewed_source, *partition.source_table_index)
        : nullptr;
    if (partition.source_table_index.has_value() &&
        (analyzed == nullptr || reviewed == nullptr ||
         analyzed->source_offset_bytes != reviewed->offset_bytes ||
         analyzed->source_size_bytes != reviewed->size_bytes ||
         partition.source_size_bytes != analyzed->source_size_bytes)) {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"WinPE offline task source対応",
          L"normalized decisionを解析済みsource table index/extentへ一意に対応できません");
    }
    directshrink::PartitionTask task{
        .target_number = partition.target_number,
        .source_table_index = partition.source_table_index,
        .role = partition.role,
        .active = partition.active,
        .source_offset_bytes = analyzed == nullptr
            ? 0U
            : analyzed->source_offset_bytes,
        .target_offset_bytes = partition.offset_bytes,
        .construction_size_bytes = partition.minimum_size_bytes,
        .target_size_bytes = partition.size_bytes,
        .source_size_bytes = partition.source_size_bytes,
        .source_used_bytes = partition.source_used_bytes,
    };
    if (partition.role == migrationcore::MigrationPartitionRole::windows) {
      ++windows_count;
    } else if (
        partition.role == migrationcore::MigrationPartitionRole::efi_system) {
      ++efi_count;
    } else if (
        partition.role ==
        migrationcore::MigrationPartitionRole::microsoft_reserved) {
      ++msr_count;
    } else if (
        partition.role == migrationcore::MigrationPartitionRole::recovery) {
      ++recovery_count;
    }
    if (partition.transfer ==
        migrationcore::DirectClonePartitionTransfer::recreate) {
      if (partition.role ==
              migrationcore::MigrationPartitionRole::efi_system &&
          partition.file_system == migrationcore::MigrationFileSystem::fat32) {
        task.kind = directshrink::PartitionTaskKind::recreate_efi_system;
      } else if (
          partition.role ==
              migrationcore::MigrationPartitionRole::microsoft_reserved &&
          partition.file_system == migrationcore::MigrationFileSystem::none) {
        task.kind =
            directshrink::PartitionTaskKind::recreate_microsoft_reserved;
      } else {
        return failure<WinPeOfflineNtfsDirectShrinkPlan>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"WinPE offline generated partition",
            L"初期sliceは生成ESP/MSR以外のrecreate taskを扱いません");
      }
    } else if (
        partition.transfer ==
            migrationcore::DirectClonePartitionTransfer::
                file_system_content &&
        partition.file_system == migrationcore::MigrationFileSystem::ntfs &&
        partition.source_table_index.has_value()) {
      if (partition.source_used_bytes == 0U) {
        if (partition.role != migrationcore::MigrationPartitionRole::data) {
          return failure<WinPeOfflineNtfsDirectShrinkPlan>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"WinPE offline empty NTFS role",
              L"空NTFSの再作成は選択済みdata partitionだけを扱います");
        }
        task.kind = directshrink::PartitionTaskKind::create_empty_ntfs;
      } else {
        const auto* volume = find_analysis_volume(
            analysis, *partition.source_table_index);
        if (volume == nullptr || volume->volume_guid_path.empty()) {
          return failure<WinPeOfflineNtfsDirectShrinkPlan>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_NOT_FOUND,
              L"WinPE offline NTFS Volume対応",
              L"選択NTFS sourceへcanonical Volume GUID rootを一意に対応できません");
        }
        task.kind = directshrink::PartitionTaskKind::apply_ntfs_wim;
        task.original_volume_guid_path = volume->volume_guid_path;
        ++data.archive_task_count;
      }
    } else if (
        partition.transfer ==
            migrationcore::DirectClonePartitionTransfer::exact_content &&
        partition.file_system ==
            migrationcore::MigrationFileSystem::unsupported &&
        partition.role == migrationcore::MigrationPartitionRole::data &&
        partition.source_table_index.has_value() && analyzed != nullptr &&
        !all_zero(analyzed->type_id) &&
        partition.minimum_size_bytes == partition.source_size_bytes &&
        partition.size_bytes == partition.source_size_bytes) {
      task.kind = directshrink::PartitionTaskKind::copy_exact_raw;
      task.source_partition_type = analyzed->type_id;
    } else {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"WinPE offline partition transfer",
          L"NTFS WIM reconstruction、生成ESP/MSR、またはdata未対応FSの元サイズexact RAW以外は開始しません");
    }
    if (task.construction_size_bytes < task.target_size_bytes) {
      if (task.kind != directshrink::PartitionTaskKind::apply_ntfs_wim &&
          task.kind != directshrink::PartitionTaskKind::create_empty_ntfs) {
        return failure<WinPeOfflineNtfsDirectShrinkPlan>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"WinPE offline NTFS extension",
            L"最終容量へ伸長するpartitionは検証可能なNTFS taskに限定します");
      }
      ++data.ntfs_extension_task_count;
    }
    if (!checked_add(
            expected_work_bytes,
            task.target_size_bytes,
            expected_work_bytes)) {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"WinPE offline work upper bound",
          L"最終partition容量合計がオーバーフローしました");
    }
    std::uint64_t end{};
    if (!checked_add(task.target_offset_bytes, task.target_size_bytes, end)) {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"WinPE offline final extent",
          L"最終partition終端がオーバーフローしました");
    }
    final_end = (std::max)(final_end, end);
    data.tasks.push_back(std::move(task));
  }
  if (data.boot_finalization_required
          ? windows_count != 1U || efi_count != 1U || msr_count != 1U ||
              recovery_count > 1U
          : windows_count != 0U || efi_count != 0U || recovery_count != 0U ||
              msr_count > 1U) {
    return failure<WinPeOfflineNtfsDirectShrinkPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPE offline GPT role集合",
        L"Windows/ESP/MSR/WinREまたはdata-only GPT roleを一意に確定できません");
  }

  data.source_partition_mappings.reserve(
      direct_plan.value().partition_selection().size());
  for (const auto& selection : direct_plan.value().partition_selection()) {
    const auto transferred = std::find_if(
        data.tasks.begin(),
        data.tasks.end(),
        [&](const directshrink::PartitionTask& task) {
          return task.source_table_index == selection.source_table_index;
        });
    directshrink::SourcePartitionMapping mapping{
        .source_table_index = selection.source_table_index,
        .role = selection.role,
        .requested = selection.requested,
        .selected = selection.selected,
        .required = selection.required,
    };
    if (!selection.selected) {
      mapping.disposition =
          directshrink::SourcePartitionDisposition::omitted_unselected;
    } else if (transferred != data.tasks.end()) {
      mapping.disposition =
          directshrink::SourcePartitionDisposition::transferred_to_target;
      mapping.target_number = transferred->target_number;
    } else if (
        selection.role == migrationcore::MigrationPartitionRole::efi_system ||
        selection.role ==
            migrationcore::MigrationPartitionRole::microsoft_reserved) {
      const auto generated = std::find_if(
          data.tasks.begin(),
          data.tasks.end(),
          [&](const directshrink::PartitionTask& task) {
            return !task.source_table_index.has_value() &&
                task.role == selection.role;
          });
      if (generated == data.tasks.end()) {
        return failure<WinPeOfflineNtfsDirectShrinkPlan>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"WinPE offline generated system mapping",
            L"source ESP/MSRに対応する生成target taskがありません");
      }
      mapping.disposition = directshrink::SourcePartitionDisposition::
          recreated_as_generated_system_partition;
      mapping.target_number = generated->target_number;
    } else {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"WinPE offline source disposition",
          L"選択source partitionのtransfer/system再作成/省略を一意に確定できません");
    }
    data.source_partition_mappings.push_back(std::move(mapping));
  }

  std::uint64_t staging_requirement{};
  if (!checked_add(
          directshrink::kStagingControlReserveBytes,
          directshrink::kStagingFileSystemReserveBytes,
          staging_requirement)) {
    return failure<WinPeOfflineNtfsDirectShrinkPlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"WinPE offline staging requirement",
        L"staging controlとNTFS/DISM reserveの合計がオーバーフローしました");
  }
  std::uint64_t staging_offset{};
  std::uint64_t staging_length{};
  std::optional<std::uint32_t> growth_owner;
  if (direct_plan.value().surplus_allocation() ==
      migrationcore::ShrinkSurplusAllocation::leave_unallocated) {
    if (data.ntfs_extension_task_count != 0U ||
        !align_up(
            final_end,
            directshrink::kStagingAlignmentBytes,
            staging_offset) ||
        staging_offset != final_end ||
        direct_plan.value().unallocated_tail_bytes() < staging_requirement) {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"WinPE offline unallocated staging",
          L"final GPTと非重複の末尾へtarget-owned WIM/DISM stagingを保持できません");
    }
    staging_length = direct_plan.value().unallocated_tail_bytes();
  } else {
    for (const auto& task : data.tasks) {
      if (task.construction_size_bytes >= task.target_size_bytes) {
        continue;
      }
      const auto length =
          task.target_size_bytes - task.construction_size_bytes;
      if (length < staging_requirement || length <= staging_length) {
        continue;
      }
      if (!checked_add(
              task.target_offset_bytes,
              task.construction_size_bytes,
              staging_offset)) {
        return failure<WinPeOfflineNtfsDirectShrinkPlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"WinPE offline growth staging offset",
            L"NTFS construction終端がオーバーフローしました");
      }
      staging_length = length;
      growth_owner = task.target_number;
    }
    if (!growth_owner.has_value()) {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"WinPE offline growth staging",
          L"NTFS growth extentへtarget-owned WIM/DISM stagingを保持できません");
    }
  }
  std::uint64_t staging_end{};
  std::uint64_t archive_offset{};
  if (!checked_add(staging_offset, staging_length, staging_end) ||
      !checked_add(
          staging_offset,
          directshrink::kStagingControlReserveBytes,
          archive_offset) ||
      staging_offset % directshrink::kStagingAlignmentBytes != 0U ||
      staging_length % directshrink::kStagingAlignmentBytes != 0U ||
      staging_end > request.reviewed_target.size_bytes ||
      request.reviewed_target.size_bytes - staging_end <
          directshrink::kStagingAlignmentBytes) {
    return failure<WinPeOfflineNtfsDirectShrinkPlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"WinPE offline staging placement",
        L"staging終端、archive開始、整列、または末尾GPT metadata保護を確定できません");
  }
  data.staging = directshrink::TargetOwnedStagingPlan{
      .offset_bytes = staging_offset,
      .length_bytes = staging_length,
      .control_reserve_bytes = directshrink::kStagingControlReserveBytes,
      .archive_offset_bytes = archive_offset,
      .archive_capacity_bytes =
          staging_length - directshrink::kStagingControlReserveBytes,
      .final_growth_owner_target_number = growth_owner,
  };
  data.maximum_archive_upper_bound_bytes =
      data.staging.archive_capacity_bytes;
  for (auto& task : data.tasks) {
    if (task.kind == directshrink::PartitionTaskKind::apply_ntfs_wim) {
      task.archive_upper_bound_bytes = data.staging.archive_capacity_bytes;
    }
  }
  auto final_hash = final_layout_hash(
      direct_plan.value(), data.staging, data.tasks);
  if (!final_hash) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        final_hash.error());
  }
  data.final_layout_hash = final_hash.take_value();
  auto payload = target_payload_hash(
      request, epoch.value(), direct_plan.value(), data);
  if (!payload) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        payload.error());
  }
  data.operation_plan = operationcore::OperationPlan{
      .schema_version = operationcore::kOperationPlanSchemaVersion,
      .operation_id = request.operation_id,
      .kind = operationcore::OperationKind::clone,
      .environment = operationcore::OperationEnvironment::winpe,
      .source = data.expected_source,
      .target = data.expected_target,
      .expected_work_bytes = expected_work_bytes,
      .immutable_payload_hash = payload.take_value(),
  };
  auto target_plan = directshrink::make_target_plan(std::move(data));
  if (!target_plan) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        target_plan.error());
  }
  return build_winpe_offline_ntfs_direct_shrink_plan(
      target_plan.take_value(),
      WinPeOfflineNtfsPlanningEvidence{
          .source_epoch = epoch.take_value(),
          .analysis_created_utc = request.analysis_created_utc,
          .app_version = request.app_version,
          .winpe_environment_verified = request.winpe_environment_verified,
          .source_contains_windows = source_contains_windows,
          .source_supported_basic_disk = true,
          .source_health_allows_standard_clone = true,
          .target_supported_fixed_disk = true,
          .target_non_system = true,
          .target_health_allows_destructive_clone = true,
      });
}

clonecore::Status validate_product_inputs(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis) {
  const bool source_contains_windows = analysis.windows_version.has_value();
  auto reviewed_source_identity = diskmodel::make_stable_disk_identity(
      request.reviewed_source, false);
  if (!reviewed_source_identity) {
    return clonecore::Status::failure(reviewed_source_identity.error());
  }
  if (!request.administrator || !request.winpe_environment_verified ||
      request.analysis_created_utc.empty() || request.app_version.empty() ||
      request.reviewed_source.logical_sector_size != 512U ||
      request.reviewed_target.logical_sector_size != 512U ||
      diskmodel::normalize_disk_partition_style(
          request.reviewed_source.partition_style,
          request.reviewed_source.partitions.size()) !=
          diskmodel::PartitionStyle::gpt ||
      !request.reviewed_source.read_only.value_or(false) ||
      request.reviewed_source.offline.value_or(true) ||
      request.reviewed_source.removable.value_or(true) ||
      request.reviewed_target.read_only.value_or(true) ||
      request.reviewed_target.removable.value_or(true) ||
      request.reviewed_target.is_system_disk ||
      request.reviewed_source.partitions.empty() ||
      analysis.partition_style !=
          migrationcore::MigrationPartitionStyle::gpt ||
      analysis.partitions.size() != request.reviewed_source.partitions.size() ||
      analysis.partition_snapshot.empty() ||
      analysis.content_volumes.empty() ||
      !analysis.bitlocker_fully_decrypted ||
      !stable_identity_exactly_matches(
          analysis.source,
          reviewed_source_identity.value()) ||
      (source_contains_windows &&
       (analysis.windows_version->major != 10U ||
        analysis.windows_version->build == 0U ||
        analysis.windows_version->architecture != "AMD64"))) {
    return clonecore::Status::failure(product_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPE offline NTFS product条件",
        L"管理者WinPE、OS-level read-only固定GPT source、512B、whole-disk解析、完全復号NTFS、固定非system target、またはWindows 10/11 AMD64条件を満たしません"));
  }
  const auto source_health = diskmodel::disk_health_operation_advice(
      request.reviewed_source.health, true);
  const auto target_health = diskmodel::disk_health_operation_advice(
      request.reviewed_target.health, false);
  if (source_health != diskmodel::DiskHealthOperationAdvice::proceed ||
      target_health == diskmodel::DiskHealthOperationAdvice::block_target) {
    return clonecore::Status::failure(product_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_DEVICE_HARDWARE_ERROR,
        L"WinPE offline NTFS disk health",
        L"source注意/異常またはtarget重大健康異常は標準縮小経路で開始しません"));
  }
  const auto source_class =
      imageformat::classify_tsumugi_physical_restore_target(
          request.reviewed_source);
  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(
          request.reviewed_target);
  if (source_class.dynamic_disk || source_class.storage_spaces ||
      source_class.software_raid || source_class.unresolved_hardware_raid ||
      source_class.unsupported_virtual || target_class.dynamic_disk ||
      target_class.storage_spaces || target_class.software_raid ||
      target_class.unresolved_hardware_raid ||
      target_class.unsupported_virtual) {
    return clonecore::Status::failure(product_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPE offline NTFS basic disk分類",
        L"Dynamic Disk、Storage Spaces、RAID、iSCSI、またはunsupported virtual diskは初期sliceで扱いません"));
  }
  return clonecore::success_status();
}

struct ProductObservation final {
  WinPeOfflineNtfsProductPlanningRequest request;
  windowsshrink::ShrinkSourceAnalysis analysis;
};

std::wstring volume_child(
    const std::wstring& root,
    const std::wstring_view relative) {
  std::wstring result = root;
  if (!result.ends_with(L'\\')) {
    result.push_back(L'\\');
  }
  result.append(relative);
  return result;
}

clonecore::Result<windowsshrink::ShrinkSourceAnalysis>
analyze_winpe_offline_gpt_source(
    const diskmodel::ReadOnlyPhysicalDiskHandle& opened,
    const clonecore::GptDisk& gpt,
    const std::string_view analysis_created_utc,
    const std::string_view app_version) {
  windowsshrink::ShrinkSourceAnalysisContext data_context{
      .source_identity = opened.observed.identity,
      .physical_sector_size = opened.observed.observed.physical_sector_size,
      .created_utc = std::string(analysis_created_utc),
      .app_version = std::string(app_version),
  };
  auto analysis = windowsshrink::analyze_gpt_shrink_source_with_windows_apis(
      opened.observed.observed, *opened.reader, gpt, data_context);
  if (!analysis) {
    return analysis;
  }

  std::optional<windowsshrink::WindowsSourceVersion> offline_windows;
  for (const auto& volume : analysis.value().content_volumes) {
    const auto kernel = volume_child(
        volume.volume_guid_path, L"Windows\\System32\\ntoskrnl.exe");
    const DWORD attributes = GetFileAttributesW(kernel.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
      continue;
    }
    if (offline_windows.has_value()) {
      return failure<windowsshrink::ShrinkSourceAnalysis>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DUP_NAME,
          L"WinPE offline Windows一意性",
          L"対応Windows volumeが複数あるためBoot/WinRE targetを一意に確定できません");
    }
    auto verified =
        bootrepair::verify_offline_windows_amd64(volume.volume_guid_path);
    auto version = bootrepair::read_offline_windows_version_hive(
        volume_child(
            volume.volume_guid_path,
            L"Windows\\System32\\Config\\SOFTWARE"));
    if (!verified || !version) {
      return clonecore::Result<
          windowsshrink::ShrinkSourceAnalysis>::failure(
          !verified ? verified.error() : version.error());
    }
    offline_windows = windowsshrink::WindowsSourceVersion{
        .major = version.value().major,
        .minor = 0U,
        .build = version.value().build,
        .architecture = "AMD64",
    };
  }
  if (!offline_windows.has_value()) {
    return analysis;
  }

  auto system_identity = opened.observed.identity;
  // The source is offline relative to this WinPE boot. This semantic flag is
  // used only by the shared analyzer to classify Windows/Boot roles and is
  // deliberately excluded from stable hardware identity comparison.
  system_identity.is_system_disk = true;
  windowsshrink::ShrinkSourceAnalysisContext system_context{
      .source_identity = std::move(system_identity),
      .physical_sector_size = opened.observed.observed.physical_sector_size,
      .created_utc = std::string(analysis_created_utc),
      .app_version = std::string(app_version),
      .known_windows_version = std::move(offline_windows),
  };
  return windowsshrink::analyze_gpt_shrink_source_with_windows_apis(
      opened.observed.observed, *opened.reader, gpt, system_context);
}

clonecore::Result<ProductObservation> observe_product_with_windows_apis(
    const WinPeOfflineNtfsProductPlanningRequest& request) {
  auto no_io_preflight =
      validate_winpe_offline_ntfs_direct_shrink_no_io_preflight(request);
  if (!no_io_preflight) {
    return clonecore::Result<ProductObservation>::failure(
        no_io_preflight.error());
  }
  auto initial_source = diskmodel::make_stable_disk_identity(
      request.reviewed_source, false);
  auto initial_target = diskmodel::make_stable_disk_identity(
      request.reviewed_target, false);
  auto initial_source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_source);
  auto initial_target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_target);
  if (!initial_source || !initial_target || !initial_source_layout ||
      !initial_target_layout) {
    return clonecore::Result<ProductObservation>::failure(
        !initial_source
            ? initial_source.error()
            : !initial_target
                ? initial_target.error()
                : !initial_source_layout ? initial_source_layout.error()
                                         : initial_target_layout.error());
  }
  auto read_only =
      diskmodel::set_verified_source_read_only_with_windows_apis(
          initial_source.value(), true);
  if (!read_only) {
    return clonecore::Result<ProductObservation>::failure(read_only.error());
  }
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  auto observed = diskmodel::reidentify_physical_clone_selection(
      initial_source.value(), initial_target.value(), *inventory, false);
  if (!observed) {
    return clonecore::Result<ProductObservation>::failure(observed.error());
  }
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().target);
  if (!source_layout || !target_layout ||
      source_layout.value() != initial_source_layout.value() ||
      target_layout.value() != initial_target_layout.value() ||
      !observed.value().source.read_only.value_or(false)) {
    return failure<ProductObservation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"WinPE offline planning再識別",
        L"source read-only設定後にsource/target stable identityまたはcanonical layoutが変化しました");
  }
  auto opened =
      diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          initial_source.value());
  if (!opened) {
    return clonecore::Result<ProductObservation>::failure(opened.error());
  }
  auto gpt = clonecore::parse_gpt(*opened.value().reader);
  if (!gpt) {
    return clonecore::Result<ProductObservation>::failure(gpt.error());
  }
  auto analysis = analyze_winpe_offline_gpt_source(
      opened.value(),
      gpt.value(),
      request.analysis_created_utc,
      request.app_version);
  if (!analysis) {
    return clonecore::Result<ProductObservation>::failure(analysis.error());
  }

  ProductObservation result{
      .request = request,
      .analysis = analysis.take_value(),
  };
  result.request.reviewed_source = opened.value().observed.observed;
  result.request.reviewed_target = observed.value().target;
  return clonecore::Result<ProductObservation>::success(std::move(result));
}

clonecore::Status validate_epoch_for_product_guard(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const WinPeOfflineNtfsSourceEpochEvidence& epoch) {
  auto canonical = hash_winpe_offline_ntfs_source_epoch_v1(epoch);
  const auto& expected = plan.planning_evidence().source_epoch;
  if (!canonical ||
      !stable_identity_exactly_matches(
          plan.target_plan().expected_source(), epoch.observed_source) ||
      epoch.source_layout_hash !=
          plan.target_plan().expected_source_layout_hash() ||
      epoch.source_partition_snapshot_hash !=
          plan.target_plan().source_partition_snapshot_hash() ||
      epoch.source_analysis_hash != expected.source_analysis_hash ||
      canonical.value() != epoch.canonical_epoch_hash ||
      epoch.canonical_epoch_hash != expected.canonical_epoch_hash ||
      epoch.logical_sector_size != 512U ||
      !epoch.stable_identity_reidentified || !epoch.source_os_read_only ||
      !epoch.physical_handle_read_only || !epoch.gpt_source ||
      !epoch.whole_disk_analysis || !epoch.bitlocker_fully_decrypted) {
    return canonical
        ? status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"WinPE product source epoch",
              L"保持したread-only sourceのidentity、layout、raw snapshot、analysis、またはcanonical epochが計画と一致しません")
        : clonecore::Status::failure(canonical.error());
  }

  std::size_t archive_count{};
  for (const auto& task : plan.target_plan().tasks()) {
    if (task.kind != directshrink::PartitionTaskKind::apply_ntfs_wim) {
      continue;
    }
    ++archive_count;
    if (!task.source_table_index.has_value()) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"WinPE product source epoch task",
          L"NTFS archive taskにsource table indexがありません");
    }
    const WinPeOfflineNtfsVolumeBinding* matched = nullptr;
    for (const auto& volume : epoch.ntfs_volumes) {
      if (volume.source_table_index != *task.source_table_index) {
        continue;
      }
      if (matched != nullptr) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"WinPE product source epoch Volume",
            L"1つのsource table indexへ複数のVolume GUIDが対応しました");
      }
      matched = std::addressof(volume);
    }
    if (matched == nullptr ||
        matched->source_offset_bytes != task.source_offset_bytes ||
        matched->source_size_bytes != task.source_size_bytes ||
        !equal_path(
            matched->volume_guid_path, task.original_volume_guid_path)) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"WinPE product source epoch extent",
          L"選択NTFSのsource table index、物理extent、またはVolume GUID rootが計画と一致しません");
    }
  }
  if (archive_count != plan.target_plan().archive_task_count() ||
      epoch.ntfs_volumes.size() != archive_count) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"WinPE product source epoch件数",
        L"全archive taskとretained Volume bindingがexactly one対応ではありません");
  }
  return clonecore::success_status();
}

struct RetainedSourceEpoch final {
  WinPeOfflineNtfsSourceEpochEvidence epoch;
  std::unique_ptr<diskmodel::ReadOnlyPhysicalDiskHandle> physical;
};

clonecore::Result<RetainedSourceEpoch> observe_retained_source_epoch(
    const WinPeOfflineNtfsDirectShrinkPlan& plan) {
  const auto read_only =
      diskmodel::set_verified_source_read_only_with_windows_apis(
          plan.target_plan().expected_source(), true);
  if (!read_only) {
    return clonecore::Result<RetainedSourceEpoch>::failure(
        read_only.error());
  }
  auto opened =
      diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          plan.target_plan().expected_source());
  if (!opened) {
    return clonecore::Result<RetainedSourceEpoch>::failure(opened.error());
  }
  const auto& observed = opened.value().observed.observed;
  if (!observed.read_only.has_value() || !observed.read_only.value() ||
      !observed.offline.has_value() || observed.offline.value() ||
      !observed.removable.has_value() || observed.removable.value() ||
      observed.logical_sector_size != 512U || !opened.value().reader) {
    return failure<RetainedSourceEpoch>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_WRITE_PROTECT,
        L"WinPE retained source physical handle",
        L"OS-level read-only、online fixed GPT source、512-byte sector、または保持read handleを証明できません");
  }
  auto gpt = clonecore::parse_gpt(*opened.value().reader);
  if (!gpt) {
    return clonecore::Result<RetainedSourceEpoch>::failure(gpt.error());
  }
  auto analysis = analyze_winpe_offline_gpt_source(
      opened.value(),
      gpt.value(),
      plan.planning_evidence().analysis_created_utc,
      plan.planning_evidence().app_version);
  if (!analysis) {
    return clonecore::Result<RetainedSourceEpoch>::failure(analysis.error());
  }
  if (analysis.value().windows_version.has_value() !=
      plan.planning_evidence().source_contains_windows) {
    return failure<RetainedSourceEpoch>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"WinPE retained source Windows role",
        L"offline Windowsの有無が計画時と一致しません");
  }

  std::vector<std::uint32_t> retained_indexes;
  retained_indexes.reserve(plan.target_plan().archive_task_count());
  for (const auto& task : plan.target_plan().tasks()) {
    if (task.kind == directshrink::PartitionTaskKind::apply_ntfs_wim &&
        task.source_table_index.has_value()) {
      retained_indexes.push_back(*task.source_table_index);
    }
  }
  WinPeOfflineNtfsProductPlanningRequest request{
      .reviewed_source = observed,
      .analysis_created_utc = plan.planning_evidence().analysis_created_utc,
      .app_version = plan.planning_evidence().app_version,
  };
  auto epoch = make_source_epoch(request, analysis.value(), retained_indexes);
  if (!epoch) {
    return clonecore::Result<RetainedSourceEpoch>::failure(epoch.error());
  }
  auto valid = validate_epoch_for_product_guard(plan, epoch.value());
  if (!valid) {
    return clonecore::Result<RetainedSourceEpoch>::failure(valid.error());
  }
  RetainedSourceEpoch result{
      .epoch = epoch.take_value(),
      .physical = std::make_unique<diskmodel::ReadOnlyPhysicalDiskHandle>(
          opened.take_value()),
  };
  return clonecore::Result<RetainedSourceEpoch>::success(std::move(result));
}

clonecore::Result<std::wstring> resolve_volume_guid_root(
    const std::wstring& expected_root) {
  if (!is_canonical_volume_guid_root(expected_root)) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"WinPE canonical Volume GUID root",
        L"capture pathは末尾\\付きcanonical Volume GUID rootでなければなりません");
  }
  std::array<wchar_t, 64U> resolved{};
  if (!GetVolumeNameForVolumeMountPointW(
          expected_root.c_str(),
          resolved.data(),
          static_cast<DWORD>(resolved.size()))) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"WinPE Volume GUID root再解決",
            GetLastError()));
  }
  std::wstring value(resolved.data());
  if (!is_canonical_volume_guid_root(value) ||
      !equal_path(value, expected_root)) {
    return failure<std::wstring>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"WinPE Volume GUID root再照合",
        L"Volume GUID rootがopen前後のcanonical identityと一致しません");
  }
  return clonecore::Result<std::wstring>::success(std::move(value));
}

clonecore::Status verify_held_volume_extent(
    const HANDLE volume,
    const diskmodel::DiskInfo& observed_source,
    const WinPeOfflineNtfsVolumeBinding& expected) {
  constexpr std::size_t kExtentBufferBytes = 64U * 1024U;
  constexpr std::size_t kMaximumExtentCount = 256U;
  std::vector<std::byte> buffer(kExtentBufferBytes);
  DWORD extent_bytes{};
  const bool extents_available = DeviceIoControl(
      volume,
      IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
      nullptr,
      0U,
      buffer.data(),
      static_cast<DWORD>(buffer.size()),
      &extent_bytes,
      nullptr) != FALSE;
  const DWORD extent_error = extents_available ? ERROR_SUCCESS : GetLastError();
  if (!extents_available && extent_error != ERROR_INVALID_FUNCTION) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"WinPE held Volume physical extent",
        extent_error));
  }

  STORAGE_DEVICE_NUMBER device{};
  DWORD device_bytes{};
  if (!DeviceIoControl(
          volume,
          IOCTL_STORAGE_GET_DEVICE_NUMBER,
          nullptr,
          0U,
          &device,
          static_cast<DWORD>(sizeof(device)),
          &device_bytes,
          nullptr) ||
      device_bytes < sizeof(device) || device.DeviceType != FILE_DEVICE_DISK) {
    const DWORD native = GetLastError();
    return status_failure(
        clonecore::ErrorCode::query_failed,
        native == ERROR_SUCCESS ? ERROR_INVALID_DATA : native,
        L"WinPE held Volume device number",
        L"read-only Volume handleのdisk/partition番号を取得できません");
  }

  PARTITION_INFORMATION_EX partition{};
  DWORD partition_bytes{};
  if (!DeviceIoControl(
          volume,
          IOCTL_DISK_GET_PARTITION_INFO_EX,
          nullptr,
          0U,
          &partition,
          static_cast<DWORD>(sizeof(partition)),
          &partition_bytes,
          nullptr) ||
      partition_bytes < sizeof(partition) ||
      partition.PartitionNumber == 0U ||
      partition.StartingOffset.QuadPart < 0 ||
      partition.PartitionLength.QuadPart <= 0) {
    const DWORD native = GetLastError();
    return status_failure(
        clonecore::ErrorCode::query_failed,
        native == ERROR_SUCCESS ? ERROR_INVALID_DATA : native,
        L"WinPE held Volume partition info",
        L"read-only Volume handleのpartition extentを取得できません");
  }

  const auto partition_offset = static_cast<std::uint64_t>(
      partition.StartingOffset.QuadPart);
  const auto partition_length = static_cast<std::uint64_t>(
      partition.PartitionLength.QuadPart);
  const bool storage_partition_known = device.PartitionNumber != 0U &&
      device.PartitionNumber != MAXDWORD;
  if (device.DeviceNumber != observed_source.disk_number ||
      partition.PartitionNumber != expected.source_table_index ||
      (storage_partition_known &&
       device.PartitionNumber != partition.PartitionNumber) ||
      partition_offset != expected.source_offset_bytes ||
      partition_length != expected.source_size_bytes) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"WinPE held Volume exact extent",
        L"Volume handleのdisk番号、source table index、offset、またはlengthが計画と一致しません");
  }

  if (extents_available) {
    constexpr std::size_t kHeaderBytes =
        offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (extent_bytes < kHeaderBytes) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"WinPE held Volume extent応答",
          L"Volume extent応答がheaderより短いです");
    }
    const auto* extents =
        reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    const std::size_t count = extents->NumberOfDiskExtents;
    const std::size_t required = kHeaderBytes + count * sizeof(DISK_EXTENT);
    if (count != 1U || count > kMaximumExtentCount ||
        required > extent_bytes || extents->Extents[0].StartingOffset.QuadPart < 0 ||
        extents->Extents[0].ExtentLength.QuadPart <= 0 ||
        extents->Extents[0].DiskNumber != observed_source.disk_number ||
        static_cast<std::uint64_t>(
            extents->Extents[0].StartingOffset.QuadPart) !=
            expected.source_offset_bytes ||
        static_cast<std::uint64_t>(extents->Extents[0].ExtentLength.QuadPart) !=
            expected.source_size_bytes) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"WinPE held Volume extent再照合",
          L"single-disk Volume extentと独立device/partition queryが一致しません");
    }
  }
  return clonecore::success_status();
}

std::wstring capture_identity_for(
    const WinPeOfflineNtfsSourceEpochEvidence& epoch,
    const std::uint32_t source_table_index) {
  constexpr wchar_t kHex[] = L"0123456789ABCDEF";
  std::wstring identity = L"WINPE-RO-EPOCH:";
  identity.reserve(identity.size() + epoch.canonical_epoch_hash.size() * 2U +
                   12U);
  for (const std::byte value : epoch.canonical_epoch_hash) {
    const auto byte = std::to_integer<unsigned int>(value);
    identity.push_back(kHex[(byte >> 4U) & 0x0FU]);
    identity.push_back(kHex[byte & 0x0FU]);
  }
  identity.push_back(L':');
  identity.append(std::to_wstring(source_table_index));
  return identity;
}

class WindowsWinPeOfflineNtfsCaptureLease final
    : public IWinPeOfflineNtfsCaptureLease {
 public:
  WindowsWinPeOfflineNtfsCaptureLease(
      WinPeOfflineNtfsCaptureLeaseEvidence evidence,
      std::unique_ptr<diskmodel::ReadOnlyPhysicalDiskHandle> physical,
      clonecore::UniqueHandle volume) noexcept
      : evidence_(std::move(evidence)),
        physical_(std::move(physical)),
        volume_(std::move(volume)) {}

  [[nodiscard]] const WinPeOfflineNtfsCaptureLeaseEvidence& evidence()
      const noexcept override {
    return evidence_;
  }

 private:
  WinPeOfflineNtfsCaptureLeaseEvidence evidence_;
  std::unique_ptr<diskmodel::ReadOnlyPhysicalDiskHandle> physical_;
  clonecore::UniqueHandle volume_;
};

class WindowsWinPeOfflineNtfsSourceGuard final
    : public IWinPeOfflineNtfsSourceGuard {
 public:
  [[nodiscard]] clonecore::Result<WinPeOfflineNtfsSourceEpochEvidence>
  lock_source_read_only_and_revalidate(
      const WinPeOfflineNtfsDirectShrinkPlan& plan) override {
    auto observed = observe_retained_source_epoch(plan);
    if (!observed) {
      return clonecore::Result<
          WinPeOfflineNtfsSourceEpochEvidence>::failure(observed.error());
    }
    source_left_os_read_only_ = true;
    retained_physical_ = std::move(observed.value().physical);
    return clonecore::Result<WinPeOfflineNtfsSourceEpochEvidence>::success(
        std::move(observed.value().epoch));
  }

  [[nodiscard]] clonecore::Result<std::unique_ptr<
      IWinPeOfflineNtfsCaptureLease>>
  acquire_capture_lease_after_revalidation(
      const WinPeOfflineNtfsDirectShrinkPlan& plan,
      const directshrink::PartitionTask& task) override {
    if (task.kind != directshrink::PartitionTaskKind::apply_ntfs_wim ||
        !task.source_table_index.has_value()) {
      return failure<std::unique_ptr<IWinPeOfflineNtfsCaptureLease>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"WinPE capture lease task",
          L"capture leaseはsource-bound NTFS archive taskだけに発行します");
    }
    auto observed = observe_retained_source_epoch(plan);
    if (!observed) {
      return clonecore::Result<std::unique_ptr<
          IWinPeOfflineNtfsCaptureLease>>::failure(observed.error());
    }
    source_left_os_read_only_ = true;
    const WinPeOfflineNtfsVolumeBinding* binding = nullptr;
    for (const auto& volume : observed.value().epoch.ntfs_volumes) {
      if (volume.source_table_index == *task.source_table_index) {
        if (binding != nullptr) {
          return failure<std::unique_ptr<IWinPeOfflineNtfsCaptureLease>>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DUP_NAME,
              L"WinPE capture lease Volume",
              L"source table indexへ複数のVolume GUID rootが対応しました");
        }
        binding = std::addressof(volume);
      }
    }
    if (binding == nullptr ||
        binding->source_offset_bytes != task.source_offset_bytes ||
        binding->source_size_bytes != task.source_size_bytes ||
        !equal_path(
            binding->volume_guid_path, task.original_volume_guid_path)) {
      return failure<std::unique_ptr<IWinPeOfflineNtfsCaptureLease>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"WinPE capture lease binding",
          L"source table index、physical extent、またはVolume GUID rootを一意に再対応できません");
    }
    auto before = resolve_volume_guid_root(binding->volume_guid_path);
    if (!before) {
      return clonecore::Result<std::unique_ptr<
          IWinPeOfflineNtfsCaptureLease>>::failure(before.error());
    }
    std::wstring open_path = before.value();
    open_path.pop_back();
    clonecore::UniqueHandle volume(CreateFileW(
        open_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!volume) {
      return clonecore::Result<std::unique_ptr<
          IWinPeOfflineNtfsCaptureLease>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::access_denied,
              L"WinPE canonical Volume read-only lease open",
              GetLastError()));
    }
    auto extent = verify_held_volume_extent(
        volume.get(),
        observed.value().physical->observed.observed,
        *binding);
    auto after = resolve_volume_guid_root(binding->volume_guid_path);
    if (!extent || !after || !equal_path(before.value(), after.value())) {
      return !extent
          ? clonecore::Result<std::unique_ptr<
                IWinPeOfflineNtfsCaptureLease>>::failure(extent.error())
          : !after
              ? clonecore::Result<std::unique_ptr<
                    IWinPeOfflineNtfsCaptureLease>>::failure(after.error())
              : failure<std::unique_ptr<IWinPeOfflineNtfsCaptureLease>>(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"WinPE capture lease Volume GUID再照合",
                    L"Volume GUID rootがread-only handle取得後に変化しました");
    }

    WinPeOfflineNtfsCaptureLeaseEvidence evidence{
        .source_epoch = observed.value().epoch,
        .volume = *binding,
        .capture_device_path = before.take_value(),
        .capture_identity = capture_identity_for(
            observed.value().epoch, *task.source_table_index),
        .source_disk_handle_held_read_only =
            observed.value().physical != nullptr,
        .source_volume_handle_held_read_only = true,
        .source_volume_extent_reverified = true,
    };
    std::unique_ptr<IWinPeOfflineNtfsCaptureLease> lease =
        std::make_unique<WindowsWinPeOfflineNtfsCaptureLease>(
            std::move(evidence),
            std::move(observed.value().physical),
            std::move(volume));
    return clonecore::Result<std::unique_ptr<
        IWinPeOfflineNtfsCaptureLease>>::success(std::move(lease));
  }

  [[nodiscard]] clonecore::Result<WinPeOfflineNtfsSourceEpochEvidence>
  revalidate_immediately_before_final_commit(
      const WinPeOfflineNtfsDirectShrinkPlan& plan) override {
    auto observed = observe_retained_source_epoch(plan);
    if (!observed) {
      return clonecore::Result<
          WinPeOfflineNtfsSourceEpochEvidence>::failure(observed.error());
    }
    source_left_os_read_only_ = true;
    retained_physical_ = std::move(observed.value().physical);
    return clonecore::Result<WinPeOfflineNtfsSourceEpochEvidence>::success(
        std::move(observed.value().epoch));
  }

  [[nodiscard]] bool source_left_os_read_only() const noexcept override {
    return source_left_os_read_only_ && retained_physical_ != nullptr;
  }

 private:
  std::unique_ptr<diskmodel::ReadOnlyPhysicalDiskHandle> retained_physical_;
  bool source_left_os_read_only_{};
};

}  // namespace

clonecore::Status
validate_winpe_offline_ntfs_direct_shrink_no_io_preflight(
    const WinPeOfflineNtfsProductPlanningRequest& request) {
  auto source = diskmodel::make_stable_disk_identity(
      request.reviewed_source, false);
  auto target = diskmodel::make_stable_disk_identity(
      request.reviewed_target, false);
  if (!source || !target) {
    return clonecore::Status::failure(
        !source ? source.error() : target.error());
  }
  const bool same_current_disk =
      request.reviewed_source.disk_number ==
      request.reviewed_target.disk_number;
  const bool same_stable_disk = stable_identity_exactly_matches(
      source.value(), target.value());
  if (!request.administrator || !request.winpe_environment_verified ||
      request.analysis_created_utc.empty() || request.app_version.empty() ||
      request.reviewed_source.logical_sector_size != 512U ||
      request.reviewed_target.logical_sector_size != 512U ||
      request.reviewed_source.size_bytes == 0U ||
      request.reviewed_target.size_bytes == 0U ||
      request.reviewed_source.removable.value_or(false) ||
      request.reviewed_source.offline.value_or(false) ||
      request.reviewed_target.removable.value_or(false) ||
      request.reviewed_target.read_only.value_or(false) ||
      request.reviewed_target.is_system_disk || same_current_disk ||
      same_stable_disk) {
    return clonecore::Status::failure(product_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"WinPE offline NTFS no-I/O preflight",
        L"管理者WinPE、512-byte固定source/target、書込み可能な非system target、分離されたstable identity、または基本入力条件を満たしません"));
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::Sha256Digest>
hash_winpe_offline_ntfs_source_analysis_v1(
    const windowsshrink::ShrinkSourceAnalysis& analysis) {
  constexpr std::string_view kDomain =
      "YTEC-WINPE-OFFLINE-NTFS-SOURCE-ANALYSIS-V1";
  std::vector<std::byte> bytes;
  bytes.reserve(
      512U + analysis.partition_snapshot.size() +
      analysis.partitions.size() * 160U +
      analysis.content_volumes.size() * 96U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_wstring(bytes, analysis.source.model);
  append_u64(bytes, analysis.source.size_bytes);
  append_u32(bytes, analysis.source.logical_sector_size);
  append_string(bytes, analysis.source.serial_suffix);
  append_wstring(bytes, analysis.source.device_instance_id);
  append_u32(bytes, analysis.physical_sector_size);
  append_u8(bytes, static_cast<std::uint8_t>(analysis.partition_style));
  append_u8(bytes, analysis.windows_version.has_value() ? 1U : 0U);
  if (analysis.windows_version.has_value()) {
    append_u32(bytes, analysis.windows_version->major);
    append_u32(bytes, analysis.windows_version->minor);
    append_u32(bytes, analysis.windows_version->build);
    append_string(bytes, analysis.windows_version->architecture);
  }
  append_u8(bytes, analysis.bitlocker_fully_decrypted ? 1U : 0U);
  append_u32(
      bytes, static_cast<std::uint32_t>(analysis.partition_snapshot.size()));
  bytes.insert(
      bytes.end(),
      analysis.partition_snapshot.begin(),
      analysis.partition_snapshot.end());
  auto partitions = analysis.partitions;
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const auto& left, const auto& right) {
        return left.source_table_index < right.source_table_index;
      });
  append_u32(bytes, static_cast<std::uint32_t>(partitions.size()));
  for (const auto& partition : partitions) {
    append_u32(bytes, partition.source_table_index);
    append_u8(bytes, static_cast<std::uint8_t>(partition.role));
    append_u8(bytes, static_cast<std::uint8_t>(partition.file_system));
    append_u64(bytes, partition.source_offset_bytes);
    append_u64(bytes, partition.source_size_bytes);
    append_u64(bytes, partition.used_bytes);
    append_u64(bytes, partition.cluster_size);
    append_u8(bytes, partition.active ? 1U : 0U);
    append_wstring(bytes, partition.label);
    append_wstring(bytes, partition.name);
    append_array(bytes, partition.type_id);
    append_array(bytes, partition.unique_id);
  }
  auto volumes = analysis.content_volumes;
  std::sort(
      volumes.begin(),
      volumes.end(),
      [](const auto& left, const auto& right) {
        return left.source_table_index < right.source_table_index;
      });
  append_u32(bytes, static_cast<std::uint32_t>(volumes.size()));
  for (const auto& volume : volumes) {
    append_u32(bytes, volume.source_table_index);
    append_wstring(bytes, volume.volume_guid_path);
  }
  return imageformat::sha256(bytes);
}

clonecore::Result<WinPeOfflineNtfsPartitionInspection>
inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis) {
  auto valid = validate_product_inputs(request, analysis);
  if (!valid) {
    return clonecore::Result<WinPeOfflineNtfsPartitionInspection>::failure(
        valid.error());
  }
  auto source = diskmodel::make_stable_disk_identity(
      request.reviewed_source, false);
  auto target = diskmodel::make_stable_disk_identity(
      request.reviewed_target, false);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_target);
  auto epoch = make_source_epoch(request, analysis, {});
  if (!source || !target || !target_layout || !epoch) {
    return clonecore::Result<WinPeOfflineNtfsPartitionInspection>::failure(
        !source
            ? source.error()
            : !target
                ? target.error()
                : !target_layout ? target_layout.error() : epoch.error());
  }
  WinPeOfflineNtfsPartitionInspection inspection;
  inspection.binding = WinPeOfflineNtfsPartitionReviewBinding{
      .source = source.take_value(),
      .target = target.take_value(),
      .source_layout_hash = epoch.value().source_layout_hash,
      .target_layout_hash = target_layout.take_value(),
      .source_partition_snapshot_hash =
          epoch.value().source_partition_snapshot_hash,
      .source_analysis_hash = epoch.value().source_analysis_hash,
      .source_epoch_hash = epoch.value().canonical_epoch_hash,
      .source_contains_windows = analysis.windows_version.has_value(),
  };
  auto binding_hash = review_binding_hash(inspection.binding);
  if (!binding_hash) {
    return clonecore::Result<WinPeOfflineNtfsPartitionInspection>::failure(
        binding_hash.error());
  }
  inspection.binding.binding_hash = binding_hash.take_value();
  inspection.candidates.reserve(analysis.partitions.size());
  for (const auto& partition : analysis.partitions) {
    inspection.candidates.push_back(WinPeOfflineNtfsPartitionCandidate{
        .source_table_index = partition.source_table_index,
        .role = partition.role,
        .file_system = partition.role ==
                migrationcore::MigrationPartitionRole::efi_system
            ? migrationcore::MigrationFileSystem::fat32
            : partition.file_system,
        .source_size_bytes = partition.source_size_bytes,
        .used_bytes = partition.used_bytes,
        .cluster_size = partition.cluster_size,
        .label = partition.label,
        .selected_by_default = true,
        .required = required_for_offline_windows(
            inspection.binding.source_contains_windows, partition.role),
    });
  }
  return clonecore::Result<WinPeOfflineNtfsPartitionInspection>::success(
      std::move(inspection));
}

clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>
build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis,
    const WinPeOfflineNtfsPartitionReviewBinding& completed_review) {
  auto current = inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
      request, analysis);
  if (!current) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        current.error());
  }
  if (!review_bindings_equal(completed_review, current.value().binding)) {
    return failure<WinPeOfflineNtfsDirectShrinkPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"WinPE offline partition review再照合",
        L"UI review後にsource/target identity、layout、raw snapshot、analysis、またはsource epochが変化しました");
  }
  return build_target_plan(request, analysis, current.value());
}

clonecore::Result<WinPeOfflineNtfsPartitionInspection>
inspect_winpe_offline_ntfs_direct_shrink_with_windows_apis(
    const WinPeOfflineNtfsProductPlanningRequest& request) {
  auto observed = observe_product_with_windows_apis(request);
  if (!observed) {
    return clonecore::Result<WinPeOfflineNtfsPartitionInspection>::failure(
        observed.error());
  }
  return inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
      observed.value().request, observed.value().analysis);
}

clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>
plan_winpe_offline_ntfs_direct_shrink_after_review_with_windows_apis(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const WinPeOfflineNtfsPartitionReviewBinding& completed_review) {
  auto observed = observe_product_with_windows_apis(request);
  if (!observed) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        observed.error());
  }
  return build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
      observed.value().request,
      observed.value().analysis,
      completed_review);
}

WinPeOfflineNtfsDirectShrinkDependencies
make_winpe_offline_ntfs_direct_shrink_dependencies_with_windows_apis(
    const WinPeOfflineNtfsDirectShrinkExecutionOptions& options) {
  return WinPeOfflineNtfsDirectShrinkDependencies{
      .reidentify_selection =
          [](const clonecore::StableDiskIdentity& expected_source,
             const clonecore::StableDiskIdentity& expected_target) {
            auto inventory =
                diskmodel::make_windows_disk_inventory_provider();
            return diskmodel::reidentify_physical_clone_selection(
                expected_source,
                expected_target,
                *inventory,
                false);
          },
      .reidentify_confirmed =
          [](const clonecore::StableDiskIdentity& expected_source,
             const clonecore::StableDiskIdentity& expected_target,
             const clonecore::TargetConfirmation& confirmation) {
            auto inventory =
                diskmodel::make_windows_disk_inventory_provider();
            return diskmodel::reidentify_physical_clone(
                expected_source,
                expected_target,
                confirmation,
                *inventory,
                false);
          },
      .make_source_guard =
          [](const WinPeOfflineNtfsDirectShrinkPlan&,
             const diskmodel::ReidentifiedPhysicalClone&) {
            std::unique_ptr<IWinPeOfflineNtfsSourceGuard> guard =
                std::make_unique<WindowsWinPeOfflineNtfsSourceGuard>();
            return clonecore::Result<std::unique_ptr<
                IWinPeOfflineNtfsSourceGuard>>::success(std::move(guard));
          },
      .make_target_platform =
          [options](const WinPeOfflineNtfsDirectShrinkPlan& plan,
                    const diskmodel::ReidentifiedPhysicalClone& observed) {
            return directshrink::make_windows_target_platform_for_winpe(
                plan.target_plan(),
                observed,
                directshrink::WindowsTargetPlatformRequest{
                    .confirmation = options.confirmation,
                    .callbacks = options.callbacks,
                });
          },
      .open_read_only_raw_source =
          diskmodel::open_verified_read_only_physical_disk_with_windows_apis,
  };
}

}  // namespace ytec::winpeapp

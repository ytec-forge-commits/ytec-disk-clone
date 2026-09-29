#include "ytec/windowsapp/online_direct_shrink_clone.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/diskmodel/connection_identity.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/windowsapp/windows_direct_shrink_clone_platform.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

static_assert(kWindowsDirectShrinkCheckpointOffsetBytes % 512U == 0U);
static_assert(kWindowsDirectShrinkCheckpointRecordBytes % 512U == 0U);
static_assert(
    kWindowsDirectShrinkCheckpointOffsetBytes +
            kWindowsDirectShrinkCheckpointRecordBytes <=
        kWindowsDirectShrinkStagingAlignmentBytes);

clonecore::Error shrink_clone_error(
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
  return clonecore::Result<T>::failure(shrink_clone_error(
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
  return clonecore::Status::failure(shrink_clone_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
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

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
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

bool is_volume_guid_path(const std::wstring_view path) noexcept {
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

void append_string(
    std::vector<std::byte>& bytes,
    const std::string_view value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

clonecore::Result<imageformat::Sha256Digest> hash_mbr_signature_inventory(
    std::vector<std::uint32_t> signatures) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-MBR-SIGNATURE-INVENTORY-V1";
  std::sort(signatures.begin(), signatures.end());
  signatures.erase(
      std::unique(signatures.begin(), signatures.end()), signatures.end());
  std::vector<std::byte> bytes;
  bytes.reserve(8U + kDomain.size() + signatures.size() * sizeof(std::uint32_t));
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_u32(bytes, static_cast<std::uint32_t>(signatures.size()));
  for (const auto signature : signatures) {
    append_u32(bytes, signature);
  }
  return imageformat::sha256(bytes);
}

bool is_mbr_preserving_plan(
    const WindowsDirectShrinkClonePlan& plan) noexcept {
  return plan.partition_style_choice() ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      plan.source_partition_style() ==
          migrationcore::MigrationPartitionStyle::mbr &&
      plan.partition_style() == migrationcore::MigrationPartitionStyle::mbr;
}

clonecore::Status validate_mbr_safety_evidence(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkMbrSafetyEvidence& evidence,
    const bool target_signature_must_still_be_fresh,
    const std::wstring_view operation) {
  if (!is_mbr_preserving_plan(plan)) {
    return clonecore::success_status();
  }
  const auto& binding = plan.mbr_preserve_binding();
  if (!binding.has_value() ||
      evidence.source_sector0_hash != binding->source_sector0_hash ||
      evidence.source_bootstrap != binding->source_bootstrap ||
      evidence.source_disk_signature != binding->source_disk_signature ||
      binding->target_disk_signature == 0U ||
      std::find(
          evidence.connected_mbr_signatures_excluding_target.begin(),
          evidence.connected_mbr_signatures_excluding_target.end(),
          binding->target_disk_signature) !=
          evidence.connected_mbr_signatures_excluding_target.end() ||
      (target_signature_must_still_be_fresh &&
       evidence.target_mbr_signature == binding->target_disk_signature)) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        std::wstring(operation),
        L"コピー元raw MBR sector0/bootstrapまたは計画済みコピー先MBR署名の非衝突性が不変計画と一致しません");
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::Sha256Digest> hash_final_layout(
    const migrationcore::DirectClonePlan& direct_plan,
    const WindowsDirectShrinkTargetOwnedStagingPlan& staging,
    std::span<const WindowsDirectShrinkPartitionTask> tasks,
    const std::optional<WindowsDirectShrinkMbrPlanBinding>& mbr_binding) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-FINAL-LAYOUT-V5";
  std::vector<std::byte> bytes;
  bytes.reserve(128U + tasks.size() * 96U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_u8(bytes, static_cast<std::uint8_t>(direct_plan.source_style()));
  append_u8(bytes, static_cast<std::uint8_t>(direct_plan.target_style()));
  append_u64(bytes, direct_plan.target_size_bytes());
  append_u32(bytes, direct_plan.target_logical_sector_size());
  append_u8(
      bytes, static_cast<std::uint8_t>(direct_plan.surplus_allocation()));
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
  append_u8(bytes, mbr_binding.has_value() ? 1U : 0U);
  if (mbr_binding.has_value()) {
    append_array(bytes, mbr_binding->source_sector0_hash);
    append_array(bytes, mbr_binding->source_bootstrap);
    append_u32(bytes, mbr_binding->source_disk_signature);
    append_u32(bytes, mbr_binding->target_disk_signature);
  }
  append_u32(bytes, static_cast<std::uint32_t>(tasks.size()));
  for (const auto& task : tasks) {
    append_u8(bytes, static_cast<std::uint8_t>(task.kind));
    append_u32(bytes, task.target_number);
    append_u8(bytes, task.source_table_index ? 1U : 0U);
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

clonecore::Result<operationcore::Sha256Digest> hash_immutable_payload(
    const WindowsDirectShrinkPlanningRequest& request,
    const migrationcore::DirectClonePlan& direct_plan,
    const std::uint64_t checkpoint_offset_bytes,
    const WindowsDirectShrinkTargetOwnedStagingPlan& staging,
    std::span<const WindowsDirectShrinkPartitionTask> tasks,
    std::span<const WindowsDirectShrinkSourcePartitionMapping>
        source_partition_mappings,
    const imageformat::Sha256Digest& final_layout_hash) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-PLAN-V7";
  std::vector<std::byte> bytes;
  bytes.reserve(
      320U + tasks.size() * 160U +
      source_partition_mappings.size() * 24U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_array(bytes, request.expected_source_layout_hash);
  append_array(bytes, request.expected_target_layout_hash);
  append_array(bytes, request.expected_source_partition_snapshot_hash);
  append_u8(bytes, request.mbr_preserve_binding.has_value() ? 1U : 0U);
  if (request.mbr_preserve_binding.has_value()) {
    append_array(bytes, request.mbr_preserve_binding->source_sector0_hash);
    append_array(bytes, request.mbr_preserve_binding->source_bootstrap);
    append_u32(bytes, request.mbr_preserve_binding->source_disk_signature);
    append_u32(bytes, request.mbr_preserve_binding->target_disk_signature);
    append_array(
        bytes,
        request.mbr_preserve_binding->planning_signature_inventory_hash);
  }
  append_array(bytes, final_layout_hash);
  append_u8(bytes, request.bitlocker_fully_decrypted ? 1U : 0U);
  append_u8(bytes, request.target_is_active_rescue_media ? 1U : 0U);
  append_u8(bytes, static_cast<std::uint8_t>(direct_plan.mode()));
  append_u8(
      bytes,
      static_cast<std::uint8_t>(direct_plan.partition_style_choice()));
  append_u8(
      bytes,
      static_cast<std::uint8_t>(direct_plan.surplus_allocation()));
  append_u8(
      bytes,
      direct_plan.surplus_target_source_table_index().has_value() ? 1U : 0U);
  append_u32(
      bytes, direct_plan.surplus_target_source_table_index().value_or(0U));
  append_u64(bytes, checkpoint_offset_bytes);
  append_u64(bytes, kWindowsDirectShrinkCheckpointRecordBytes);
  append_u64(bytes, staging.offset_bytes);
  append_u64(bytes, staging.length_bytes);
  append_u64(bytes, staging.control_reserve_bytes);
  append_u64(bytes, staging.archive_offset_bytes);
  append_u64(bytes, staging.archive_capacity_bytes);
  append_u8(
      bytes, staging.final_growth_owner_target_number.has_value() ? 1U : 0U);
  append_u32(
      bytes, staging.final_growth_owner_target_number.value_or(0U));
  append_u32(bytes, static_cast<std::uint32_t>(tasks.size()));
  for (const auto& task : tasks) {
    append_u8(bytes, static_cast<std::uint8_t>(task.kind));
    append_u32(bytes, task.target_number);
    append_u8(bytes, task.source_table_index ? 1U : 0U);
    append_u32(bytes, task.source_table_index.value_or(0U));
    append_u8(bytes, static_cast<std::uint8_t>(task.role));
    append_u8(bytes, task.active ? 1U : 0U);
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
      bytes, static_cast<std::uint32_t>(source_partition_mappings.size()));
  for (const auto& mapping : source_partition_mappings) {
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

clonecore::Status validate_reviewed_disk_state(
    const diskmodel::DiskInfo& source,
    const diskmodel::DiskInfo& target,
    const bool target_is_active_rescue_media) {
  if (!source.offline.has_value() || !source.read_only.has_value() ||
      !source.removable.has_value() || !target.offline.has_value() ||
      !target.read_only.has_value() || !target.removable.has_value() ||
      source.offline.value() || source.read_only.value() ||
      source.removable.value() ||
      target.read_only.value() || target.removable.value() ||
      target.is_system_disk || source.logical_sector_size != 512U ||
      target.logical_sector_size != 512U) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンのディスク属性",
        L"オンライン固定コピー元、非system・書込み可能固定コピー先、および512-byte論理セクターだけを扱います");
  }
  if (diskmodel::disk_health_operation_advice(source.health, true) !=
          diskmodel::DiskHealthOperationAdvice::proceed ||
      diskmodel::disk_health_operation_advice(target.health, false) ==
          diskmodel::DiskHealthOperationAdvice::block_target) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_DEVICE_HARDWARE_ERROR,
        L"Windows直接縮小クローンの健康状態",
        L"この初期sliceはコピー元の注意・異常またはコピー先の重大健康異常を標準縮小として開始しません");
  }
  const auto source_class = imageformat::
      classify_tsumugi_physical_restore_target(source);
  if (source_class.dynamic_disk || source_class.storage_spaces ||
      source_class.software_raid ||
      source_class.unresolved_hardware_raid ||
      source_class.unsupported_virtual) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンのコピー元安全分類",
        L"Dynamic Disk、Storage Spaces、software/hardware RAID、iSCSI、または仮想ディスクはこのsliceで開始しません");
  }
  const auto target_class = imageformat::
      classify_tsumugi_physical_restore_target(target);
  auto target_status = imageformat::validate_tsumugi_physical_restore_target(
      target, target_class, target_is_active_rescue_media);
  if (!target_status) {
    return target_status;
  }
  const auto source_style = diskmodel::normalize_disk_partition_style(
      source.partition_style, source.partitions.size());
  const auto target_style = diskmodel::normalize_disk_partition_style(
      target.partition_style, target.partitions.size());
  if ((source_style != diskmodel::PartitionStyle::gpt &&
       source_style != diskmodel::PartitionStyle::mbr) ||
      (target_style != diskmodel::PartitionStyle::raw &&
       target_style != diskmodel::PartitionStyle::gpt &&
       target_style != diskmodel::PartitionStyle::mbr)) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンのディスク形式",
        L"解析済みGPT/MBRコピー元とRAW/GPT/MBR基本コピー先だけを扱います");
  }
  return clonecore::success_status();
}

clonecore::Status validate_reviewed_identities_and_layouts(
    const WindowsDirectShrinkPlanningRequest& request) {
  auto source_selection = diskmodel::DiskSelectionIdentity::create(
      request.reviewed_source);
  auto target_selection = diskmodel::DiskSelectionIdentity::create(
      request.reviewed_target);
  if (!source_selection || !target_selection) {
    return clonecore::Status::failure(
        !source_selection ? source_selection.error() :
                            target_selection.error());
  }
  // This execution slice persists only StableDiskIdentity.  Do not silently
  // weaken CLN-010 by accepting a serial-less fixed USB disk whose approval
  // is valid only while a stateful same-connection binding remains latched.
  // A later adapter may carry DiskSelectionIdentity end-to-end; until then,
  // reject before VSS creation and before any target I/O.
  if (source_selection.value().kind() ==
          diskmodel::DiskIdentityBindingKind::same_connection ||
      target_selection.value().kind() ==
          diskmodel::DiskIdentityBindingKind::same_connection) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンのUSB接続識別",
        L"serialなし固定USBはsame-connection複合bindingの状態を保持できる実行adapterが接続されるまで開始しません");
  }
  auto source_identity = diskmodel::make_stable_disk_identity(
      request.reviewed_source, request.reviewed_source.is_system_disk);
  auto target_identity = diskmodel::make_stable_disk_identity(
      request.reviewed_target, false);
  if (!source_identity || !target_identity) {
    return clonecore::Status::failure(
        !source_identity ? source_identity.error() : target_identity.error());
  }
  auto status = clonecore::validate_clone_selection(
      request.expected_source,
      source_identity.value(),
      request.expected_target,
      target_identity.value(),
      false);
  if (!status) {
    return status;
  }
  auto source_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_source);
  auto target_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_target);
  if (!source_layout || !target_layout) {
    return clonecore::Status::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  if (all_zero(request.expected_source_layout_hash) ||
      all_zero(request.expected_target_layout_hash) ||
      source_layout.value() != request.expected_source_layout_hash ||
      target_layout.value() != request.expected_target_layout_hash) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows直接縮小クローンのレビューHash",
        L"画面で確認したコピー元・コピー先レイアウトと不変要求が一致しません");
  }
  return validate_reviewed_disk_state(
      request.reviewed_source,
      request.reviewed_target,
      request.target_is_active_rescue_media);
}

const WindowsDirectShrinkNtfsVolume* find_volume(
    const WindowsDirectShrinkPlanningRequest& request,
    const std::uint32_t source_table_index) noexcept {
  const WindowsDirectShrinkNtfsVolume* result = nullptr;
  for (const auto& volume : request.ntfs_volumes) {
    if (volume.source_table_index != source_table_index) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &volume;
  }
  return result;
}

const WindowsDirectShrinkExactRawPartition* find_exact_raw_partition(
    const WindowsDirectShrinkPlanningRequest& request,
    const std::uint32_t source_table_index) noexcept {
  const WindowsDirectShrinkExactRawPartition* result = nullptr;
  for (const auto& partition : request.exact_raw_partitions) {
    if (partition.source_table_index != source_table_index) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &partition;
  }
  return result;
}

const diskmodel::PartitionInfo* find_reviewed_source_partition(
    const diskmodel::DiskInfo& source,
    const std::uint32_t source_table_index) noexcept {
  const diskmodel::PartitionInfo* result = nullptr;
  for (const auto& partition : source.partitions) {
    if (partition.number != source_table_index) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &partition;
  }
  return result;
}

bool source_partition_style_matches(
    const diskmodel::PartitionStyle reviewed,
    const migrationcore::MigrationPartitionStyle planned) noexcept {
  return (reviewed == diskmodel::PartitionStyle::gpt &&
          planned == migrationcore::MigrationPartitionStyle::gpt) ||
      (reviewed == diskmodel::PartitionStyle::mbr &&
       planned == migrationcore::MigrationPartitionStyle::mbr);
}

bool analyzed_mbr_type_matches(
    const windowsshrink::AnalyzedShrinkPartition& analyzed,
    const diskmodel::PartitionInfo& reviewed,
    const std::uint8_t expected_type) noexcept {
  if (analyzed.type_id[0] != static_cast<std::byte>(expected_type) ||
      !std::all_of(
          analyzed.type_id.begin() + 1,
          analyzed.type_id.end(),
          [](const std::byte value) { return value == std::byte{0}; })) {
    return false;
  }
  const std::wstring_view expected_name = expected_type == 0x07U
      ? std::wstring_view(L"0x07")
      : std::wstring_view(L"0x27");
  return equal_path(reviewed.type, expected_name) &&
      reviewed.bootable == analyzed.active;
}

bool already_contains_volume_path(
    const vssrequester::WorkflowRequest& workflow,
    const std::wstring_view candidate) noexcept {
  return std::any_of(
      workflow.volumes.begin(),
      workflow.volumes.end(),
      [candidate](const vssrequester::VolumeRequest& volume) {
        return equal_path(volume.volume_guid_path, candidate);
      });
}

clonecore::Status validate_observed_clone(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const clonecore::TargetConfirmation* confirmation) {
  auto status = confirmation != nullptr
      ? clonecore::validate_clone_identities(
            plan.expected_source(),
            observed.source_identity,
            plan.expected_target(),
            observed.target_identity,
            *confirmation,
            false)
      : clonecore::validate_clone_selection(
            plan.expected_source(),
            observed.source_identity,
            plan.expected_target(),
            observed.target_identity,
            false);
  if (!status) {
    return status;
  }
  status = validate_reviewed_disk_state(
      observed.source,
      observed.target,
      plan.target_is_active_rescue_media());
  if (!status) {
    return status;
  }
  auto source_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(observed.source);
  auto target_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(observed.target);
  if (!source_layout || !target_layout) {
    return clonecore::Status::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  if (source_layout.value() != plan.expected_source_layout_hash() ||
      target_layout.value() != plan.expected_target_layout_hash()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        confirmation != nullptr
            ? L"Windows直接縮小クローンの書込み直前layout"
            : L"Windows直接縮小クローンの確認前layout",
        L"最終確認後にコピー元またはコピー先のレイアウトが変化しました");
  }
  return clonecore::success_status();
}

clonecore::Status validate_observed_source_after_snapshot_cleanup(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed) {
  auto status = clonecore::validate_clone_selection(
      plan.expected_source(),
      observed.source_identity,
      plan.expected_target(),
      observed.target_identity,
      false);
  if (!status) {
    return status;
  }
  if (!observed.source.offline.has_value() ||
      !observed.source.read_only.has_value() ||
      !observed.source.removable.has_value() ||
      observed.source.offline.value() ||
      observed.source.read_only.value() ||
      observed.source.removable.value() ||
      observed.source.is_system_disk !=
          plan.expected_source().is_system_disk ||
      observed.source.logical_sector_size !=
          plan.expected_source().logical_sector_size ||
      observed.source.size_bytes != plan.expected_source().size_bytes ||
      !source_partition_style_matches(
          diskmodel::normalize_disk_partition_style(
              observed.source.partition_style,
              observed.source.partitions.size()),
          plan.source_partition_style())) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows直接縮小クローンのSnapshot削除後コピー元",
        L"コピー元の接続、属性、寸法、またはpartition styleが計画時から変化しました");
  }
  auto source_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(observed.source);
  if (!source_layout ||
      source_layout.value() != plan.expected_source_layout_hash()) {
    return source_layout
        ? status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"Windows直接縮小クローンのSnapshot削除後コピー元layout",
              L"VSS cleanup後にコピー元partition layoutの不変性を確認できません")
        : clonecore::Status::failure(source_layout.error());
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::Sha256Digest> combine_digest(
    const imageformat::Sha256Digest& current,
    const imageformat::Sha256Digest& next,
    const std::uint32_t target_number,
    const std::uint64_t verified_bytes) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-WRITE-DIGEST-V1";
  std::vector<std::byte> bytes;
  bytes.reserve(4U + kDomain.size() + current.size() + next.size() + 12U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_array(bytes, current);
  append_array(bytes, next);
  append_u32(bytes, target_number);
  append_u64(bytes, verified_bytes);
  return imageformat::sha256(bytes);
}

clonecore::Status validate_checkpoint_common(
    const WindowsDirectShrinkClonePlan& plan,
    const operationcore::Sha256Digest& operation_plan_hash,
    const WindowsDirectShrinkCheckpointEvidence& checkpoint) {
  const auto target = clonecore::validate_stable_identity(
      plan.expected_target(),
      checkpoint.observed_target,
      L"Windows直接縮小checkpointコピー先");
  if (!target) {
    return target;
  }
  if (checkpoint.revision == 0U ||
      checkpoint.plan_hash != operation_plan_hash ||
      all_zero(checkpoint.staging_identity_hash) ||
      all_zero(checkpoint.record_hash) || !checkpoint.durable ||
      !checkpoint.flushed || !checkpoint.read_back_verified ||
      !checkpoint.target_offline || checkpoint.final_layout_committed) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Windows直接縮小checkpoint耐久性",
        L"計画Hash、対象識別、flush、読戻し、offline、または未完成状態の証跡が不足しています");
  }
  return clonecore::success_status();
}

clonecore::Status validate_checkpoint_transition(
    const WindowsDirectShrinkClonePlan& plan,
    const operationcore::Sha256Digest& operation_plan_hash,
    const WindowsDirectShrinkCheckpointEvidence& previous,
    const WindowsDirectShrinkCheckpointEvidence& current,
    const WindowsDirectShrinkCheckpointPhase expected_phase,
    const std::uint64_t expected_task_count,
    const std::uint64_t expected_verified_bytes,
    const imageformat::Sha256Digest& expected_write_digest,
    const bool allow_same_revision) {
  auto status = validate_checkpoint_common(
      plan, operation_plan_hash, current);
  if (!status) {
    return status;
  }
  const bool revision_valid = allow_same_revision
      ? current.revision == previous.revision
      : current.revision > previous.revision;
  if (!revision_valid || current.phase != expected_phase ||
      current.staging_identity_hash != previous.staging_identity_hash ||
      current.completed_task_count != expected_task_count ||
      current.verified_target_bytes != expected_verified_bytes ||
      current.aggregate_write_digest != expected_write_digest ||
      (!allow_same_revision && current.record_hash == previous.record_hash) ||
      (allow_same_revision && current.record_hash != previous.record_hash)) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows直接縮小checkpoint遷移",
        L"revision、phase、staging、進捗、または記録Hashが期待した耐久遷移と一致しません");
  }
  return clonecore::success_status();
}

bool stable_identity_exactly_matches(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  return left.disk_number == right.disk_number &&
      left.model == right.model && left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.serial_suffix == right.serial_suffix &&
      left.device_instance_id == right.device_instance_id &&
      left.is_system_disk == right.is_system_disk;
}

clonecore::Status validate_checkpoint_exactly_unchanged(
    const WindowsDirectShrinkClonePlan& plan,
    const operationcore::Sha256Digest& operation_plan_hash,
    const WindowsDirectShrinkCheckpointEvidence& expected,
    const WindowsDirectShrinkCheckpointEvidence& observed,
    const std::wstring_view operation) {
  auto status = validate_checkpoint_common(
      plan, operation_plan_hash, observed);
  if (!status) {
    return status;
  }
  if (observed.phase != expected.phase ||
      observed.revision != expected.revision ||
      observed.plan_hash != expected.plan_hash ||
      observed.staging_identity_hash != expected.staging_identity_hash ||
      observed.record_hash != expected.record_hash ||
      observed.aggregate_write_digest != expected.aggregate_write_digest ||
      !stable_identity_exactly_matches(
          observed.observed_target, expected.observed_target) ||
      observed.completed_task_count != expected.completed_task_count ||
      observed.verified_target_bytes != expected.verified_target_bytes ||
      observed.durable != expected.durable ||
      observed.flushed != expected.flushed ||
      observed.read_back_verified != expected.read_back_verified ||
      observed.target_offline != expected.target_offline ||
      observed.final_layout_committed != expected.final_layout_committed) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        std::wstring(operation),
        L"commit-ready checkpointのbyte/hash/revisionまたは耐久証跡が不変入力と完全一致しません");
  }
  return clonecore::success_status();
}

const vssrequester::SnapshotMapping* find_snapshot_mapping(
    const vssrequester::SnapshotCopyContext& context,
    const std::wstring_view volume_path) noexcept {
  const vssrequester::SnapshotMapping* result = nullptr;
  for (const auto& mapping : context.mappings) {
    if (!equal_path(mapping.original_volume_guid_path, volume_path)) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &mapping;
  }
  return result;
}

clonecore::Status validate_snapshot_context(
    const WindowsDirectShrinkClonePlan& plan,
    const vssrequester::SnapshotCopyContext& context) {
  if (context.snapshot_set_id.empty() ||
      context.mappings.size() != plan.archive_task_count()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows直接縮小Snapshot集合",
        L"Snapshot Set識別またはVolume件数が不変計画と一致しません");
  }
  for (std::size_t left = 0U; left < context.mappings.size(); ++left) {
    const auto& mapping = context.mappings[left];
    if (mapping.original_volume_guid_path.empty() ||
        mapping.snapshot_id.empty() || mapping.snapshot_device_path.empty()) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Windows直接縮小Snapshot識別",
          L"Snapshot mappingの元Volume、Snapshot ID、またはdevice pathが空です");
    }
    for (std::size_t right = left + 1U;
         right < context.mappings.size();
         ++right) {
      const auto& other = context.mappings[right];
      if (equal_path(
              mapping.original_volume_guid_path,
              other.original_volume_guid_path) ||
          equal_path(mapping.snapshot_id, other.snapshot_id) ||
          equal_path(
              mapping.snapshot_device_path,
              other.snapshot_device_path)) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"Windows直接縮小Snapshot一意性",
            L"異なる選択Volumeが同じ元Volume、Snapshot ID、またはdevice pathへ重複対応しています");
      }
    }
  }
  for (const auto& task : plan.tasks()) {
    if (task.kind != WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
      continue;
    }
    const auto* mapping = find_snapshot_mapping(
        context, task.original_volume_guid_path);
    if (mapping == nullptr || mapping->snapshot_id.empty() ||
        mapping->snapshot_device_path.empty()) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Windows直接縮小Snapshot対応",
          L"レビュー済みNTFS Volumeを現在のSnapshot Setへ一意に対応できません");
    }
  }
  return clonecore::success_status();
}

clonecore::Status cancelled_status(const std::wstring_view operation) {
  return status_failure(
      clonecore::ErrorCode::cancelled,
      ERROR_CANCELLED,
      std::wstring(operation),
      L"安全なパーティション境界で取消されました");
}

clonecore::Status check_safe_boundary(
    const WindowsDirectShrinkCloneExecutionOptions& options,
    const std::uint32_t source_table_index,
    const std::uint64_t completed_bytes,
    const std::uint64_t completed_units) {
  if (clonecore::disk_operation_cancellation_requested(options.callbacks) ||
      clonecore::disk_operation_control_at_safe_boundary(
          options.callbacks,
          clonecore::DiskOperationSafeBoundary{
              .kind = clonecore::DiskOperationSafeBoundaryKind::
                  verified_partition,
              .stage = clonecore::DiskOperationStage::copying_data,
              .partition_index = source_table_index,
              .completed_bytes = completed_bytes,
              .completed_units = completed_units,
          }) == clonecore::DiskOperationControlDecision::cancel_operation) {
    return cancelled_status(L"Windows直接縮小クローンの安全境界");
  }
  return clonecore::success_status();
}

void report_progress(
    const WindowsDirectShrinkCloneExecutionOptions& options,
    const clonecore::DiskOperationStage stage,
    const std::optional<std::uint32_t> source_table_index,
    const std::uint64_t total_bytes,
    const std::uint64_t verified_bytes,
    const bool cancellation_allowed,
    const bool pause_allowed) noexcept {
  clonecore::report_disk_operation_progress(
      options.callbacks,
      clonecore::DiskOperationProgress{
          .stage = stage,
          .partition_index = source_table_index,
          .total_read_bytes = total_bytes,
          .total_write_bytes = total_bytes,
          .total_verify_bytes = total_bytes,
          .read_bytes = verified_bytes,
          .written_bytes = verified_bytes,
          .verified_bytes = verified_bytes,
          .cancellation_allowed = cancellation_allowed,
          .pause_allowed = pause_allowed,
      });
}

clonecore::Result<imageformat::Sha256Digest> execution_evidence_hash(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkCloneExecutionReport& report) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-EVIDENCE-V7";
  std::vector<std::byte> bytes;
  bytes.reserve(256U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_array(bytes, plan.operation_plan().immutable_payload_hash);
  append_array(bytes, plan.final_layout_hash());
  append_wstring(bytes, report.workflow.snapshot_set_id);
  append_u64(bytes, report.applied_archive_count);
  append_u64(bytes, report.copied_exact_raw_count);
  append_u64(bytes, report.verified_target_bytes);
  append_array(bytes, report.aggregate_write_digest);
  append_array(bytes, report.commit_ready_checkpoint.record_hash);
  append_array(bytes, report.final_commit.committed_layout_hash);
  append_array(bytes, report.final_commit.aggregate_write_digest);
  append_u8(bytes, static_cast<std::uint8_t>(
      report.final_commit.final_partition_style));
  append_u8(bytes, report.boot.target_only_reconstruction ? 1U : 0U);
  append_u8(bytes, report.boot.exact_target_volume_extents ? 1U : 0U);
  append_u8(bytes, report.boot.legacy_bios ? 1U : 0U);
  append_u8(bytes, report.boot.real_boot_not_claimed ? 1U : 0U);
  append_u8(bytes, report.final_commit.checkpoint_retired ? 1U : 0U);
  append_u8(
      bytes,
      report.final_commit.checkpoint_retirement_pending ? 1U : 0U);
  append_u8(bytes, report.final_commit.source_reidentified ? 1U : 0U);
  append_u8(bytes, report.final_commit.source_layout_unchanged ? 1U : 0U);
  append_u8(
      bytes,
      report.final_commit.construction_layout_non_bootable ? 1U : 0U);
  append_u8(
      bytes,
      report.final_commit.checkpoint_retained_through_extensions_and_boot
          ? 1U
          : 0U);
  append_u8(
      bytes,
      report.final_commit.boot_completed_before_final_layout_publication
          ? 1U
          : 0U);
  append_u8(
      bytes,
      report.final_commit.final_layout_published_before_checkpoint_retirement
          ? 1U
          : 0U);
  append_u8(
      bytes,
      report.final_commit.hidden_final_layout_published_and_read_back
          ? 1U
          : 0U);
  append_u64(bytes, report.final_commit.extended_ntfs_partition_count);
  append_u8(
      bytes,
      report.final_commit.every_required_ntfs_extension_verified ? 1U : 0U);
  append_u8(
      bytes,
      report.final_commit.targeted_surplus_source_table_index.has_value()
          ? 1U
          : 0U);
  append_u32(
      bytes,
      report.final_commit.targeted_surplus_source_table_index.value_or(0U));
  append_u8(
      bytes,
      report.final_commit.targeted_surplus_target_number.has_value()
          ? 1U
          : 0U);
  append_u32(
      bytes,
      report.final_commit.targeted_surplus_target_number.value_or(0U));
  append_u64(
      bytes,
      report.final_commit.targeted_surplus_previous_file_system_bytes);
  append_u64(
      bytes,
      report.final_commit.targeted_surplus_final_file_system_bytes);
  append_u8(
      bytes,
      report.final_commit.targeted_surplus_owner_verified ? 1U : 0U);
  append_u8(
      bytes,
      report.final_commit.targeted_surplus_exact_size_verified ? 1U : 0U);
  append_u8(
      bytes,
      report.final_commit.targeted_surplus_readback_verified ? 1U : 0U);
  append_u8(
      bytes,
      report.every_payload_captured_and_applied_inside_snapshot_callback
          ? 1U
          : 0U);
  append_u8(
      bytes,
      report.snapshots_deleted_before_final_layout_commit ? 1U : 0U);
  append_u8(bytes, report.target_left_offline ? 1U : 0U);
  append_u8(
      bytes, report.final_commit.source_mbr_sector0_unchanged ? 1U : 0U);
  append_u8(
      bytes, report.final_commit.source_mbr_bootstrap_unchanged ? 1U : 0U);
  append_u8(
      bytes,
      report.final_commit.target_mbr_signature_collision_free ? 1U : 0U);
  append_u8(
      bytes,
      report.final_commit.final_mbr_sector0_read_back_verified ? 1U : 0U);
  append_u32(bytes, report.final_commit.final_mbr_disk_signature);
  append_u32(bytes, report.final_commit.final_mbr_active_partition_count);
  return imageformat::sha256(bytes);
}

bool has_valid_targeted_surplus_evidence(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkFinalCommitEvidence& evidence) noexcept {
  const bool targeted = plan.surplus_allocation() ==
      migrationcore::ShrinkSurplusAllocation::selected_data_partition;
  if (!targeted) {
    return !evidence.targeted_surplus_source_table_index.has_value() &&
        !evidence.targeted_surplus_target_number.has_value() &&
        evidence.targeted_surplus_previous_file_system_bytes == 0U &&
        evidence.targeted_surplus_final_file_system_bytes == 0U &&
        !evidence.targeted_surplus_owner_verified &&
        !evidence.targeted_surplus_exact_size_verified &&
        !evidence.targeted_surplus_readback_verified;
  }
  if (!plan.surplus_target_source_table_index().has_value() ||
      !plan.staging().final_growth_owner_target_number.has_value()) {
    return false;
  }
  const auto task = std::find_if(
      plan.tasks().begin(),
      plan.tasks().end(),
      [&plan](const WindowsDirectShrinkPartitionTask& value) {
        return value.source_table_index ==
                plan.surplus_target_source_table_index() &&
            value.target_number ==
                *plan.staging().final_growth_owner_target_number;
      });
  return task != plan.tasks().end() &&
      task->role == migrationcore::MigrationPartitionRole::data &&
      task->construction_size_bytes < task->target_size_bytes &&
      evidence.targeted_surplus_source_table_index ==
          plan.surplus_target_source_table_index() &&
      evidence.targeted_surplus_target_number ==
          plan.staging().final_growth_owner_target_number &&
      evidence.targeted_surplus_previous_file_system_bytes ==
          task->construction_size_bytes &&
      evidence.targeted_surplus_final_file_system_bytes ==
          task->target_size_bytes &&
      evidence.targeted_surplus_owner_verified &&
      evidence.targeted_surplus_exact_size_verified &&
      evidence.targeted_surplus_readback_verified;
}

std::uint64_t exact_raw_task_count(
    const WindowsDirectShrinkClonePlan& plan) noexcept {
  return static_cast<std::uint64_t>(std::count_if(
      plan.tasks().begin(),
      plan.tasks().end(),
      [](const WindowsDirectShrinkPartitionTask& task) {
        return task.kind ==
            WindowsDirectShrinkPartitionTaskKind::copy_exact_raw;
      }));
}

bool has_valid_partition_style_commit_evidence(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkFinalCommitEvidence& evidence) noexcept {
  if (evidence.final_partition_style != plan.partition_style()) {
    return false;
  }
  if (!is_mbr_preserving_plan(plan)) {
    return !evidence.source_mbr_sector0_unchanged &&
        !evidence.source_mbr_bootstrap_unchanged &&
        !evidence.target_mbr_signature_collision_free &&
        !evidence.final_mbr_sector0_read_back_verified &&
        evidence.final_mbr_disk_signature == 0U &&
        evidence.final_mbr_active_partition_count == 0U;
  }
  return plan.mbr_preserve_binding().has_value() &&
      evidence.source_mbr_sector0_unchanged &&
      evidence.source_mbr_bootstrap_unchanged &&
      evidence.target_mbr_signature_collision_free &&
      evidence.final_mbr_sector0_read_back_verified &&
      evidence.final_mbr_disk_signature ==
          plan.mbr_preserve_binding()->target_disk_signature &&
      evidence.final_mbr_active_partition_count ==
          (plan.boot_finalization_required() ? 1U : 0U);
}

clonecore::Status validate_execution_report(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkCloneExecutionReport& report) {
  if (report.workflow.snapshot_set_id.empty() ||
      report.workflow.volume_count != plan.archive_task_count() ||
      !report.workflow.snapshot_data_copied ||
      !report.workflow.backup_completed ||
      !report.workflow.snapshots_deleted ||
      report.applied_archive_count != plan.archive_task_count() ||
      report.copied_exact_raw_count != exact_raw_task_count(plan) ||
      report.verified_target_bytes == 0U ||
      report.verified_target_bytes > plan.operation_plan().expected_work_bytes ||
      all_zero(report.aggregate_write_digest) ||
      report.commit_ready_checkpoint.phase !=
          WindowsDirectShrinkCheckpointPhase::commit_ready ||
      report.commit_ready_checkpoint.completed_task_count !=
          plan.tasks().size() ||
      report.commit_ready_checkpoint.verified_target_bytes !=
          report.verified_target_bytes ||
      report.commit_ready_checkpoint.aggregate_write_digest !=
          report.aggregate_write_digest ||
      report.boot.required != plan.boot_finalization_required() ||
      !report.boot.completed || !report.boot.boot_files_read_back_verified ||
      !report.boot.recovery_configuration_verified ||
      !report.boot.target_offline ||
      !report.boot.target_only_reconstruction ||
      !report.boot.exact_target_volume_extents ||
      report.boot.legacy_bios !=
          (is_mbr_preserving_plan(plan) &&
           plan.boot_finalization_required()) ||
      !report.boot.real_boot_not_claimed ||
      report.final_commit.committed_layout_hash != plan.final_layout_hash() ||
      report.final_commit.aggregate_write_digest !=
          report.aggregate_write_digest ||
      !report.final_commit.source_reidentified ||
      !report.final_commit.source_layout_unchanged ||
      !report.final_commit.target_reidentified ||
      !report.final_commit.staging_identity_reverified ||
      !report.final_commit.checkpoint_reverified ||
      !report.final_commit.staging_removed ||
      (report.final_commit.checkpoint_retired ==
       report.final_commit.checkpoint_retirement_pending) ||
      !report.final_commit.construction_layout_non_bootable ||
      !report.final_commit.checkpoint_retained_through_extensions_and_boot ||
      !report.final_commit.boot_completed_before_final_layout_publication ||
      !report.final_commit.final_layout_published_before_checkpoint_retirement ||
      !report.final_commit.hidden_final_layout_published_and_read_back ||
      report.final_commit.extended_ntfs_partition_count !=
          plan.ntfs_extension_task_count() ||
      !report.final_commit.every_required_ntfs_extension_verified ||
      !has_valid_targeted_surplus_evidence(plan, report.final_commit) ||
      !report.final_commit.every_write_flushed ||
      !report.final_commit.every_write_read_back ||
      !report.final_commit.primary_layout_committed_last ||
      !report.final_commit.target_offline ||
      !has_valid_partition_style_commit_evidence(plan, report.final_commit) ||
      !has_valid_windows_direct_shrink_precomputed_completion_evidence(
          report) ||
      !report.every_payload_captured_and_applied_inside_snapshot_callback ||
      !report.snapshots_deleted_before_final_layout_commit ||
      !report.target_left_offline) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Windows直接縮小クローンの最終証跡",
        L"VSS、WIM、checkpoint保持、非boot construction、コピー元不変、事前計算済み完成Hash、全書込み読戻し、最終layout公開順序、またはoffline証跡が不足しています");
  }
  return clonecore::success_status();
}

clonecore::Result<WindowsDirectShrinkCloneExecutionReport> run_execution(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkCloneExecutionOptions& options,
    const WindowsDirectShrinkCloneDependencies& dependencies) {
  auto operation_plan_hash = operationcore::hash_operation_plan(
      plan.operation_plan());
  if (!operation_plan_hash) {
    return clonecore::Result<WindowsDirectShrinkCloneExecutionReport>::failure(
        operation_plan_hash.error());
  }

  bool callback_called = false;
  bool callback_completed = false;
  std::optional<std::wstring> callback_snapshot_set_id;
  std::unique_ptr<IWindowsDirectShrinkClonePlatform> platform;
  std::optional<WindowsDirectShrinkCheckpointEvidence> checkpoint;
  std::optional<WindowsDirectShrinkBootEvidence> boot;
  imageformat::Sha256Digest aggregate_digest{};
  std::uint64_t verified_bytes{};
  std::uint64_t completed_tasks{};
  std::uint64_t applied_archives{};
  std::uint64_t copied_exact_raw{};
  bool final_layout_publication_latched = false;

  // Platform construction is a pure preflight contract.  Build it before
  // VSS so a production adapter can reject every unsupported style, role,
  // capacity, and staging invariant without creating a Snapshot Set.  The
  // callback performs another full confirmed reidentification immediately
  // before begin_target_owned_staging() performs the first target I/O.
  auto preflight_observed = dependencies.reidentify_confirmed(
      plan.expected_source(),
      plan.expected_target(),
      options.confirmation);
  if (!preflight_observed) {
    return clonecore::Result<WindowsDirectShrinkCloneExecutionReport>::failure(
        preflight_observed.error());
  }
  auto preflight_status = validate_observed_clone(
      plan, preflight_observed.value(), &options.confirmation);
  if (preflight_status && is_mbr_preserving_plan(plan)) {
    if (!dependencies.observe_mbr_safety) {
      preflight_status = status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_HANDLE,
          L"Windows直接縮小MBR実行直前observer",
          L"source sector0と全接続MBR署名の読取り専用observerがありません");
    } else {
      auto mbr = dependencies.observe_mbr_safety(
          plan.expected_source(), plan.expected_target(), true);
      preflight_status = mbr
          ? validate_mbr_safety_evidence(
                plan, mbr.value(), true, L"Windows直接縮小MBR実行直前再照合")
          : clonecore::Status::failure(mbr.error());
    }
  }
  if (!preflight_status) {
    return clonecore::Result<WindowsDirectShrinkCloneExecutionReport>::failure(
        preflight_status.error());
  }
  auto made_platform = dependencies.make_platform(
      plan, preflight_observed.value());
  if (!made_platform || !made_platform.value()) {
    return clonecore::Result<WindowsDirectShrinkCloneExecutionReport>::failure(
        made_platform
            ? shrink_clone_error(
                  clonecore::ErrorCode::internal_error,
                  ERROR_INVALID_HANDLE,
                  L"Windows直接縮小target platform preflight",
                  L"対象専用platformがありません")
            : made_platform.error());
  }
  platform = made_platform.take_value();

  report_progress(
      options,
      clonecore::DiskOperationStage::verifying_source,
      std::nullopt,
      plan.operation_plan().expected_work_bytes,
      0U,
      true,
      false);

  const auto abort_incomplete = [&]() noexcept {
    if (platform) {
      platform->abort_keep_offline_incomplete();
    }
  };

  clonecore::Result<vssrequester::WorkflowReport> workflow =
      clonecore::Result<vssrequester::WorkflowReport>::failure(
          shrink_clone_error(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_STATE,
              L"Windows直接縮小VSS",
              L"VSS workflowを開始できませんでした"));
  try {
    workflow = dependencies.run_snapshot_workflow(
        plan.workflow(),
        options.async_wait,
        options.logger,
        [&](const vssrequester::SnapshotCopyContext& context) {
          if (callback_called) {
            return status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_STATE,
                L"Windows直接縮小Snapshot callback",
                L"一つの操作でSnapshot callbackを複数回実行できません");
          }
          callback_called = true;
          auto status = validate_snapshot_context(plan, context);
          if (!status) {
            return status;
          }
          callback_snapshot_set_id = context.snapshot_set_id;
          auto observed = dependencies.reidentify_confirmed(
              plan.expected_source(),
              plan.expected_target(),
              options.confirmation);
          if (!observed) {
            return clonecore::Status::failure(observed.error());
          }
          status = validate_observed_clone(
              plan, observed.value(), &options.confirmation);
          if (status && is_mbr_preserving_plan(plan)) {
            auto mbr = dependencies.observe_mbr_safety(
                plan.expected_source(), plan.expected_target(), true);
            status = mbr
                ? validate_mbr_safety_evidence(
                      plan,
                      mbr.value(),
                      true,
                      L"Windows直接縮小MBR target I/O直前再照合")
                : clonecore::Status::failure(mbr.error());
          }
          if (!status) {
            return status;
          }
          std::unique_ptr<vssrequester::VssDiffAreaOperationMonitor>
              diff_area_monitor;
          WindowsDirectShrinkCloneExecutionOptions active_options =
              options;
          if (dependencies.make_diff_area_monitor) {
            if (clonecore::disk_operation_cancellation_requested(
                    options.callbacks)) {
              return status_failure(
                  clonecore::ErrorCode::cancelled,
                  ERROR_CANCELLED,
                  L"Windows直接縮小VSS差分領域初回poll",
                  L"初回target変更前に取消要求を確認しました");
            }
            auto made_monitor =
                dependencies.make_diff_area_monitor(context);
            if (!made_monitor || !made_monitor.value()) {
              return clonecore::Status::failure(
                  made_monitor
                      ? shrink_clone_error(
                            clonecore::ErrorCode::internal_error,
                            ERROR_INVALID_HANDLE,
                            L"Windows直接縮小VSS差分領域monitor",
                            L"製品monitor factoryが空のmonitorを返しました")
                      : made_monitor.error());
            }
            diff_area_monitor = made_monitor.take_value();
            const auto monitored = diff_area_monitor->initial_poll();
            if (!monitored) {
              return monitored;
            }
            active_options.callbacks =
                diff_area_monitor->callbacks(options.callbacks);
          }
          report_progress(
              active_options,
              clonecore::DiskOperationStage::invalidating_target,
              std::nullopt,
              plan.operation_plan().expected_work_bytes,
              0U,
              false,
              false);

          auto begun = platform->begin_target_owned_staging(
              plan, operation_plan_hash.value());
          if (!begun) {
            abort_incomplete();
            return clonecore::Status::failure(begun.error());
          }
          checkpoint = begun.take_value();
          status = validate_checkpoint_common(
              plan, operation_plan_hash.value(), *checkpoint);
          if (status &&
              (checkpoint->phase !=
                   WindowsDirectShrinkCheckpointPhase::prepared ||
               checkpoint->completed_task_count != 0U ||
               checkpoint->verified_target_bytes != 0U ||
               !all_zero(checkpoint->aggregate_write_digest))) {
            status = status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Windows直接縮小初期checkpoint",
                L"初期checkpointが0件・0 bytesのprepared状態ではありません");
          }
          if (!status) {
            abort_incomplete();
            return status;
          }

          const auto non_archive_count = static_cast<std::uint64_t>(
              std::count_if(
                  plan.tasks().begin(),
                  plan.tasks().end(),
                  [](const WindowsDirectShrinkPartitionTask& task) {
                    return task.kind !=
                            WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim &&
                        task.kind !=
                            WindowsDirectShrinkPartitionTaskKind::copy_exact_raw;
                  }));
          std::uint64_t non_archive_upper_bound{};
          for (const auto& task : plan.tasks()) {
            if (task.kind ==
                    WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim ||
                task.kind ==
                    WindowsDirectShrinkPartitionTaskKind::copy_exact_raw) {
              continue;
            }
            if (!checked_add(
                    non_archive_upper_bound,
                    task.target_size_bytes,
                    non_archive_upper_bound)) {
              abort_incomplete();
              return status_failure(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_ARITHMETIC_OVERFLOW,
                  L"Windows直接縮小非archive上限",
                  L"再作成領域の検証容量上限がオーバーフローしました");
            }
          }
          report_progress(
              active_options,
              clonecore::DiskOperationStage::staging_partition_table,
              std::nullopt,
              plan.operation_plan().expected_work_bytes,
              verified_bytes,
              false,
              false);
          auto prepared =
              platform->prepare_non_archive_partitions_and_verify(
                  plan.tasks());
          if (!prepared) {
            abort_incomplete();
            return clonecore::Status::failure(prepared.error());
          }
          if (prepared.value().prepared_task_count != non_archive_count ||
              (non_archive_count != 0U &&
               (prepared.value().verified_target_bytes == 0U ||
                prepared.value().verified_target_bytes >
                    non_archive_upper_bound ||
                all_zero(prepared.value().write_digest))) ||
              (non_archive_count == 0U &&
               (prepared.value().verified_target_bytes != 0U ||
                !all_zero(prepared.value().write_digest))) ||
              !prepared.value().every_write_flushed ||
              !prepared.value().every_write_read_back ||
              !prepared.value().target_offline ||
              prepared.value().final_layout_committed) {
            abort_incomplete();
            return status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Windows直接縮小非archive領域",
                L"再作成領域の件数、flush、読戻し、offline、または未commit証跡が不足しています");
          }
          aggregate_digest = prepared.value().write_digest;
          completed_tasks = non_archive_count;
          verified_bytes = prepared.value().verified_target_bytes;
          if (non_archive_count != 0U) {
            auto persisted_preparation =
                platform->persist_prepared_partitions_checkpoint(
                    *checkpoint,
                    completed_tasks,
                    verified_bytes,
                    aggregate_digest);
            if (!persisted_preparation) {
              abort_incomplete();
              return clonecore::Status::failure(
                  persisted_preparation.error());
            }
            status = validate_checkpoint_transition(
                plan,
                operation_plan_hash.value(),
                *checkpoint,
                persisted_preparation.value(),
                WindowsDirectShrinkCheckpointPhase::applying,
                completed_tasks,
                verified_bytes,
                aggregate_digest,
                false);
            if (!status) {
              abort_incomplete();
              return status;
            }
            checkpoint = persisted_preparation.take_value();
          }

          for (const auto& task : plan.tasks()) {
            if (task.kind ==
                WindowsDirectShrinkPartitionTaskKind::copy_exact_raw) {
              status = check_safe_boundary(
                  active_options,
                  task.source_table_index.value_or(0U),
                  verified_bytes,
                  completed_tasks);
              if (!status) {
                abort_incomplete();
                return status;
              }
              if (!dependencies.open_read_only_raw_source ||
                  !task.source_table_index.has_value()) {
                abort_incomplete();
                return status_failure(
                    clonecore::ErrorCode::invalid_argument,
                    ERROR_INVALID_HANDLE,
                    L"Windows直接縮小exact RAW source",
                    L"read-only source openerまたはsource extentがありません");
              }
              report_progress(
                  active_options,
                  clonecore::DiskOperationStage::copying_data,
                  task.source_table_index,
                  plan.operation_plan().expected_work_bytes,
                  verified_bytes,
                  true,
                  true);
              auto opened = dependencies.open_read_only_raw_source(
                  plan.expected_source());
              if (!opened || !opened.value().reader) {
                abort_incomplete();
                return opened
                    ? status_failure(
                          clonecore::ErrorCode::internal_error,
                          ERROR_INVALID_HANDLE,
                          L"Windows直接縮小exact RAW source",
                          L"read-only source readerがありません")
                    : clonecore::Status::failure(opened.error());
              }
              auto same_source = clonecore::validate_stable_identity(
                  plan.expected_source(),
                  opened.value().observed.identity,
                  L"Windows直接縮小exact RAWコピー元");
              auto source_layout =
                  imageformat::hash_tsumugi_physical_restore_target_layout_v1(
                      opened.value().observed.observed);
              if (!same_source || !source_layout ||
                  source_layout.value() != plan.expected_source_layout_hash() ||
                  opened.value().reader->size_bytes() !=
                      plan.expected_source().size_bytes ||
                  opened.value().reader->logical_sector_size() !=
                      plan.expected_source().logical_sector_size) {
                abort_incomplete();
                return status_failure(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"Windows直接縮小exact RAWコピー元再照合",
                    !same_source
                        ? same_source.error().message
                        : !source_layout
                            ? source_layout.error().message
                            : L"read-only sourceのstable identity、canonical layout、容量、またはlogical sectorが計画と一致しません");
              }
              auto copied = platform->copy_exact_raw_and_verify(
                  task, *opened.value().reader);
              if (!copied) {
                abort_incomplete();
                return clonecore::Status::failure(copied.error());
              }
              const auto& evidence = copied.value();
              if (evidence.source_table_index != *task.source_table_index ||
                  evidence.target_number != task.target_number ||
                  evidence.verified_target_bytes != task.source_size_bytes ||
                  evidence.verified_chunk_count == 0U ||
                  all_zero(evidence.source_sha256) ||
                  evidence.source_sha256 != evidence.target_sha256 ||
                  evidence.target_write_digest != evidence.target_sha256 ||
                  !evidence.source_reader_read_only ||
                  !evidence.source_extent_exact ||
                  !evidence.every_write_flushed ||
                  !evidence.every_chunk_read_back ||
                  !evidence.complete_target_hash_verified ||
                  !evidence.target_offline) {
                abort_incomplete();
                return status_failure(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_CRC,
                    L"Windows直接縮小exact RAW検証",
                    L"元サイズ、全chunk flush/readback、source/target SHA-256、read-only source、またはoffline target証跡が不足しています");
              }
              std::uint64_t next_verified{};
              if (!checked_add(
                      verified_bytes,
                      evidence.verified_target_bytes,
                      next_verified) ||
                  next_verified > plan.operation_plan().expected_work_bytes) {
                abort_incomplete();
                return status_failure(
                    clonecore::ErrorCode::invalid_data,
                    ERROR_ARITHMETIC_OVERFLOW,
                    L"Windows直接縮小exact RAW検証済み容量",
                    L"検証済みtarget容量が不変処理上限を超えました");
              }
              auto combined = combine_digest(
                  aggregate_digest,
                  evidence.target_write_digest,
                  task.target_number,
                  evidence.verified_target_bytes);
              if (!combined) {
                abort_incomplete();
                return clonecore::Status::failure(combined.error());
              }
              aggregate_digest = combined.take_value();
              verified_bytes = next_verified;
              ++completed_tasks;
              ++copied_exact_raw;
              auto persisted = platform->persist_progress_checkpoint(
                  *checkpoint,
                  completed_tasks,
                  verified_bytes,
                  aggregate_digest);
              if (!persisted) {
                abort_incomplete();
                return clonecore::Status::failure(persisted.error());
              }
              status = validate_checkpoint_transition(
                  plan,
                  operation_plan_hash.value(),
                  *checkpoint,
                  persisted.value(),
                  WindowsDirectShrinkCheckpointPhase::applying,
                  completed_tasks,
                  verified_bytes,
                  aggregate_digest,
                  false);
              if (!status) {
                abort_incomplete();
                return status;
              }
              checkpoint = persisted.take_value();
              report_progress(
                  active_options,
                  clonecore::DiskOperationStage::flushing_data,
                  task.source_table_index,
                  plan.operation_plan().expected_work_bytes,
                  verified_bytes,
                  true,
                  true);
              status = check_safe_boundary(
                  active_options,
                  *task.source_table_index,
                  verified_bytes,
                  completed_tasks);
              if (!status) {
                abort_incomplete();
                return status;
              }
              continue;
            }
            if (task.kind !=
                WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
              continue;
            }
            status = check_safe_boundary(
                active_options,
                task.source_table_index.value_or(0U),
                verified_bytes,
                completed_tasks);
            if (!status) {
              abort_incomplete();
              return status;
            }
            report_progress(
                active_options,
                clonecore::DiskOperationStage::copying_data,
                task.source_table_index,
                plan.operation_plan().expected_work_bytes,
                verified_bytes,
                true,
                true);
            const auto* mapping = find_snapshot_mapping(
                context, task.original_volume_guid_path);
            if (mapping == nullptr) {
              abort_incomplete();
              return status_failure(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_NOT_FOUND,
                  L"Windows直接縮小Snapshot再対応",
                  L"capture直前にSnapshot mappingを一意に再取得できません");
            }
            auto staged = platform->capture_ntfs_wim_to_owned_staging(
                task, *mapping);
            if (!staged) {
              abort_incomplete();
              return clonecore::Status::failure(staged.error());
            }
            const auto& archive = staged.value();
            if (!task.source_table_index ||
                archive.source_table_index != *task.source_table_index ||
                archive.target_number != task.target_number ||
                archive.snapshot_id != mapping->snapshot_id ||
                archive.snapshot_device_path !=
                    mapping->snapshot_device_path ||
                archive.archive_length == 0U ||
                archive.archive_length > task.archive_upper_bound_bytes ||
                archive.archive_length >
                    plan.staging().archive_capacity_bytes ||
                all_zero(archive.archive_hash) ||
                !archive.sealed_no_write_delete_sharing ||
                !archive.flushed ||
                !archive.complete_read_back_hash_verified ||
                !archive.target_offline) {
              abort_incomplete();
              return status_failure(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windows直接縮小WIM封印",
                  L"WIM上限、Snapshot対応、seal、flush、全読戻しHash、またはoffline証跡が不足しています");
            }
            auto applied = platform->apply_staged_ntfs_wim_and_verify(
                task, archive);
            if (!applied) {
              const auto discarded =
                  platform->discard_exact_staged_archive(archive);
              abort_incomplete();
              if (!discarded) {
                auto error = applied.error();
                error.message +=
                    L" / さらに封印済みWIMの厳密破棄にも失敗: " +
                    discarded.error().message;
                return clonecore::Status::failure(std::move(error));
              }
              return clonecore::Status::failure(applied.error());
            }
            const auto& applied_value = applied.value();
            if (applied_value.source_table_index !=
                    *task.source_table_index ||
                applied_value.target_number != task.target_number ||
                applied_value.verified_target_bytes == 0U ||
                applied_value.verified_target_bytes >
                    task.target_size_bytes ||
                applied_value.archive_hash != archive.archive_hash ||
                all_zero(applied_value.target_write_digest) ||
                !applied_value.every_write_flushed ||
                !applied_value.every_write_read_back ||
                !applied_value.file_system_metadata_verified ||
                !applied_value.target_offline) {
              const auto discarded =
                  platform->discard_exact_staged_archive(archive);
              abort_incomplete();
              if (!discarded) {
                auto error = discarded.error();
                error.operation =
                    L"Windows直接縮小NTFS適用証跡とWIM破棄";
                error.message =
                    L"NTFS適用証跡が不足し、さらに封印済みWIMの厳密破棄にも失敗しました: " +
                    error.message;
                return clonecore::Status::failure(std::move(error));
              }
              return status_failure(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windows直接縮小NTFS適用",
                  L"対象対応、書込みHash、flush、全読戻し、filesystem metadata、またはoffline証跡が不足しています");
            }
            const auto discarded =
                platform->discard_exact_staged_archive(archive);
            if (!discarded) {
              abort_incomplete();
              return discarded;
            }
            std::uint64_t next_verified{};
            if (!checked_add(
                    verified_bytes,
                    applied_value.verified_target_bytes,
                    next_verified) ||
                next_verified > plan.operation_plan().expected_work_bytes) {
              abort_incomplete();
              return status_failure(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_ARITHMETIC_OVERFLOW,
                  L"Windows直接縮小検証済み容量",
                  L"検証済み対象容量が不変処理上限を超えました");
            }
            auto combined = combine_digest(
                aggregate_digest,
                applied_value.target_write_digest,
                task.target_number,
                applied_value.verified_target_bytes);
            if (!combined) {
              abort_incomplete();
              return clonecore::Status::failure(combined.error());
            }
            aggregate_digest = combined.take_value();
            verified_bytes = next_verified;
            ++completed_tasks;
            auto persisted = platform->persist_progress_checkpoint(
                *checkpoint,
                completed_tasks,
                verified_bytes,
                aggregate_digest);
            if (!persisted) {
              abort_incomplete();
              return clonecore::Status::failure(persisted.error());
            }
            report_progress(
                active_options,
                clonecore::DiskOperationStage::flushing_data,
                task.source_table_index,
                plan.operation_plan().expected_work_bytes,
                verified_bytes,
                false,
                false);
            status = validate_checkpoint_transition(
                plan,
                operation_plan_hash.value(),
                *checkpoint,
                persisted.value(),
                WindowsDirectShrinkCheckpointPhase::applying,
                completed_tasks,
                verified_bytes,
                aggregate_digest,
                false);
            if (!status) {
              abort_incomplete();
              return status;
            }
            checkpoint = persisted.take_value();
            ++applied_archives;
            report_progress(
                active_options,
                clonecore::DiskOperationStage::copying_data,
                task.source_table_index,
                plan.operation_plan().expected_work_bytes,
                verified_bytes,
                true,
                true);
            status = check_safe_boundary(
                active_options,
                *task.source_table_index,
                verified_bytes,
                completed_tasks);
            if (!status) {
              abort_incomplete();
              return status;
            }
          }

          if (completed_tasks != plan.tasks().size() ||
              applied_archives != plan.archive_task_count() ||
              copied_exact_raw != exact_raw_task_count(plan) ||
              verified_bytes == 0U || all_zero(aggregate_digest)) {
            abort_incomplete();
            return status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Windows直接縮小payload完了",
                L"全partition、WIM、検証済み容量、または集約書込みHashが揃っていません");
          }
          auto sealed = platform->seal_commit_ready_checkpoint(
              *checkpoint,
              completed_tasks,
              verified_bytes,
              aggregate_digest);
          if (!sealed) {
            abort_incomplete();
            return clonecore::Status::failure(sealed.error());
          }
          report_progress(
              active_options,
              clonecore::DiskOperationStage::verifying_final,
              std::nullopt,
              plan.operation_plan().expected_work_bytes,
              verified_bytes,
              false,
              false);
          status = validate_checkpoint_transition(
              plan,
              operation_plan_hash.value(),
              *checkpoint,
              sealed.value(),
              WindowsDirectShrinkCheckpointPhase::commit_ready,
              completed_tasks,
              verified_bytes,
              aggregate_digest,
              false);
          if (!status) {
            abort_incomplete();
            return status;
          }
          checkpoint = sealed.take_value();
          if (diff_area_monitor) {
            auto monitored = diff_area_monitor->completion_poll();
            if (monitored) {
              monitored = vssrequester::
                  validate_completed_vss_diff_area_operation_evidence(
                      diff_area_monitor->evidence());
            }
            if (!monitored) {
              abort_incomplete();
              return monitored;
            }
          }
          callback_completed = true;
          return clonecore::success_status();
        });
  } catch (...) {
    abort_incomplete();
    return failure<WindowsDirectShrinkCloneExecutionReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Windows直接縮小クローン例外",
        L"VSSまたは対象platform境界から予期しない例外が発生しました");
  }

  if (!workflow) {
    abort_incomplete();
    return clonecore::Result<WindowsDirectShrinkCloneExecutionReport>::failure(
        workflow.error());
  }
  if (!callback_called || !callback_completed || !platform || !checkpoint ||
      !callback_snapshot_set_id ||
      workflow.value().snapshot_set_id.empty() ||
      workflow.value().snapshot_set_id != *callback_snapshot_set_id ||
      workflow.value().volume_count != plan.archive_task_count() ||
      !workflow.value().snapshot_data_copied ||
      !workflow.value().backup_completed ||
      !workflow.value().snapshots_deleted) {
    abort_incomplete();
    return failure<WindowsDirectShrinkCloneExecutionReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        L"Windows直接縮小VSS完了",
        L"callback、BackupComplete、Snapshot削除、またはcommit-ready checkpointが揃いません");
  }

  try {
    report_progress(
        options,
        clonecore::DiskOperationStage::committing_partition_table,
        std::nullopt,
        plan.operation_plan().expected_work_bytes,
        verified_bytes,
        false,
        false);
    auto source_after_cleanup = dependencies.reidentify_selection(
        plan.expected_source(), plan.expected_target());
    if (!source_after_cleanup) {
      abort_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(
          source_after_cleanup.error());
    }
    auto status = validate_observed_source_after_snapshot_cleanup(
        plan, source_after_cleanup.value());
    if (status && is_mbr_preserving_plan(plan)) {
      auto mbr = dependencies.observe_mbr_safety(
          plan.expected_source(), plan.expected_target(), true);
      status = mbr
          ? validate_mbr_safety_evidence(
                plan,
                mbr.value(),
                true,
                L"Windows直接縮小VSS cleanup後raw MBR再照合")
          : clonecore::Status::failure(mbr.error());
    }
    if (!status) {
      abort_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(status.error());
    }

    auto prepared_final =
        platform->prepare_final_extents_keep_incomplete_and_verify(
            plan, *checkpoint);
    if (!prepared_final) {
      abort_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(
          prepared_final.error());
    }
    status = validate_checkpoint_exactly_unchanged(
        plan,
        operation_plan_hash.value(),
        *checkpoint,
        prepared_final.value(),
        L"Windows直接縮小final extent準備後checkpoint");
    if (!status) {
      abort_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(status.error());
    }
    checkpoint = prepared_final.take_value();

    if (plan.boot_finalization_required()) {
      report_progress(
          options,
          clonecore::DiskOperationStage::rebuilding_boot,
          std::nullopt,
          plan.operation_plan().expected_work_bytes,
          verified_bytes,
          false,
          false);
      auto finalized =
          platform->finalize_boot_from_staged_layout_and_verify(plan);
      if (!finalized) {
        abort_incomplete();
        return clonecore::Result<
            WindowsDirectShrinkCloneExecutionReport>::failure(
            finalized.error());
      }
      boot = finalized.take_value();
      if (!boot->required || !boot->completed ||
          !boot->boot_files_read_back_verified ||
          !boot->recovery_configuration_verified ||
          !boot->target_offline) {
        abort_incomplete();
        return failure<WindowsDirectShrinkCloneExecutionReport>(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"Windows直接縮小起動構成",
            L"final extent準備後の起動ファイル再構築、読戻し、またはoffline証跡が不足しています");
      }
    } else {
      boot = WindowsDirectShrinkBootEvidence{
          .required = false,
          .completed = true,
          .boot_files_read_back_verified = true,
          .recovery_configuration_verified = true,
          .target_offline = true,
          .target_only_reconstruction = true,
          .exact_target_volume_extents = true,
          .legacy_bios = false,
          .real_boot_not_claimed = true,
      };
    }

    auto revalidated = platform->revalidate_before_final_commit(
        plan, *checkpoint);
    if (!revalidated) {
      abort_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(
          revalidated.error());
    }
    status = validate_checkpoint_exactly_unchanged(
        plan,
        operation_plan_hash.value(),
        *checkpoint,
        revalidated.value(),
        L"Windows直接縮小最終公開直前checkpoint");
    if (!status) {
      abort_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(status.error());
    }
    checkpoint = revalidated.take_value();

    // Everything needed for the two legitimate completion outcomes is known
    // before final GPT publication. Perform every allocation and SHA call now
    // so the irreversible boundary is followed only by fixed-size evidence
    // checks and digest selection.
    const auto precompute_completion_hash =
        [&](const bool checkpoint_retired)
        -> clonecore::Result<imageformat::Sha256Digest> {
      WindowsDirectShrinkCloneExecutionReport candidate;
      candidate.workflow = workflow.value();
      candidate.applied_archive_count = applied_archives;
      candidate.copied_exact_raw_count = copied_exact_raw;
      candidate.verified_target_bytes = verified_bytes;
      candidate.aggregate_write_digest = aggregate_digest;
      candidate.commit_ready_checkpoint = *checkpoint;
      candidate.boot = *boot;
      candidate.final_commit = WindowsDirectShrinkFinalCommitEvidence{
          .committed_layout_hash = plan.final_layout_hash(),
          .aggregate_write_digest = aggregate_digest,
          .source_reidentified = true,
          .source_layout_unchanged = true,
          .target_reidentified = true,
          .staging_identity_reverified = true,
          .checkpoint_reverified = true,
          .staging_removed = true,
          .checkpoint_retired = checkpoint_retired,
          .checkpoint_retirement_pending = !checkpoint_retired,
          .construction_layout_non_bootable = true,
          .checkpoint_retained_through_extensions_and_boot = true,
          .boot_completed_before_final_layout_publication = true,
          .final_layout_published_before_checkpoint_retirement = true,
          .hidden_final_layout_published_and_read_back = true,
          .extended_ntfs_partition_count =
              plan.ntfs_extension_task_count(),
          .every_required_ntfs_extension_verified = true,
          .every_write_flushed = true,
          .every_write_read_back = true,
          .primary_layout_committed_last = true,
          .target_offline = true,
          .final_partition_style = plan.partition_style(),
          .source_mbr_sector0_unchanged = is_mbr_preserving_plan(plan),
          .source_mbr_bootstrap_unchanged = is_mbr_preserving_plan(plan),
          .target_mbr_signature_collision_free =
              is_mbr_preserving_plan(plan),
          .final_mbr_sector0_read_back_verified =
              is_mbr_preserving_plan(plan),
          .final_mbr_disk_signature = is_mbr_preserving_plan(plan)
              ? plan.mbr_preserve_binding()->target_disk_signature
              : 0U,
          .final_mbr_active_partition_count = is_mbr_preserving_plan(plan) &&
                  plan.boot_finalization_required()
              ? 1U
              : 0U,
      };
      if (plan.surplus_allocation() ==
          migrationcore::ShrinkSurplusAllocation::
              selected_data_partition) {
        const auto targeted_task = std::find_if(
            plan.tasks().begin(),
            plan.tasks().end(),
            [&plan](const WindowsDirectShrinkPartitionTask& task) {
              return task.source_table_index ==
                      plan.surplus_target_source_table_index() &&
                  plan.staging().final_growth_owner_target_number ==
                      task.target_number;
            });
        if (targeted_task == plan.tasks().end()) {
          return failure<imageformat::Sha256Digest>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"Windows直接縮小指定NTFS完成証跡の事前計算",
              L"reviewed source table indexと最終growth owner taskが一致しません");
        }
        candidate.final_commit.targeted_surplus_source_table_index =
            targeted_task->source_table_index;
        candidate.final_commit.targeted_surplus_target_number =
            targeted_task->target_number;
        candidate.final_commit
            .targeted_surplus_previous_file_system_bytes =
            targeted_task->construction_size_bytes;
        candidate.final_commit.targeted_surplus_final_file_system_bytes =
            targeted_task->target_size_bytes;
        candidate.final_commit.targeted_surplus_owner_verified = true;
        candidate.final_commit.targeted_surplus_exact_size_verified = true;
        candidate.final_commit.targeted_surplus_readback_verified = true;
      }
      candidate.every_payload_captured_and_applied_inside_snapshot_callback =
          true;
      candidate.snapshots_deleted_before_final_layout_commit = true;
      candidate.target_left_offline = true;
      return execution_evidence_hash(plan, candidate);
    };
    auto retired_completion = precompute_completion_hash(true);
    auto pending_completion = precompute_completion_hash(false);
    if (!retired_completion || !pending_completion) {
      abort_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(
          !retired_completion ? retired_completion.error()
                              : pending_completion.error());
    }
    auto retired_completion_hash = retired_completion.take_value();
    auto pending_completion_hash = pending_completion.take_value();
    if (all_zero(retired_completion_hash) ||
        all_zero(pending_completion_hash) ||
        retired_completion_hash == pending_completion_hash) {
      abort_incomplete();
      return failure<WindowsDirectShrinkCloneExecutionReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"Windows直接縮小完成証跡の事前Hash",
          L"checkpoint退役済み／cleanup保留のcanonical完成証跡Hashをfinal GPT公開前に一意生成できません");
    }

    auto committed = platform->commit_final_layout_last(plan, *checkpoint);
    if (!committed) {
      abort_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(
          committed.error());
    }
    auto final = committed.take_value();
    final.source_reidentified = true;
    final.source_layout_unchanged = true;
    final_layout_publication_latched =
        final.committed_layout_hash == plan.final_layout_hash() &&
        final.aggregate_write_digest == aggregate_digest &&
        final.final_layout_published_before_checkpoint_retirement &&
        has_valid_targeted_surplus_evidence(plan, final) &&
        final.every_write_flushed && final.every_write_read_back &&
        final.primary_layout_committed_last && final.target_offline;
    if (final.committed_layout_hash != plan.final_layout_hash() ||
        final.aggregate_write_digest != aggregate_digest ||
        !final.target_reidentified ||
        !final.staging_identity_reverified ||
        !final.checkpoint_reverified || !final.staging_removed ||
        (final.checkpoint_retired == final.checkpoint_retirement_pending) ||
        !final.construction_layout_non_bootable ||
        !final.checkpoint_retained_through_extensions_and_boot ||
        !final.boot_completed_before_final_layout_publication ||
        !final.final_layout_published_before_checkpoint_retirement ||
        !has_valid_targeted_surplus_evidence(plan, final) ||
        !final.every_write_flushed ||
        !final.every_write_read_back ||
        !final.primary_layout_committed_last || !final.target_offline) {
      if (!final_layout_publication_latched) {
        abort_incomplete();
      }
      return failure<WindowsDirectShrinkCloneExecutionReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"Windows直接縮小最終layout commit",
          L"Snapshot削除後の再識別、非boot construction、checkpoint保持、staging除去、flush、読戻し、最終layout公開後のcheckpoint退役、またはoffline証跡が不足しています");
    }
    const bool checkpoint_retired = final.checkpoint_retired;
    WindowsDirectShrinkCloneExecutionReport report{
        .workflow = workflow.take_value(),
        .applied_archive_count = applied_archives,
        .copied_exact_raw_count = copied_exact_raw,
        .verified_target_bytes = verified_bytes,
        .aggregate_write_digest = aggregate_digest,
        .commit_ready_checkpoint = std::move(*checkpoint),
        .boot = *boot,
        .final_commit = std::move(final),
        .precomputed_retired_completion_hash = retired_completion_hash,
        .precomputed_pending_completion_hash = pending_completion_hash,
        .selected_completion_hash = checkpoint_retired
            ? retired_completion_hash
            : pending_completion_hash,
        .every_payload_captured_and_applied_inside_snapshot_callback = true,
        .snapshots_deleted_before_final_layout_commit = true,
        .target_left_offline = true,
    };
    status = validate_execution_report(plan, report);
    if (!status) {
      return clonecore::Result<
          WindowsDirectShrinkCloneExecutionReport>::failure(status.error());
    }
    report_progress(
        options,
        clonecore::DiskOperationStage::completed,
        std::nullopt,
        plan.operation_plan().expected_work_bytes,
        verified_bytes,
        false,
        false);
    return clonecore::Result<WindowsDirectShrinkCloneExecutionReport>::success(
        std::move(report));
  } catch (...) {
    if (!final_layout_publication_latched) {
      abort_incomplete();
    }
    return failure<WindowsDirectShrinkCloneExecutionReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Windows直接縮小最終commit例外",
        L"Snapshot削除後の再識別または最終commit境界から例外が発生しました");
  }
}

}  // namespace

clonecore::Result<imageformat::Sha256Digest>
hash_windows_direct_shrink_source_analysis_v1(
    const windowsshrink::ShrinkSourceAnalysis& analysis) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-SOURCE-ANALYSIS-V1";
  constexpr auto kMaximumSerializedCount =
      static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
  const auto string_too_long = [&](const auto& value) noexcept {
    return value.size() > kMaximumSerializedCount;
  };
  if (string_too_long(analysis.source.model) ||
      string_too_long(analysis.source.serial_suffix) ||
      string_too_long(analysis.source.device_instance_id) ||
      string_too_long(analysis.created_utc) ||
      string_too_long(analysis.app_version) ||
      string_too_long(analysis.partition_snapshot) ||
      analysis.partitions.size() > kMaximumSerializedCount ||
      analysis.content_volumes.size() > kMaximumSerializedCount ||
      (analysis.windows_version.has_value() &&
       string_too_long(analysis.windows_version->architecture)) ||
      std::any_of(
          analysis.partitions.begin(),
          analysis.partitions.end(),
          [&](const windowsshrink::AnalyzedShrinkPartition& partition) {
            return string_too_long(partition.label) ||
                string_too_long(partition.name);
          }) ||
      std::any_of(
          analysis.content_volumes.begin(),
          analysis.content_volumes.end(),
          [&](const windowsshrink::AnalyzedShrinkVolume& volume) {
            return string_too_long(volume.volume_guid_path);
          })) {
    return failure<imageformat::Sha256Digest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Windows直接縮小analysis Hash",
        L"解析文字列、partition snapshot、または要素数がcanonical長の上限を超えています");
  }

  std::vector<std::byte> bytes;
  bytes.reserve(
      1024U + analysis.partition_snapshot.size() +
      analysis.partitions.size() * 160U +
      analysis.content_volumes.size() * 128U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_u32(bytes, analysis.source.disk_number);
  append_wstring(bytes, analysis.source.model);
  append_u64(bytes, analysis.source.size_bytes);
  append_u32(bytes, analysis.source.logical_sector_size);
  append_string(bytes, analysis.source.serial_suffix);
  append_wstring(bytes, analysis.source.device_instance_id);
  append_u8(bytes, analysis.source.is_system_disk ? 1U : 0U);
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
  append_string(bytes, analysis.created_utc);
  append_string(bytes, analysis.app_version);
  append_array(bytes, analysis.mbr_bootstrap);
  append_u32(
      bytes, static_cast<std::uint32_t>(analysis.partition_snapshot.size()));
  bytes.insert(
      bytes.end(),
      analysis.partition_snapshot.begin(),
      analysis.partition_snapshot.end());
  append_u32(bytes, static_cast<std::uint32_t>(analysis.partitions.size()));
  for (const auto& partition : analysis.partitions) {
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
  append_u32(
      bytes, static_cast<std::uint32_t>(analysis.content_volumes.size()));
  for (const auto& volume : analysis.content_volumes) {
    append_u32(bytes, volume.source_table_index);
    append_wstring(bytes, volume.volume_guid_path);
  }
  return imageformat::sha256(bytes);
}

clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>
observe_windows_direct_shrink_mbr_safety_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const bool include_target_signature) {
  if (expected_source.logical_sector_size != 512U ||
      expected_target.logical_sector_size != 512U) {
    return failure<WindowsDirectShrinkMbrSafetyEvidence>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小MBR安全観測",
        L"source/targetとも512-byte論理セクターでなければMBRを再照合できません");
  }
  auto source_open =
      diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          expected_source);
  if (!source_open) {
    return clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure(
        source_open.error());
  }
  auto source_mbr = clonecore::parse_mbr(*source_open.value().reader);
  auto source_sector0 = source_open.value().reader->read(0U, 512U);
  if (!source_mbr || !source_sector0 || source_sector0.value().size() != 512U) {
    return !source_mbr
        ? clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure(
              source_mbr.error())
        : !source_sector0
              ? clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure(
                    source_sector0.error())
              : failure<WindowsDirectShrinkMbrSafetyEvidence>(
                    clonecore::ErrorCode::io_failed,
                    ERROR_HANDLE_EOF,
                    L"Windows直接縮小source raw MBR読取り",
                    L"source sector0を512 bytes完全に読み取れません");
  }
  auto source_hash = imageformat::sha256(source_sector0.value());
  if (!source_hash) {
    return clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure(
        source_hash.error());
  }

  WindowsDirectShrinkMbrSafetyEvidence evidence{
      .source_sector0_hash = source_hash.take_value(),
      .source_bootstrap = source_mbr.value().bootstrap,
      .source_disk_signature = source_mbr.value().disk_signature,
  };
  evidence.connected_mbr_signatures_excluding_target.push_back(
      source_mbr.value().disk_signature);

  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  auto report = inventory->enumerate();
  if (!report) {
    return clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return failure<WindowsDirectShrinkMbrSafetyEvidence>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"Windows直接縮小MBR署名全接続列挙",
        L"未解決のinventory診断があるためMBR署名衝突を証明できません");
  }

  std::size_t source_matches{};
  std::size_t target_matches{};
  for (const auto& disk : report.value().disks) {
    auto identity = diskmodel::make_stable_disk_identity(
        disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure(
          identity.error());
    }
    const bool is_source = static_cast<bool>(clonecore::validate_stable_identity(
        expected_source, identity.value(), L"Windows直接縮小MBR列挙source"));
    const bool is_target = static_cast<bool>(clonecore::validate_stable_identity(
        expected_target, identity.value(), L"Windows直接縮小MBR列挙target"));
    if (is_source) {
      ++source_matches;
      if (diskmodel::normalize_disk_partition_style(
              disk.partition_style, disk.partitions.size()) !=
          diskmodel::PartitionStyle::mbr) {
        return failure<WindowsDirectShrinkMbrSafetyEvidence>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"Windows直接縮小MBR列挙source形式",
            L"sourceがMBRとして再識別されません");
      }
      continue;
    }
    if (is_target) {
      ++target_matches;
      if (!include_target_signature ||
          diskmodel::normalize_disk_partition_style(
              disk.partition_style, disk.partitions.size()) !=
              diskmodel::PartitionStyle::mbr) {
        continue;
      }
    } else if (
        diskmodel::normalize_disk_partition_style(
            disk.partition_style, disk.partitions.size()) !=
        diskmodel::PartitionStyle::mbr) {
      continue;
    }

    auto opened =
        diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
            identity.value());
    if (!opened) {
      return clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure(
          opened.error());
    }
    auto parsed = clonecore::parse_mbr(*opened.value().reader);
    if (!parsed) {
      return clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure(
          parsed.error());
    }
    if (is_target) {
      evidence.target_mbr_signature = parsed.value().disk_signature;
    } else {
      evidence.connected_mbr_signatures_excluding_target.push_back(
          parsed.value().disk_signature);
    }
  }
  if (source_matches != 1U || target_matches != 1U) {
    return failure<WindowsDirectShrinkMbrSafetyEvidence>(
        clonecore::ErrorCode::identity_mismatch,
        source_matches == 0U || target_matches == 0U
            ? ERROR_NOT_FOUND
            : ERROR_DUP_NAME,
        L"Windows直接縮小MBR列挙identity",
        L"source/targetを全接続inventoryへ各1台として再識別できません");
  }
  std::sort(
      evidence.connected_mbr_signatures_excluding_target.begin(),
      evidence.connected_mbr_signatures_excluding_target.end());
  evidence.connected_mbr_signatures_excluding_target.erase(
      std::unique(
          evidence.connected_mbr_signatures_excluding_target.begin(),
          evidence.connected_mbr_signatures_excluding_target.end()),
      evidence.connected_mbr_signatures_excluding_target.end());
  return clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::success(
      std::move(evidence));
}

bool has_valid_windows_direct_shrink_precomputed_completion_evidence(
    const WindowsDirectShrinkCloneExecutionReport& report) noexcept {
  if (report.final_commit.checkpoint_retired ==
          report.final_commit.checkpoint_retirement_pending ||
      all_zero(report.precomputed_retired_completion_hash) ||
      all_zero(report.precomputed_pending_completion_hash) ||
      all_zero(report.selected_completion_hash) ||
      report.precomputed_retired_completion_hash ==
          report.precomputed_pending_completion_hash) {
    return false;
  }
  const auto& expected = report.final_commit.checkpoint_retired
      ? report.precomputed_retired_completion_hash
      : report.precomputed_pending_completion_hash;
  return report.selected_completion_hash == expected;
}

clonecore::Result<WindowsDirectShrinkClonePlan>
build_windows_direct_shrink_clone_plan(
    const WindowsDirectShrinkPlanningRequest& request,
    const migrationcore::DirectClonePlan& direct_plan) {
  if (!request.administrator) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"Windows直接縮小クローンの管理者確認",
        L"アプリ起動時の管理者権限が必要です");
  }
  if (!request.bitlocker_fully_decrypted) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンのBitLocker状態",
        L"この初期production sliceは完全復号済みNTFSだけを扱います。Unlock済み暗号化Volumeを非暗号化先へ再構成する経路はまだ開始しません");
  }
  auto status = validate_reviewed_identities_and_layouts(request);
  if (!status) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        status.error());
  }
  const bool preserve_style =
      direct_plan.partition_style_choice() ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      direct_plan.source_style() == direct_plan.target_style();
  const bool mbr_to_gpt =
      direct_plan.partition_style_choice() ==
          migrationcore::DirectClonePartitionStyleChoice::mbr_to_gpt &&
      direct_plan.source_style() ==
          migrationcore::MigrationPartitionStyle::mbr &&
      direct_plan.target_style() ==
          migrationcore::MigrationPartitionStyle::gpt;
  const bool preserve_mbr = preserve_style &&
      direct_plan.source_style() ==
          migrationcore::MigrationPartitionStyle::mbr &&
      direct_plan.target_style() ==
          migrationcore::MigrationPartitionStyle::mbr;
  const bool mbr_binding_valid = !preserve_mbr
      ? !request.mbr_preserve_binding.has_value()
      : request.mbr_preserve_binding.has_value() &&
          !all_zero(request.mbr_preserve_binding->source_sector0_hash) &&
          request.mbr_preserve_binding->target_disk_signature != 0U &&
          request.mbr_preserve_binding->source_disk_signature !=
              request.mbr_preserve_binding->target_disk_signature &&
          !all_zero(
              request.mbr_preserve_binding->planning_signature_inventory_hash);
  if (direct_plan.mode() != migrationcore::DirectCloneMode::shrink ||
      (!preserve_style && !mbr_to_gpt) ||
      (direct_plan.surplus_allocation() !=
           migrationcore::ShrinkSurplusAllocation::leave_unallocated &&
       direct_plan.surplus_allocation() !=
           migrationcore::ShrinkSurplusAllocation::automatic_proportional &&
       direct_plan.surplus_allocation() !=
           migrationcore::ShrinkSurplusAllocation::selected_data_partition) ||
      ((direct_plan.surplus_allocation() ==
        migrationcore::ShrinkSurplusAllocation::selected_data_partition) !=
       direct_plan.surplus_target_source_table_index().has_value()) ||
      !direct_plan.source_remains_unchanged() ||
      direct_plan.source_logical_sector_size() != 512U ||
      direct_plan.target_logical_sector_size() != 512U ||
      direct_plan.source_size_bytes() != request.expected_source.size_bytes ||
      direct_plan.target_size_bytes() != request.expected_target.size_bytes ||
      !source_partition_style_matches(
          diskmodel::normalize_disk_partition_style(
              request.reviewed_source.partition_style,
              request.reviewed_source.partitions.size()),
          direct_plan.source_style()) ||
      direct_plan.boot_finalization_required() !=
          request.expected_source.is_system_disk ||
      direct_plan.target_partitions().empty() ||
      ((mbr_to_gpt || preserve_mbr) &&
       all_zero(request.expected_source_partition_snapshot_hash)) ||
      !mbr_binding_valid) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンの対応slice",
        L"v2は形式維持または解析済みMBRからGPT、512-byte sector、NTFS、検証付き余剰配分または余剰未割当、コピー元不変の縮小計画だけを扱います");
  }

  WindowsDirectShrinkClonePlan plan;
  plan.expected_source_ = request.expected_source;
  plan.expected_target_ = request.expected_target;
  plan.expected_source_layout_hash_ = request.expected_source_layout_hash;
  plan.expected_target_layout_hash_ = request.expected_target_layout_hash;
  plan.source_partition_snapshot_hash_ =
      request.expected_source_partition_snapshot_hash;
  plan.mbr_preserve_binding_ = request.mbr_preserve_binding;
  plan.source_partition_style_ = direct_plan.source_style();
  plan.partition_style_ = direct_plan.target_style();
  plan.partition_style_choice_ = direct_plan.partition_style_choice();
  plan.surplus_allocation_ = direct_plan.surplus_allocation();
  plan.surplus_target_source_table_index_ =
      direct_plan.surplus_target_source_table_index();
  plan.checkpoint_offset_bytes_ =
      kWindowsDirectShrinkCheckpointOffsetBytes;
  plan.boot_finalization_required_ =
      direct_plan.boot_finalization_required();
  plan.target_is_active_rescue_media_ =
      request.target_is_active_rescue_media;
  plan.workflow_.administrator = true;
  plan.tasks_.reserve(direct_plan.target_partitions().size());

  std::map<std::uint32_t, bool> target_numbers;
  std::map<std::uint32_t, bool> source_indexes;
  std::uint64_t final_end{};
  std::uint64_t expected_work_bytes{};
  std::size_t used_volume_count{};
  std::size_t used_raw_count{};
  std::size_t windows_task_count{};
  std::size_t bios_system_task_count{};
  std::size_t efi_system_task_count{};
  std::size_t microsoft_reserved_task_count{};
  std::size_t recovery_task_count{};
  std::size_t active_task_count{};
  constexpr std::uint64_t checkpoint_end =
      kWindowsDirectShrinkCheckpointOffsetBytes +
      kWindowsDirectShrinkCheckpointRecordBytes;
  if (checkpoint_end > direct_plan.target_size_bytes()) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows直接縮小クローンの固定checkpoint配置",
        L"64KiB位置の4KiB checkpointをGPT予約gap内へ安全に配置できません");
  }
  for (const auto& partition : direct_plan.target_partitions()) {
    const diskmodel::PartitionInfo* reviewed_source_partition = nullptr;
    if (partition.source_table_index.has_value()) {
      if (*partition.source_table_index == 0U) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Windows直接縮小クローンのコピー元table index",
            L"コピー元partition table indexは1以上でなければなりません");
      }
      reviewed_source_partition = find_reviewed_source_partition(
          request.reviewed_source, *partition.source_table_index);
      std::uint64_t reviewed_source_end{};
      if (reviewed_source_partition == nullptr ||
          !source_partition_style_matches(
              reviewed_source_partition->style,
              direct_plan.source_style()) ||
          reviewed_source_partition->size_bytes !=
              partition.source_size_bytes ||
          reviewed_source_partition->offset_bytes % 512U != 0U ||
          reviewed_source_partition->size_bytes % 512U != 0U ||
          !checked_add(
              reviewed_source_partition->offset_bytes,
              reviewed_source_partition->size_bytes,
              reviewed_source_end) ||
          reviewed_source_end > request.reviewed_source.size_bytes) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"Windows直接縮小クローンのコピー元partition対応",
            L"縮小計画のtable index、形式、元容量、または範囲がレビュー済み物理layoutと一致しません");
      }
    }
    std::uint64_t end{};
    if (partition.target_number == 0U ||
        (preserve_mbr && partition.target_number > 4U) ||
        !target_numbers.emplace(partition.target_number, true).second ||
        partition.size_bytes == 0U || partition.minimum_size_bytes == 0U ||
        partition.minimum_size_bytes > partition.size_bytes ||
        partition.offset_bytes <
            kWindowsDirectShrinkStagingAlignmentBytes ||
        partition.offset_bytes %
                kWindowsDirectShrinkStagingAlignmentBytes !=
            0U ||
        partition.size_bytes % 512U != 0U ||
        partition.minimum_size_bytes % 512U != 0U ||
        !checked_add(
            partition.offset_bytes,
            partition.size_bytes,
            end) ||
        end > direct_plan.target_size_bytes() ||
        (partition.offset_bytes < checkpoint_end &&
         plan.checkpoint_offset_bytes_ < end)) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Windows直接縮小クローンの最終partition配置",
          L"番号、offset、容量、整列、または対象範囲が不正です");
    }
    for (const auto& existing : plan.tasks_) {
      std::uint64_t existing_end{};
      if (!checked_add(
              existing.target_offset_bytes,
              existing.target_size_bytes,
              existing_end) ||
          (partition.offset_bytes < existing_end &&
           existing.target_offset_bytes < end)) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Windows直接縮小クローンの最終partition重複",
            L"レビュー済み最終partitionが重複しています");
      }
    }
    final_end = (std::max)(final_end, end);

    WindowsDirectShrinkPartitionTask task{
        .target_number = partition.target_number,
        .source_table_index = partition.source_table_index,
        .role = partition.role,
        .active = partition.active,
        .source_offset_bytes = reviewed_source_partition == nullptr
            ? 0U
            : reviewed_source_partition->offset_bytes,
        .target_offset_bytes = partition.offset_bytes,
        .construction_size_bytes = partition.minimum_size_bytes,
        .target_size_bytes = partition.size_bytes,
        .source_size_bytes = partition.source_size_bytes,
        .source_used_bytes = partition.source_used_bytes,
    };
    if (task.role == migrationcore::MigrationPartitionRole::windows) {
      ++windows_task_count;
    }
    if (task.role == migrationcore::MigrationPartitionRole::bios_system) {
      ++bios_system_task_count;
    }
    if (task.role == migrationcore::MigrationPartitionRole::efi_system) {
      ++efi_system_task_count;
    }
    if (task.role ==
        migrationcore::MigrationPartitionRole::microsoft_reserved) {
      ++microsoft_reserved_task_count;
    }
    if (task.role == migrationcore::MigrationPartitionRole::recovery) {
      ++recovery_task_count;
    }
    if (partition.active) {
      ++active_task_count;
    }
    if (partition.transfer ==
        migrationcore::DirectClonePartitionTransfer::recreate) {
      if (partition.source_table_index ||
          partition.source_size_bytes != 0U ||
          partition.source_used_bytes != 0U) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Windows直接縮小クローンの再作成領域",
            L"再作成領域にコピー元payloadを指定できません");
      }
      if (partition.role ==
              migrationcore::MigrationPartitionRole::efi_system &&
          partition.file_system ==
              migrationcore::MigrationFileSystem::fat32) {
        task.kind = WindowsDirectShrinkPartitionTaskKind::
            recreate_efi_system;
      } else if (
          partition.role == migrationcore::MigrationPartitionRole::
                                microsoft_reserved &&
          partition.file_system ==
              migrationcore::MigrationFileSystem::none) {
        task.kind = WindowsDirectShrinkPartitionTaskKind::
            recreate_microsoft_reserved;
      } else {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"Windows直接縮小クローンの再作成形式",
            L"このsliceは生成ESPまたはMSR以外の再作成領域を扱いません");
      }
    } else if (
        partition.transfer ==
        migrationcore::DirectClonePartitionTransfer::file_system_content) {
      if (!partition.source_table_index ||
          !source_indexes.emplace(*partition.source_table_index, true).second ||
          partition.file_system !=
              migrationcore::MigrationFileSystem::ntfs ||
          partition.source_size_bytes == 0U ||
          partition.source_used_bytes > partition.source_size_bytes) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"Windows直接縮小クローンのpayload形式",
            L"このsliceは一意な選択済みNTFSだけをファイル単位で扱います。FAT32、exFAT、RAWは開始しません");
      }
      const auto* volume = find_volume(
          request, *partition.source_table_index);
      if (partition.source_used_bytes == 0U && volume == nullptr) {
        if (partition.role ==
            migrationcore::MigrationPartitionRole::recovery) {
          return failure<WindowsDirectShrinkClonePlan>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"Windows直接縮小クローンの回復領域内容",
              L"選択済み回復領域はWinre.wimを含む非空NTFSとして証明できなければ開始しません");
        }
        task.kind = WindowsDirectShrinkPartitionTaskKind::create_empty_ntfs;
      } else {
        if (volume == nullptr ||
            volume->source_offset_bytes != task.source_offset_bytes ||
            volume->source_size_bytes != task.source_size_bytes ||
            already_contains_volume_path(
                plan.workflow_, volume->original_volume_guid_path) ||
            !is_volume_guid_path(volume->original_volume_guid_path)) {
          return failure<WindowsDirectShrinkClonePlan>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_NOT_FOUND,
              L"Windows直接縮小クローンのNTFS Volume",
              L"選択済みNTFSのtable index、物理extent、または一意なVolume GUID対応を証明できません");
        }
        task.kind = WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim;
        task.original_volume_guid_path = volume->original_volume_guid_path;
        ++plan.archive_task_count_;
        ++used_volume_count;
        plan.workflow_.volumes.push_back(vssrequester::VolumeRequest{
            .volume_guid_path = task.original_volume_guid_path,
            .file_system = L"NTFS",
        });
      }
    } else if (
        partition.transfer ==
            migrationcore::DirectClonePartitionTransfer::exact_content &&
        partition.file_system ==
            migrationcore::MigrationFileSystem::unsupported &&
        partition.role == migrationcore::MigrationPartitionRole::data &&
        partition.source_table_index.has_value()) {
      const auto* raw = find_exact_raw_partition(
          request, *partition.source_table_index);
      if (raw == nullptr ||
          !source_indexes.emplace(*partition.source_table_index, true).second ||
          raw->source_offset_bytes != task.source_offset_bytes ||
          raw->source_size_bytes != task.source_size_bytes ||
          all_zero(raw->source_partition_type) ||
          task.construction_size_bytes != task.source_size_bytes ||
          task.target_size_bytes != task.source_size_bytes) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"Windows直接縮小exact RAW対応",
            L"選択済み未対応FSのsource table index、extent、partition type、または元サイズを一意に拘束できません");
      }
      task.kind = WindowsDirectShrinkPartitionTaskKind::copy_exact_raw;
      task.source_partition_type = raw->source_partition_type;
      ++used_raw_count;
    } else {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows直接縮小クローンの転送方式",
          L"転送方式がNTFS reconstructionまたは元サイズexact RAWの製品経路に一致しません");
    }
    if (task.construction_size_bytes < task.target_size_bytes) {
      if (task.kind != WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim &&
          task.kind != WindowsDirectShrinkPartitionTaskKind::create_empty_ntfs) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"Windows直接縮小クローンの自動余剰配分",
            L"最終容量へ伸長する領域は検証可能なNTFS taskに限定します");
      }
      ++plan.ntfs_extension_task_count_;
    }
    if (!checked_add(
            expected_work_bytes,
            task.target_size_bytes,
            expected_work_bytes)) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Windows直接縮小クローンの処理上限",
          L"最終partition容量の合計がオーバーフローしました");
    }
    plan.tasks_.push_back(std::move(task));
  }

  const bool invalid_mbr_boot_roles =
      plan.partition_style_ == migrationcore::MigrationPartitionStyle::mbr &&
      (windows_task_count != 1U || active_task_count != 1U ||
       bios_system_task_count > 1U || efi_system_task_count != 0U ||
       microsoft_reserved_task_count != 0U);
  const bool invalid_gpt_boot_roles =
      plan.partition_style_ == migrationcore::MigrationPartitionStyle::gpt &&
      (windows_task_count != 1U || active_task_count != 0U ||
       bios_system_task_count != 0U || efi_system_task_count != 1U ||
        microsoft_reserved_task_count != 1U);
  const bool invalid_data_roles = !plan.boot_finalization_required_ &&
      (windows_task_count != 0U || active_task_count != 0U ||
       bios_system_task_count != 0U || recovery_task_count != 0U ||
       efi_system_task_count != 0U ||
       (plan.partition_style_ == migrationcore::MigrationPartitionStyle::mbr
            ? microsoft_reserved_task_count != 0U
            : microsoft_reserved_task_count > 1U));
  if ((plan.boot_finalization_required_ &&
       (invalid_mbr_boot_roles || invalid_gpt_boot_roles ||
        recovery_task_count > 1U)) ||
      invalid_data_roles) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンの起動役割",
        L"形式維持後のWindows、ESP/MSRまたはActive起動領域を一意に確定できなければ開始しません");
  }

  plan.source_partition_mappings_.reserve(
      direct_plan.partition_selection().size());
  std::map<std::uint32_t, bool> mapped_source_indexes;
  for (const auto& selection : direct_plan.partition_selection()) {
    if (selection.source_table_index == 0U ||
        !mapped_source_indexes.emplace(
             selection.source_table_index, true).second ||
        find_reviewed_source_partition(
            request.reviewed_source, selection.source_table_index) == nullptr) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"Windows直接縮小クローンの全コピー元対応",
          L"レビュー済みsource partitionと計画選択を一意に対応できません");
    }

    const WindowsDirectShrinkPartitionTask* transferred = nullptr;
    for (const auto& task : plan.tasks_) {
      if (task.source_table_index != selection.source_table_index) {
        continue;
      }
      if (transferred != nullptr) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"Windows直接縮小クローンのsource転送対応",
            L"一つのsource partitionが複数のtarget taskへ対応しています");
      }
      transferred = &task;
    }

    WindowsDirectShrinkSourcePartitionMapping mapping{
        .source_table_index = selection.source_table_index,
        .role = selection.role,
        .requested = selection.requested,
        .selected = selection.selected,
        .required = selection.required,
    };
    if (!selection.selected) {
      if (selection.required || transferred != nullptr) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Windows直接縮小クローンの未選択source",
            L"未選択source partitionをrequiredまたはtarget taskとして扱えません");
      }
      mapping.disposition =
          WindowsDirectShrinkSourcePartitionDisposition::omitted_unselected;
    } else if (transferred != nullptr) {
      if (transferred->role != selection.role) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Windows直接縮小クローンのsource role対応",
            L"source selectionとtarget taskのroleが一致しません");
      }
      mapping.disposition = WindowsDirectShrinkSourcePartitionDisposition::
          transferred_to_target;
      mapping.target_number = transferred->target_number;
    } else if (
        mbr_to_gpt &&
        selection.role == migrationcore::MigrationPartitionRole::bios_system) {
      mapping.disposition = WindowsDirectShrinkSourcePartitionDisposition::
          replaced_by_generated_uefi_boot;
    } else if (
        preserve_style &&
        direct_plan.source_style() ==
            migrationcore::MigrationPartitionStyle::gpt &&
        (selection.role ==
             migrationcore::MigrationPartitionRole::efi_system ||
         selection.role ==
             migrationcore::MigrationPartitionRole::microsoft_reserved)) {
      const WindowsDirectShrinkPartitionTask* generated = nullptr;
      for (const auto& task : plan.tasks_) {
        if (task.source_table_index.has_value() ||
            task.role != selection.role) {
          continue;
        }
        if (generated != nullptr) {
          return failure<WindowsDirectShrinkClonePlan>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DUP_NAME,
              L"Windows直接縮小クローンの生成system対応",
              L"source system partitionに対応する生成targetが複数あります");
        }
        generated = &task;
      }
      if (generated == nullptr) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"Windows直接縮小クローンの生成system対応",
            L"source system partitionに対応する生成targetがありません");
      }
      mapping.disposition = WindowsDirectShrinkSourcePartitionDisposition::
          recreated_as_generated_system_partition;
      mapping.target_number = generated->target_number;
    } else {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows直接縮小クローンのsource処遇",
          L"選択済みsource partitionの転送、system再作成、またはMBR boot置換を一意に確定できません");
    }
    plan.source_partition_mappings_.push_back(std::move(mapping));
  }
  if (plan.source_partition_mappings_.size() !=
      request.reviewed_source.partitions.size()) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows直接縮小クローンのsource処遇集合",
        L"全レビュー済みsource partitionの処遇が計画へ含まれていません");
  }

  if (plan.archive_task_count_ == 0U || expected_work_bytes == 0U ||
      used_volume_count != request.ntfs_volumes.size() ||
      used_raw_count != request.exact_raw_partitions.size() ||
      expected_work_bytes > request.expected_target.size_bytes) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンのNTFS集合",
        L"1件以上の非空NTFSと余分のないVolume GUID集合が必要です");
  }

  std::uint64_t total_staging_requirement{};
  if (!checked_add(
          kWindowsDirectShrinkStagingControlReserveBytes,
          kWindowsDirectShrinkStagingFileSystemReserveBytes,
          total_staging_requirement)) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Windows直接縮小クローンのtarget-owned staging全容量",
        L"管理領域とNTFS／DISM予約の合計がオーバーフローしました");
  }

  std::uint64_t staging_offset{};
  std::uint64_t staging_length{};
  std::optional<std::uint32_t> growth_owner;
  if (direct_plan.surplus_allocation() ==
      migrationcore::ShrinkSurplusAllocation::leave_unallocated) {
    if (plan.ntfs_extension_task_count_ != 0U ||
        !align_up(
            final_end,
            kWindowsDirectShrinkStagingAlignmentBytes,
            staging_offset) ||
        staging_offset < final_end) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"Windows直接縮小クローンの未割当staging容量",
          L"最終layout後の整列済み未割当末尾を安全に確定できません");
    }
    const std::uint64_t alignment_padding = staging_offset - final_end;
    if (direct_plan.unallocated_tail_bytes() < alignment_padding ||
        direct_plan.unallocated_tail_bytes() - alignment_padding <
            total_staging_requirement) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"Windows直接縮小クローンの未割当staging容量",
          L"最終layoutと非重複の整列padding後へ、64MiB管理領域と1GiB以上のtarget-owned WIM／NTFS／DISM領域を保持できません");
    }
    staging_length =
        direct_plan.unallocated_tail_bytes() - alignment_padding;
  } else {
    if (plan.ntfs_extension_task_count_ == 0U) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows直接縮小クローンの自動余剰staging",
          L"自動余剰配分にNTFS伸長領域がありません");
    }
    for (const auto& task : plan.tasks_) {
      if (task.construction_size_bytes >= task.target_size_bytes) {
        continue;
      }
      const std::uint64_t candidate_length =
          task.target_size_bytes - task.construction_size_bytes;
      if (candidate_length < total_staging_requirement ||
          candidate_length <= staging_length) {
        continue;
      }
      std::uint64_t candidate_offset{};
      if (!checked_add(
              task.target_offset_bytes,
              task.construction_size_bytes,
              candidate_offset)) {
        return failure<WindowsDirectShrinkClonePlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"Windows直接縮小クローンの自動余剰staging offset",
            L"NTFS construction終端がオーバーフローしました");
      }
      staging_offset = candidate_offset;
      staging_length = candidate_length;
      growth_owner = task.target_number;
    }
    if (!growth_owner.has_value()) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"Windows直接縮小クローンの自動余剰staging容量",
          L"いずれのNTFS伸長領域にも、64MiB管理領域と1GiB以上のtarget-owned WIM／NTFS／DISM領域を保持できません");
    }
  }
  std::uint64_t staging_end{};
  std::uint64_t archive_offset{};
  if (!checked_add(
          staging_offset,
          staging_length,
          staging_end) ||
      !checked_add(
          staging_offset,
          kWindowsDirectShrinkStagingControlReserveBytes,
          archive_offset) ||
      staging_offset % kWindowsDirectShrinkStagingAlignmentBytes != 0U ||
      staging_length % kWindowsDirectShrinkStagingAlignmentBytes != 0U ||
      staging_end > direct_plan.target_size_bytes() ||
      (staging_offset < checkpoint_end &&
       plan.checkpoint_offset_bytes_ < staging_end) ||
      direct_plan.target_size_bytes() - staging_end <
          kWindowsDirectShrinkStagingAlignmentBytes) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Windows直接縮小クローンのtarget-owned staging配置",
        L"staging終端、archive開始、または末尾partition metadata保護を安全に確定できません");
  }
  plan.staging_ = WindowsDirectShrinkTargetOwnedStagingPlan{
      .offset_bytes = staging_offset,
      .length_bytes = staging_length,
      .control_reserve_bytes =
          kWindowsDirectShrinkStagingControlReserveBytes,
      .archive_offset_bytes = archive_offset,
      .archive_capacity_bytes =
          staging_length -
          kWindowsDirectShrinkStagingControlReserveBytes,
      .final_growth_owner_target_number = growth_owner,
  };
  // The staging partition is the physical write boundary. Give every
  // sequential WIM capture that exact capacity; do not infer compression or
  // claim it will fit. A larger archive fails safely at the owned boundary.
  plan.maximum_archive_upper_bound_bytes_ =
      plan.staging_.archive_capacity_bytes;
  for (auto& task : plan.tasks_) {
    if (task.kind == WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
      task.archive_upper_bound_bytes = plan.staging_.archive_capacity_bytes;
    }
  }

  auto final_hash = hash_final_layout(
      direct_plan, plan.staging_, plan.tasks_, plan.mbr_preserve_binding_);
  if (!final_hash) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        final_hash.error());
  }
  plan.final_layout_hash_ = final_hash.take_value();
  auto payload_hash = hash_immutable_payload(
      request,
      direct_plan,
      plan.checkpoint_offset_bytes_,
      plan.staging_,
      plan.tasks_,
      plan.source_partition_mappings_,
      plan.final_layout_hash_);
  if (!payload_hash) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        payload_hash.error());
  }
  plan.operation_plan_ = operationcore::OperationPlan{
      .schema_version = operationcore::kOperationPlanSchemaVersion,
      .operation_id = request.operation_id,
      .kind = operationcore::OperationKind::clone,
      .environment = operationcore::OperationEnvironment::windows,
      .source = request.expected_source,
      .target = request.expected_target,
      .expected_work_bytes = expected_work_bytes,
      .immutable_payload_hash = payload_hash.take_value(),
  };
  status = operationcore::validate_operation_plan(plan.operation_plan_);
  if (!status) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        status.error());
  }
  return clonecore::Result<WindowsDirectShrinkClonePlan>::success(
      std::move(plan));
}

clonecore::Result<WindowsDirectShrinkClonePlan>
build_windows_direct_shrink_clone_plan_from_analysis(
    const WindowsDirectShrinkProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis) {
  auto expected_source = diskmodel::make_stable_disk_identity(
      request.reviewed_source, request.reviewed_source.is_system_disk);
  auto expected_target = diskmodel::make_stable_disk_identity(
      request.reviewed_target, false);
  if (!expected_source || !expected_target) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        !expected_source ? expected_source.error() : expected_target.error());
  }
  auto status = clonecore::validate_stable_identity(
      expected_source.value(), analysis.source, L"Windows直接縮小解析元");
  if (!status) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        status.error());
  }

  const auto reviewed_style = diskmodel::normalize_disk_partition_style(
      request.reviewed_source.partition_style,
      request.reviewed_source.partitions.size());
  const bool system_source = request.reviewed_source.is_system_disk;
  const bool preserve_gpt =
      request.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      reviewed_style == diskmodel::PartitionStyle::gpt &&
      analysis.partition_style ==
          migrationcore::MigrationPartitionStyle::gpt;
  const bool preserve_mbr =
      request.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      reviewed_style == diskmodel::PartitionStyle::mbr &&
      analysis.partition_style ==
          migrationcore::MigrationPartitionStyle::mbr;
  const bool convert_mbr_to_gpt =
      request.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::mbr_to_gpt &&
      reviewed_style == diskmodel::PartitionStyle::mbr &&
      analysis.partition_style ==
          migrationcore::MigrationPartitionStyle::mbr;
  const bool version_matches = !system_source ||
      (analysis.windows_version.has_value() &&
       analysis.windows_version->major == request.windows_major &&
       analysis.windows_version->minor == request.windows_minor &&
       analysis.windows_version->build == request.windows_build &&
       analysis.windows_version->architecture ==
           request.windows_architecture &&
        request.windows_major == 10U &&
        request.windows_architecture == "AMD64");
  if (!request.administrator ||
      request.mode_choice != migrationcore::DirectCloneModeChoice::shrink ||
      (!preserve_gpt && !preserve_mbr && !convert_mbr_to_gpt) ||
      (convert_mbr_to_gpt && !system_source) ||
      analysis.source.is_system_disk != system_source ||
      analysis.physical_sector_size !=
          request.reviewed_source.physical_sector_size ||
      analysis.created_utc != request.analysis_created_utc ||
      analysis.app_version != request.app_version ||
      analysis.partitions.empty() || analysis.content_volumes.empty() ||
      !analysis.bitlocker_fully_decrypted || !version_matches ||
      ((convert_mbr_to_gpt || preserve_mbr) &&
       analysis.partition_snapshot.empty()) ||
      (preserve_mbr && !request.mbr_preserve_binding.has_value())) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンの解析済み製品条件",
        request.mode_choice != migrationcore::DirectCloneModeChoice::shrink
            ? L"Windows直接縮小はfilesystem reconstructionを行うshrink modeだけを実行します"
            : (!preserve_gpt && !preserve_mbr && !convert_mbr_to_gpt)
                  ? L"製品経路はGPT/MBR形式維持または読取り専用解析済みMBRからGPTだけを扱います"
                  : L"管理者、完全復号NTFS、Windows 10/11 AMD64、raw partition snapshot、または解析済みVolume集合が対応条件外です");
  }

  std::map<std::uint32_t, bool> explicitly_requested_indexes;
  for (const std::uint32_t index :
       request.selected_source_table_indexes) {
    if (index == 0U ||
        !explicitly_requested_indexes.emplace(index, true).second) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"Windows直接縮小クローンのpartition選択",
          L"選択source table indexは1以上かつ重複なしでなければなりません");
    }
  }

  std::map<std::uint32_t, bool> analyzed_indexes;
  std::map<std::uint32_t, bool> matched_requested_indexes;
  bool windows_explicitly_selected =
      request.selected_source_table_indexes.empty();
  if (analysis.partitions.size() !=
      request.reviewed_source.partitions.size()) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows直接縮小クローンの解析済みpartition集合",
        L"読取り専用解析と画面レビューのpartition件数が一致しません");
  }
  for (const auto& partition : analysis.partitions) {
    const auto* reviewed = find_reviewed_source_partition(
        request.reviewed_source, partition.source_table_index);
    const bool source_style_matches = source_partition_style_matches(
        reviewed == nullptr ? diskmodel::PartitionStyle::unknown
                            : reviewed->style,
        analysis.partition_style);
    bool type_and_role_match = true;
    if ((convert_mbr_to_gpt || preserve_mbr) && reviewed != nullptr) {
      const bool basic = analyzed_mbr_type_matches(
          partition, *reviewed, 0x07U);
      const bool recovery = analyzed_mbr_type_matches(
          partition, *reviewed, 0x27U);
      const bool basic_role_matches_active =
          partition.role ==
              migrationcore::MigrationPartitionRole::windows ||
          (partition.role ==
               migrationcore::MigrationPartitionRole::bios_system &&
           partition.active) ||
          (partition.role ==
               migrationcore::MigrationPartitionRole::data &&
           !partition.active);
      type_and_role_match =
          all_zero(partition.unique_id) &&
          ((basic && basic_role_matches_active) ||
           (recovery && !partition.active &&
            partition.role ==
                migrationcore::MigrationPartitionRole::recovery));
    }
    const bool explicitly_requested =
        request.selected_source_table_indexes.empty() ||
        explicitly_requested_indexes.contains(partition.source_table_index);
    if (explicitly_requested_indexes.contains(
            partition.source_table_index)) {
      matched_requested_indexes.emplace(partition.source_table_index, true);
    }
    if (partition.role ==
            migrationcore::MigrationPartitionRole::windows &&
        explicitly_requested) {
      windows_explicitly_selected = true;
    }
    if (partition.source_table_index == 0U ||
        !analyzed_indexes.emplace(
             partition.source_table_index, true).second ||
        reviewed == nullptr ||
        reviewed->offset_bytes != partition.source_offset_bytes ||
        reviewed->size_bytes != partition.source_size_bytes ||
        !source_style_matches || !type_and_role_match) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"Windows直接縮小クローンの解析済みpartition extent",
          L"table index、offset、容量、形式、MBR type/active、または解析roleを画面レビュー済みsourceへ一意に対応できません");
    }
  }
  if (matched_requested_indexes.size() !=
          explicitly_requested_indexes.size() ||
      (system_source && !windows_explicitly_selected)) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"Windows直接縮小クローンのpartition選択対応",
        system_source && !windows_explicitly_selected
            ? L"system diskの直接クローンでは解析済みWindows partitionを選択してください"
            : L"選択source table indexを解析済みpartitionへ一意に対応できません");
  }

  imageformat::Sha256Digest partition_snapshot_hash{};
  if (!analysis.partition_snapshot.empty()) {
    auto hashed_snapshot = imageformat::sha256(
        std::span<const std::byte>(analysis.partition_snapshot));
    if (!hashed_snapshot) {
      return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
          hashed_snapshot.error());
    }
    partition_snapshot_hash = hashed_snapshot.take_value();
  }
  if ((convert_mbr_to_gpt || preserve_mbr) &&
      all_zero(partition_snapshot_hash)) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Windows直接縮小クローンのMBR snapshot Hash",
        L"読取り専用で取得したraw MBR partition snapshotを計画へ束縛できません");
  }
  if (preserve_mbr) {
    auto inspected = imageformat::inspect_partition_snapshot_v1(
        analysis.partition_snapshot);
    if (!inspected ||
        inspected.value().style != imageformat::PartitionTableStyle::mbr ||
        inspected.value().logical_sector_size != 512U ||
        inspected.value().regions.size() != 1U ||
        inspected.value().regions.front().disk_offset != 0U ||
        inspected.value().regions.front().data.size() != 512U) {
      return inspected
          ? failure<WindowsDirectShrinkClonePlan>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Windows直接縮小クローンのraw MBR snapshot",
                L"canonical snapshotから512-byte raw sector0を一意に復元できません")
          : clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
                inspected.error());
    }
    const auto& sector0 = inspected.value().regions.front().data;
    const auto sector0_hash = imageformat::sha256(sector0);
    std::uint32_t source_signature{};
    std::memcpy(
        &source_signature,
        sector0.data() + 440U,
        sizeof(source_signature));
    const auto& binding = request.mbr_preserve_binding.value();
    if (!sector0_hash || sector0_hash.value() != binding.source_sector0_hash ||
        !std::equal(
            binding.source_bootstrap.begin(),
            binding.source_bootstrap.end(),
            sector0.begin()) ||
        binding.source_bootstrap != analysis.mbr_bootstrap ||
        binding.source_disk_signature != source_signature ||
        binding.target_disk_signature == 0U ||
        binding.target_disk_signature == binding.source_disk_signature ||
        all_zero(binding.planning_signature_inventory_hash)) {
      return !sector0_hash
          ? clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
                sector0_hash.error())
          : failure<WindowsDirectShrinkClonePlan>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"Windows直接縮小クローンのraw MBR binding",
                L"sector0 Hash、source bootstrap、source/fresh target署名、または計画時signature inventoryが一致しません");
    }
  }

  migrationcore::DirectClonePlanningRequest direct_request{
      .mode_choice = request.mode_choice,
      .partition_style_choice = request.partition_style_choice,
      .source_style = analysis.partition_style,
      .source_size_bytes = analysis.source.size_bytes,
      .source_logical_sector_size = analysis.source.logical_sector_size,
      .target_size_bytes = expected_target.value().size_bytes,
      .target_logical_sector_size =
          expected_target.value().logical_sector_size,
      .source_is_windows_system = system_source,
      .windows_is_amd64 = !system_source ||
          (analysis.windows_version.has_value() &&
           analysis.windows_version->architecture == "AMD64"),
      .bitlocker_fully_decrypted = analysis.bitlocker_fully_decrypted,
      .mbr_to_gpt_eligible = convert_mbr_to_gpt && system_source &&
          analysis.windows_version.has_value() &&
          analysis.windows_version->major == 10U &&
          analysis.windows_version->build != 0U &&
          analysis.windows_version->architecture == "AMD64",
      .surplus_allocation = request.surplus_allocation,
      .surplus_target_source_table_index =
          request.surplus_target_source_table_index,
  };
  direct_request.source_partitions.reserve(analysis.partitions.size());
  for (const auto& partition : analysis.partitions) {
    const auto planning_file_system =
        partition.role == migrationcore::MigrationPartitionRole::efi_system
        ? migrationcore::MigrationFileSystem::fat32
        : partition.file_system;
    const bool selected = request.selected_source_table_indexes.empty() ||
        explicitly_requested_indexes.contains(partition.source_table_index);
    direct_request.source_partitions.push_back(
        migrationcore::DirectCloneSourcePartition{
            .partition = migrationcore::ShrinkSourcePartition{
                .source_table_index = partition.source_table_index,
                .role = partition.role,
                // Source analysis intentionally classifies the existing ESP
                // as opaque RAW. Direct shrink never copies that payload: the
                // normalized GPT planner generates a fresh FAT32 ESP and the
                // signed BCDBoot finalizer reconstructs its contents.
                .file_system = planning_file_system,
                .source_size_bytes = partition.source_size_bytes,
                .used_bytes = partition.used_bytes,
                .minimum_target_bytes = 0U,
                .cluster_size = partition.cluster_size,
                .label = partition.label,
                .active = partition.active,
            },
            .selected = selected,
            .required_for_windows =
                partition.role ==
                migrationcore::MigrationPartitionRole::recovery,
        });
  }
  auto direct_plan = migrationcore::plan_direct_clone(direct_request);
  if (!direct_plan) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        direct_plan.error());
  }

  std::map<std::uint32_t, bool> retained_ntfs_indexes;
  std::map<std::uint32_t, bool> retained_raw_indexes;
  for (const auto& partition : direct_plan.value().target_partitions()) {
    if (partition.source_table_index.has_value() &&
        partition.transfer ==
            migrationcore::DirectClonePartitionTransfer::
                file_system_content) {
      retained_ntfs_indexes.emplace(*partition.source_table_index, true);
    } else if (
        partition.source_table_index.has_value() &&
        partition.transfer ==
            migrationcore::DirectClonePartitionTransfer::exact_content &&
        partition.file_system ==
            migrationcore::MigrationFileSystem::unsupported) {
      retained_raw_indexes.emplace(*partition.source_table_index, true);
    }
  }

  auto source_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_source);
  auto target_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_target);
  if (!source_layout || !target_layout) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }

  WindowsDirectShrinkPlanningRequest plan_request{
      .administrator = request.administrator,
      .bitlocker_fully_decrypted = analysis.bitlocker_fully_decrypted,
      .target_is_active_rescue_media =
          request.target_is_active_rescue_media,
      .reviewed_source = request.reviewed_source,
      .reviewed_target = request.reviewed_target,
      .expected_source = expected_source.take_value(),
      .expected_target = expected_target.take_value(),
      .expected_source_layout_hash = source_layout.take_value(),
      .expected_target_layout_hash = target_layout.take_value(),
      .expected_source_partition_snapshot_hash = partition_snapshot_hash,
      .mbr_preserve_binding = request.mbr_preserve_binding,
      .operation_id = request.operation_id,
  };
  plan_request.ntfs_volumes.reserve(retained_ntfs_indexes.size());
  std::map<std::uint32_t, bool> retained_volume_indexes;
  for (const auto& volume : analysis.content_volumes) {
    const auto partition = std::find_if(
        analysis.partitions.begin(),
        analysis.partitions.end(),
        [&](const windowsshrink::AnalyzedShrinkPartition& candidate) {
          return candidate.source_table_index == volume.source_table_index;
        });
    if (partition == analysis.partitions.end() ||
        partition->file_system !=
            migrationcore::MigrationFileSystem::ntfs) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Windows直接縮小クローンの解析済みVolume対応",
          L"NTFS Volume GUIDと解析済みpartition extentを一意に対応できません");
    }
    if (!retained_ntfs_indexes.contains(volume.source_table_index)) {
      continue;
    }
    if (!retained_volume_indexes.emplace(
             volume.source_table_index, true).second) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Windows直接縮小クローンの保持Volume対応",
          L"一つの保持source partitionへ複数のVolume GUIDが対応しています");
    }
    plan_request.ntfs_volumes.push_back(WindowsDirectShrinkNtfsVolume{
        .source_table_index = volume.source_table_index,
        .source_offset_bytes = partition->source_offset_bytes,
        .source_size_bytes = partition->source_size_bytes,
        .original_volume_guid_path = volume.volume_guid_path,
    });
  }
  if (retained_volume_indexes.size() != retained_ntfs_indexes.size()) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"Windows直接縮小クローンの保持Volume集合",
        L"targetへ残る全NTFS source partitionのVolume GUIDが揃っていません");
  }
  plan_request.exact_raw_partitions.reserve(retained_raw_indexes.size());
  for (const auto& partition : analysis.partitions) {
    if (!retained_raw_indexes.contains(partition.source_table_index)) {
      continue;
    }
    if (partition.file_system !=
            migrationcore::MigrationFileSystem::unsupported ||
        partition.role != migrationcore::MigrationPartitionRole::data ||
        all_zero(partition.type_id)) {
      return failure<WindowsDirectShrinkClonePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows直接縮小exact RAW解析",
          L"未対応FSはdata role、認証済みpartition type、元区画全extentを保持した場合だけRAW転送できます");
    }
    plan_request.exact_raw_partitions.push_back(
        WindowsDirectShrinkExactRawPartition{
            .source_table_index = partition.source_table_index,
            .source_offset_bytes = partition.source_offset_bytes,
            .source_size_bytes = partition.source_size_bytes,
            .source_partition_type = partition.type_id,
        });
  }
  if (plan_request.exact_raw_partitions.size() !=
      retained_raw_indexes.size()) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"Windows直接縮小exact RAW集合",
        L"targetへ残る全未対応FSのsource extent/typeが解析結果へ揃っていません");
  }
  return build_windows_direct_shrink_clone_plan(
      plan_request, direct_plan.value());
}

namespace {

struct WindowsDirectShrinkObservedSource final {
  windowsshrink::ShrinkSourceAnalysis analysis;
  WindowsDirectShrinkPartitionCapacityInspection inspection;
};

clonecore::Result<WindowsDirectShrinkObservedSource>
observe_windows_direct_shrink_source_with_windows_apis(
    const WindowsDirectShrinkProductPlanningRequest& request) {
  const auto reviewed_style = diskmodel::normalize_disk_partition_style(
      request.reviewed_source.partition_style,
      request.reviewed_source.partitions.size());
  const bool preserve_gpt =
      request.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      reviewed_style == diskmodel::PartitionStyle::gpt;
  const bool preserve_mbr =
      request.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      reviewed_style == diskmodel::PartitionStyle::mbr;
  const bool convert_mbr_to_gpt =
      request.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::mbr_to_gpt &&
      reviewed_style == diskmodel::PartitionStyle::mbr &&
      request.reviewed_source.is_system_disk;
  if (request.mode_choice !=
          migrationcore::DirectCloneModeChoice::shrink ||
      (!preserve_gpt && !preserve_mbr && !convert_mbr_to_gpt)) {
    return failure<WindowsDirectShrinkObservedSource>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンの製品経路",
        request.mode_choice != migrationcore::DirectCloneModeChoice::shrink
            ? L"Windows版はfilesystem reconstructionを行うshrink modeだけを実行します"
            : L"Windows版はGPT/基本primary MBR形式維持またはsystem MBRからGPTへのtarget-only再構築だけを実行します");
  }
  std::map<std::uint32_t, bool> selected_indexes;
  for (const std::uint32_t index :
       request.selected_source_table_indexes) {
    if (index == 0U || !selected_indexes.emplace(index, true).second) {
      return failure<WindowsDirectShrinkObservedSource>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"Windows直接縮小クローンの製品partition選択",
          L"選択source table indexは1以上かつ重複なしでなければなりません");
    }
  }
  const bool targets_selected_data = request.surplus_allocation ==
      migrationcore::ShrinkSurplusAllocation::selected_data_partition;
  if (targets_selected_data !=
          request.surplus_target_source_table_index.has_value() ||
      (request.surplus_target_source_table_index.has_value() &&
       *request.surplus_target_source_table_index == 0U)) {
    return failure<WindowsDirectShrinkObservedSource>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windows直接縮小クローンの余剰容量対象",
        L"指定データ領域への配分には1以上のsource table indexが1つ必要です");
  }
  if (!request.administrator || all_zero(request.operation_id) ||
      (request.surplus_allocation !=
           migrationcore::ShrinkSurplusAllocation::leave_unallocated &&
       request.surplus_allocation !=
           migrationcore::ShrinkSurplusAllocation::automatic_proportional &&
       request.surplus_allocation !=
           migrationcore::ShrinkSurplusAllocation::selected_data_partition) ||
      request.analysis_created_utc.empty() || request.app_version.empty() ||
      (request.reviewed_source.is_system_disk &&
       (request.windows_major != 10U || request.windows_build == 0U ||
        request.windows_architecture != "AMD64"))) {
    return failure<WindowsDirectShrinkObservedSource>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows直接縮小クローンのWindows解析情報",
        L"Windows 10/11 AMD64の版、解析時刻、またはアプリ版を確定できません");
  }
  auto expected_source = diskmodel::make_stable_disk_identity(
      request.reviewed_source, request.reviewed_source.is_system_disk);
  auto expected_target = diskmodel::make_stable_disk_identity(
      request.reviewed_target, false);
  auto source_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_source);
  auto target_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_target);
  if (!expected_source || !expected_target || !source_layout ||
      !target_layout) {
    return clonecore::Result<WindowsDirectShrinkObservedSource>::failure(
        !expected_source
            ? expected_source.error()
            : !expected_target
                  ? expected_target.error()
                  : !source_layout ? source_layout.error()
                                   : target_layout.error());
  }
  auto reviewed = validate_reviewed_identities_and_layouts(
      WindowsDirectShrinkPlanningRequest{
          .administrator = request.administrator,
          .bitlocker_fully_decrypted = true,
          .target_is_active_rescue_media =
              request.target_is_active_rescue_media,
          .reviewed_source = request.reviewed_source,
          .reviewed_target = request.reviewed_target,
          .expected_source = expected_source.value(),
          .expected_target = expected_target.value(),
          .expected_source_layout_hash = source_layout.value(),
          .expected_target_layout_hash = target_layout.value(),
          .operation_id = request.operation_id,
      });
  if (!reviewed) {
    return clonecore::Result<WindowsDirectShrinkObservedSource>::failure(
        reviewed.error());
  }
  auto opened =
      diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
          expected_source.value());
  if (!opened) {
    return clonecore::Result<WindowsDirectShrinkObservedSource>::failure(
        opened.error());
  }
  if (diskmodel::normalize_disk_partition_style(
          opened.value().observed.observed.partition_style,
          opened.value().observed.observed.partitions.size()) !=
      reviewed_style) {
    return failure<WindowsDirectShrinkObservedSource>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows直接縮小クローンの読取り元形式照合",
        L"レビュー後にコピー元のpartition styleが変化しました");
  }
  auto reopened_source_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          opened.value().observed.observed);
  if (!reopened_source_layout ||
      reopened_source_layout.value() != source_layout.value()) {
    return reopened_source_layout
        ? failure<WindowsDirectShrinkObservedSource>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"Windows直接縮小クローンの読取り元layout Hash",
              L"画面選択後にコピー元のpartition配置または識別が変化しました")
        : clonecore::Result<WindowsDirectShrinkObservedSource>::failure(
              reopened_source_layout.error());
  }
  windowsshrink::ShrinkSourceAnalysisContext context{
      .source_identity = opened.value().observed.identity,
      .physical_sector_size =
          opened.value().observed.observed.physical_sector_size,
      .created_utc = request.analysis_created_utc,
      .app_version = request.app_version,
  };
  if (request.reviewed_source.is_system_disk) {
    context.known_windows_version = windowsshrink::WindowsSourceVersion{
        .major = request.windows_major,
        .minor = request.windows_minor,
        .build = request.windows_build,
        .architecture = request.windows_architecture,
    };
  }
  auto analysis = [&]()
      -> clonecore::Result<windowsshrink::ShrinkSourceAnalysis> {
    if (preserve_gpt) {
      auto gpt = clonecore::parse_gpt(*opened.value().reader);
      if (!gpt) {
        return clonecore::Result<
            windowsshrink::ShrinkSourceAnalysis>::failure(gpt.error());
      }
      return windowsshrink::analyze_gpt_shrink_source_with_windows_apis(
          opened.value().observed.observed,
          *opened.value().reader,
          gpt.value(),
          context);
    }
    auto mbr = clonecore::parse_mbr(*opened.value().reader);
    if (!mbr) {
      return clonecore::Result<
          windowsshrink::ShrinkSourceAnalysis>::failure(mbr.error());
    }
    return windowsshrink::analyze_mbr_shrink_source_with_windows_apis(
        opened.value().observed.observed,
        *opened.value().reader,
        mbr.value(),
        context);
  }();
  if (!analysis) {
    return clonecore::Result<WindowsDirectShrinkObservedSource>::failure(
        analysis.error());
  }
  auto analysis_hash = hash_windows_direct_shrink_source_analysis_v1(
      analysis.value());
  if (!analysis_hash) {
    return clonecore::Result<WindowsDirectShrinkObservedSource>::failure(
        analysis_hash.error());
  }
  WindowsDirectShrinkObservedSource observed;
  observed.inspection.binding = WindowsClonePartitionCapacityBinding{
      .source = analysis.value().source,
      .source_partition_style = analysis.value().partition_style,
      .source_layout_hash = source_layout.value(),
      .source_analysis_hash = analysis_hash.take_value(),
  };
  observed.inspection.candidates.reserve(analysis.value().partitions.size());
  for (const auto& partition : analysis.value().partitions) {
    observed.inspection.candidates.push_back(
        WindowsClonePartitionCapacityCandidate{
            .partition = migrationcore::ShrinkSourcePartition{
                .source_table_index = partition.source_table_index,
                .role = partition.role,
                .file_system = partition.file_system,
                .source_size_bytes = partition.source_size_bytes,
                .used_bytes = partition.used_bytes,
                .minimum_target_bytes = 0U,
                .cluster_size = partition.cluster_size,
                .label = partition.label,
                .active = partition.active,
            },
            .required_for_windows =
                partition.role ==
                migrationcore::MigrationPartitionRole::recovery,
        });
  }
  observed.analysis = analysis.take_value();
  return clonecore::Result<WindowsDirectShrinkObservedSource>::success(
      std::move(observed));
}

clonecore::Status require_exact_partition_capacity_binding(
    const WindowsClonePartitionCapacityBinding& expected,
    const WindowsClonePartitionCapacityBinding& observed) {
  auto status = clonecore::validate_stable_identity(
      expected.source, observed.source, L"パーティション・容量設定コピー元");
  if (!status) {
    return status;
  }
  if (expected.source.is_system_disk != observed.source.is_system_disk ||
      expected.source_partition_style != observed.source_partition_style ||
      expected.source_layout_hash != observed.source_layout_hash ||
      expected.source_analysis_hash != observed.source_analysis_hash) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"パーティション・容量設定の計画直前再解析",
        L"決定後にコピー元のsystem属性、形式、exact layout、またはanalysisが変化しました");
  }
  return clonecore::success_status();
}

clonecore::Result<WindowsDirectShrinkClonePlan>
build_product_plan_with_fresh_mbr_binding(
    const WindowsDirectShrinkProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis) {
  WindowsDirectShrinkProductPlanningRequest bound_request = request;
  // Product callers cannot inject stale inventory evidence. GPT preserve and
  // MBR-to-GPT keep this absent; MBR preserve always replaces it below.
  bound_request.mbr_preserve_binding.reset();
  const bool preserve_mbr = request.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      analysis.partition_style == migrationcore::MigrationPartitionStyle::mbr;
  if (!preserve_mbr) {
    return build_windows_direct_shrink_clone_plan_from_analysis(
        bound_request, analysis);
  }

  auto expected_target = diskmodel::make_stable_disk_identity(
      request.reviewed_target, false);
  if (!expected_target) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        expected_target.error());
  }
  auto observed = observe_windows_direct_shrink_mbr_safety_with_windows_apis(
      analysis.source, expected_target.value(), true);
  if (!observed) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        observed.error());
  }
  std::vector<std::uint32_t> all_signatures =
      observed.value().connected_mbr_signatures_excluding_target;
  if (observed.value().target_mbr_signature.has_value()) {
    all_signatures.push_back(*observed.value().target_mbr_signature);
  }
  auto inventory_hash = hash_mbr_signature_inventory(all_signatures);
  auto generator = clonecore::make_windows_mbr_signature_generator();
  if (!inventory_hash || !generator) {
    return !inventory_hash
        ? clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
              inventory_hash.error())
        : failure<WindowsDirectShrinkClonePlan>(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_HANDLE,
              L"Windows直接縮小fresh MBR signature generator",
              L"Windows暗号学的乱数generatorを作成できません");
  }
  std::uint32_t target_signature{};
  constexpr std::size_t kMaximumAttempts = 32U;
  for (std::size_t attempt = 0U; attempt < kMaximumAttempts; ++attempt) {
    auto candidate = generator->next_signature();
    if (!candidate) {
      return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
          candidate.error());
    }
    if (candidate.value() != 0U &&
        std::find(
            all_signatures.begin(),
            all_signatures.end(),
            candidate.value()) == all_signatures.end()) {
      target_signature = candidate.value();
      break;
    }
  }
  if (target_signature == 0U) {
    return failure<WindowsDirectShrinkClonePlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DUP_NAME,
        L"Windows直接縮小fresh MBR signature",
        L"全接続MBRと衝突しない署名を32回以内に生成できません");
  }
  bound_request.mbr_preserve_binding = WindowsDirectShrinkMbrPlanBinding{
      .source_sector0_hash = observed.value().source_sector0_hash,
      .source_bootstrap = observed.value().source_bootstrap,
      .source_disk_signature = observed.value().source_disk_signature,
      .target_disk_signature = target_signature,
      .planning_signature_inventory_hash = inventory_hash.take_value(),
  };
  return build_windows_direct_shrink_clone_plan_from_analysis(
      bound_request, analysis);
}

}  // namespace

clonecore::Result<WindowsDirectShrinkPartitionCapacityInspection>
inspect_windows_direct_shrink_partition_capacity_with_windows_apis(
    const WindowsDirectShrinkProductPlanningRequest& request) {
  auto observed =
      observe_windows_direct_shrink_source_with_windows_apis(request);
  if (!observed) {
    return clonecore::Result<
        WindowsDirectShrinkPartitionCapacityInspection>::failure(
        observed.error());
  }
  return clonecore::Result<
      WindowsDirectShrinkPartitionCapacityInspection>::success(
      std::move(observed.value().inspection));
}

clonecore::Result<WindowsDirectShrinkClonePlan>
plan_windows_direct_shrink_clone_with_windows_apis(
    const WindowsDirectShrinkProductPlanningRequest& request) {
  auto observed =
      observe_windows_direct_shrink_source_with_windows_apis(request);
  if (!observed) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        observed.error());
  }
  return build_product_plan_with_fresh_mbr_binding(
      request, observed.value().analysis);
}

clonecore::Result<WindowsDirectShrinkClonePlan>
plan_windows_direct_shrink_clone_after_partition_review_with_windows_apis(
    const WindowsDirectShrinkProductPlanningRequest& request,
    const WindowsClonePartitionCapacityBinding& completed_review_binding) {
  auto observed =
      observe_windows_direct_shrink_source_with_windows_apis(request);
  if (!observed) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        observed.error());
  }
  auto status = require_exact_partition_capacity_binding(
      completed_review_binding, observed.value().inspection.binding);
  if (!status) {
    return clonecore::Result<WindowsDirectShrinkClonePlan>::failure(
        status.error());
  }
  // No further source observation occurs here: the immutable plan is built
  // immediately from the exact analysis whose digest was compared above.
  return build_product_plan_with_fresh_mbr_binding(
      request, observed.value().analysis);
}

clonecore::Result<WindowsDirectShrinkCloneOperationReport>
execute_windows_direct_shrink_clone(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkCloneExecutionOptions& options,
    const WindowsDirectShrinkCloneDependencies& dependencies) {
  auto status = operationcore::validate_operation_plan(plan.operation_plan());
  if (status) {
    status = operationcore::validate_operation_confirmation(
        plan.operation_plan(), options.confirmation.typed_token);
  }
  if (!status || !options.confirmation.first_step_acknowledged ||
      !dependencies.reidentify_selection ||
      !dependencies.reidentify_confirmed ||
      !dependencies.run_snapshot_workflow || !dependencies.make_platform ||
      (is_mbr_preserving_plan(plan) && !dependencies.observe_mbr_safety) ||
      (exact_raw_task_count(plan) != 0U &&
       !dependencies.open_read_only_raw_source) ||
      plan.archive_task_count() == 0U || plan.workflow().volumes.empty() ||
      plan.workflow().volumes.size() != plan.archive_task_count() ||
      all_zero(plan.final_layout_hash())) {
    return failure<WindowsDirectShrinkCloneOperationReport>(
        clonecore::ErrorCode::invalid_argument,
        status ? ERROR_INVALID_PARAMETER : status.error().native_code,
        L"Windows直接縮小クローンの実行依存",
        !options.confirmation.first_step_acknowledged
            ? L"コピー先の一段目確認が完了していません"
            : status
                ? L"不変計画、再識別、VSS、target platform、または最終layout Hashが不足しています"
                : status.error().message);
  }

  // Copy the string-owning plan before any target write. The final operation
  // report can then move it after publication without a late allocation.
  operationcore::OperationPlan report_plan = plan.operation_plan();
  std::optional<WindowsDirectShrinkCloneExecutionReport> execution;
  operationcore::OperationCallbacks callbacks{
      .reidentify =
          [&](const operationcore::OperationPlan&) {
            auto observed = dependencies.reidentify_selection(
                plan.expected_source(), plan.expected_target());
            if (!observed) {
              return clonecore::Result<
                  operationcore::ReidentifiedOperation>::failure(
                  observed.error());
            }
            const auto valid = validate_observed_clone(
                plan, observed.value(), nullptr);
            if (!valid) {
              return clonecore::Result<
                  operationcore::ReidentifiedOperation>::failure(
                  valid.error());
            }
            return clonecore::Result<
                operationcore::ReidentifiedOperation>::success({
                .source = observed.value().source_identity,
                .target = observed.value().target_identity,
            });
          },
      .execute =
          [&](const operationcore::OperationPlan&,
              const clonecore::DiskOperationCallbacks&) {
            auto result = run_execution(plan, options, dependencies);
            if (!result) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  result.error());
            }
            execution = result.take_value();
            return clonecore::Result<
                operationcore::ExecutionEvidence>::success({
                .processed_work_bytes = execution->verified_target_bytes,
                .output_hash = execution->selected_completion_hash,
            });
          },
      .verify =
          [&](const operationcore::OperationPlan&,
              const operationcore::ExecutionEvidence& executed,
              const clonecore::DiskOperationCallbacks&) {
            if (!execution) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windows直接縮小クローンの検証結果",
                  L"実行証跡がありません");
            }
            if (executed.processed_work_bytes !=
                    execution->verified_target_bytes ||
                executed.output_hash !=
                    execution->selected_completion_hash) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windows直接縮小クローンの証跡照合",
                  L"実行時と最終検証時の容量または証跡Hashが一致しません");
            }
            return clonecore::Result<
                operationcore::VerificationEvidence>::success({
                .verified_work_bytes = execution->verified_target_bytes,
                .output_hash = execution->selected_completion_hash,
            });
          },
      .disk_operation = options.callbacks,
  };
  auto lifecycle = operationcore::run_operation(
      plan.operation_plan(), options.confirmation.typed_token, callbacks);
  return clonecore::Result<WindowsDirectShrinkCloneOperationReport>::success({
      .plan = std::move(report_plan),
      .lifecycle = std::move(lifecycle),
      .execution = std::move(execution),
  });
}

WindowsDirectShrinkCloneDependencies
make_windows_direct_shrink_clone_dependencies(
    const WindowsDirectShrinkCloneExecutionOptions& options) {
  const clonecore::TargetConfirmation confirmation = options.confirmation;
  const clonecore::DiskOperationCallbacks callbacks = options.callbacks;
  return WindowsDirectShrinkCloneDependencies{
      .reidentify_selection =
          [](const clonecore::StableDiskIdentity& source,
             const clonecore::StableDiskIdentity& target) {
            auto inventory = diskmodel::make_windows_disk_inventory_provider();
            return diskmodel::reidentify_physical_clone_selection(
                source, target, *inventory, false);
          },
      .reidentify_confirmed =
          [](const clonecore::StableDiskIdentity& source,
             const clonecore::StableDiskIdentity& target,
             const clonecore::TargetConfirmation& confirmed) {
            auto inventory = diskmodel::make_windows_disk_inventory_provider();
            return diskmodel::reidentify_physical_clone(
                source, target, confirmed, *inventory, false);
          },
      .run_snapshot_workflow =
          [](const vssrequester::WorkflowRequest& workflow,
             const vssrequester::AsyncWaitOptions& async_wait,
             const clonecore::Logger* logger,
             vssrequester::SnapshotCopyCallback callback) {
            vssrequester::WindowsVssBackend backend(
                vssrequester::WindowsVssBackendOptions{
                    .async_wait = async_wait,
                    .copy_snapshot_data = std::move(callback),
                    .logger = logger,
                });
            return vssrequester::execute_backup_workflow(workflow, backend);
          },
      .make_platform =
          [confirmation, callbacks](
              const WindowsDirectShrinkClonePlan& plan,
              const diskmodel::ReidentifiedPhysicalClone& observed) {
            return make_windows_direct_shrink_clone_platform(
                plan,
                observed,
                WindowsDirectShrinkClonePlatformRequest{
                    .confirmation = confirmation,
                    .callbacks = callbacks,
                });
          },
      .observe_mbr_safety =
          observe_windows_direct_shrink_mbr_safety_with_windows_apis,
      .open_read_only_raw_source =
          diskmodel::open_verified_read_only_physical_disk_with_windows_apis,
  };
}

clonecore::Result<WindowsDirectShrinkCloneOperationReport>
execute_windows_direct_shrink_clone_with_windows_apis(
    const WindowsDirectShrinkClonePlan& plan,
    const WindowsDirectShrinkCloneExecutionOptions& options) {
  auto source_token =
      vssrequester::encode_vss_diff_area_source_epoch_token(
          std::span<const std::byte>(
              plan.operation_plan().immutable_payload_hash));
  if (!source_token || !options.diff_area_review_callback) {
    return failure<WindowsDirectShrinkCloneOperationReport>(
        source_token
            ? clonecore::ErrorCode::invalid_argument
            : source_token.error().code,
        source_token ? ERROR_INVALID_PARAMETER
                     : source_token.error().native_code,
        L"Windows直接縮小VSS差分領域監視",
        source_token
            ? L"source epochまたは利用者review callbackがありません"
            : source_token.error().message);
  }
  const std::wstring expected_source_token = source_token.take_value();
  WindowsDirectShrinkCloneDependencies dependencies =
      make_windows_direct_shrink_clone_dependencies(options);
  dependencies.make_diff_area_monitor =
      [&plan, options, expected_source_token](
          const vssrequester::SnapshotCopyContext& context) {
        return vssrequester::make_windows_vss_diff_area_operation_monitor(
            context,
            vssrequester::WindowsVssDiffAreaOperationMonitorOptions{
                .expected_source_identity_token = expected_source_token,
                .probe_source_identity =
                    [&plan, options, expected_source_token](
                        const vssrequester::VssDiffAreaSnapshotBinding&) {
                      auto inventory =
                          diskmodel::make_windows_disk_inventory_provider();
                      auto observed = diskmodel::reidentify_physical_clone(
                          plan.expected_source(),
                          plan.expected_target(),
                          options.confirmation,
                          *inventory,
                          false);
                      if (!observed) {
                        return clonecore::Result<std::wstring>::failure(
                            observed.error());
                      }
                      const auto status = validate_observed_clone(
                          plan,
                          observed.value(),
                          &options.confirmation);
                      return status
                          ? clonecore::Result<std::wstring>::success(
                                expected_source_token)
                          : clonecore::Result<std::wstring>::failure(
                                status.error());
                    },
                .review_callback = options.diff_area_review_callback,
                .logger = options.logger,
            });
      };
  return execute_windows_direct_shrink_clone(plan, options, dependencies);
}

}  // namespace ytec::windowsapp

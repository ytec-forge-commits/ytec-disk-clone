#include "ytec/winpeapp/offline_ntfs_direct_shrink_clone.h"

#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

clonecore::Error offline_shrink_error(
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
  return clonecore::Result<T>::failure(offline_shrink_error(
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
  return clonecore::Status::failure(offline_shrink_error(
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
  // disk_number and is_system_disk are observations of the current boot, not
  // stable hardware identity fields.  validate_stable_identity deliberately
  // applies the same model/size/sector/serial/device-instance definition.
  return left.model == right.model &&
      left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.serial_suffix == right.serial_suffix &&
      left.device_instance_id == right.device_instance_id;
}

std::uint64_t exact_raw_task_count(
    const directshrink::TargetPlan& plan) noexcept {
  return static_cast<std::uint64_t>(std::count_if(
      plan.tasks().begin(),
      plan.tasks().end(),
      [](const directshrink::PartitionTask& task) {
        return task.kind == directshrink::PartitionTaskKind::copy_exact_raw;
      }));
}

const WinPeOfflineNtfsVolumeBinding* find_epoch_volume(
    const WinPeOfflineNtfsSourceEpochEvidence& epoch,
    const std::uint32_t source_table_index) noexcept {
  const WinPeOfflineNtfsVolumeBinding* found = nullptr;
  for (const auto& volume : epoch.ntfs_volumes) {
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

clonecore::Status validate_source_epoch_against_plan(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const WinPeOfflineNtfsSourceEpochEvidence& epoch,
    const std::wstring_view operation) {
  const auto& target_plan = plan.target_plan();
  auto stable = clonecore::validate_stable_identity(
      target_plan.expected_source(), epoch.observed_source, L"WinPEコピー元");
  if (!stable) {
    return stable;
  }
  auto canonical = hash_winpe_offline_ntfs_source_epoch_v1(epoch);
  if (!canonical) {
    return clonecore::Status::failure(canonical.error());
  }
  const auto& planned = plan.planning_evidence().source_epoch;
  if (!stable_identity_exactly_matches(
          planned.observed_source, epoch.observed_source) ||
      epoch.source_layout_hash != target_plan.expected_source_layout_hash() ||
      epoch.source_partition_snapshot_hash !=
          target_plan.source_partition_snapshot_hash() ||
      epoch.source_analysis_hash != planned.source_analysis_hash ||
      canonical.value() != epoch.canonical_epoch_hash ||
      epoch.canonical_epoch_hash != planned.canonical_epoch_hash ||
      epoch.logical_sector_size != 512U ||
      epoch.logical_sector_size !=
          target_plan.expected_source().logical_sector_size ||
      !epoch.stable_identity_reidentified || !epoch.source_os_read_only ||
      !epoch.physical_handle_read_only || !epoch.gpt_source ||
      !epoch.whole_disk_analysis || !epoch.bitlocker_fully_decrypted) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        std::wstring(operation),
        L"コピー元の安定識別、read-only属性、canonical layout、partition snapshot、analysis、またはsource epochが計画時と一致しません");
  }
  std::uint64_t archive_count{};
  for (const auto& task : target_plan.tasks()) {
    if (task.kind != directshrink::PartitionTaskKind::apply_ntfs_wim) {
      continue;
    }
    ++archive_count;
    if (!task.source_table_index.has_value()) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          std::wstring(operation),
          L"NTFS capture taskにsource table indexがありません");
    }
    const auto* volume = find_epoch_volume(epoch, *task.source_table_index);
    if (volume == nullptr ||
        volume->source_offset_bytes != task.source_offset_bytes ||
        volume->source_size_bytes != task.source_size_bytes ||
        !equal_path(volume->volume_guid_path, task.original_volume_guid_path)) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          std::wstring(operation),
          L"レビュー済みsource table index、物理extent、またはVolume GUID rootを現在のread-only sourceへ一意に再対応できません");
    }
  }
  if (archive_count == 0U ||
      archive_count != target_plan.archive_task_count() ||
      epoch.ntfs_volumes.size() != archive_count) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"選択NTFS task数とsource epochのVolume binding数が一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_observed_clone(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const clonecore::TargetConfirmation* confirmation) {
  const auto& target_plan = plan.target_plan();
  auto status = confirmation != nullptr
      ? clonecore::validate_clone_identities(
            target_plan.expected_source(),
            observed.source_identity,
            target_plan.expected_target(),
            observed.target_identity,
            *confirmation,
            false)
      : clonecore::validate_clone_selection(
            target_plan.expected_source(),
            observed.source_identity,
            target_plan.expected_target(),
            observed.target_identity,
            false);
  if (!status) {
    return status;
  }
  if (!observed.source.read_only.has_value() ||
      !observed.source.offline.has_value() ||
      !observed.source.removable.has_value() ||
      !observed.target.offline.has_value() ||
      !observed.target.read_only.has_value() ||
      !observed.target.removable.has_value() ||
      !observed.source.read_only.value() ||
      observed.source.offline.value() || observed.source.removable.value() ||
      observed.target.read_only.value() || observed.target.removable.value() ||
      observed.target.is_system_disk ||
      observed.source.logical_sector_size != 512U ||
      observed.target.logical_sector_size != 512U ||
      diskmodel::normalize_disk_partition_style(
          observed.source.partition_style,
          observed.source.partitions.size()) !=
          diskmodel::PartitionStyle::gpt) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        confirmation == nullptr
            ? L"WinPE直接縮小の確認前ディスク属性"
            : L"WinPE直接縮小のtarget I/O直前ディスク属性",
        L"OSレベルread-onlyのGPT固定コピー元と、分離された512-byte固定・非systemコピー先だけを扱います");
  }
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.target);
  if (!source_layout || !target_layout) {
    return clonecore::Status::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  if (source_layout.value() != target_plan.expected_source_layout_hash() ||
      target_layout.value() != target_plan.expected_target_layout_hash()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        confirmation == nullptr
            ? L"WinPE直接縮小の確認前canonical layout"
            : L"WinPE直接縮小のtarget I/O直前canonical layout",
        L"計画後にコピー元またはコピー先のcanonical layoutが変化しました");
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::Sha256Digest> make_winpe_immutable_payload_hash(
    const directshrink::TargetPlan& target_plan,
    const WinPeOfflineNtfsPlanningEvidence& evidence) {
  constexpr std::string_view kDomain =
      "YTEC-WINPE-OFFLINE-NTFS-DIRECT-SHRINK-PLAN-V2";
  std::vector<std::byte> bytes;
  bytes.reserve(256U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_array(bytes, target_plan.operation_plan().immutable_payload_hash);
  append_array(bytes, target_plan.expected_source_layout_hash());
  append_array(bytes, target_plan.expected_target_layout_hash());
  append_array(bytes, target_plan.source_partition_snapshot_hash());
  append_array(bytes, target_plan.final_layout_hash());
  append_array(bytes, evidence.source_epoch.canonical_epoch_hash);
  append_string(bytes, evidence.analysis_created_utc);
  append_string(bytes, evidence.app_version);
  append_u8(bytes, evidence.winpe_environment_verified ? 1U : 0U);
  append_u8(bytes, evidence.source_contains_windows ? 1U : 0U);
  append_u8(bytes, evidence.source_supported_basic_disk ? 1U : 0U);
  append_u8(bytes, evidence.source_health_allows_standard_clone ? 1U : 0U);
  append_u8(bytes, evidence.target_supported_fixed_disk ? 1U : 0U);
  append_u8(bytes, evidence.target_non_system ? 1U : 0U);
  append_u8(bytes, evidence.target_health_allows_destructive_clone ? 1U : 0U);
  append_u8(
      bytes, static_cast<std::uint8_t>(target_plan.surplus_allocation()));
  append_u8(
      bytes,
      target_plan.surplus_target_source_table_index().has_value() ? 1U : 0U);
  append_u32(
      bytes,
      target_plan.surplus_target_source_table_index().value_or(0U));
  return imageformat::sha256(bytes);
}

clonecore::Result<imageformat::Sha256Digest> combine_digest(
    const imageformat::Sha256Digest& current,
    const imageformat::Sha256Digest& next,
    const std::uint32_t target_number,
    const std::uint64_t verified_bytes) {
  constexpr std::string_view kDomain =
      "YTEC-WINPE-OFFLINE-NTFS-DIRECT-SHRINK-WRITE-DIGEST-V1";
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
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const operationcore::Sha256Digest& operation_plan_hash,
    const directshrink::CheckpointEvidence& checkpoint) {
  auto target = clonecore::validate_stable_identity(
      plan.target_plan().expected_target(),
      checkpoint.observed_target,
      L"WinPE直接縮小checkpointコピー先");
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
        L"WinPE直接縮小checkpoint耐久性",
        L"WinPE計画Hash、対象識別、flush、読戻し、offline、または未完成状態の証跡が不足しています");
  }
  return clonecore::success_status();
}

clonecore::Status validate_checkpoint_transition(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const operationcore::Sha256Digest& operation_plan_hash,
    const directshrink::CheckpointEvidence& previous,
    const directshrink::CheckpointEvidence& current,
    const directshrink::CheckpointPhase expected_phase,
    const std::uint64_t expected_task_count,
    const std::uint64_t expected_verified_bytes,
    const imageformat::Sha256Digest& expected_write_digest) {
  auto status = validate_checkpoint_common(plan, operation_plan_hash, current);
  if (!status) {
    return status;
  }
  if (current.revision <= previous.revision ||
      current.phase != expected_phase ||
      current.staging_identity_hash != previous.staging_identity_hash ||
      current.completed_task_count != expected_task_count ||
      current.verified_target_bytes != expected_verified_bytes ||
      current.aggregate_write_digest != expected_write_digest ||
      current.record_hash == previous.record_hash) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"WinPE直接縮小checkpoint遷移",
        L"revision、phase、staging、進捗、またはrecord Hashが期待した耐久遷移と一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_checkpoint_exactly_unchanged(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const operationcore::Sha256Digest& operation_plan_hash,
    const directshrink::CheckpointEvidence& expected,
    const directshrink::CheckpointEvidence& observed,
    const std::wstring_view operation) {
  auto status = validate_checkpoint_common(plan, operation_plan_hash, observed);
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

clonecore::Status check_safe_boundary(
    const WinPeOfflineNtfsDirectShrinkExecutionOptions& options,
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
    return status_failure(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"WinPE直接縮小クローンの安全境界",
        L"安全なpartition境界で操作を取消しました");
  }
  return clonecore::success_status();
}

void report_progress(
    const WinPeOfflineNtfsDirectShrinkExecutionOptions& options,
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

bool has_valid_targeted_surplus_evidence(
    const directshrink::TargetPlan& plan,
    const directshrink::FinalCommitEvidence& evidence)
    noexcept {
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
      [&plan](const directshrink::PartitionTask& value) {
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

clonecore::Status validate_final_commit(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const imageformat::Sha256Digest& aggregate_digest,
    const directshrink::FinalCommitEvidence& final) {
  const auto& target_plan = plan.target_plan();
  if (final.committed_layout_hash != target_plan.final_layout_hash() ||
      final.aggregate_write_digest != aggregate_digest ||
      !final.source_reidentified || !final.source_layout_unchanged ||
      !final.target_reidentified || !final.staging_identity_reverified ||
      !final.checkpoint_reverified || !final.staging_removed ||
      (final.checkpoint_retired == final.checkpoint_retirement_pending) ||
      !final.construction_layout_non_bootable ||
      !final.checkpoint_retained_through_extensions_and_boot ||
      !final.boot_completed_before_final_layout_publication ||
      !final.final_layout_published_before_checkpoint_retirement ||
      !final.hidden_final_layout_published_and_read_back ||
      final.extended_ntfs_partition_count !=
          target_plan.ntfs_extension_task_count() ||
      !final.every_required_ntfs_extension_verified ||
      !has_valid_targeted_surplus_evidence(target_plan, final) ||
      !final.every_write_flushed || !final.every_write_read_back ||
      !final.primary_layout_committed_last || !final.target_offline ||
      final.final_partition_style !=
          migrationcore::MigrationPartitionStyle::gpt ||
      final.source_mbr_sector0_unchanged ||
      final.source_mbr_bootstrap_unchanged ||
      final.target_mbr_signature_collision_free ||
      final.final_mbr_sector0_read_back_verified ||
      final.final_mbr_disk_signature != 0U ||
      final.final_mbr_active_partition_count != 0U) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"WinPE直接縮小の最終GPT commit証跡",
        L"source再照合、非boot construction、checkpoint保持、staging除去、flush、読戻し、GPT commit-last、またはoffline証跡が不足しています");
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::Sha256Digest> completion_evidence_hash(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const directshrink::CheckpointEvidence& checkpoint,
    const directshrink::BootEvidence& boot,
    const imageformat::Sha256Digest& aggregate_digest,
    const std::uint64_t verified_bytes,
    const std::uint64_t applied_archives,
    const std::uint64_t copied_exact_raw,
    const std::uint64_t source_revalidations,
    const bool checkpoint_retired) {
  constexpr std::string_view kDomain =
      "YTEC-WINPE-OFFLINE-NTFS-DIRECT-SHRINK-COMPLETION-V2";
  std::vector<std::byte> bytes;
  bytes.reserve(320U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_array(bytes, plan.operation_plan().immutable_payload_hash);
  append_array(bytes, plan.planning_evidence().source_epoch.canonical_epoch_hash);
  append_array(bytes, plan.target_plan().final_layout_hash());
  append_array(bytes, checkpoint.record_hash);
  append_array(bytes, aggregate_digest);
  append_u64(bytes, verified_bytes);
  append_u64(bytes, applied_archives);
  append_u64(bytes, copied_exact_raw);
  append_u64(bytes, source_revalidations);
  append_u8(bytes, boot.required ? 1U : 0U);
  append_u8(bytes, boot.completed ? 1U : 0U);
  append_u8(bytes, boot.boot_files_read_back_verified ? 1U : 0U);
  append_u8(bytes, boot.recovery_configuration_verified ? 1U : 0U);
  append_u8(bytes, boot.target_only_reconstruction ? 1U : 0U);
  append_u8(bytes, boot.exact_target_volume_extents ? 1U : 0U);
  append_u8(bytes, boot.real_boot_not_claimed ? 1U : 0U);
  append_u8(bytes, checkpoint_retired ? 1U : 0U);
  append_u8(bytes, checkpoint_retired ? 0U : 1U);
  append_u8(bytes, 1U);  // source remained OS-level read-only
  append_u8(bytes, 1U);  // every capture used a held read-only lease
  append_u8(bytes, 1U);  // source epoch checked after Boot/WinRE
  append_u8(bytes, 1U);  // construction remained non-bootable
  append_u8(bytes, 1U);  // final GPT was committed last
  append_u8(bytes, 1U);  // target remained offline
  append_u8(bytes, 1U);  // real boot is explicitly not claimed
  return imageformat::sha256(bytes);
}

clonecore::Status validate_capture_lease(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const directshrink::PartitionTask& task,
    const WinPeOfflineNtfsCaptureLeaseEvidence& lease) {
  auto epoch = validate_source_epoch_against_plan(
      plan, lease.source_epoch, L"WinPE直接縮小capture直前source epoch");
  if (!epoch) {
    return epoch;
  }
  if (!task.source_table_index.has_value() ||
      lease.volume.source_table_index != *task.source_table_index ||
      lease.volume.source_offset_bytes != task.source_offset_bytes ||
      lease.volume.source_size_bytes != task.source_size_bytes ||
      !equal_path(
          lease.volume.volume_guid_path, task.original_volume_guid_path) ||
      !equal_path(lease.capture_device_path, lease.volume.volume_guid_path) ||
      !is_volume_guid_path(lease.capture_device_path) ||
      lease.capture_identity.empty() ||
      !lease.source_disk_handle_held_read_only ||
      !lease.source_volume_handle_held_read_only ||
      !lease.source_volume_extent_reverified) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"WinPE直接縮小capture lease",
        L"source table index、物理extent、canonical Volume GUID、read-only handle、またはcapture identityが計画と一致しません");
  }
  return clonecore::success_status();
}

clonecore::Result<WinPeOfflineNtfsDirectShrinkExecutionReport> run_execution(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const WinPeOfflineNtfsDirectShrinkExecutionOptions& options,
    const WinPeOfflineNtfsDirectShrinkDependencies& dependencies) {
  auto operation_plan_hash = operationcore::hash_operation_plan(
      plan.operation_plan());
  if (!operation_plan_hash) {
    return clonecore::Result<
        WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
        operation_plan_hash.error());
  }

  std::unique_ptr<IWinPeOfflineNtfsSourceGuard> source_guard;
  std::unique_ptr<directshrink::ITargetPlatform> target;
  std::optional<directshrink::CheckpointEvidence> checkpoint;
  std::optional<directshrink::BootEvidence> boot;
  imageformat::Sha256Digest aggregate_digest{};
  std::uint64_t verified_bytes{};
  std::uint64_t completed_tasks{};
  std::uint64_t applied_archives{};
  std::uint64_t copied_exact_raw{};
  std::uint64_t source_revalidations{};
  bool final_layout_publication_latched = false;

  const auto abort_incomplete = [&]() noexcept {
    if (target) {
      target->abort_keep_offline_incomplete();
    }
  };

  try {
    auto observed = dependencies.reidentify_confirmed(
        plan.target_plan().expected_source(),
        plan.target_plan().expected_target(),
        options.confirmation);
    if (!observed) {
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          observed.error());
    }
    auto status = validate_observed_clone(
        plan, observed.value(), &options.confirmation);
    if (!status) {
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          status.error());
    }

    auto made_source = dependencies.make_source_guard(plan, observed.value());
    if (!made_source || !made_source.value()) {
      return made_source
          ? failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
                clonecore::ErrorCode::internal_error,
                ERROR_INVALID_HANDLE,
                L"WinPE直接縮小source guard",
                L"read-only source guardを構築できません")
          : clonecore::Result<
                WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
                made_source.error());
    }
    source_guard = made_source.take_value();
    auto initial_epoch =
        source_guard->lock_source_read_only_and_revalidate(plan);
    if (!initial_epoch) {
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          initial_epoch.error());
    }
    ++source_revalidations;
    status = validate_source_epoch_against_plan(
        plan,
        initial_epoch.value(),
        L"WinPE直接縮小target I/O前source epoch");
    if (!status || !source_guard->source_left_os_read_only()) {
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          status
              ? offline_shrink_error(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_WRITE_PROTECT,
                    L"WinPE直接縮小source read-only latch",
                    L"target I/O前にコピー元のOSレベルread-only保持を証明できません")
              : status.error());
    }

    auto made_target =
        dependencies.make_target_platform(plan, observed.value());
    if (!made_target || !made_target.value()) {
      return made_target
          ? failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
                clonecore::ErrorCode::internal_error,
                ERROR_INVALID_HANDLE,
                L"WinPE直接縮小target platform",
                L"target専用construction platformを構築できません")
          : clonecore::Result<
                WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
                made_target.error());
    }
    target = made_target.take_value();

    report_progress(
        options,
        clonecore::DiskOperationStage::invalidating_target,
        std::nullopt,
        plan.operation_plan().expected_work_bytes,
        0U,
        false,
        false);
    auto begun = target->begin_target_owned_staging(
        plan.target_plan(), operation_plan_hash.value());
    if (!begun) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          begun.error());
    }
    checkpoint = begun.take_value();
    status = validate_checkpoint_common(
        plan, operation_plan_hash.value(), *checkpoint);
    if (status &&
        (checkpoint->phase !=
             directshrink::CheckpointPhase::prepared ||
         checkpoint->completed_task_count != 0U ||
         checkpoint->verified_target_bytes != 0U ||
         !all_zero(checkpoint->aggregate_write_digest))) {
      status = status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"WinPE直接縮小初期checkpoint",
          L"初期checkpointが0件・0 bytesのdurable prepared状態ではありません");
    }
    if (!status) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          status.error());
    }

    const auto& target_plan = plan.target_plan();
    const auto non_archive_count = static_cast<std::uint64_t>(
        std::count_if(
            target_plan.tasks().begin(),
            target_plan.tasks().end(),
            [](const directshrink::PartitionTask& task) {
              return task.kind !=
                      directshrink::PartitionTaskKind::apply_ntfs_wim &&
                  task.kind !=
                      directshrink::PartitionTaskKind::copy_exact_raw;
            }));
    std::uint64_t non_archive_upper_bound{};
    for (const auto& task : target_plan.tasks()) {
      if (task.kind == directshrink::PartitionTaskKind::apply_ntfs_wim ||
          task.kind == directshrink::PartitionTaskKind::copy_exact_raw) {
        continue;
      }
      if (!checked_add(
              non_archive_upper_bound,
              task.target_size_bytes,
              non_archive_upper_bound)) {
        abort_incomplete();
        return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"WinPE直接縮小非archive上限",
            L"再作成領域の検証容量上限がオーバーフローしました");
      }
    }
    report_progress(
        options,
        clonecore::DiskOperationStage::staging_partition_table,
        std::nullopt,
        plan.operation_plan().expected_work_bytes,
        verified_bytes,
        false,
        false);
    auto prepared = target->prepare_non_archive_partitions_and_verify(
        target_plan.tasks());
    if (!prepared) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          prepared.error());
    }
    if (prepared.value().prepared_task_count != non_archive_count ||
        (non_archive_count != 0U &&
         (prepared.value().verified_target_bytes == 0U ||
          prepared.value().verified_target_bytes > non_archive_upper_bound ||
          all_zero(prepared.value().write_digest))) ||
        (non_archive_count == 0U &&
         (prepared.value().verified_target_bytes != 0U ||
          !all_zero(prepared.value().write_digest))) ||
        !prepared.value().every_write_flushed ||
        !prepared.value().every_write_read_back ||
        !prepared.value().target_offline ||
        prepared.value().final_layout_committed) {
      abort_incomplete();
      return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"WinPE直接縮小non-archive construction",
          L"BasicData construction、件数、flush、読戻し、offline、または未commit証跡が不足しています");
    }
    aggregate_digest = prepared.value().write_digest;
    completed_tasks = non_archive_count;
    verified_bytes = prepared.value().verified_target_bytes;
    if (non_archive_count != 0U) {
      auto persisted = target->persist_prepared_partitions_checkpoint(
          *checkpoint,
          completed_tasks,
          verified_bytes,
          aggregate_digest);
      if (!persisted) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            persisted.error());
      }
      status = validate_checkpoint_transition(
          plan,
          operation_plan_hash.value(),
          *checkpoint,
          persisted.value(),
          directshrink::CheckpointPhase::applying,
          completed_tasks,
          verified_bytes,
          aggregate_digest);
      if (!status) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            status.error());
      }
      checkpoint = persisted.take_value();
    }

    for (const auto& task : target_plan.tasks()) {
      if (task.kind == directshrink::PartitionTaskKind::copy_exact_raw) {
        status = check_safe_boundary(
            options,
            task.source_table_index.value_or(0U),
            verified_bytes,
            completed_tasks);
        if (!status) {
          abort_incomplete();
          return clonecore::Result<
              WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
              status.error());
        }
        if (!dependencies.open_read_only_raw_source ||
            !task.source_table_index.has_value() ||
            !source_guard->source_left_os_read_only()) {
          abort_incomplete();
          return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_HANDLE,
              L"WinPE直接縮小exact RAW source",
              L"read-only source opener、source extent、またはOSレベルread-only latchがありません");
        }
        report_progress(
            options,
            clonecore::DiskOperationStage::copying_data,
            task.source_table_index,
            plan.operation_plan().expected_work_bytes,
            verified_bytes,
            true,
            true);
        auto opened = dependencies.open_read_only_raw_source(
            target_plan.expected_source());
        if (!opened || !opened.value().reader) {
          abort_incomplete();
          return opened
              ? failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
                    clonecore::ErrorCode::internal_error,
                    ERROR_INVALID_HANDLE,
                    L"WinPE直接縮小exact RAW source",
                    L"read-only source readerがありません")
              : clonecore::Result<
                    WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
                    opened.error());
        }
        ++source_revalidations;
        auto same_source = clonecore::validate_stable_identity(
            target_plan.expected_source(),
            opened.value().observed.identity,
            L"WinPE直接縮小exact RAWコピー元");
        auto source_layout =
            imageformat::hash_tsumugi_physical_restore_target_layout_v1(
                opened.value().observed.observed);
        const auto& opened_disk = opened.value().observed.observed;
        if (!same_source || !source_layout ||
            source_layout.value() !=
                target_plan.expected_source_layout_hash() ||
            !opened_disk.read_only.value_or(false) ||
            opened.value().reader->size_bytes() !=
                target_plan.expected_source().size_bytes ||
            opened.value().reader->logical_sector_size() !=
                target_plan.expected_source().logical_sector_size) {
          abort_incomplete();
          return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"WinPE直接縮小exact RAWコピー元再照合",
              !same_source
                  ? same_source.error().message
                  : !source_layout
                      ? source_layout.error().message
                      : L"read-only sourceのstable identity、canonical layout、容量、logical sector、またはOS read-only属性が計画と一致しません");
        }
        auto copied = target->copy_exact_raw_and_verify(
            task, *opened.value().reader);
        if (!copied) {
          abort_incomplete();
          return clonecore::Result<
              WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
              copied.error());
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
            !evidence.target_offline ||
            !source_guard->source_left_os_read_only()) {
          abort_incomplete();
          return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"WinPE直接縮小exact RAW検証",
              L"元サイズ、全chunk flush/readback、source/target SHA-256、read-only source、またはoffline target証跡が不足しています");
        }
        std::uint64_t next_verified{};
        if (!checked_add(
                verified_bytes,
                evidence.verified_target_bytes,
                next_verified) ||
            next_verified > plan.operation_plan().expected_work_bytes) {
          abort_incomplete();
          return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"WinPE直接縮小exact RAW検証済み容量",
              L"検証済みtarget容量が不変処理上限を超えました");
        }
        auto combined = combine_digest(
            aggregate_digest,
            evidence.target_write_digest,
            task.target_number,
            evidence.verified_target_bytes);
        if (!combined) {
          abort_incomplete();
          return clonecore::Result<
              WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
              combined.error());
        }
        aggregate_digest = combined.take_value();
        verified_bytes = next_verified;
        ++completed_tasks;
        ++copied_exact_raw;
        auto persisted = target->persist_progress_checkpoint(
            *checkpoint,
            completed_tasks,
            verified_bytes,
            aggregate_digest);
        if (!persisted) {
          abort_incomplete();
          return clonecore::Result<
              WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
              persisted.error());
        }
        status = validate_checkpoint_transition(
            plan,
            operation_plan_hash.value(),
            *checkpoint,
            persisted.value(),
            directshrink::CheckpointPhase::applying,
            completed_tasks,
            verified_bytes,
            aggregate_digest);
        if (!status) {
          abort_incomplete();
          return clonecore::Result<
              WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
              status.error());
        }
        checkpoint = persisted.take_value();
        report_progress(
            options,
            clonecore::DiskOperationStage::flushing_data,
            task.source_table_index,
            plan.operation_plan().expected_work_bytes,
            verified_bytes,
            true,
            true);
        status = check_safe_boundary(
            options,
            *task.source_table_index,
            verified_bytes,
            completed_tasks);
        if (!status) {
          abort_incomplete();
          return clonecore::Result<
              WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
              status.error());
        }
        continue;
      }
      if (task.kind != directshrink::PartitionTaskKind::apply_ntfs_wim) {
        continue;
      }
      status = check_safe_boundary(
          options,
          task.source_table_index.value_or(0U),
          verified_bytes,
          completed_tasks);
      if (!status) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            status.error());
      }
      report_progress(
          options,
          clonecore::DiskOperationStage::verifying_source,
          task.source_table_index,
          plan.operation_plan().expected_work_bytes,
          verified_bytes,
          true,
          true);
      auto lease = source_guard->acquire_capture_lease_after_revalidation(
          plan, task);
      if (!lease || !lease.value()) {
        abort_incomplete();
        return lease
            ? failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
                  clonecore::ErrorCode::internal_error,
                  ERROR_INVALID_HANDLE,
                  L"WinPE直接縮小capture lease",
                  L"capture直前read-only leaseがありません")
            : clonecore::Result<
                  WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
                  lease.error());
      }
      ++source_revalidations;
      status = validate_capture_lease(plan, task, lease.value()->evidence());
      if (!status || !source_guard->source_left_os_read_only()) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            status
                ? offline_shrink_error(
                      clonecore::ErrorCode::verification_failed,
                      ERROR_WRITE_PROTECT,
                      L"WinPE直接縮小capture read-only latch",
                      L"capture直前にコピー元のOSレベルread-only保持を証明できません")
                : status.error());
      }
      const auto& lease_evidence = lease.value()->evidence();
      directshrink::CaptureSource offline_source{
          .kind = directshrink::CaptureSourceKind::winpe_read_only_volume,
          .source_table_index = lease_evidence.volume.source_table_index,
          .source_offset_bytes = lease_evidence.volume.source_offset_bytes,
          .source_size_bytes = lease_evidence.volume.source_size_bytes,
          .original_volume_guid_path =
              lease_evidence.volume.volume_guid_path,
          .capture_identity = lease_evidence.capture_identity,
          .read_device_path = lease_evidence.capture_device_path,
          .source_os_read_only = lease_evidence.source_epoch.source_os_read_only,
          .source_disk_handle_held_read_only =
              lease_evidence.source_disk_handle_held_read_only,
          .source_volume_handle_held_read_only =
              lease_evidence.source_volume_handle_held_read_only,
          .source_volume_extent_reverified =
              lease_evidence.source_volume_extent_reverified,
      };
      report_progress(
          options,
          clonecore::DiskOperationStage::copying_data,
          task.source_table_index,
          plan.operation_plan().expected_work_bytes,
          verified_bytes,
          true,
          false);
      auto staged = target->capture_ntfs_wim_to_owned_staging(
          task, offline_source);
      // The lease intentionally remains alive through the full DISM capture.
      if (!staged) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            staged.error());
      }
      const auto& archive = staged.value();
      if (!task.source_table_index.has_value() ||
          archive.source_table_index != *task.source_table_index ||
          archive.target_number != task.target_number ||
          archive.capture_identity != offline_source.capture_identity ||
          !equal_path(
              archive.read_device_path,
              offline_source.read_device_path) ||
          archive.archive_length == 0U ||
          archive.archive_length > task.archive_upper_bound_bytes ||
          archive.archive_length >
              target_plan.staging().archive_capacity_bytes ||
          all_zero(archive.archive_hash) ||
          !archive.sealed_no_write_delete_sharing || !archive.flushed ||
          !archive.complete_read_back_hash_verified ||
          !archive.target_offline) {
        abort_incomplete();
        return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"WinPE直接縮小WIM封印",
            L"WIM上限、offline source対応、seal、flush、全読戻しHash、またはtarget offline証跡が不足しています");
      }
      lease.value().reset();

      auto applied = target->apply_staged_ntfs_wim_and_verify(task, archive);
      if (!applied) {
        const auto discarded = target->discard_exact_staged_archive(archive);
        abort_incomplete();
        if (!discarded) {
          auto error = applied.error();
          error.message +=
              L" / さらに封印済みWIMの厳密破棄にも失敗: " +
              discarded.error().message;
          return clonecore::Result<
              WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
              std::move(error));
        }
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            applied.error());
      }
      const auto& applied_value = applied.value();
      if (applied_value.source_table_index != *task.source_table_index ||
          applied_value.target_number != task.target_number ||
          applied_value.verified_target_bytes == 0U ||
          applied_value.verified_target_bytes > task.target_size_bytes ||
          applied_value.archive_hash != archive.archive_hash ||
          all_zero(applied_value.target_write_digest) ||
          !applied_value.every_write_flushed ||
          !applied_value.every_write_read_back ||
          !applied_value.file_system_metadata_verified ||
          !applied_value.target_offline) {
        const auto discarded = target->discard_exact_staged_archive(archive);
        abort_incomplete();
        if (!discarded) {
          return clonecore::Result<
              WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
              discarded.error());
        }
        return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"WinPE直接縮小NTFS apply/readback",
            L"対象対応、書込みHash、flush、全file読戻し、filesystem metadata、またはoffline証跡が不足しています");
      }
      const auto discarded = target->discard_exact_staged_archive(archive);
      if (!discarded) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            discarded.error());
      }

      std::uint64_t next_verified{};
      if (!checked_add(
              verified_bytes,
              applied_value.verified_target_bytes,
              next_verified) ||
          next_verified > plan.operation_plan().expected_work_bytes) {
        abort_incomplete();
        return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"WinPE直接縮小検証済み容量",
            L"検証済みtarget容量が不変処理上限を超えました");
      }
      auto combined = combine_digest(
          aggregate_digest,
          applied_value.target_write_digest,
          task.target_number,
          applied_value.verified_target_bytes);
      if (!combined) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            combined.error());
      }
      aggregate_digest = combined.take_value();
      verified_bytes = next_verified;
      ++completed_tasks;
      ++applied_archives;
      auto persisted = target->persist_progress_checkpoint(
          *checkpoint,
          completed_tasks,
          verified_bytes,
          aggregate_digest);
      if (!persisted) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            persisted.error());
      }
      status = validate_checkpoint_transition(
          plan,
          operation_plan_hash.value(),
          *checkpoint,
          persisted.value(),
          directshrink::CheckpointPhase::applying,
          completed_tasks,
          verified_bytes,
          aggregate_digest);
      if (!status) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            status.error());
      }
      checkpoint = persisted.take_value();
      report_progress(
          options,
          clonecore::DiskOperationStage::flushing_data,
          task.source_table_index,
          plan.operation_plan().expected_work_bytes,
          verified_bytes,
          true,
          true);
      status = check_safe_boundary(
          options,
          *task.source_table_index,
          verified_bytes,
          completed_tasks);
      if (!status) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            status.error());
      }
    }

    if (completed_tasks != target_plan.tasks().size() ||
        applied_archives != target_plan.archive_task_count() ||
        copied_exact_raw != exact_raw_task_count(target_plan) ||
        verified_bytes == 0U || all_zero(aggregate_digest)) {
      abort_incomplete();
      return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"WinPE直接縮小payload完了",
          L"全partition、WIM、検証済み容量、または集約書込みHashが揃っていません");
    }
    auto sealed = target->seal_commit_ready_checkpoint(
        *checkpoint,
        completed_tasks,
        verified_bytes,
        aggregate_digest);
    if (!sealed) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          sealed.error());
    }
    status = validate_checkpoint_transition(
        plan,
        operation_plan_hash.value(),
        *checkpoint,
        sealed.value(),
        directshrink::CheckpointPhase::commit_ready,
        completed_tasks,
        verified_bytes,
        aggregate_digest);
    if (!status) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          status.error());
    }
    checkpoint = sealed.take_value();

    auto prepared_final =
        target->prepare_final_extents_keep_incomplete_and_verify(
            target_plan, *checkpoint);
    if (!prepared_final) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          prepared_final.error());
    }
    status = validate_checkpoint_exactly_unchanged(
        plan,
        operation_plan_hash.value(),
        *checkpoint,
        prepared_final.value(),
        L"WinPE直接縮小final extent準備後checkpoint");
    if (!status) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          status.error());
    }
    checkpoint = prepared_final.take_value();

    if (target_plan.boot_finalization_required()) {
      report_progress(
          options,
          clonecore::DiskOperationStage::rebuilding_boot,
          std::nullopt,
          plan.operation_plan().expected_work_bytes,
          verified_bytes,
          false,
          false);
      auto finalized =
          target->finalize_boot_from_staged_layout_and_verify(target_plan);
      if (!finalized) {
        abort_incomplete();
        return clonecore::Result<
            WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
            finalized.error());
      }
      boot = finalized.take_value();
    } else {
      boot = directshrink::BootEvidence{
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
    if (boot->required != target_plan.boot_finalization_required() ||
        !boot->completed || !boot->boot_files_read_back_verified ||
        !boot->recovery_configuration_verified || !boot->target_offline ||
        !boot->target_only_reconstruction ||
        !boot->exact_target_volume_extents || boot->legacy_bios ||
        !boot->real_boot_not_claimed) {
      abort_incomplete();
      return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"WinPE直接縮小Boot/WinRE証跡",
          L"target-only Boot/WinRE再構築、読戻し、UEFI、offline、または実起動未証明の明示が不足しています");
    }

    // This gate is intentionally after Boot/WinRE and immediately before the
    // target checkpoint revalidation/final publication boundary.
    auto final_epoch =
        source_guard->revalidate_immediately_before_final_commit(plan);
    if (!final_epoch) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          final_epoch.error());
    }
    ++source_revalidations;
    status = validate_source_epoch_against_plan(
        plan,
        final_epoch.value(),
        L"WinPE直接縮小final GPT直前source epoch");
    if (!status || !source_guard->source_left_os_read_only()) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          status
              ? offline_shrink_error(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_WRITE_PROTECT,
                    L"WinPE直接縮小final source read-only latch",
                    L"final GPT直前にコピー元のOSレベルread-only保持を証明できません")
              : status.error());
    }

    auto revalidated = target->revalidate_before_final_commit(
        target_plan, *checkpoint);
    if (!revalidated) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          revalidated.error());
    }
    status = validate_checkpoint_exactly_unchanged(
        plan,
        operation_plan_hash.value(),
        *checkpoint,
        revalidated.value(),
        L"WinPE直接縮小final GPT直前checkpoint");
    if (!status) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          status.error());
    }
    checkpoint = revalidated.take_value();

    // Allocate and hash both legitimate completion outcomes before the
    // irreversible GPT publication.  After commit only fixed-size evidence
    // checks and digest selection are performed.
    auto retired_hash = completion_evidence_hash(
        plan,
        *checkpoint,
        *boot,
        aggregate_digest,
        verified_bytes,
        applied_archives,
        copied_exact_raw,
        source_revalidations,
        true);
    auto pending_hash = completion_evidence_hash(
        plan,
        *checkpoint,
        *boot,
        aggregate_digest,
        verified_bytes,
        applied_archives,
        copied_exact_raw,
        source_revalidations,
        false);
    if (!retired_hash || !pending_hash ||
        all_zero(retired_hash.value()) || all_zero(pending_hash.value()) ||
        retired_hash.value() == pending_hash.value()) {
      abort_incomplete();
      return !retired_hash
          ? clonecore::Result<
                WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
                retired_hash.error())
          : !pending_hash
              ? clonecore::Result<
                    WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
                    pending_hash.error())
              : failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_CRC,
                    L"WinPE直接縮小完成証跡の事前Hash",
                    L"checkpoint退役済み／cleanup保留のcanonical完成Hashをfinal GPT公開前に一意生成できません");
    }

    report_progress(
        options,
        clonecore::DiskOperationStage::committing_partition_table,
        std::nullopt,
        plan.operation_plan().expected_work_bytes,
        verified_bytes,
        false,
        false);
    auto committed = target->commit_final_layout_last(target_plan, *checkpoint);
    if (!committed) {
      abort_incomplete();
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          committed.error());
    }
    auto final = committed.take_value();
    // Independent source-guard evidence is injected without any allocation.
    final.source_reidentified = true;
    final.source_layout_unchanged = true;
    final_layout_publication_latched =
        final.committed_layout_hash == target_plan.final_layout_hash() &&
        final.aggregate_write_digest == aggregate_digest &&
        final.final_layout_published_before_checkpoint_retirement &&
        final.primary_layout_committed_last && final.target_offline;
    status = validate_final_commit(plan, aggregate_digest, final);
    if (!status) {
      if (!final_layout_publication_latched) {
        abort_incomplete();
      }
      return clonecore::Result<
          WinPeOfflineNtfsDirectShrinkExecutionReport>::failure(
          status.error());
    }

    const bool checkpoint_retired = final.checkpoint_retired;
    WinPeOfflineNtfsDirectShrinkExecutionReport report{
        .applied_archive_count = applied_archives,
        .copied_exact_raw_count = copied_exact_raw,
        .source_epoch_revalidation_count = source_revalidations,
        .verified_target_bytes = verified_bytes,
        .aggregate_write_digest = aggregate_digest,
        .precomputed_retired_completion_hash = retired_hash.value(),
        .precomputed_pending_completion_hash = pending_hash.value(),
        .completion_evidence_hash = checkpoint_retired
            ? retired_hash.value()
            : pending_hash.value(),
        .commit_ready_checkpoint = std::move(*checkpoint),
        .boot = *boot,
        .final_commit = std::move(final),
        .final_source_epoch = final_epoch.take_value(),
        .source_locked_read_only_before_target_io = true,
        .every_capture_used_exact_read_only_lease = true,
        .source_epoch_rechecked_after_boot_before_final_commit = true,
        .construction_layout_non_bootable = true,
        .durable_checkpoint_preceded_payload = true,
        .final_gpt_committed_last = true,
        .source_left_os_read_only = source_guard->source_left_os_read_only(),
        .target_left_offline = true,
        .real_boot_not_proven = true,
    };
    if (!report.source_left_os_read_only ||
        report.source_epoch_revalidation_count !=
            target_plan.archive_task_count() +
                exact_raw_task_count(target_plan) + 2U ||
        report.applied_archive_count != target_plan.archive_task_count() ||
        report.copied_exact_raw_count != exact_raw_task_count(target_plan) ||
        report.completion_evidence_hash ==
            (checkpoint_retired
                 ? report.precomputed_pending_completion_hash
                 : report.precomputed_retired_completion_hash)) {
      return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"WinPE直接縮小最終完成証跡",
          L"source read-only、全capture epoch、final epoch、または事前計算済み完成Hashが一致しません");
    }
    report_progress(
        options,
        clonecore::DiskOperationStage::completed,
        std::nullopt,
        plan.operation_plan().expected_work_bytes,
        verified_bytes,
        false,
        false);
    return clonecore::Result<
        WinPeOfflineNtfsDirectShrinkExecutionReport>::success(
        std::move(report));
  } catch (...) {
    if (!final_layout_publication_latched) {
      abort_incomplete();
    }
    return failure<WinPeOfflineNtfsDirectShrinkExecutionReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"WinPE直接縮小state machine例外",
        L"source guardまたはtarget platform境界から予期しない例外が発生しました");
  }
}

}  // namespace

clonecore::Result<imageformat::Sha256Digest>
hash_winpe_offline_ntfs_source_epoch_v1(
    const WinPeOfflineNtfsSourceEpochEvidence& evidence) {
  constexpr std::string_view kDomain =
      "YTEC-WINPE-OFFLINE-NTFS-SOURCE-EPOCH-V1";
  constexpr auto kMaximumSerializedCount =
      static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
  if (evidence.observed_source.model.size() > kMaximumSerializedCount ||
      evidence.observed_source.serial_suffix.size() >
          kMaximumSerializedCount ||
      evidence.observed_source.device_instance_id.size() >
          kMaximumSerializedCount ||
      evidence.ntfs_volumes.size() > kMaximumSerializedCount ||
      std::any_of(
          evidence.ntfs_volumes.begin(),
          evidence.ntfs_volumes.end(),
          [=](const WinPeOfflineNtfsVolumeBinding& volume) {
            return volume.volume_guid_path.size() > kMaximumSerializedCount;
          })) {
    return failure<imageformat::Sha256Digest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"WinPE source epoch Hash",
        L"識別文字列またはVolume binding数がcanonical長上限を超えています");
  }
  if (evidence.observed_source.model.empty() ||
      evidence.observed_source.size_bytes == 0U ||
      evidence.observed_source.logical_sector_size == 0U ||
      (evidence.observed_source.serial_suffix.empty() &&
       evidence.observed_source.device_instance_id.empty()) ||
      all_zero(evidence.source_layout_hash) ||
      all_zero(evidence.source_partition_snapshot_hash) ||
      all_zero(evidence.source_analysis_hash) ||
      evidence.ntfs_volumes.empty()) {
    return failure<imageformat::Sha256Digest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"WinPE source epoch入力",
        L"安定識別、canonical layout、partition snapshot、analysis Hash、またはNTFS bindingが不足しています");
  }

  auto volumes = evidence.ntfs_volumes;
  std::sort(
      volumes.begin(),
      volumes.end(),
      [](const WinPeOfflineNtfsVolumeBinding& left,
         const WinPeOfflineNtfsVolumeBinding& right) {
        return left.source_table_index < right.source_table_index;
      });
  for (std::size_t index = 0U; index < volumes.size(); ++index) {
    const auto& volume = volumes[index];
    if (volume.source_table_index == 0U || volume.source_size_bytes == 0U ||
        !is_volume_guid_path(volume.volume_guid_path) ||
        (index != 0U &&
         (volumes[index - 1U].source_table_index ==
              volume.source_table_index ||
          equal_path(
              volumes[index - 1U].volume_guid_path,
              volume.volume_guid_path)))) {
      return failure<imageformat::Sha256Digest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"WinPE source epoch Volume binding",
          L"source table index、物理extent、またはcanonical Volume GUID rootが無効か重複しています");
    }
  }

  std::vector<std::byte> bytes;
  bytes.reserve(256U + volumes.size() * 96U);
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_wstring(bytes, evidence.observed_source.model);
  append_u64(bytes, evidence.observed_source.size_bytes);
  append_u32(bytes, evidence.observed_source.logical_sector_size);
  append_string(bytes, evidence.observed_source.serial_suffix);
  append_wstring(bytes, evidence.observed_source.device_instance_id);
  append_array(bytes, evidence.source_layout_hash);
  append_array(bytes, evidence.source_partition_snapshot_hash);
  append_array(bytes, evidence.source_analysis_hash);
  append_u32(bytes, evidence.logical_sector_size);
  append_u8(bytes, evidence.stable_identity_reidentified ? 1U : 0U);
  append_u8(bytes, evidence.source_os_read_only ? 1U : 0U);
  append_u8(bytes, evidence.physical_handle_read_only ? 1U : 0U);
  append_u8(bytes, evidence.gpt_source ? 1U : 0U);
  append_u8(bytes, evidence.whole_disk_analysis ? 1U : 0U);
  append_u8(bytes, evidence.bitlocker_fully_decrypted ? 1U : 0U);
  append_u32(bytes, static_cast<std::uint32_t>(volumes.size()));
  for (const auto& volume : volumes) {
    append_u32(bytes, volume.source_table_index);
    append_u64(bytes, volume.source_offset_bytes);
    append_u64(bytes, volume.source_size_bytes);
    append_wstring(bytes, volume.volume_guid_path);
  }
  return imageformat::sha256(bytes);
}

clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>
build_winpe_offline_ntfs_direct_shrink_plan(
    directshrink::TargetPlan target_plan,
    WinPeOfflineNtfsPlanningEvidence planning_evidence) {
  auto source_epoch_hash =
      hash_winpe_offline_ntfs_source_epoch_v1(planning_evidence.source_epoch);
  auto stable = clonecore::validate_stable_identity(
      target_plan.expected_source(),
      planning_evidence.source_epoch.observed_source,
      L"WinPE計画時コピー元");
  const auto surplus = target_plan.surplus_allocation();
  const bool supported_surplus =
      surplus == migrationcore::ShrinkSurplusAllocation::leave_unallocated ||
      surplus ==
          migrationcore::ShrinkSurplusAllocation::automatic_proportional ||
      surplus ==
          migrationcore::ShrinkSurplusAllocation::selected_data_partition;
  if (!source_epoch_hash || !stable ||
      source_epoch_hash.value() !=
          planning_evidence.source_epoch.canonical_epoch_hash ||
      target_plan.source_partition_style() !=
          migrationcore::MigrationPartitionStyle::gpt ||
      target_plan.partition_style() !=
          migrationcore::MigrationPartitionStyle::gpt ||
      target_plan.partition_style_choice() !=
          migrationcore::DirectClonePartitionStyleChoice::preserve ||
      target_plan.mbr_preserve_binding().has_value() ||
      target_plan.expected_source().logical_sector_size != 512U ||
      target_plan.expected_target().logical_sector_size != 512U ||
      target_plan.expected_target().is_system_disk ||
      target_plan.target_is_active_rescue_media() ||
      target_plan.archive_task_count() == 0U ||
      all_zero(target_plan.final_layout_hash()) ||
      !supported_surplus ||
      planning_evidence.analysis_created_utc.empty() ||
      planning_evidence.app_version.empty() ||
      !planning_evidence.winpe_environment_verified ||
      !planning_evidence.source_supported_basic_disk ||
      !planning_evidence.source_health_allows_standard_clone ||
      !planning_evidence.target_supported_fixed_disk ||
      !planning_evidence.target_non_system ||
      !planning_evidence.target_health_allows_destructive_clone ||
      !planning_evidence.source_epoch.source_os_read_only ||
      !planning_evidence.source_epoch.physical_handle_read_only ||
      !planning_evidence.source_epoch.gpt_source ||
      !planning_evidence.source_epoch.whole_disk_analysis ||
      !planning_evidence.source_epoch.bitlocker_fully_decrypted) {
    return failure<WinPeOfflineNtfsDirectShrinkPlan>(
        clonecore::ErrorCode::unsupported_layout,
        !source_epoch_hash
            ? source_epoch_hash.error().native_code
            : !stable ? stable.error().native_code : ERROR_NOT_SUPPORTED,
        L"WinPE offline NTFS直接縮小plan",
        !source_epoch_hash
            ? source_epoch_hash.error().message
            : !stable
                ? stable.error().message
                : L"初期sliceはread-onlyのwhole-disk GPT/512B/NTFS/完全復号source、GPT preserve、分離fixed non-system target、健康状態許可、および3余剰policyだけを扱います");
  }

  std::uint64_t archive_tasks{};
  for (const auto& task : target_plan.tasks()) {
    switch (task.kind) {
      case directshrink::PartitionTaskKind::apply_ntfs_wim: {
        ++archive_tasks;
        if (!task.source_table_index.has_value() ||
            task.source_size_bytes == 0U || task.source_used_bytes == 0U ||
            task.source_used_bytes > task.source_size_bytes ||
            task.construction_size_bytes == 0U ||
            task.construction_size_bytes > task.target_size_bytes ||
            task.archive_upper_bound_bytes == 0U ||
            !is_volume_guid_path(task.original_volume_guid_path)) {
          return failure<WinPeOfflineNtfsDirectShrinkPlan>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"WinPE offline NTFS task",
              L"選択payloadは一意なsource extentとVolume GUIDを持つNTFS WIM taskだけを扱います");
        }
        break;
      }
      case directshrink::PartitionTaskKind::recreate_efi_system:
      case directshrink::PartitionTaskKind::recreate_microsoft_reserved:
        if (task.source_table_index.has_value() ||
            !task.original_volume_guid_path.empty()) {
          return failure<WinPeOfflineNtfsDirectShrinkPlan>(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"WinPE generated partition task",
              L"生成partition taskがsource payloadへ結合されています");
        }
        break;
      case directshrink::PartitionTaskKind::create_empty_ntfs:
        if (!task.source_table_index.has_value() ||
            task.role != migrationcore::MigrationPartitionRole::data ||
            task.source_size_bytes == 0U || task.source_used_bytes != 0U ||
            !task.original_volume_guid_path.empty() ||
            task.archive_upper_bound_bytes != 0U) {
          return failure<WinPeOfflineNtfsDirectShrinkPlan>(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"WinPE source-bound empty NTFS task",
              L"空NTFS taskは選択済みsource data extent、used=0、archiveなしでなければなりません");
        }
        break;
      case directshrink::PartitionTaskKind::copy_exact_raw:
        if (!task.source_table_index.has_value() ||
            task.role != migrationcore::MigrationPartitionRole::data ||
            task.source_size_bytes == 0U ||
            task.construction_size_bytes != task.source_size_bytes ||
            task.target_size_bytes != task.source_size_bytes ||
            all_zero(task.source_partition_type) ||
            !task.original_volume_guid_path.empty() ||
            task.archive_upper_bound_bytes != 0U) {
          return failure<WinPeOfflineNtfsDirectShrinkPlan>(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"WinPE source-bound exact RAW task",
              L"exact RAW taskは選択済みdata source extent、元区画と同一容量、元partition type、archiveなしでなければなりません");
        }
        break;
    }
  }
  for (const auto& mapping : target_plan.source_partition_mappings()) {
    if (mapping.required && !mapping.selected) {
      return failure<WinPeOfflineNtfsDirectShrinkPlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_ACCESS_DENIED,
          L"WinPE required partition選択",
          L"Windows/Boot/WinREに必須のpartitionは解除できません");
    }
  }
  if (archive_tasks != target_plan.archive_task_count() ||
      planning_evidence.source_epoch.ntfs_volumes.size() != archive_tasks) {
    return failure<WinPeOfflineNtfsDirectShrinkPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"WinPE NTFS binding件数",
        L"immutable taskと計画前source epochのNTFS Volume件数が一致しません");
  }

  WinPeOfflineNtfsDirectShrinkPlan result;
  result.target_plan_ =
      std::make_shared<const directshrink::TargetPlan>(
          std::move(target_plan));
  result.planning_evidence_ = std::move(planning_evidence);
  auto payload = make_winpe_immutable_payload_hash(
      *result.target_plan_, result.planning_evidence_);
  if (!payload) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        payload.error());
  }
  result.operation_plan_ = result.target_plan_->operation_plan();
  result.operation_plan_.environment =
      operationcore::OperationEnvironment::winpe;
  result.operation_plan_.immutable_payload_hash = payload.take_value();
  auto operation_status =
      operationcore::validate_operation_plan(result.operation_plan_);
  if (!operation_status) {
    return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::failure(
        operation_status.error());
  }
  return clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>::success(
      std::move(result));
}

clonecore::Result<WinPeOfflineNtfsDirectShrinkOperationReport>
execute_winpe_offline_ntfs_direct_shrink_clone(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const WinPeOfflineNtfsDirectShrinkExecutionOptions& options,
    const WinPeOfflineNtfsDirectShrinkDependencies& dependencies) {
  auto status = operationcore::validate_operation_plan(plan.operation_plan());
  if (status) {
    status = operationcore::validate_operation_confirmation(
        plan.operation_plan(), options.confirmation.typed_token);
  }
  if (!status || !options.confirmation.first_step_acknowledged ||
      !dependencies.reidentify_selection ||
      !dependencies.reidentify_confirmed ||
      !dependencies.make_source_guard ||
      !dependencies.make_target_platform ||
      (exact_raw_task_count(plan.target_plan()) != 0U &&
       !dependencies.open_read_only_raw_source)) {
    return failure<WinPeOfflineNtfsDirectShrinkOperationReport>(
        clonecore::ErrorCode::invalid_argument,
        status ? ERROR_INVALID_PARAMETER : status.error().native_code,
        L"WinPE offline NTFS直接縮小の実行依存",
        !options.confirmation.first_step_acknowledged
            ? L"コピー先消去の一段目確認が完了していません"
            : status
                ? L"再識別、source guard、またはtarget platformが不足しています"
                : status.error().message);
  }

  operationcore::OperationPlan report_plan = plan.operation_plan();
  std::optional<WinPeOfflineNtfsDirectShrinkExecutionReport> execution;
  operationcore::OperationCallbacks callbacks{
      .reidentify =
          [&](const operationcore::OperationPlan&) {
            auto observed = dependencies.reidentify_selection(
                plan.target_plan().expected_source(),
                plan.target_plan().expected_target());
            if (!observed) {
              return clonecore::Result<
                  operationcore::ReidentifiedOperation>::failure(
                  observed.error());
            }
            auto valid = validate_observed_clone(plan, observed.value(), nullptr);
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
                .output_hash = execution->completion_evidence_hash,
            });
          },
      .verify =
          [&](const operationcore::OperationPlan&,
              const operationcore::ExecutionEvidence& executed,
              const clonecore::DiskOperationCallbacks&) {
            if (!execution ||
                executed.processed_work_bytes !=
                    execution->verified_target_bytes ||
                executed.output_hash != execution->completion_evidence_hash ||
                !execution->source_left_os_read_only ||
                !execution->target_left_offline ||
                !execution->real_boot_not_proven) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"WinPE offline NTFS直接縮小の完了証跡",
                  L"容量、完成Hash、source read-only、target offline、または実起動未証明の明示が一致しません");
            }
            return clonecore::Result<
                operationcore::VerificationEvidence>::success({
                .verified_work_bytes = execution->verified_target_bytes,
                .output_hash = execution->completion_evidence_hash,
            });
          },
      .disk_operation = options.callbacks,
  };
  auto lifecycle = operationcore::run_operation(
      plan.operation_plan(), options.confirmation.typed_token, callbacks);
  return clonecore::Result<
      WinPeOfflineNtfsDirectShrinkOperationReport>::success({
      .plan = std::move(report_plan),
      .lifecycle = std::move(lifecycle),
      .execution = std::move(execution),
  });
}

}  // namespace ytec::winpeapp

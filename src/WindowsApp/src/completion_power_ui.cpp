#include "ytec/windowsapp/completion_power_ui.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace ytec::windowsapp {
namespace {

using clonecore::CompletionOperationOutcome;
using clonecore::MandatoryVerificationState;

[[nodiscard]] bool is_known_operation(
    const WindowsCompletionPowerOperation operation) noexcept {
  switch (operation) {
    case WindowsCompletionPowerOperation::clone:
    case WindowsCompletionPowerOperation::image_create:
    case WindowsCompletionPowerOperation::image_restore:
    case WindowsCompletionPowerOperation::rescue_media:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_action(
    const clonecore::CompletionPowerAction action) noexcept {
  switch (action) {
    case clonecore::CompletionPowerAction::none:
    case clonecore::CompletionPowerAction::sleep:
    case clonecore::CompletionPowerAction::restart:
    case clonecore::CompletionPowerAction::shutdown:
      return true;
  }
  return false;
}

template <std::size_t Size>
[[nodiscard]] bool all_zero(
    const std::array<std::byte, Size>& digest) noexcept {
  return std::all_of(
      digest.begin(), digest.end(), [](const std::byte value) {
        return value == std::byte{};
      });
}

[[nodiscard]] CompletionOperationOutcome operation_outcome(
    const operationcore::OperationResult& lifecycle,
    const bool has_report) noexcept {
  switch (lifecycle.outcome) {
    case operationcore::OperationOutcome::completed:
      return has_report &&
              lifecycle.phase == operationcore::OperationPhase::completed
          ? CompletionOperationOutcome::succeeded
          : CompletionOperationOutcome::failed;
    case operationcore::OperationOutcome::cancelled:
      return CompletionOperationOutcome::cancelled;
    case operationcore::OperationOutcome::failed:
      return CompletionOperationOutcome::failed;
  }
  return CompletionOperationOutcome::unknown;
}

[[nodiscard]] bool lifecycle_matches_plan(
    const operationcore::OperationPlan& plan,
    const operationcore::OperationResult& lifecycle) noexcept {
  return plan.schema_version == operationcore::kOperationPlanSchemaVersion &&
      !all_zero(plan.operation_id) &&
      !all_zero(plan.immutable_payload_hash) &&
      !all_zero(lifecycle.plan_hash) &&
      lifecycle.phase == operationcore::OperationPhase::completed &&
      lifecycle.outcome == operationcore::OperationOutcome::completed &&
      lifecycle.operation_id == plan.operation_id;
}

[[nodiscard]] bool is_sha256_hex(
    const std::string_view value) noexcept {
  return value.size() == 64U &&
      std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
            (character >= 'A' && character <= 'F') ||
            (character >= 'a' && character <= 'f');
      });
}

[[nodiscard]] bool add_checked(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& sum) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  sum = left + right;
  return true;
}

[[nodiscard]] WindowsCompletionPowerProof make_proof(
    const WindowsCompletionPowerOperation operation,
    const CompletionOperationOutcome outcome,
    const bool mandatory_verification_completed,
    const clonecore::SleepPreventionReleaseState sleep_prevention_release,
    const std::uint64_t operation_binding) noexcept {
  return WindowsCompletionPowerProof{
      .operation = operation,
      .outcome = outcome,
      .mandatory_verification = mandatory_verification_completed
          ? MandatoryVerificationState::completed
          : MandatoryVerificationState::incomplete,
      .sleep_prevention_release = sleep_prevention_release,
      .operation_binding = operation_binding,
  };
}

}  // namespace

std::uint64_t take_completion_power_operation_binding(
    std::uint64_t& next_binding) noexcept {
  if (next_binding == 0U) {
    return 0U;
  }
  const std::uint64_t binding = next_binding;
  if (next_binding == (std::numeric_limits<std::uint64_t>::max)()) {
    next_binding = 0U;
  } else {
    ++next_binding;
  }
  return binding;
}

WindowsCompletionPowerProof make_clone_completion_power_proof(
    const OnlineDirectCloneOperationReport& report,
    const clonecore::SleepPreventionReleaseState sleep_prevention_release,
    const std::uint64_t operation_binding) noexcept {
  const bool has_clone = report.clone.has_value();
  bool verified = lifecycle_matches_plan(report.plan, report.lifecycle) &&
      report.plan.kind == operationcore::OperationKind::clone &&
      report.plan.environment == operationcore::OperationEnvironment::windows &&
      report.plan.source.has_value() && has_clone;
  if (has_clone) {
    const auto& clone = report.clone.value();
    verified = verified && report.plan.expected_work_bytes != 0U &&
        clone.copied_data_bytes != 0U &&
        clone.copied_data_bytes <= report.plan.expected_work_bytes &&
        report.lifecycle.processed_work_bytes == clone.copied_data_bytes &&
        report.lifecycle.verified_work_bytes == clone.copied_data_bytes &&
        !all_zero(clone.verified_write_digest) &&
        clone.read_back_verified && clone.partition_table_committed &&
        clone.source_consistency_verified &&
        ((clone.used_vss_snapshot && clone.snapshot_backup_completed &&
          clone.snapshots_deleted) ||
         (!clone.used_vss_snapshot &&
          !clone.snapshot_backup_completed &&
          !clone.snapshots_deleted &&
          clone.locked_volume_count != 0U)) &&
        clone.target_left_offline &&
        clone.boot_finalization_required ==
            report.plan.source->is_system_disk &&
        (!clone.boot_finalization_required ||
         clone.boot_finalization_completed);
  }
  return make_proof(
      WindowsCompletionPowerOperation::clone,
      operation_outcome(report.lifecycle, has_clone),
      verified,
      sleep_prevention_release,
      operation_binding);
}

WindowsCompletionPowerProof
make_direct_shrink_clone_completion_power_proof(
    const WindowsDirectShrinkCloneOperationReport& report,
    const clonecore::SleepPreventionReleaseState sleep_prevention_release,
    const std::uint64_t operation_binding) noexcept {
  const bool has_execution = report.execution.has_value();
  bool verified = lifecycle_matches_plan(report.plan, report.lifecycle) &&
      report.plan.kind == operationcore::OperationKind::clone &&
      report.plan.environment == operationcore::OperationEnvironment::windows &&
      report.plan.source.has_value() && has_execution;
  if (has_execution) {
    const auto& execution = report.execution.value();
    const bool mbr_final = execution.final_commit.final_partition_style ==
        migrationcore::MigrationPartitionStyle::mbr;
    const bool gpt_final = execution.final_commit.final_partition_style ==
        migrationcore::MigrationPartitionStyle::gpt;
    const bool partition_style_proof = mbr_final
        ? execution.final_commit.source_mbr_sector0_unchanged &&
              execution.final_commit.source_mbr_bootstrap_unchanged &&
              execution.final_commit.target_mbr_signature_collision_free &&
              execution.final_commit.final_mbr_sector0_read_back_verified &&
              execution.final_commit.final_mbr_disk_signature != 0U &&
              execution.final_commit.final_mbr_active_partition_count ==
                  (report.plan.source->is_system_disk ? 1U : 0U)
        : gpt_final &&
              !execution.final_commit.source_mbr_sector0_unchanged &&
              !execution.final_commit.source_mbr_bootstrap_unchanged &&
              !execution.final_commit.target_mbr_signature_collision_free &&
              !execution.final_commit.final_mbr_sector0_read_back_verified &&
              execution.final_commit.final_mbr_disk_signature == 0U &&
              execution.final_commit.final_mbr_active_partition_count == 0U;
    verified = verified &&
        has_valid_windows_direct_shrink_precomputed_completion_evidence(
            execution) &&
        report.plan.expected_work_bytes != 0U &&
        execution.verified_target_bytes != 0U &&
        execution.verified_target_bytes <= report.plan.expected_work_bytes &&
        report.lifecycle.processed_work_bytes ==
            execution.verified_target_bytes &&
        report.lifecycle.verified_work_bytes ==
            execution.verified_target_bytes &&
        execution.workflow.volume_count != 0U &&
        execution.workflow.volume_count == execution.applied_archive_count &&
        !execution.workflow.snapshot_set_id.empty() &&
        execution.workflow.snapshot_data_copied &&
        execution.workflow.backup_completed &&
        execution.workflow.snapshots_deleted &&
        !all_zero(execution.aggregate_write_digest) &&
        execution.commit_ready_checkpoint.phase ==
            WindowsDirectShrinkCheckpointPhase::commit_ready &&
        execution.commit_ready_checkpoint.plan_hash ==
            report.lifecycle.plan_hash &&
        !all_zero(execution.commit_ready_checkpoint.staging_identity_hash) &&
        !all_zero(execution.commit_ready_checkpoint.record_hash) &&
        execution.commit_ready_checkpoint.aggregate_write_digest ==
            execution.aggregate_write_digest &&
        execution.commit_ready_checkpoint.verified_target_bytes ==
            execution.verified_target_bytes &&
        execution.commit_ready_checkpoint.durable &&
        execution.commit_ready_checkpoint.flushed &&
        execution.commit_ready_checkpoint.read_back_verified &&
        execution.commit_ready_checkpoint.target_offline &&
        !execution.commit_ready_checkpoint.final_layout_committed &&
        execution.boot.required == report.plan.source->is_system_disk &&
        execution.boot.completed &&
        execution.boot.boot_files_read_back_verified &&
        execution.boot.recovery_configuration_verified &&
        execution.boot.target_offline &&
        execution.boot.target_only_reconstruction &&
        execution.boot.exact_target_volume_extents &&
        execution.boot.legacy_bios ==
            (mbr_final && report.plan.source->is_system_disk) &&
        execution.boot.real_boot_not_claimed && partition_style_proof &&
        !all_zero(execution.final_commit.committed_layout_hash) &&
        execution.final_commit.aggregate_write_digest ==
            execution.aggregate_write_digest &&
        execution.final_commit.source_reidentified &&
        execution.final_commit.source_layout_unchanged &&
        execution.final_commit.target_reidentified &&
        execution.final_commit.staging_identity_reverified &&
        execution.final_commit.checkpoint_reverified &&
        execution.final_commit.staging_removed &&
        (execution.final_commit.checkpoint_retired !=
         execution.final_commit.checkpoint_retirement_pending) &&
        execution.final_commit.construction_layout_non_bootable &&
        execution.final_commit.
            checkpoint_retained_through_extensions_and_boot &&
        execution.final_commit.
            boot_completed_before_final_layout_publication &&
        execution.final_commit.
            final_layout_published_before_checkpoint_retirement &&
        execution.final_commit.hidden_final_layout_published_and_read_back &&
        execution.final_commit.every_required_ntfs_extension_verified &&
        execution.final_commit.every_write_flushed &&
        execution.final_commit.every_write_read_back &&
        execution.final_commit.primary_layout_committed_last &&
        execution.final_commit.target_offline &&
        execution.every_payload_captured_and_applied_inside_snapshot_callback &&
        execution.snapshots_deleted_before_final_layout_commit &&
        execution.target_left_offline;
  }
  return make_proof(
      WindowsCompletionPowerOperation::clone,
      operation_outcome(report.lifecycle, has_execution),
      verified,
      sleep_prevention_release,
      operation_binding);
}

WindowsCompletionPowerProof make_image_create_completion_power_proof(
    const vssrequester::OnlineTsumugiBackupReport& report,
    const bool rescue_mode,
    const clonecore::SleepPreventionReleaseState sleep_prevention_release,
    const std::uint64_t operation_binding) noexcept {
  const bool verified = !report.workflow.snapshot_set_id.empty() &&
      report.workflow.snapshot_data_copied &&
      report.workflow.backup_completed && report.workflow.snapshots_deleted &&
      report.image.stream.image_length != 0U &&
      !report.image.stream.final_path.empty() &&
      imageformat::selected_tsumugi_creation_verification_passed(
          report.image) &&
      report.image.stream.committed &&
      report.final_file_committed_after_vss;
  return make_proof(
      WindowsCompletionPowerOperation::image_create,
      rescue_mode ? CompletionOperationOutcome::partial
                  : CompletionOperationOutcome::succeeded,
      verified,
      sleep_prevention_release,
      operation_binding);
}

WindowsCompletionPowerProof make_exact_restore_completion_power_proof(
    const OnlineImageRestoreOperationReport& report,
    const clonecore::SleepPreventionReleaseState sleep_prevention_release,
    const std::uint64_t operation_binding) noexcept {
  const bool has_restore = report.restore.has_value();
  bool partial = false;
  bool verified = lifecycle_matches_plan(report.plan, report.lifecycle) &&
      report.plan.kind == operationcore::OperationKind::image_restore &&
      report.plan.environment == operationcore::OperationEnvironment::windows &&
      has_restore;
  if (has_restore) {
    const auto& outer = report.restore.value();
    const auto& restore = outer.restore;
    partial = outer.partial_loss || restore.partial_loss;
    verified = verified && report.plan.expected_work_bytes != 0U &&
        restore.written_logical_bytes == report.plan.expected_work_bytes &&
        report.lifecycle.processed_work_bytes ==
            restore.written_logical_bytes &&
        report.lifecycle.verified_work_bytes ==
            restore.written_logical_bytes &&
        outer.initial_image_verification_completed &&
        outer.target_reidentified_before_offline &&
        outer.target_handle_reidentified && outer.target_left_offline &&
        restore.callbacks_started_after_complete_verification &&
        restore.image_matched_prepared_plan &&
        restore.target_reidentified_before_write &&
        restore.all_writes_read_back_verified &&
        restore.final_layout_committed && !partial;
  }
  CompletionOperationOutcome outcome =
      operation_outcome(report.lifecycle, has_restore);
  if (outcome == CompletionOperationOutcome::succeeded && partial) {
    outcome = CompletionOperationOutcome::partial;
  }
  return make_proof(
      WindowsCompletionPowerOperation::image_restore,
      outcome,
      verified,
      sleep_prevention_release,
      operation_binding);
}

WindowsCompletionPowerProof make_shrink_restore_completion_power_proof(
    const WindowsOnlineShrinkRestoreOperationReport& report,
    const clonecore::SleepPreventionReleaseState sleep_prevention_release,
    const std::uint64_t operation_binding) noexcept {
  const bool has_restore = report.restore.has_value();
  bool verified = lifecycle_matches_plan(report.plan, report.lifecycle) &&
      report.plan.kind == operationcore::OperationKind::image_restore &&
      report.plan.environment == operationcore::OperationEnvironment::windows &&
      has_restore;
  if (has_restore) {
    const auto& outer = report.restore.value();
    const auto& restore = outer.restore;
    std::uint64_t verified_bytes{};
    std::uint64_t with_exact{};
    const bool byte_sum_valid = add_checked(
            restore.archive_logical_bytes,
            restore.exact_raw_logical_bytes,
            with_exact) &&
        add_checked(
            with_exact,
            restore.intentionally_omitted_logical_bytes,
            verified_bytes);
    verified = verified && byte_sum_valid && verified_bytes != 0U &&
        verified_bytes == report.plan.expected_work_bytes &&
        report.lifecycle.processed_work_bytes == verified_bytes &&
        report.lifecycle.verified_work_bytes == verified_bytes &&
        outer.image_completely_reverified &&
        outer.target_reidentified_before_plan &&
        outer.work_placement_reidentified_before_write &&
        outer.target_left_offline &&
        restore.callbacks_started_after_complete_verification &&
        restore.image_matched_prepared_plan &&
        restore.target_reidentified_before_write &&
        restore.all_payloads_verified_by_adapter &&
        restore.final_layout_committed;
  }
  return make_proof(
      WindowsCompletionPowerOperation::image_restore,
      operation_outcome(report.lifecycle, has_restore),
      verified,
      sleep_prevention_release,
      operation_binding);
}

WindowsCompletionPowerProof make_rescue_media_completion_power_proof(
    const RescueMediaCreationReport& report,
    const RescueMediaKind requested_kind,
    const clonecore::SleepPreventionReleaseState sleep_prevention_release,
    const std::uint64_t operation_binding) noexcept {
  bool verified = false;
  switch (requested_kind) {
    case RescueMediaKind::usb_drive: {
      const bool drive_root = report.usb_root_path.size() == 3U &&
          report.usb_root_path[0] >= L'A' &&
          report.usb_root_path[0] <= L'Z' &&
          report.usb_root_path[1] == L':' &&
          report.usb_root_path[2] == L'\\';
      verified = drive_root && !report.manifest_path.empty() &&
          !report.retained_work_root.empty() &&
          is_sha256_hex(report.usb_boot_wim_sha256) &&
          report.complete_usb_verified &&
          report.final_iso_path.empty() && !report.complete_iso_verified;
      break;
    }
    case RescueMediaKind::iso_file:
      verified = !report.final_iso_path.empty() &&
          !report.manifest_path.empty() &&
          !report.retained_work_root.empty() && report.iso_length != 0U &&
          is_sha256_hex(report.iso_sha256) &&
          report.complete_iso_verified &&
          report.published_without_overwrite &&
          !report.complete_usb_verified;
      break;
  }
  return make_proof(
      WindowsCompletionPowerOperation::rescue_media,
      CompletionOperationOutcome::succeeded,
      verified,
      sleep_prevention_release,
      operation_binding);
}

WindowsCompletionPowerPromptPlan plan_windows_completion_power_prompt(
    const WindowsCompletionPowerProof& proof) noexcept {
  return WindowsCompletionPowerPromptPlan{
      .prompt_allowed = is_known_operation(proof.operation) &&
          proof.outcome == CompletionOperationOutcome::succeeded &&
          proof.mandatory_verification ==
              MandatoryVerificationState::completed &&
          proof.sleep_prevention_release ==
              clonecore::SleepPreventionReleaseState::released &&
          proof.operation_binding != 0U,
  };
}

bool completion_power_action_expects_ui_session_end(
    const clonecore::CompletionPowerAction action) noexcept {
  switch (action) {
    case clonecore::CompletionPowerAction::restart:
    case clonecore::CompletionPowerAction::shutdown:
      return true;
    case clonecore::CompletionPowerAction::none:
    case clonecore::CompletionPowerAction::sleep:
      return false;
  }
  return false;
}

clonecore::CompletionPowerExecutionRequest
make_windows_completion_power_execution_request(
    const WindowsCompletionPowerProof& proof,
    const clonecore::CompletionPowerAction selected_action,
    const bool explicitly_selected,
    const bool explicitly_reconfirmed_immediately_before_execution) noexcept {
  const auto plan = plan_windows_completion_power_prompt(proof);
  const bool action_valid = is_known_action(selected_action);
  const clonecore::CompletionPowerAction effective_selection =
      plan.prompt_allowed && action_valid
      ? selected_action
      : clonecore::kDefaultCompletionPowerAction;
  const bool non_default =
      effective_selection != clonecore::kDefaultCompletionPowerAction;
  return clonecore::CompletionPowerExecutionRequest{
      .environment = clonecore::CompletionPowerEnvironment::windows,
      .operation_outcome = plan.prompt_allowed
          ? proof.outcome
          : CompletionOperationOutcome::unknown,
      .mandatory_verification = plan.prompt_allowed
          ? proof.mandatory_verification
          : MandatoryVerificationState::unknown,
      .sleep_prevention_release = plan.prompt_allowed
          ? proof.sleep_prevention_release
          : clonecore::SleepPreventionReleaseState::unknown,
      .operation_binding = plan.prompt_allowed
          ? proof.operation_binding
          : 0U,
      .selection = {
          .action = effective_selection,
          .operation_binding = non_default ? proof.operation_binding : 0U,
          .explicitly_selected =
              non_default && explicitly_selected,
      },
      .reconfirmation = {
          .action = effective_selection,
          .operation_binding = non_default ? proof.operation_binding : 0U,
          .explicitly_reconfirmed_immediately_before_execution =
              non_default &&
              explicitly_reconfirmed_immediately_before_execution,
      },
  };
}

}  // namespace ytec::windowsapp

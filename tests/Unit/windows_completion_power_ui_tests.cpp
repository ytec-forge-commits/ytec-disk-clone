#include "ytec/windowsapp/completion_power_ui.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using ytec::clonecore::CompletionPowerAction;
using ytec::clonecore::CompletionPowerAvailabilityState;
using ytec::clonecore::CompletionPowerExecutionDisposition;
using ytec::clonecore::ICompletionPowerPlatform;
using ytec::clonecore::SleepCapabilityReport;
using ytec::clonecore::SleepPreventionReleaseState;
using ytec::clonecore::Status;

constexpr std::uint64_t kBinding = 0x53414645303037ULL;
constexpr std::uint64_t kWorkBytes = 4096U;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class MockPlatform final : public ICompletionPowerPlatform {
 public:
  std::uint32_t query_calls{};
  std::uint32_t sleep_calls{};
  std::uint32_t restart_calls{};
  std::uint32_t shutdown_calls{};

  SleepCapabilityReport query_sleep_capability() override {
    ++query_calls;
    return SleepCapabilityReport{
        .state = CompletionPowerAvailabilityState::available,
    };
  }

  Status request_sleep() override {
    ++sleep_calls;
    return ytec::clonecore::success_status();
  }

  Status request_restart() override {
    ++restart_calls;
    return ytec::clonecore::success_status();
  }

  Status request_shutdown() override {
    ++shutdown_calls;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] std::uint32_t request_calls() const noexcept {
    return sleep_calls + restart_calls + shutdown_calls;
  }
};

[[nodiscard]] ytec::operationcore::OperationPlan ready_plan() {
  ytec::operationcore::OperationPlan plan;
  plan.operation_id[0] = std::byte{0x37};
  plan.expected_work_bytes = kWorkBytes;
  plan.immutable_payload_hash[0] = std::byte{0x51};
  plan.source.emplace();
  plan.source->is_system_disk = true;
  return plan;
}

[[nodiscard]] ytec::operationcore::OperationResult ready_lifecycle(
    const ytec::operationcore::OperationPlan& plan) {
  auto lifecycle = ytec::operationcore::OperationResult{
      .operation_id = plan.operation_id,
      .phase = ytec::operationcore::OperationPhase::completed,
      .outcome = ytec::operationcore::OperationOutcome::completed,
      .processed_work_bytes = kWorkBytes,
      .verified_work_bytes = kWorkBytes,
  };
  lifecycle.plan_hash[0] = std::byte{0xA3};
  return lifecycle;
}

[[nodiscard]] ytec::windowsapp::OnlineDirectCloneOperationReport
ready_clone_report() {
  auto plan = ready_plan();
  auto lifecycle = ready_lifecycle(plan);
  ytec::windowsapp::OnlineDirectCloneReport clone;
  clone.copied_data_bytes = kWorkBytes;
  clone.copied_partition_count = 1U;
  clone.verified_write_digest[0] = std::byte{0xA5};
  clone.read_back_verified = true;
  clone.partition_table_committed = true;
  clone.snapshot_backup_completed = true;
  clone.snapshots_deleted = true;
  clone.used_vss_snapshot = true;
  clone.source_consistency_verified = true;
  clone.target_left_offline = true;
  clone.boot_finalization_required = true;
  clone.boot_finalization_completed = true;
  return ytec::windowsapp::OnlineDirectCloneOperationReport{
      .plan = std::move(plan),
      .lifecycle = std::move(lifecycle),
      .clone = std::move(clone),
  };
}

[[nodiscard]] ytec::windowsapp::WindowsDirectShrinkCloneOperationReport
ready_direct_shrink_clone_report() {
  auto plan = ready_plan();
  auto lifecycle = ready_lifecycle(plan);
  ytec::windowsapp::WindowsDirectShrinkCloneExecutionReport execution;
  execution.workflow.snapshot_set_id = L"direct-shrink-snapshot";
  execution.workflow.volume_count = 1U;
  execution.workflow.snapshot_data_copied = true;
  execution.workflow.backup_completed = true;
  execution.workflow.snapshots_deleted = true;
  execution.applied_archive_count = 1U;
  execution.verified_target_bytes = kWorkBytes;
  execution.aggregate_write_digest[0] = std::byte{0xB1};
  execution.commit_ready_checkpoint.phase = ytec::windowsapp::
      WindowsDirectShrinkCheckpointPhase::commit_ready;
  execution.commit_ready_checkpoint.revision = 3U;
  execution.commit_ready_checkpoint.plan_hash = lifecycle.plan_hash;
  execution.commit_ready_checkpoint.staging_identity_hash[0] =
      std::byte{0xB2};
  execution.commit_ready_checkpoint.record_hash[0] = std::byte{0xB3};
  execution.commit_ready_checkpoint.aggregate_write_digest =
      execution.aggregate_write_digest;
  execution.commit_ready_checkpoint.completed_task_count = 4U;
  execution.commit_ready_checkpoint.verified_target_bytes = kWorkBytes;
  execution.commit_ready_checkpoint.durable = true;
  execution.commit_ready_checkpoint.flushed = true;
  execution.commit_ready_checkpoint.read_back_verified = true;
  execution.commit_ready_checkpoint.target_offline = true;
  execution.boot.required = true;
  execution.boot.completed = true;
  execution.boot.boot_files_read_back_verified = true;
  execution.boot.recovery_configuration_verified = true;
  execution.boot.target_offline = true;
  execution.boot.target_only_reconstruction = true;
  execution.boot.exact_target_volume_extents = true;
  execution.boot.legacy_bios = false;
  execution.boot.real_boot_not_claimed = true;
  execution.final_commit.committed_layout_hash[0] = std::byte{0xB4};
  execution.final_commit.aggregate_write_digest =
      execution.aggregate_write_digest;
  execution.final_commit.source_reidentified = true;
  execution.final_commit.source_layout_unchanged = true;
  execution.final_commit.target_reidentified = true;
  execution.final_commit.staging_identity_reverified = true;
  execution.final_commit.checkpoint_reverified = true;
  execution.final_commit.staging_removed = true;
  execution.final_commit.checkpoint_retired = true;
  execution.final_commit.checkpoint_retirement_pending = false;
  execution.final_commit.construction_layout_non_bootable = true;
  execution.final_commit.checkpoint_retained_through_extensions_and_boot =
      true;
  execution.final_commit.boot_completed_before_final_layout_publication =
      true;
  execution.final_commit.final_layout_published_before_checkpoint_retirement =
      true;
  execution.final_commit.hidden_final_layout_published_and_read_back = true;
  execution.final_commit.extended_ntfs_partition_count = 1U;
  execution.final_commit.every_required_ntfs_extension_verified = true;
  execution.final_commit.every_write_flushed = true;
  execution.final_commit.every_write_read_back = true;
  execution.final_commit.primary_layout_committed_last = true;
  execution.final_commit.target_offline = true;
  execution.precomputed_retired_completion_hash[0] = std::byte{0xC1};
  execution.precomputed_pending_completion_hash[0] = std::byte{0xC2};
  execution.selected_completion_hash =
      execution.precomputed_retired_completion_hash;
  execution.every_payload_captured_and_applied_inside_snapshot_callback = true;
  execution.snapshots_deleted_before_final_layout_commit = true;
  execution.target_left_offline = true;
  return ytec::windowsapp::WindowsDirectShrinkCloneOperationReport{
      .plan = std::move(plan),
      .lifecycle = std::move(lifecycle),
      .execution = std::move(execution),
  };
}

[[nodiscard]] ytec::vssrequester::OnlineTsumugiBackupReport
ready_image_create_report() {
  ytec::vssrequester::OnlineTsumugiBackupReport report;
  report.workflow.snapshot_set_id = L"snapshot";
  report.workflow.snapshot_data_copied = true;
  report.workflow.backup_completed = true;
  report.workflow.snapshots_deleted = true;
  report.image.stream.final_path = L"D:\\image.tsumugi";
  report.image.stream.image_length = kWorkBytes;
  report.image.stream.all_chunks_read_back_verified = true;
  report.image.stream.all_chunks_authenticated_and_hashed = true;
  report.image.stream.global_hash_read_back_verified = true;
  report.image.stream.final_metadata_read_back_verified = true;
  report.image.stream.final_complete_scan_performed = true;
  report.image.stream.committed = true;
  report.image.selected_verification_passed = true;
  report.image.complete_verification_passed = true;
  report.final_file_committed_after_vss = true;
  return report;
}

[[nodiscard]] ytec::windowsapp::OnlineImageRestoreOperationReport
ready_exact_restore_report() {
  auto plan = ready_plan();
  plan.kind = ytec::operationcore::OperationKind::image_restore;
  plan.source.reset();
  auto lifecycle = ready_lifecycle(plan);
  ytec::windowsapp::OnlineImageRestoreReport outer;
  outer.initial_image_verification_completed = true;
  outer.target_reidentified_before_offline = true;
  outer.target_handle_reidentified = true;
  outer.target_left_offline = true;
  outer.restore.written_logical_bytes = kWorkBytes;
  outer.restore.callbacks_started_after_complete_verification = true;
  outer.restore.image_matched_prepared_plan = true;
  outer.restore.target_reidentified_before_write = true;
  outer.restore.all_writes_read_back_verified = true;
  outer.restore.final_layout_committed = true;
  return ytec::windowsapp::OnlineImageRestoreOperationReport{
      .plan = std::move(plan),
      .lifecycle = std::move(lifecycle),
      .restore = std::move(outer),
  };
}

[[nodiscard]]
ytec::windowsapp::WindowsOnlineShrinkRestoreOperationReport
ready_shrink_restore_report() {
  auto plan = ready_plan();
  plan.kind = ytec::operationcore::OperationKind::image_restore;
  plan.source.reset();
  auto lifecycle = ready_lifecycle(plan);
  ytec::windowsapp::WindowsOnlineShrinkRestoreExecutionReport outer;
  outer.image_completely_reverified = true;
  outer.target_reidentified_before_plan = true;
  outer.work_placement_reidentified_before_write = true;
  outer.target_left_offline = true;
  outer.restore.archive_logical_bytes = 2048U;
  outer.restore.exact_raw_logical_bytes = 1024U;
  outer.restore.intentionally_omitted_logical_bytes = 1024U;
  outer.restore.callbacks_started_after_complete_verification = true;
  outer.restore.image_matched_prepared_plan = true;
  outer.restore.target_reidentified_before_write = true;
  outer.restore.all_payloads_verified_by_adapter = true;
  outer.restore.final_layout_committed = true;
  return ytec::windowsapp::WindowsOnlineShrinkRestoreOperationReport{
      .plan = std::move(plan),
      .lifecycle = std::move(lifecycle),
      .restore = std::move(outer),
  };
}

[[nodiscard]] ytec::windowsapp::RescueMediaCreationReport
ready_usb_report() {
  return ytec::windowsapp::RescueMediaCreationReport{
      .manifest_path = L"D:\\work\\usb-media-manifest.json",
      .retained_work_root = L"D:\\work",
      .usb_root_path = L"E:\\",
      .usb_boot_wim_sha256 = std::string(64U, 'A'),
      .complete_usb_verified = true,
  };
}

[[nodiscard]] ytec::windowsapp::RescueMediaCreationReport
ready_iso_report() {
  return ytec::windowsapp::RescueMediaCreationReport{
      .final_iso_path = L"D:\\rescue.iso",
      .manifest_path = L"D:\\rescue.iso.manifest.json",
      .retained_work_root = L"D:\\work",
      .iso_length = kWorkBytes,
      .iso_sha256 = std::string(64U, 'b'),
      .complete_iso_verified = true,
      .published_without_overwrite = true,
  };
}

void binding_allocator_never_reuses_after_exhaustion() {
  std::uint64_t next = 1U;
  check(
      ytec::windowsapp::take_completion_power_operation_binding(next) == 1U &&
          next == 2U,
      "The first operation binding must be non-zero and monotonic");
  next = (std::numeric_limits<std::uint64_t>::max)();
  check(
      ytec::windowsapp::take_completion_power_operation_binding(next) ==
              (std::numeric_limits<std::uint64_t>::max)() &&
          next == 0U &&
          ytec::windowsapp::take_completion_power_operation_binding(next) ==
              0U,
      "Binding exhaustion must fail closed instead of wrapping");
}

void every_product_success_requires_concrete_verification() {
  auto clone = ready_clone_report();
  auto proof = ytec::windowsapp::make_clone_completion_power_proof(
      clone, SleepPreventionReleaseState::released, kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A fully verified normal clone must be eligible");
  auto locked_clone = ready_clone_report();
  locked_clone.clone->snapshot_backup_completed = false;
  locked_clone.clone->snapshots_deleted = false;
  locked_clone.clone->used_vss_snapshot = false;
  locked_clone.clone->locked_volume_count = 1U;
  proof = ytec::windowsapp::make_clone_completion_power_proof(
      locked_clone, SleepPreventionReleaseState::released, kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A clone with retained FAT/exFAT lock evidence must be eligible");
  locked_clone.clone->locked_volume_count = 0U;
  proof = ytec::windowsapp::make_clone_completion_power_proof(
      locked_clone, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A non-VSS clone without lock evidence must not prompt");
  clone.clone->target_left_offline = false;
  proof = ytec::windowsapp::make_clone_completion_power_proof(
      clone, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A clone without offline proof must not prompt");

  auto image = ready_image_create_report();
  proof = ytec::windowsapp::make_image_create_completion_power_proof(
      image, false, SleepPreventionReleaseState::released, kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A fully verified normal image creation must be eligible");
  proof = ytec::windowsapp::make_image_create_completion_power_proof(
      image, true, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A rescue image must remain partial even with zero known loss");
  image.workflow.snapshots_deleted = false;
  proof = ytec::windowsapp::make_image_create_completion_power_proof(
      image, false, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Image creation without Snapshot deletion must not prompt");

  auto exact = ready_exact_restore_report();
  proof = ytec::windowsapp::make_exact_restore_completion_power_proof(
      exact, SleepPreventionReleaseState::released, kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A fully verified exact restore must be eligible");
  exact.restore->partial_loss = true;
  proof = ytec::windowsapp::make_exact_restore_completion_power_proof(
      exact, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A partial restore must never prompt");

  auto shrink = ready_shrink_restore_report();
  proof = ytec::windowsapp::make_shrink_restore_completion_power_proof(
      shrink, SleepPreventionReleaseState::released, kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A fully verified shrink restore must be eligible");
  shrink.lifecycle.verified_work_bytes -= 1U;
  proof = ytec::windowsapp::make_shrink_restore_completion_power_proof(
      shrink, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A shrink restore with mismatched verified bytes must not prompt");
}

void rescue_media_is_bound_to_the_requested_kind() {
  auto usb = ready_usb_report();
  auto proof =
      ytec::windowsapp::make_rescue_media_completion_power_proof(
          usb,
          ytec::windowsapp::RescueMediaKind::usb_drive,
          SleepPreventionReleaseState::released,
          kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A fully verified USB report must be eligible for its USB request");
  proof = ytec::windowsapp::make_rescue_media_completion_power_proof(
      usb,
      ytec::windowsapp::RescueMediaKind::iso_file,
      SleepPreventionReleaseState::released,
      kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A USB report must not satisfy an ISO request");

  const auto iso = ready_iso_report();
  proof = ytec::windowsapp::make_rescue_media_completion_power_proof(
      iso,
      ytec::windowsapp::RescueMediaKind::iso_file,
      SleepPreventionReleaseState::released,
      kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A fully verified non-overwriting ISO report must be eligible");
}

void direct_shrink_completion_requires_full_mandatory_evidence() {
  auto report = ready_direct_shrink_clone_report();
  auto proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A fully verified direct shrink clone must be eligible");

  report.execution->final_commit.final_partition_style =
      ytec::migrationcore::MigrationPartitionStyle::mbr;
  report.execution->final_commit.source_mbr_sector0_unchanged = true;
  report.execution->final_commit.source_mbr_bootstrap_unchanged = true;
  report.execution->final_commit.target_mbr_signature_collision_free = true;
  report.execution->final_commit.final_mbr_sector0_read_back_verified = true;
  report.execution->final_commit.final_mbr_disk_signature = 0x50607080U;
  report.execution->final_commit.final_mbr_active_partition_count = 1U;
  report.execution->boot.legacy_bios = true;
  proof = ytec::windowsapp::make_direct_shrink_clone_completion_power_proof(
      report, SleepPreventionReleaseState::released, kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A verified MBR/Legacy BIOS sector0-last report must be eligible");

  report.execution->final_commit.source_mbr_bootstrap_unchanged = false;
  proof = ytec::windowsapp::make_direct_shrink_clone_completion_power_proof(
      report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "MBR completion without unchanged source bootstrap must fail closed");

  report = ready_direct_shrink_clone_report();

  report.execution->final_commit.checkpoint_retired = false;
  report.execution->final_commit.checkpoint_retirement_pending = true;
  report.execution->selected_completion_hash =
      report.execution->precomputed_pending_completion_hash;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      ytec::windowsapp::plan_windows_completion_power_prompt(proof)
          .prompt_allowed,
      "A verified final layout with only checkpoint cleanup pending must remain eligible");

  report.execution->final_commit.checkpoint_retired = true;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Conflicting retired and cleanup-pending evidence must fail closed");

  report = ready_direct_shrink_clone_report();
  report.execution->selected_completion_hash = {};
  proof = ytec::windowsapp::make_direct_shrink_clone_completion_power_proof(
      report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A zero precomputed completion hash must fail the power prompt closed");

  report = ready_direct_shrink_clone_report();
  report.execution->selected_completion_hash =
      report.execution->precomputed_pending_completion_hash;
  proof = ytec::windowsapp::make_direct_shrink_clone_completion_power_proof(
      report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A completion hash for the opposite checkpoint outcome must fail the power prompt closed");

  report = ready_direct_shrink_clone_report();
  report.execution->final_commit.primary_layout_committed_last = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink without commit-last proof must not prompt");

  report = ready_direct_shrink_clone_report();
  report.execution->target_left_offline = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink without final offline proof must not prompt");

  report = ready_direct_shrink_clone_report();
  report.lifecycle.verified_work_bytes -= 1U;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink lifecycle and payload evidence must remain bound");

  report = ready_direct_shrink_clone_report();
  report.execution->boot.required = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "A system direct shrink clone must retain required boot proof");

  report = ready_direct_shrink_clone_report();
  report.execution->final_commit.source_reidentified = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink without fresh source identity proof must not prompt");

  report = ready_direct_shrink_clone_report();
  report.execution->final_commit.source_layout_unchanged = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink without unchanged source layout proof must not prompt");

  report = ready_direct_shrink_clone_report();
  report.execution->final_commit.construction_layout_non_bootable = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink must prove the pre-publication construction layout was non-bootable");

  report = ready_direct_shrink_clone_report();
  report.execution->final_commit.
      checkpoint_retained_through_extensions_and_boot = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink must retain its checkpoint through extent and boot preparation");

  report = ready_direct_shrink_clone_report();
  report.execution->final_commit.
      boot_completed_before_final_layout_publication = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink must finish boot preparation before final GPT publication");

  report = ready_direct_shrink_clone_report();
  report.execution->final_commit.
      final_layout_published_before_checkpoint_retirement = false;
  proof = ytec::windowsapp::
      make_direct_shrink_clone_completion_power_proof(
          report, SleepPreventionReleaseState::released, kBinding);
  check(
      !ytec::windowsapp::plan_windows_completion_power_prompt(proof)
           .prompt_allowed,
      "Direct shrink must publish and verify the final GPT before checkpoint retirement");

  for (const auto [release, binding] : {
           std::pair{SleepPreventionReleaseState::release_failed, kBinding},
           std::pair{SleepPreventionReleaseState::released, 0ULL},
       }) {
    const auto complete = ready_direct_shrink_clone_report();
    proof = ytec::windowsapp::
        make_direct_shrink_clone_completion_power_proof(
            complete, release, binding);
    const auto request = ytec::windowsapp::
        make_windows_completion_power_execution_request(
            proof, CompletionPowerAction::shutdown, true, true);
    MockPlatform platform;
    static_cast<void>(ytec::clonecore::execute_completion_power_action(
        request, platform));
    check(
        request.selection.action == CompletionPowerAction::none &&
            platform.query_calls == 0U && platform.request_calls() == 0U,
        "Direct shrink release and binding failures must stop before platform access");
  }
}

void release_and_binding_fail_closed_before_any_platform_call() {
  const auto clone = ready_clone_report();
  for (const auto release : {
           SleepPreventionReleaseState::still_active,
           SleepPreventionReleaseState::release_failed,
           SleepPreventionReleaseState::unknown,
       }) {
    const auto proof =
        ytec::windowsapp::make_clone_completion_power_proof(
            clone, release, kBinding);
    const auto request = ytec::windowsapp::
        make_windows_completion_power_execution_request(
            proof, CompletionPowerAction::shutdown, true, true);
    MockPlatform platform;
    const auto result = ytec::clonecore::execute_completion_power_action(
        request, platform);
    check(
        request.selection.action == CompletionPowerAction::none &&
            result.disposition ==
                CompletionPowerExecutionDisposition::no_action &&
            platform.query_calls == 0U && platform.request_calls() == 0U,
        "Unreleased sleep prevention must force none before platform access");
  }

  const auto proof = ytec::windowsapp::make_clone_completion_power_proof(
      clone, SleepPreventionReleaseState::released, 0U);
  const auto request =
      ytec::windowsapp::make_windows_completion_power_execution_request(
          proof, CompletionPowerAction::restart, true, true);
  MockPlatform platform;
  static_cast<void>(ytec::clonecore::execute_completion_power_action(
      request, platform));
  check(
      request.selection.action == CompletionPowerAction::none &&
          platform.query_calls == 0U && platform.request_calls() == 0U,
      "A zero binding must force none before platform access");
}

void selection_and_reconfirmation_reach_only_the_mock_seam() {
  const auto clone = ready_clone_report();
  const auto proof = ytec::windowsapp::make_clone_completion_power_proof(
      clone, SleepPreventionReleaseState::released, kBinding);

  auto request =
      ytec::windowsapp::make_windows_completion_power_execution_request(
          proof, CompletionPowerAction::none, true, false);
  MockPlatform default_platform;
  auto result = ytec::clonecore::execute_completion_power_action(
      request, default_platform);
  check(
      result.disposition == CompletionPowerExecutionDisposition::no_action &&
          default_platform.query_calls == 0U &&
          default_platform.request_calls() == 0U,
      "The default action must do nothing without a capability query");

  request = ytec::windowsapp::
      make_windows_completion_power_execution_request(
          proof, CompletionPowerAction::restart, true, true);
  MockPlatform selected_platform;
  result = ytec::clonecore::execute_completion_power_action(
      request, selected_platform);
  check(
      result.disposition ==
              CompletionPowerExecutionDisposition::request_accepted &&
          selected_platform.query_calls == 1U &&
          selected_platform.restart_calls == 1U &&
          selected_platform.sleep_calls == 0U &&
          selected_platform.shutdown_calls == 0U,
      "Two matching confirmations must dispatch once to the mock seam");

  request = ytec::windowsapp::
      make_windows_completion_power_execution_request(
          proof, CompletionPowerAction::shutdown, true, false);
  MockPlatform missing_reconfirmation;
  result = ytec::clonecore::execute_completion_power_action(
      request, missing_reconfirmation);
  check(
      result.disposition ==
              CompletionPowerExecutionDisposition::forced_none &&
          missing_reconfirmation.query_calls == 0U &&
          missing_reconfirmation.request_calls() == 0U,
      "A missing immediate reconfirmation must never query or dispatch");
}

void only_restart_and_shutdown_skip_the_continuing_ui_refresh() {
  check(
      !ytec::windowsapp::
           completion_power_action_expects_ui_session_end(
               CompletionPowerAction::none) &&
          !ytec::windowsapp::
               completion_power_action_expects_ui_session_end(
                   CompletionPowerAction::sleep) &&
          ytec::windowsapp::completion_power_action_expects_ui_session_end(
              CompletionPowerAction::restart) &&
          ytec::windowsapp::completion_power_action_expects_ui_session_end(
              CompletionPowerAction::shutdown) &&
          !ytec::windowsapp::
               completion_power_action_expects_ui_session_end(
                   static_cast<CompletionPowerAction>(0xFFU)),
      "Sleep resumes the same UI; only restart/shutdown may skip refresh");
}

}  // namespace

int main() {
  try {
    binding_allocator_never_reuses_after_exhaustion();
    every_product_success_requires_concrete_verification();
    rescue_media_is_bound_to_the_requested_kind();
    direct_shrink_completion_requires_full_mandatory_evidence();
    release_and_binding_fail_closed_before_any_platform_call();
    selection_and_reconfirmation_reach_only_the_mock_seam();
    only_restart_and_shutdown_skip_the_continuing_ui_refresh();
    std::cout << "windows completion power UI tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "windows completion power UI tests: FAIL: "
              << exception.what() << '\n';
    return 1;
  }
}

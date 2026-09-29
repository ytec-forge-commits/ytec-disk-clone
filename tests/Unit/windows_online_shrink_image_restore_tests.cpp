#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/windowsapp/online_shrink_image_restore.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetBytes = 64ULL * kMiB;
constexpr std::uint64_t kPayloadBytes = 4096U;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::diskmodel::DiskInfo target_disk() {
  return {
      .disk_number = 3U,
      .device_path = L"\\\\.\\PhysicalDrive3",
      .device_instance_id = L"SYNTHETIC\\TARGET-3",
      .model = L"Synthetic target",
      .size_bytes = kTargetBytes,
      .sector_count = kTargetBytes / 512U,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "TGT3",
      .partition_style = ytec::diskmodel::PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
      .partitions = {
          {
              .number = 1U,
              .offset_bytes = 1ULL * kMiB,
              .size_bytes = 8ULL * kMiB,
              .style = ytec::diskmodel::PartitionStyle::gpt,
              .type = L"Basic Data",
              .identifier = L"{TARGET-PARTITION}",
              .name = L"Old data",
          },
      },
  };
}

ytec::clonecore::StableDiskIdentity identity_for(
    const ytec::diskmodel::DiskInfo& disk) {
  auto identity = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(identity.has_value(), "stable identity fixture should build");
  return identity.take_value();
}

ytec::clonecore::StableDiskIdentity image_disk() {
  return {
      .disk_number = 4U,
      .model = L"Synthetic image disk",
      .size_bytes = 128ULL * kMiB,
      .logical_sector_size = 512U,
      .serial_suffix = "IMG4",
      .device_instance_id = L"SYNTHETIC\\IMAGE-4",
  };
}

ytec::clonecore::StableDiskIdentity work_disk() {
  return {
      .disk_number = 5U,
      .model = L"Synthetic work disk",
      .size_bytes = 256ULL * kMiB,
      .logical_sector_size = 512U,
      .serial_suffix = "WORK5",
      .device_instance_id = L"SYNTHETIC\\WORK-5",
  };
}

std::vector<std::byte> source_partition_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::gpt,
      .source_disk_size = 96ULL * kMiB,
      .logical_sector_size = 512U,
  };
  snapshot.regions.push_back({
      .disk_offset = 0U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  });
  snapshot.regions.push_back({
      .disk_offset = snapshot.source_disk_size - 512U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  });
  auto bytes = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(bytes.has_value(), "partition snapshot fixture should build");
  return bytes.take_value();
}

ytec::imageformat::TsumugiManifest shrink_manifest() {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::shrink,
      .partition_style = TsumugiManifestPartitionStyle::gpt,
      .flags = TsumugiManifestFlags::automatic_surplus_allocation,
      .source_disk_size = 96ULL * kMiB,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-10T00:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = source_partition_snapshot(),
  };
  manifest.source_model_hash[0] = std::byte{0x11};
  manifest.source_serial_hash[0] = std::byte{0x22};
  manifest.source_state_hash[0] = std::byte{0x33};
  TsumugiManifestPartition partition{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected,
      .source_offset = 1ULL * kMiB,
      .source_size = 32ULL * kMiB,
      .used_bytes = 2ULL * kMiB,
      .minimum_target_bytes = 8ULL * kMiB,
      .planned_target_bytes = 16ULL * kMiB,
      .payload_logical_offset = 0U,
      .payload_logical_length = kPayloadBytes,
      .payload_encoding =
          TsumugiManifestPayloadEncoding::microsoft_wim_single_image,
      .payload_format_version = kTsumugiWimPayloadFormatVersion,
      .cluster_size = 4096U,
      .name_utf8 = "Data",
      .label_utf8 = "Data",
  };
  partition.type_id[0] = std::byte{0xA2};
  partition.unique_id[0] = std::byte{0xB3};
  manifest.partitions.push_back(std::move(partition));
  return manifest;
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
reviewed_layout() {
  using namespace ytec;
  imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 result;
  result.migration = {
      .target_style = migrationcore::MigrationPartitionStyle::gpt,
      .alignment_bytes = 1ULL * kMiB,
      .minimum_target_size_bytes = 20ULL * kMiB,
      .target_size_bytes = kTargetBytes,
      .unallocated_tail_bytes = 47ULL * kMiB,
      .source_remains_unchanged = true,
      .boot_finalization_required = false,
      .target_partitions = {
          {
              .target_number = 1U,
              .source_table_index = 1U,
              .role = migrationcore::MigrationPartitionRole::data,
              .file_system = migrationcore::MigrationFileSystem::ntfs,
              .action =
                  migrationcore::MigrationPartitionAction::apply_file_image,
              .offset_bytes = 1ULL * kMiB,
              .size_bytes = 16ULL * kMiB,
              .source_size_bytes = 32ULL * kMiB,
              .source_used_bytes = 2ULL * kMiB,
              .label = L"Data",
          },
      },
  };
  result.metadata = {
      .style = imageformat::PartitionTableStyle::gpt,
      .target_size_bytes = kTargetBytes,
      .logical_sector_size = 512U,
      .invalidation_ranges = {{.offset = 0U, .length = 512U}},
      .staged_writes = {{
          .kind = imageformat::TsumugiRestoreLayoutWriteKind::
              gpt_primary_entries,
          .offset = 1024U,
          .bytes = std::vector<std::byte>(512U, std::byte{0x61}),
      }},
      .commit_writes = {{
          .kind = imageformat::TsumugiRestoreLayoutWriteKind::
              gpt_primary_header,
          .offset = 512U,
          .bytes = std::vector<std::byte>(512U, std::byte{0x62}),
      }},
      .target_layout = clonecore::GptDisk{
          .logical_sector_size = 512U,
          .sector_count = kTargetBytes / 512U,
          .partition_entry_count = 128U,
          .partition_entry_size = 128U,
      },
  };
  auto& gpt = std::get<clonecore::GptDisk>(result.metadata.target_layout);
  clonecore::GptPartition partition{
      .entry_index = 0U,
      .first_lba = (1ULL * kMiB) / 512U,
      .last_lba = ((17ULL * kMiB) / 512U) - 1U,
      .name = u"Data",
  };
  partition.type_guid.bytes[0] = std::byte{0xA2};
  partition.unique_guid.bytes[0] = std::byte{0xB3};
  gpt.partitions.push_back(std::move(partition));
  return result;
}

ytec::windowsapp::WindowsOnlineShrinkRestoreRequest request_fixture() {
  using namespace ytec;
  auto target = target_disk();
  auto target_identity = identity_for(target);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(target);
  check(target_layout.has_value(), "target layout fixture should hash");
  windowsapp::WindowsOnlineShrinkRestoreRequest request{
      .reviewed_image = {
          .canonical_path = L"D:\\images\\backup.tsumugi",
          .storage_file_system =
              imageformat::TsumugiImageStorageFileSystem::ntfs,
          .image_length = 8192U,
          .manifest = shrink_manifest(),
          .encrypted = false,
          .complete_container_verified = true,
          .metadata_verified = true,
          .restore_layout_verified = true,
      },
      .reviewed_target = target,
      .image = {
          .image_path = L"D:\\images\\backup.tsumugi",
          .storage_file_system =
              imageformat::TsumugiImageStorageFileSystem::ntfs,
      },
      .image_backing_disk = image_disk(),
      .expected_target = target_identity,
      .expected_target_layout_hash = target_layout.take_value(),
      .reviewed_layout = reviewed_layout(),
      .work_paths = {
          .scratch_directory = L"E:\\YTEC\\data",
          .checkpoint_path = L"E:\\YTEC\\data\\active.checkpoint",
          .log_path = L"E:\\YTEC\\data\\logs\\restore.log",
      },
      .confirmation = {
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
      .administrator = true,
  };
  request.reviewed_image.global_hash[0] = std::byte{0x44};
  request.operation_id[0] = std::byte{0x55};
  const auto work = work_disk();
  request.observed_work = {
      .scratch = {
          .canonical_path = request.work_paths.scratch_directory,
          .backing_disk = work,
          .local_volume = true,
      },
      .checkpoint = {
          .canonical_path = request.work_paths.checkpoint_path,
          .backing_disk = work,
          .local_volume = true,
      },
      .log = {
          .canonical_path = request.work_paths.log_path,
          .backing_disk = work,
          .local_volume = true,
      },
  };
  return request;
}

struct Harness final {
  ytec::windowsapp::WindowsOnlineShrinkRestoreRequest request{
      request_fixture()};
  int execute_calls{};
  bool protected_target{};
  bool drift_work{};
  bool complete_evidence{true};

  ytec::windowsapp::WindowsOnlineShrinkRestoreDependencies dependencies() {
    using namespace ytec;
    return {
        .reidentify_target =
            [this](const clonecore::StableDiskIdentity&,
                   const clonecore::TargetConfirmation&) {
              return clonecore::Result<
                  diskmodel::ReidentifiedPhysicalTarget>::success({
                  .target = request.reviewed_target,
                  .target_identity = request.expected_target,
              });
            },
        .is_protected_rescue_media =
            [this](const clonecore::StableDiskIdentity&) {
              return clonecore::Result<bool>::success(protected_target);
            },
        .observe_work_placement =
            [this](const windowsapp::WindowsShrinkWorkPaths&) {
              auto observed = request.observed_work;
              if (drift_work) {
                observed.scratch.canonical_path += L"-changed";
              }
              return clonecore::Result<windowsapp::
                  WindowsShrinkWorkPlacementObservation>::success(
                  std::move(observed));
            },
        .execute_reviewed_restore =
            [this](const windowsapp::WindowsOnlineShrinkRestoreRequest&) {
              ++execute_calls;
              windowsapp::WindowsOnlineShrinkRestoreExecutionReport report{
                  .restore = {
                      .archive_logical_bytes = kPayloadBytes,
                      .archive_chunk_count = 1U,
                      .completed_archive_partitions = 1U,
                      .callbacks_started_after_complete_verification = true,
                      .image_matched_prepared_plan = true,
                      .target_reidentified_before_write = true,
                      .all_payloads_verified_by_adapter = true,
                      .final_layout_committed = true,
                  },
                  .image_completely_reverified = true,
                  .target_reidentified_before_plan = true,
                  .work_placement_reidentified_before_write = true,
                  .target_left_offline = true,
              };
              if (!complete_evidence) {
                report.restore.all_payloads_verified_by_adapter = false;
              }
              return clonecore::Result<windowsapp::
                  WindowsOnlineShrinkRestoreExecutionReport>::success(
                  std::move(report));
            },
    };
  }
};

void test_success_runs_reviewed_lifecycle() {
  Harness harness;
  auto result = ytec::windowsapp::
      execute_windows_online_shrink_restore_operation(
          harness.request, harness.dependencies());
  if (!result) {
    std::cerr << "code=" << static_cast<int>(result.error().code)
              << " native=" << result.error().native_code << '\n';
  }
  check(result.has_value(), "wrapper should return lifecycle report");
  check(
      result.value().lifecycle.outcome ==
          ytec::operationcore::OperationOutcome::completed,
      "complete evidence should complete lifecycle");
  check(harness.execute_calls == 1, "executor should run once");
  check(
      result.value().lifecycle.processed_work_bytes == kPayloadBytes &&
          result.value().lifecycle.verified_work_bytes == kPayloadBytes,
      "lifecycle accounting should equal selected payload bytes");
}

void test_lowercase_ok_never_executes() {
  Harness harness;
  harness.request.confirmation.typed_token = L"ok";
  auto result = ytec::windowsapp::
      execute_windows_online_shrink_restore_operation(
          harness.request, harness.dependencies());
  check(result.has_value(), "confirmation failure is lifecycle evidence");
  check(
      result.value().lifecycle.outcome ==
          ytec::operationcore::OperationOutcome::failed,
      "lowercase confirmation must fail");
  check(harness.execute_calls == 0, "lowercase confirmation must not execute");
}

void test_protected_rescue_media_stops_before_execute() {
  Harness harness;
  harness.protected_target = true;
  auto result = ytec::windowsapp::
      execute_windows_online_shrink_restore_operation(
          harness.request, harness.dependencies());
  check(result.has_value(), "protected target is lifecycle evidence");
  check(
      result.value().lifecycle.outcome ==
          ytec::operationcore::OperationOutcome::failed,
      "protected rescue media must fail closed");
  check(harness.execute_calls == 0, "protected target must not execute");
}

void test_work_path_drift_stops_before_execute() {
  Harness harness;
  harness.drift_work = true;
  auto result = ytec::windowsapp::
      execute_windows_online_shrink_restore_operation(
          harness.request, harness.dependencies());
  check(result.has_value(), "work drift is lifecycle evidence");
  check(
      result.value().lifecycle.outcome ==
          ytec::operationcore::OperationOutcome::failed,
      "work path drift must fail closed");
  check(harness.execute_calls == 0, "work drift must not execute");
}

void test_missing_readback_evidence_fails_verification() {
  Harness harness;
  harness.complete_evidence = false;
  auto result = ytec::windowsapp::
      execute_windows_online_shrink_restore_operation(
          harness.request, harness.dependencies());
  check(result.has_value(), "verification failure is lifecycle evidence");
  check(
      result.value().lifecycle.outcome ==
          ytec::operationcore::OperationOutcome::failed,
      "missing adapter evidence must fail lifecycle");
  check(harness.execute_calls == 1, "verification test should execute once");
}

void test_image_backing_target_is_rejected_during_planning() {
  auto request = request_fixture();
  request.image_backing_disk = request.expected_target;
  auto result = ytec::windowsapp::
      make_windows_online_shrink_restore_operation_plan(request);
  check(!result.has_value(), "image backing target must be rejected");
}

void test_reviewed_layout_changes_immutable_hash() {
  auto first_request = request_fixture();
  auto second_request = request_fixture();
  second_request.reviewed_layout.migration.unallocated_tail_bytes -= 1U;
  auto first = ytec::windowsapp::
      make_windows_online_shrink_restore_operation_plan(first_request);
  auto second = ytec::windowsapp::
      make_windows_online_shrink_restore_operation_plan(second_request);
  check(first.has_value() && second.has_value(), "both plans should be valid");
  check(
      first.value().immutable_payload_hash !=
          second.value().immutable_payload_hash,
      "reviewed layout must be bound by immutable hash");
}

}  // namespace

int wmain() {
  try {
    test_success_runs_reviewed_lifecycle();
    test_lowercase_ok_never_executes();
    test_protected_rescue_media_stops_before_execute();
    test_work_path_drift_stops_before_execute();
    test_missing_readback_evidence_fails_verification();
    test_image_backing_target_is_rejected_during_planning();
    test_reviewed_layout_changes_immutable_hash();
  } catch (const std::exception& error) {
    std::cerr << "windows online shrink restore test failed: "
              << error.what() << '\n';
    return 1;
  }
  std::cout << "windows online shrink restore tests passed\n";
  return 0;
}

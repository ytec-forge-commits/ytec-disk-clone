#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/winpeapp/direct_shrink_image_restore.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
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
  check(identity.has_value(), "stable target identity should build");
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
      .model = L"Synthetic rescue work disk",
      .size_bytes = 256ULL * kMiB,
      .logical_sector_size = 512U,
      .serial_suffix = "WORK5",
      .device_instance_id = L"SYNTHETIC\\WORK-5",
  };
}

std::vector<std::byte> source_partition_snapshot(
    const ytec::imageformat::PartitionTableStyle style =
        ytec::imageformat::PartitionTableStyle::gpt) {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = style,
      .source_disk_size = 96ULL * kMiB,
      .logical_sector_size = 512U,
  };
  if (style == ytec::imageformat::PartitionTableStyle::mbr) {
    ytec::imageformat::PartitionTableRegion region{
        .disk_offset = 0U,
        .data = std::vector<std::byte>(512U, std::byte{0}),
    };
    region.data[446U + 4U] = std::byte{0x07};
    region.data[510U] = std::byte{0x55};
    region.data[511U] = std::byte{0xAA};
    snapshot.regions.push_back(std::move(region));
    auto bytes = ytec::imageformat::build_partition_snapshot_v1(snapshot);
    check(bytes.has_value(), "MBR partition snapshot should build");
    return bytes.take_value();
  }
  snapshot.regions.push_back({
      .disk_offset = 0U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  });
  snapshot.regions.push_back({
      .disk_offset = snapshot.source_disk_size - 512U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  });
  auto bytes = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(bytes.has_value(), "partition snapshot should build");
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
      .created_utc = "2026-08-13T00:00:00Z",
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

void append_exact_raw_partition(
    ytec::imageformat::TsumugiManifest& manifest,
    ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& layout) {
  using namespace ytec;
  imageformat::TsumugiManifestPartition raw{
      .source_table_index = 2U,
      .source_partition_number = 2U,
      .role = imageformat::TsumugiManifestPartitionRole::recovery,
      .file_system = imageformat::TsumugiManifestFileSystem::unknown,
      .flags = imageformat::TsumugiManifestPartitionFlags::selected,
      .source_offset = 40ULL * kMiB,
      .source_size = 1ULL * kMiB,
      .used_bytes = 1ULL * kMiB,
      .minimum_target_bytes = 1ULL * kMiB,
      .planned_target_bytes = 1ULL * kMiB,
      .payload_logical_offset = kPayloadBytes,
      .payload_logical_length = 1ULL * kMiB,
      .payload_encoding =
          imageformat::TsumugiManifestPayloadEncoding::exact_raw,
      .name_utf8 = "Recovery RAW",
  };
  raw.type_id[0] = std::byte{0xC4};
  raw.unique_id[0] = std::byte{0xD5};
  manifest.partitions.push_back(std::move(raw));

  layout.migration.target_partitions.push_back({
      .target_number = 2U,
      .source_table_index = 2U,
      .role = migrationcore::MigrationPartitionRole::recovery,
      .file_system = migrationcore::MigrationFileSystem::unsupported,
      .action = migrationcore::MigrationPartitionAction::copy_exact_raw,
      .offset_bytes = 18ULL * kMiB,
      .size_bytes = 1ULL * kMiB,
      .source_size_bytes = 1ULL * kMiB,
      .source_used_bytes = 1ULL * kMiB,
      .label = L"Recovery RAW",
  });
  layout.migration.unallocated_tail_bytes = 44ULL * kMiB;
  auto& gpt = std::get<clonecore::GptDisk>(
      layout.metadata.target_layout);
  clonecore::GptPartition partition{
      .entry_index = 1U,
      .first_lba = (18ULL * kMiB) / 512U,
      .last_lba = ((19ULL * kMiB) / 512U) - 1U,
      .name = u"Recovery RAW",
  };
  partition.type_guid.bytes[0] = std::byte{0xC4};
  partition.unique_guid.bytes[0] = std::byte{0xD5};
  gpt.partitions.push_back(std::move(partition));
}

void append_generated_esp_and_msr(
    ytec::imageformat::TsumugiManifest& manifest,
    ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& layout) {
  using namespace ytec;
  imageformat::TsumugiManifestPartition esp{
      .source_table_index = 2U,
      .source_partition_number = 2U,
      .role = imageformat::TsumugiManifestPartitionRole::efi_system,
      .file_system = imageformat::TsumugiManifestFileSystem::fat32,
      .flags = imageformat::TsumugiManifestPartitionFlags::selected |
          imageformat::TsumugiManifestPartitionFlags::required,
      .source_offset = 40ULL * kMiB,
      .source_size = 1ULL * kMiB,
      .used_bytes = 256U * 1024U,
      .minimum_target_bytes = 1ULL * kMiB,
      .planned_target_bytes = 1ULL * kMiB,
      .payload_logical_offset = kPayloadBytes,
      .payload_logical_length = 1ULL * kMiB,
      .payload_encoding =
          imageformat::TsumugiManifestPayloadEncoding::exact_raw,
      .name_utf8 = "ESP",
  };
  esp.type_id[0] = std::byte{0xE1};
  esp.unique_id[0] = std::byte{0xE2};
  imageformat::TsumugiManifestPartition msr{
      .source_table_index = 3U,
      .source_partition_number = 3U,
      .role = imageformat::TsumugiManifestPartitionRole::microsoft_reserved,
      .file_system = imageformat::TsumugiManifestFileSystem::none,
      .flags = imageformat::TsumugiManifestPartitionFlags::selected |
          imageformat::TsumugiManifestPartitionFlags::required,
      .source_offset = 42ULL * kMiB,
      .source_size = 1ULL * kMiB,
      .used_bytes = 0U,
      .minimum_target_bytes = 1ULL * kMiB,
      .planned_target_bytes = 1ULL * kMiB,
      .payload_logical_offset = kPayloadBytes + (1ULL * kMiB),
      .payload_logical_length = 1ULL * kMiB,
      .payload_encoding =
          imageformat::TsumugiManifestPayloadEncoding::exact_raw,
      .name_utf8 = "MSR",
  };
  msr.type_id[0] = std::byte{0xF1};
  msr.unique_id[0] = std::byte{0xF2};
  manifest.partitions.push_back(std::move(esp));
  manifest.partitions.push_back(std::move(msr));

  layout.migration.target_partitions.push_back({
      .target_number = 2U,
      .role = migrationcore::MigrationPartitionRole::efi_system,
      .file_system = migrationcore::MigrationFileSystem::fat32,
      .action = migrationcore::MigrationPartitionAction::create_fat32,
      .offset_bytes = 18ULL * kMiB,
      .size_bytes = 1ULL * kMiB,
      .label = L"EFI System",
  });
  layout.migration.target_partitions.push_back({
      .target_number = 3U,
      .role = migrationcore::MigrationPartitionRole::microsoft_reserved,
      .file_system = migrationcore::MigrationFileSystem::none,
      .action = migrationcore::MigrationPartitionAction::create_reserved,
      .offset_bytes = 20ULL * kMiB,
      .size_bytes = 1ULL * kMiB,
      .label = L"Microsoft Reserved",
  });
  layout.migration.unallocated_tail_bytes = 42ULL * kMiB;
  auto& gpt = std::get<clonecore::GptDisk>(
      layout.metadata.target_layout);
  clonecore::GptPartition esp_target{
      .entry_index = 1U,
      .first_lba = (18ULL * kMiB) / 512U,
      .last_lba = ((19ULL * kMiB) / 512U) - 1U,
      .name = u"EFI System",
  };
  esp_target.type_guid.bytes[0] = std::byte{0x11};
  esp_target.unique_guid.bytes[0] = std::byte{0x12};
  gpt.partitions.push_back(std::move(esp_target));
  clonecore::GptPartition msr_target{
      .entry_index = 2U,
      .first_lba = (20ULL * kMiB) / 512U,
      .last_lba = ((21ULL * kMiB) / 512U) - 1U,
      .name = u"Microsoft Reserved",
  };
  msr_target.type_guid.bytes[0] = std::byte{0x21};
  msr_target.unique_guid.bytes[0] = std::byte{0x22};
  gpt.partitions.push_back(std::move(msr_target));
}

void convert_fixture_to_mbr(
    ytec::imageformat::TsumugiManifest& manifest,
    ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& layout) {
  using namespace ytec;
  manifest.partition_style =
      imageformat::TsumugiManifestPartitionStyle::mbr;
  manifest.partition_snapshot = source_partition_snapshot(
      imageformat::PartitionTableStyle::mbr);
  manifest.partitions[0].type_id.fill(std::byte{0});
  manifest.partitions[0].type_id[0] = std::byte{0x07};
  manifest.partitions[0].unique_id.fill(std::byte{0});

  layout.migration.target_style =
      migrationcore::MigrationPartitionStyle::mbr;
  layout.metadata.style = imageformat::PartitionTableStyle::mbr;
  layout.metadata.staged_writes.clear();
  layout.metadata.commit_writes = {{
      .kind = imageformat::TsumugiRestoreLayoutWriteKind::mbr_sector,
      .offset = 0U,
      .bytes = std::vector<std::byte>(512U, std::byte{0x62}),
  }};
  clonecore::MbrDisk mbr{
      .logical_sector_size = 512U,
      .sector_count = kTargetBytes / 512U,
      .disk_signature = 0x13572468U,
  };
  mbr.partitions.push_back({
      .table_index = 0U,
      .active = false,
      .type = 0x07U,
      .first_lba = static_cast<std::uint32_t>((1ULL * kMiB) / 512U),
      .sector_count = static_cast<std::uint32_t>((16ULL * kMiB) / 512U),
  });
  layout.metadata.target_layout = std::move(mbr);
}

ytec::winpeapp::DirectShrinkImageRestoreRequest request_fixture() {
  using namespace ytec;
  const auto target = target_disk();
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(target);
  check(target_layout.has_value(), "target layout should hash");
  winpeapp::DirectShrinkImageRestoreRequest request{
      .image = {
          .image_path = L"D:\\images\\backup.tsumugi",
          .storage_file_system =
              imageformat::TsumugiImageStorageFileSystem::ntfs,
      },
      .reviewed_manifest = shrink_manifest(),
      .image_backing_disk = image_disk(),
      .reviewed_target = target,
      .expected_target = identity_for(target),
      .expected_target_layout_hash = target_layout.take_value(),
      .reviewed_layout = reviewed_layout(),
      .work = {
          .paths = {
              .scratch_directory = L"E:\\YtecDiskClone\\data",
              .checkpoint_path =
                  L"E:\\YtecDiskClone\\data\\shrink-restore-incomplete.checkpoint",
              .log_path =
                  L"E:\\YtecDiskClone\\data\\shrink-restore.log",
              .log_is_ram_only = false,
          },
          .active_rescue_disk = work_disk(),
      },
      .confirmation = {
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
      .administrator = true,
  };
  request.expected_image_global_hash[0] = std::byte{0x44};
  request.expected_source_state_hash =
      request.reviewed_manifest.source_state_hash;
  const auto work = work_disk();
  request.work.observation = {
      .scratch = {
          .canonical_path = request.work.paths.scratch_directory,
          .backing_disk = work,
          .local_volume = true,
      },
      .checkpoint = {
          .canonical_path = request.work.paths.checkpoint_path,
          .backing_disk = work,
          .local_volume = true,
      },
      .log = {
          .canonical_path = request.work.paths.log_path,
          .backing_disk = work,
          .local_volume = true,
      },
  };
  return request;
}

struct Harness final {
  ytec::winpeapp::DirectShrinkImageRestoreRequest request{
      request_fixture()};
  int executor_calls{};
  bool drift_work{};
  bool drift_image{};
  bool drift_target{};
  bool active_target{};
  bool active_cd{};
  bool active_x_drive{};
  bool log_on_image{};
  bool scratch_on_target{};
  bool incomplete_evidence{};

  ytec::winpeapp::DirectShrinkImageRestoreDependencies dependencies() {
    using namespace ytec;
    return {
        .query_active_storage =
            [this]() {
              return clonecore::Result<winpeapp::
                  ActiveRescueMediaStorageObservation>::success({
                  .marker_path = active_x_drive
                      ? L"X:\\YtecDiskClone\\rescue-media-id.txt"
                      : L"E:\\YtecDiskClone\\rescue-media-id.txt",
                  .drive_type = static_cast<std::uint32_t>(
                      active_cd ? DRIVE_CDROM : DRIVE_REMOVABLE),
                  .physical_identity = active_target
                      ? std::optional<clonecore::StableDiskIdentity>(
                            request.expected_target)
                      : std::optional<clonecore::StableDiskIdentity>(
                            request.work.active_rescue_disk),
                  .marker_identity_from_open_handle = true,
              });
            },
        .identify_image_backing =
            [this](const std::wstring&) {
              auto observed = request.image_backing_disk;
              if (drift_image) {
                observed.serial_suffix += "-changed";
              }
              return clonecore::Result<
                  clonecore::StableDiskIdentity>::success(
                  std::move(observed));
            },
        .reidentify_target =
            [this](const clonecore::StableDiskIdentity&,
                   const clonecore::TargetConfirmation&) {
              auto target = request.reviewed_target;
              if (drift_target) {
                target.partitions[0].offset_bytes += 512U;
              }
              return clonecore::Result<
                  diskmodel::ReidentifiedPhysicalTarget>::success({
                  .target = std::move(target),
                  .target_identity = request.expected_target,
              });
            },
        .observe_work_placement =
            [this](const windowsapp::WindowsShrinkWorkPaths&) {
              auto observed = request.work.observation;
              if (drift_work) {
                observed.scratch.canonical_path += L"-changed";
              }
              if (log_on_image) {
                observed.log.backing_disk = request.image_backing_disk;
              }
              if (scratch_on_target) {
                observed.scratch.backing_disk = request.expected_target;
              }
              return clonecore::Result<windowsapp::
                  WindowsShrinkWorkPlacementObservation>::success(
                  std::move(observed));
            },
        .execute_reviewed_restore =
            [this](
                const winpeapp::DirectShrinkImageRestoreRequest&,
                const diskmodel::ReidentifiedPhysicalTarget&,
                const clonecore::StableDiskIdentity&,
                const windowsapp::
                    WindowsShrinkWorkPlacementObservation&) {
              ++executor_calls;
              winpeapp::DirectShrinkImageRestoreReport report{
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
                  .image_backing_reidentified_before_write = true,
                  .active_rescue_media_checked = true,
                  .target_reidentified_before_plan = true,
                  .work_placement_reidentified_before_write = true,
                  .target_left_offline = true,
                  .direct_execution_only = true,
              };
              if (incomplete_evidence) {
                report.restore.all_payloads_verified_by_adapter = false;
              }
              return clonecore::Result<winpeapp::
                  DirectShrinkImageRestoreReport>::success(
                  std::move(report));
            },
    };
  }
};

void mark_target_as_original_source(Harness& harness) {
  auto model = ytec::imageformat::hash_tsumugi_source_model_v1(
      harness.request.reviewed_target.model);
  auto serial = ytec::imageformat::hash_tsumugi_source_serial_v1(
      harness.request.reviewed_target.serial_suffix,
      harness.request.reviewed_target.device_instance_id);
  check(model.has_value(), "source model hash should build");
  check(serial.has_value(), "source serial hash should build");
  harness.request.reviewed_manifest.source_model_hash = model.take_value();
  harness.request.reviewed_manifest.source_serial_hash = serial.take_value();
}

void test_success_calls_direct_executor_once() {
  Harness harness;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(result.has_value(), "complete synthetic evidence should pass");
  check(harness.executor_calls == 1, "executor should run once");
  check(
      result.value().restore.final_layout_committed,
      "final commit evidence should survive");
}

void test_static_exact_raw_is_allowed_only_as_bound_whole_disk_payload() {
  Harness harness;
  append_exact_raw_partition(
      harness.request.reviewed_manifest,
      harness.request.reviewed_layout);
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(result.has_value(), "bound same-style 512 exact RAW should pass");
  check(harness.executor_calls == 1, "bound exact RAW should execute once");
}

void test_generated_esp_and_msr_are_allowed_without_payload_restore() {
  Harness harness;
  append_generated_esp_and_msr(
      harness.request.reviewed_manifest,
      harness.request.reviewed_layout);
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(result.has_value(), "reviewed generated ESP/MSR should pass");
  check(harness.executor_calls == 1, "generated system layout should run");
}

void test_mbr_style_is_preserved_for_whole_disk_shrink_restore() {
  Harness harness;
  convert_fixture_to_mbr(
      harness.request.reviewed_manifest,
      harness.request.reviewed_layout);
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(result.has_value(), "reviewed MBR-preserving layout should pass");
  check(harness.executor_calls == 1, "MBR-preserving layout should run");
}

void test_lowercase_ok_stops_before_executor() {
  Harness harness;
  harness.request.confirmation.typed_token = L"ok";
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "lowercase confirmation must fail");
  check(harness.executor_calls == 0, "lowercase OK must not execute");
}

void test_active_rescue_target_stops_before_executor() {
  Harness harness;
  harness.active_target = true;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "active rescue target must fail");
  check(harness.executor_calls == 0, "active target must not execute");
}

void test_image_and_work_same_disk_stops_before_executor() {
  Harness harness;
  harness.request.image_backing_disk = harness.request.work.active_rescue_disk;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "image/work disk alias must fail");
  check(harness.executor_calls == 0, "aliased work must not execute");
}

void test_image_and_target_same_disk_stops_before_executor() {
  Harness harness;
  harness.request.image_backing_disk = harness.request.expected_target;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "image/target disk alias must fail");
  check(harness.executor_calls == 0, "aliased target must not execute");
}

void test_all_three_work_paths_must_stay_on_active_rescue_disk() {
  Harness log_harness;
  log_harness.log_on_image = true;
  auto log_result = ytec::winpeapp::execute_direct_shrink_image_restore(
      log_harness.request, log_harness.dependencies());
  check(!log_result.has_value(), "log on image disk must fail");
  check(log_harness.executor_calls == 0, "unsafe log must not execute");

  Harness scratch_harness;
  scratch_harness.scratch_on_target = true;
  auto scratch_result = ytec::winpeapp::execute_direct_shrink_image_restore(
      scratch_harness.request, scratch_harness.dependencies());
  check(!scratch_result.has_value(), "scratch on target disk must fail");
  check(
      scratch_harness.executor_calls == 0,
      "unsafe scratch must not execute");
}

void test_ram_log_stops_before_executor() {
  Harness harness;
  harness.request.work.paths.log_path.clear();
  harness.request.work.paths.log_is_ram_only = true;
  harness.request.work.observation.log = {};
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "RAM log fallback must fail");
  check(harness.executor_calls == 0, "RAM fallback must not execute");
}

void test_cd_and_x_work_media_stop_before_executor() {
  Harness cd_harness;
  cd_harness.active_cd = true;
  auto cd_result = ytec::winpeapp::execute_direct_shrink_image_restore(
      cd_harness.request, cd_harness.dependencies());
  check(!cd_result.has_value(), "CD work media must fail");
  check(cd_harness.executor_calls == 0, "CD work media must not execute");

  Harness x_harness;
  x_harness.active_x_drive = true;
  auto x_result = ytec::winpeapp::execute_direct_shrink_image_restore(
      x_harness.request, x_harness.dependencies());
  check(!x_result.has_value(), "X: work media must fail");
  check(x_harness.executor_calls == 0, "X: work media must not execute");
}

void test_image_backing_drift_stops_before_executor() {
  Harness harness;
  harness.drift_image = true;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "image backing identity drift must fail");
  check(harness.executor_calls == 0, "drifted image must not execute");
}

void test_original_source_requires_bound_review_but_is_allowed() {
  Harness missing_review;
  mark_target_as_original_source(missing_review);
  auto missing_result = ytec::winpeapp::execute_direct_shrink_image_restore(
      missing_review.request, missing_review.dependencies());
  check(!missing_result.has_value(), "original source needs bound review");
  check(
      missing_review.executor_calls == 0,
      "unreviewed original source must not execute");

  Harness reviewed;
  mark_target_as_original_source(reviewed);
  reviewed.request.reviewed_original_source_target =
      reviewed.request.expected_target;
  auto reviewed_result = ytec::winpeapp::execute_direct_shrink_image_restore(
      reviewed.request, reviewed.dependencies());
  check(
      reviewed_result.has_value(),
      "reviewed original source should be allowed after two-step OK");
  check(reviewed.executor_calls == 1, "reviewed original source should run");
}

void test_stale_original_source_review_stops_before_executor() {
  Harness harness;
  harness.request.reviewed_original_source_target =
      harness.request.expected_target;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "stale source review must fail");
  check(harness.executor_calls == 0, "stale source review must not execute");
}

void test_original_source_review_identity_drift_stops_before_executor() {
  Harness harness;
  mark_target_as_original_source(harness);
  harness.request.reviewed_original_source_target =
      harness.request.expected_target;
  harness.request.reviewed_original_source_target->serial_suffix +=
      "-changed";
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "drifted original source review must fail");
  check(
      harness.executor_calls == 0,
      "drifted original source review must not execute");
}

void test_work_drift_stops_before_executor() {
  Harness harness;
  harness.drift_work = true;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "work path drift must fail");
  check(harness.executor_calls == 0, "work drift must not execute");
}

void test_target_layout_drift_stops_before_executor() {
  Harness harness;
  harness.drift_target = true;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "target layout drift must fail");
  check(harness.executor_calls == 0, "layout drift must not execute");
}

void test_4kn_manifest_stops_before_executor() {
  Harness harness;
  harness.request.reviewed_manifest.logical_sector_size = 4096U;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "4Kn source must fail closed");
  check(harness.executor_calls == 0, "4Kn must not execute");
}

void test_fat32_archive_stops_before_executor() {
  Harness harness;
  harness.request.reviewed_manifest.partitions[0].file_system =
      ytec::imageformat::TsumugiManifestFileSystem::fat32;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "FAT32 archive must fail closed");
  check(harness.executor_calls == 0, "FAT32 must not execute");
}

void test_exfat_archive_stops_before_executor() {
  Harness harness;
  harness.request.reviewed_manifest.partitions[0].file_system =
      ytec::imageformat::TsumugiManifestFileSystem::exfat;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "exFAT archive must fail closed");
  check(harness.executor_calls == 0, "exFAT must not execute");
}

void test_requested_and_observed_work_path_must_match() {
  Harness harness;
  auto observed = harness.request.work.observation;
  observed.log.canonical_path =
      L"E:\\YtecDiskClone\\data\\different.log";
  auto status = ytec::windowsapp::
      validate_windows_shrink_work_placement_observation(
          harness.request.image_backing_disk,
          harness.request.work.paths,
          observed);
  check(!status.has_value(), "requested/observed path mismatch must fail");

  observed = harness.request.work.observation;
  observed.scratch.canonical_path =
      L"\\\\?\\E:\\YtecDiskClone\\data";
  observed.checkpoint.canonical_path =
      L"\\\\?\\E:\\YtecDiskClone\\data\\shrink-restore-incomplete.checkpoint";
  observed.log.canonical_path =
      L"\\\\?\\E:\\YtecDiskClone\\data\\shrink-restore.log";
  status = ytec::windowsapp::
      validate_windows_shrink_work_placement_observation(
          harness.request.image_backing_disk,
          harness.request.work.paths,
          observed);
  check(status.has_value(), "equivalent extended drive paths should pass");
}

void test_reparse_work_path_stops_before_executor() {
  Harness harness;
  harness.request.work.observation.checkpoint.parent_is_reparse = true;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "reparse work path must fail");
  check(harness.executor_calls == 0, "reparse work path must not execute");
}

void test_partial_image_stops_before_executor() {
  Harness harness;
  harness.request.reviewed_image_partial_loss = true;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "partial shrink image must fail closed");
  check(harness.executor_calls == 0, "partial image must not execute");
}

void test_incomplete_evidence_is_not_success() {
  Harness harness;
  harness.incomplete_evidence = true;
  auto result = ytec::winpeapp::execute_direct_shrink_image_restore(
      harness.request, harness.dependencies());
  check(!result.has_value(), "incomplete execution evidence must fail");
  check(harness.executor_calls == 1, "evidence test should execute once");
}

}  // namespace

int wmain() {
  try {
    test_success_calls_direct_executor_once();
    test_static_exact_raw_is_allowed_only_as_bound_whole_disk_payload();
    test_generated_esp_and_msr_are_allowed_without_payload_restore();
    test_mbr_style_is_preserved_for_whole_disk_shrink_restore();
    test_lowercase_ok_stops_before_executor();
    test_active_rescue_target_stops_before_executor();
    test_image_and_work_same_disk_stops_before_executor();
    test_image_and_target_same_disk_stops_before_executor();
    test_all_three_work_paths_must_stay_on_active_rescue_disk();
    test_ram_log_stops_before_executor();
    test_cd_and_x_work_media_stop_before_executor();
    test_image_backing_drift_stops_before_executor();
    test_original_source_requires_bound_review_but_is_allowed();
    test_stale_original_source_review_stops_before_executor();
    test_original_source_review_identity_drift_stops_before_executor();
    test_work_drift_stops_before_executor();
    test_target_layout_drift_stops_before_executor();
    test_4kn_manifest_stops_before_executor();
    test_fat32_archive_stops_before_executor();
    test_exfat_archive_stops_before_executor();
    test_partial_image_stops_before_executor();
    test_incomplete_evidence_is_not_success();
    test_requested_and_observed_work_path_must_match();
    test_reparse_work_path_stops_before_executor();
  } catch (const std::exception& error) {
    std::cerr << "WinPE direct shrink restore test failed: "
              << error.what() << '\n';
    return 1;
  }
  std::cout << "WinPE direct shrink restore tests passed\n";
  return 0;
}

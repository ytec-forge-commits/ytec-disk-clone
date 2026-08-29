#include "ytec/bootrepair/system_partition_creation.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <Windows.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint64_t kEspBytes = 260ULL * kMiB;
constexpr std::uint64_t kBiosSystemBytes = 100ULL * kMiB;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

ytec::bootrepair::AutomaticBootRepairPlan missing_gpt_esp_plan() {
  using namespace ytec;
  diskmodel::PartitionInfo windows{
      .number = 1U,
      .offset_bytes = 1ULL * kMiB,
      .size_bytes = 60ULL * kGiB,
      .style = diskmodel::PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
      .identifier = L"{11111111-2222-3333-4444-555555555555}",
      .name = L"Windows",
  };
  diskmodel::DiskInfo disk{
      .disk_number = 8U,
      .device_path = L"\\\\.\\PhysicalDrive8",
      .device_interface_path = L"\\\\?\\disk#synthetic-gpt",
      .connection_location_path = L"PCIROOT(0)#SYNTHETIC",
      .device_instance_id = L"SYNTHETIC\\BOOT-REPAIR-GPT",
      .model = L"Synthetic GPT disk",
      .size_bytes = 64ULL * kGiB,
      .sector_count = 64ULL * kGiB / 512ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "A1B2C3D4",
      .partition_style = diskmodel::PartitionStyle::gpt,
      .disk_identifier = L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
      .partitions = {windows},
  };
  auto identity = diskmodel::make_stable_disk_identity(disk, false);
  check(identity.has_value(), "fixture stable identity");
  bootrepair::AutomaticBootRepairPlan plan{
      .selected_disk = disk,
      .selected_identity = identity.take_value(),
      .partition_style = diskmodel::PartitionStyle::gpt,
      .firmware = bootrepair::BcdBootFirmware::uefi,
      .required_system_partition_role =
          bootrepair::BootSystemPartitionRole::efi_system,
      .planned_bcd_store_policy =
          bootrepair::BcdBootStorePolicy::rebuild_fresh,
      .windows_installations = {
          bootrepair::DiscoveredWindowsInstallation{
              .partition = windows,
              .volume = bootrepair::BootVolumeObservation{
                  .volume_name = L"\\\\?\\Volume{AAAAAAAA-1111-2222-3333-BBBBBBBBBBBB}\\",
                  .location = bootrepair::BootRepairVolumeLocation{
                      .disk_number = 8U,
                      .starting_offset = windows.offset_bytes,
                      .extent_length = windows.size_bytes,
                      .file_system = L"NTFS",
                  },
                  .mount_points = {L"D:\\"},
              },
              .windows_directory = L"D:\\Windows",
              .version = bootrepair::OfflineWindowsVersion{
                  .major = 10U,
                  .build = 19045U,
                  .installation_type = L"Client",
              },
              .officially_supported = true,
              .winre = bootrepair::AutomaticBootRepairWinReEvidence{
                  .source_state = bootrepair::WinReSourceState::missing,
              },
          },
      },
      .system_partition_create_plan_needed = true,
  };
  return plan;
}

ytec::bootrepair::AutomaticBootRepairPlan missing_mbr_system_plan() {
  using namespace ytec;
  diskmodel::PartitionInfo windows{
      .number = 1U,
      .offset_bytes = 1ULL * kMiB,
      .size_bytes = 48ULL * kGiB,
      .style = diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
      .identifier = L"0x12345678:1",
      .name = L"Windows",
  };
  diskmodel::DiskInfo disk{
      .disk_number = 9U,
      .device_path = L"\\\\.\\PhysicalDrive9",
      .device_interface_path = L"\\\\?\\disk#synthetic-mbr",
      .connection_location_path = L"PCIROOT(0)#SYNTHETIC-MBR",
      .device_instance_id = L"SYNTHETIC\\BOOT-REPAIR-MBR",
      .model = L"Synthetic MBR disk",
      .size_bytes = 52ULL * kGiB,
      .sector_count = 52ULL * kGiB / 512ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "B1C2D3E4",
      .partition_style = diskmodel::PartitionStyle::mbr,
      .disk_identifier = L"0x12345678",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
      .partitions = {windows},
  };
  auto identity = diskmodel::make_stable_disk_identity(disk, false);
  check(identity.has_value(), "MBR fixture stable identity");
  return bootrepair::AutomaticBootRepairPlan{
      .selected_disk = disk,
      .selected_identity = identity.take_value(),
      .partition_style = diskmodel::PartitionStyle::mbr,
      .firmware = bootrepair::BcdBootFirmware::bios,
      .required_system_partition_role =
          bootrepair::BootSystemPartitionRole::bios_active,
      .planned_bcd_store_policy =
          bootrepair::BcdBootStorePolicy::rebuild_fresh,
      .windows_installations = {
          bootrepair::DiscoveredWindowsInstallation{
              .partition = windows,
              .volume = bootrepair::BootVolumeObservation{
                  .volume_name = L"\\\\?\\Volume{BBBBBBBB-1111-2222-3333-CCCCCCCCCCCC}\\",
                  .location = bootrepair::BootRepairVolumeLocation{
                      .disk_number = 9U,
                      .starting_offset = windows.offset_bytes,
                      .extent_length = windows.size_bytes,
                      .file_system = L"NTFS",
                  },
                  .mount_points = {L"E:\\"},
              },
              .windows_directory = L"E:\\Windows",
              .version = bootrepair::OfflineWindowsVersion{
                  .major = 10U,
                  .build = 19045U,
                  .installation_type = L"Client",
              },
              .officially_supported = true,
              .winre = bootrepair::AutomaticBootRepairWinReEvidence{
                  .source_state = bootrepair::WinReSourceState::unknown,
              },
          },
      },
      .system_partition_create_plan_needed = true,
  };
}

ytec::bootrepair::SystemPartitionCreationObservation safe_observation(
    ytec::bootrepair::AutomaticBootRepairPlan plan) {
  return ytec::bootrepair::SystemPartitionCreationObservation{
      .plan = std::move(plan),
      .max_reclaimable_bytes = 1024ULL * kMiB,
      .exact_windows_volume_found = true,
      .simple_ntfs_volume = true,
      .volume_online = true,
      .volume_transition_stable = true,
      .volume_health_acceptable = true,
      .forbidden_volume_role_or_encryption = false,
  };
}

ytec::bootrepair::AutomaticBootRepairPlan shrunken_plan(
    ytec::bootrepair::AutomaticBootRepairPlan plan) {
  const std::uint64_t system_bytes =
      plan.required_system_partition_role ==
              ytec::bootrepair::BootSystemPartitionRole::efi_system
          ? kEspBytes
          : kBiosSystemBytes;
  auto& disk_partition = plan.selected_disk.partitions.front();
  auto& windows = plan.windows_installations.front();
  disk_partition.size_bytes -= system_bytes;
  windows.partition.size_bytes -= system_bytes;
  windows.volume.location.extent_length -= system_bytes;
  return plan;
}

ytec::bootrepair::AutomaticBootRepairPlan completed_plan(
    ytec::bootrepair::AutomaticBootRepairPlan plan) {
  using namespace ytec;
  plan = shrunken_plan(std::move(plan));
  const bool uefi = plan.required_system_partition_role ==
      bootrepair::BootSystemPartitionRole::efi_system;
  const std::uint64_t system_bytes = uefi ? kEspBytes : kBiosSystemBytes;
  const auto system_offset = plan.selected_disk.partitions.front().offset_bytes +
      plan.selected_disk.partitions.front().size_bytes;
  diskmodel::PartitionInfo esp{
      .number = 2U,
      .offset_bytes = system_offset,
      .size_bytes = system_bytes,
      .style = uefi ? diskmodel::PartitionStyle::gpt
                    : diskmodel::PartitionStyle::mbr,
      .type = uefi ? L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}"
                   : L"0x07",
      .identifier = uefi ? L"{99999999-8888-7777-6666-555555555555}"
                         : L"0x12345678:2",
      .name = L"SYSTEM",
      .bootable = !uefi,
  };
  plan.selected_disk.partitions.push_back(esp);
  plan.system_partition_create_plan_needed = false;
  plan.system_partition_candidates = {
      bootrepair::DiscoveredSystemPartition{
          .partition = esp,
          .volume = bootrepair::BootVolumeObservation{
              .volume_name = L"\\\\?\\Volume{EEEEEEEE-1111-2222-3333-FFFFFFFFFFFF}\\",
              .location = bootrepair::BootRepairVolumeLocation{
                  .disk_number = plan.selected_disk.disk_number,
                  .starting_offset = system_offset,
                  .extent_length = system_bytes,
                  .file_system = uefi ? L"FAT32" : L"NTFS",
              },
          },
          .role = uefi ? bootrepair::BootSystemPartitionRole::efi_system
                       : bootrepair::BootSystemPartitionRole::bios_active,
          .efi_ownership = bootrepair::EfiBootOwnershipEvidence{
              .state = uefi
                  ? bootrepair::EfiBootOwnershipState::
                        microsoft_only_or_empty
                  : bootrepair::EfiBootOwnershipState::not_applicable,
              .efi_directory_present = false,
          },
      },
  };
  return plan;
}

class MockPlatform final
    : public ytec::bootrepair::ISystemPartitionCreationPlatform {
 public:
  std::vector<ytec::bootrepair::SystemPartitionCreationObservation>
      observations;
  std::size_t observation_index{};
  bool fail_shrink{};
  bool fail_create{};
  bool fail_delete{};
  bool fail_extend{};
  std::vector<std::string> calls;

  ytec::clonecore::Result<
      ytec::bootrepair::SystemPartitionCreationObservation>
  observe_read_only(
      const ytec::clonecore::StableDiskIdentity&,
      const std::uint32_t) override {
    calls.push_back("observe");
    if (observation_index >= observations.size()) {
      return failure<ytec::bootrepair::SystemPartitionCreationObservation>(
          L"mock observation exhausted");
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::SystemPartitionCreationObservation>::success(
        observations[observation_index++]);
  }

  ytec::clonecore::Status shrink_windows_exact(
      const ytec::bootrepair::ReviewedSystemPartitionCreation&) override {
    calls.push_back("shrink");
    return fail_shrink ? status_failure(L"mock shrink")
                       : ytec::clonecore::success_status();
  }

  ytec::clonecore::Status create_and_format_system_exact(
      const ytec::bootrepair::ReviewedSystemPartitionCreation&) override {
    calls.push_back("create-format");
    return fail_create ? status_failure(L"mock create")
                       : ytec::clonecore::success_status();
  }

  ytec::clonecore::Status delete_created_system_exact(
      const ytec::bootrepair::ReviewedSystemPartitionCreation&) override {
    calls.push_back("delete");
    return fail_delete ? status_failure(L"mock delete")
                       : ytec::clonecore::success_status();
  }

  ytec::clonecore::Status extend_windows_exact(
      const ytec::bootrepair::ReviewedSystemPartitionCreation&) override {
    calls.push_back("extend");
    return fail_extend ? status_failure(L"mock extend")
                       : ytec::clonecore::success_status();
  }

 private:
  template <typename T>
  static ytec::clonecore::Result<T> failure(const std::wstring& message) {
    return ytec::clonecore::Result<T>::failure(error(message));
  }

  static ytec::clonecore::Status status_failure(
      const std::wstring& message) {
    return ytec::clonecore::Status::failure(error(message));
  }

  static ytec::clonecore::Error error(const std::wstring& message) {
    return ytec::clonecore::Error{
        .code = ytec::clonecore::ErrorCode::io_failed,
        .native_code = ERROR_GEN_FAILURE,
        .operation = L"system partition creation mock",
        .message = message,
    };
  }
};

ytec::bootrepair::ReviewedSystemPartitionCreation reviewed_fixture() {
  const auto plan = missing_gpt_esp_plan();
  auto reviewed = ytec::bootrepair::review_system_partition_creation(
      plan, 1U, safe_observation(plan));
  check(reviewed.has_value(), "valid missing-ESP plan must review");
  return reviewed.take_value();
}

void pure_review_binds_exact_safe_geometry() {
  const auto reviewed = reviewed_fixture();
  check(
      reviewed.system_role() ==
              ytec::bootrepair::BootSystemPartitionRole::efi_system &&
          reviewed.system_partition_size_bytes() == kEspBytes &&
          reviewed.reclaim_bytes() == kEspBytes &&
          reviewed.shrunken_windows_size_bytes() ==
              60ULL * kGiB - kEspBytes &&
          reviewed.system_partition_offset_bytes() ==
              1ULL * kMiB + 60ULL * kGiB - kEspBytes,
      "review must bind 260 MiB ESP to exact end-shrink geometry");
}

void mbr_bios_active_creation_uses_its_own_exact_transaction() {
  const auto plan = missing_mbr_system_plan();
  auto reviewed = ytec::bootrepair::review_system_partition_creation(
      plan, 1U, safe_observation(plan));
  check(
      reviewed.has_value() &&
          reviewed.value().system_role() == ytec::bootrepair::
              BootSystemPartitionRole::bios_active &&
          reviewed.value().system_partition_size_bytes() ==
              kBiosSystemBytes &&
          reviewed.value().reclaim_bytes() == kBiosSystemBytes &&
          reviewed.value().shrunken_windows_size_bytes() ==
              48ULL * kGiB - kBiosSystemBytes,
      "MBR review must bind a 100 MiB NTFS Active end-shrink geometry");

  MockPlatform platform;
  platform.observations = {
      safe_observation(plan),
      safe_observation(shrunken_plan(plan)),
      safe_observation(completed_plan(plan)),
  };
  const auto report = ytec::bootrepair::execute_system_partition_creation(
      reviewed.value(),
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      platform);
  check(
      report.outcome ==
              ytec::bootrepair::SystemPartitionCreationOutcome::committed &&
          report.completed_plan_verified &&
          report.completed_plan.has_value() &&
          report.completed_plan->system_partition_candidates.front()
              .partition.bootable &&
          report.completed_plan->system_partition_candidates.front()
                  .volume.location.file_system == L"NTFS",
      "MBR transaction must verify the NTFS Active system partition before commit");
}

void unsafe_or_insufficient_volume_fails_closed() {
  const auto plan = missing_gpt_esp_plan();
  auto observation = safe_observation(plan);
  observation.forbidden_volume_role_or_encryption = true;
  check(
      !ytec::bootrepair::review_system_partition_creation(
           plan, 1U, observation),
      "BitLocker/system/pagefile-style VDS flags must reject review");
  observation = safe_observation(plan);
  observation.max_reclaimable_bytes = kEspBytes + 128ULL * kMiB;
  check(
      !ytec::bootrepair::review_system_partition_creation(
           plan, 1U, observation),
      "review must retain the reclaimability safety margin");

  auto logical_mbr = missing_mbr_system_plan();
  logical_mbr.selected_disk.partitions.front().number = 5U;
  logical_mbr.windows_installations.front().partition.number = 5U;
  check(
      !ytec::bootrepair::review_system_partition_creation(
          logical_mbr, 5U, safe_observation(logical_mbr)),
      "a logical or extended MBR layout must not be used for an Active primary system partition");
}

void exact_uppercase_ok_is_mandatory() {
  const auto reviewed = reviewed_fixture();
  MockPlatform platform;
  auto report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"ok"},
      platform);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::failed_before_mutation &&
          platform.calls.empty(),
      "lowercase confirmation must perform no observation or mutation");
  report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = false, .typed_token = L"OK"},
      platform);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::failed_before_mutation &&
          platform.calls.empty(),
      "uppercase OK without the separate first-step acknowledgement must perform no call");
}

void successful_transaction_reobserves_every_boundary() {
  const auto plan = missing_gpt_esp_plan();
  const auto reviewed = reviewed_fixture();
  MockPlatform platform;
  platform.observations = {
      safe_observation(plan),
      safe_observation(shrunken_plan(plan)),
      safe_observation(completed_plan(plan)),
  };
  auto report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      platform);
  check(
      report.outcome ==
              ytec::bootrepair::SystemPartitionCreationOutcome::committed &&
          report.confirmation_verified &&
          report.pre_mutation_revalidated && report.windows_shrunk &&
          report.shrunken_layout_verified &&
          report.system_partition_created_and_formatted &&
          report.completed_plan_verified &&
          report.completed_plan.has_value() &&
          platform.calls == std::vector<std::string>{
              "observe", "shrink", "observe", "create-format", "observe"},
      "transaction must confirm, reobserve, shrink, verify, create/format, and reobserve");
}

void failed_shrink_is_classified_by_exact_readback() {
  const auto plan = missing_gpt_esp_plan();
  const auto reviewed = reviewed_fixture();

  MockPlatform unchanged;
  unchanged.fail_shrink = true;
  unchanged.observations = {
      safe_observation(plan),
      safe_observation(plan),
  };
  auto report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      unchanged);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::failed_before_mutation &&
          !report.windows_shrunk && !report.rollback_attempted &&
          unchanged.calls == std::vector<std::string>{
              "observe", "shrink", "observe"},
      "a failed shrink with exact original readback must be classified as no mutation");

  MockPlatform changed_exactly;
  changed_exactly.fail_shrink = true;
  changed_exactly.observations = {
      safe_observation(plan),
      safe_observation(shrunken_plan(plan)),
      safe_observation(plan),
  };
  report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      changed_exactly);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::rolled_back_exact &&
          report.windows_shrunk && report.shrunken_layout_verified &&
          report.original_layout_restored &&
          changed_exactly.calls == std::vector<std::string>{
              "observe", "shrink", "observe", "extend", "observe"},
      "a failed shrink that nevertheless changed exact geometry must rollback exactly");

  auto partial = shrunken_plan(plan);
  partial.selected_disk.partitions.front().size_bytes += kMiB;
  partial.windows_installations.front().partition.size_bytes += kMiB;
  partial.windows_installations.front().volume.location.extent_length += kMiB;
  MockPlatform uncertain;
  uncertain.fail_shrink = true;
  uncertain.observations = {
      safe_observation(plan),
      safe_observation(std::move(partial)),
  };
  report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      uncertain);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::rollback_incomplete &&
          report.rollback_error.has_value() && !report.rollback_attempted &&
          uncertain.calls == std::vector<std::string>{
              "observe", "shrink", "observe"},
      "an uncertain partial shrink must stop without an unsafe extend");
}

void raw_layout_cleanup_binding_requires_every_partition() {
  const auto plan = missing_gpt_esp_plan();
  const auto reviewed = reviewed_fixture();
  auto shrunken = shrunken_plan(plan);
  check(
      ytec::bootrepair::validate_system_partition_creation_shrunken_plan(
          reviewed, shrunken)
          .has_value() &&
      ytec::bootrepair::validate_system_partition_creation_shrunken_disk(
          reviewed, shrunken.selected_disk)
          .has_value(),
      "exact shrunken plan and raw layout must bind");
  auto changed_mount = shrunken;
  changed_mount.windows_installations.front().volume.mount_points = {L"Q:\\"};
  check(
      !ytec::bootrepair::validate_system_partition_creation_shrunken_plan(
          reviewed, changed_mount),
      "a Windows mount binding drift must block the next mutation");

  auto created = completed_plan(plan);
  check(
      ytec::bootrepair::validate_system_partition_creation_completed_plan(
          reviewed, created)
          .has_value() &&
      ytec::bootrepair::
          validate_system_partition_creation_created_disk_for_rollback(
              reviewed, created.selected_disk)
              .has_value(),
      "exact created raw layout must bind for rollback");
  created.selected_disk.partitions.push_back(
      ytec::diskmodel::PartitionInfo{
          .number = 3U,
          .offset_bytes = 62ULL * kGiB,
          .size_bytes = kMiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
          .identifier = L"{12345678-1234-1234-1234-123456789ABC}",
      });
  check(
      !ytec::bootrepair::
          validate_system_partition_creation_created_disk_for_rollback(
              reviewed, created.selected_disk),
      "an unrelated extra partition must block rollback deletion");

  const auto mbr_plan = missing_mbr_system_plan();
  auto mbr_reviewed = ytec::bootrepair::review_system_partition_creation(
      mbr_plan, 1U, safe_observation(mbr_plan));
  check(mbr_reviewed.has_value(), "MBR raw cleanup review fixture");
  auto mbr_created = completed_plan(mbr_plan);
  mbr_created.selected_disk.partitions.back().identifier.clear();
  check(
      ytec::bootrepair::
          validate_system_partition_creation_created_disk_for_rollback(
              mbr_reviewed.value(), mbr_created.selected_disk)
              .has_value(),
      "MBR inventory has no partition GUID and must bind by exact primary extent and type");
}

void post_shrink_failure_rolls_back_exact() {
  const auto plan = missing_gpt_esp_plan();
  const auto reviewed = reviewed_fixture();
  MockPlatform platform;
  platform.fail_create = true;
  platform.observations = {
      safe_observation(plan),
      safe_observation(shrunken_plan(plan)),
      safe_observation(plan),
  };
  const auto report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      platform);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::rolled_back_exact &&
          report.rollback_attempted && report.created_partition_removed &&
          report.windows_extent_restored && report.original_layout_restored &&
          platform.calls == std::vector<std::string>{
              "observe", "shrink", "observe", "create-format", "extend", "observe"},
      "format/create failure must restore the original exact Windows extent");
}

void unsafe_post_shrink_volume_state_rolls_back_before_creation() {
  const auto plan = missing_gpt_esp_plan();
  const auto reviewed = reviewed_fixture();
  auto unsafe_after_shrink = safe_observation(shrunken_plan(plan));
  unsafe_after_shrink.volume_health_acceptable = false;
  MockPlatform platform;
  platform.observations = {
      safe_observation(plan),
      std::move(unsafe_after_shrink),
      safe_observation(plan),
  };
  const auto report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      platform);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::rolled_back_exact &&
          !report.system_partition_created_and_formatted &&
          platform.calls == std::vector<std::string>{
              "observe", "shrink", "observe", "extend", "observe"},
      "an unhealthy post-shrink NTFS volume must rollback before partition creation");
}

void changed_final_layout_is_deleted_and_rolled_back() {
  const auto plan = missing_gpt_esp_plan();
  const auto reviewed = reviewed_fixture();
  auto wrong = completed_plan(plan);
  wrong.system_partition_candidates.front().partition.size_bytes -= kMiB;
  MockPlatform platform;
  platform.observations = {
      safe_observation(plan),
      safe_observation(shrunken_plan(plan)),
      safe_observation(wrong),
      safe_observation(plan),
  };
  const auto report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      platform);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::rolled_back_exact &&
          platform.calls == std::vector<std::string>{
              "observe", "shrink", "observe", "create-format", "observe",
              "delete", "extend", "observe"},
      "unexpected created geometry must be removed before exact extent rollback");
}

void pre_mutation_drift_never_calls_shrink() {
  const auto plan = missing_gpt_esp_plan();
  const auto reviewed = reviewed_fixture();
  auto changed = plan;
  changed.selected_disk.partitions.front().size_bytes -= kMiB;
  MockPlatform platform;
  platform.observations = {safe_observation(changed)};
  const auto report = ytec::bootrepair::execute_system_partition_creation(
      reviewed,
      {.first_step_acknowledged = true, .typed_token = L"OK"},
      platform);
  check(
      report.outcome == ytec::bootrepair::
              SystemPartitionCreationOutcome::failed_before_mutation &&
          platform.calls == std::vector<std::string>{"observe"},
      "layout drift must fail before the first mutation");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"pure_review_binds_exact_safe_geometry",
       pure_review_binds_exact_safe_geometry},
      {"mbr_bios_active_creation_uses_its_own_exact_transaction",
       mbr_bios_active_creation_uses_its_own_exact_transaction},
      {"unsafe_or_insufficient_volume_fails_closed",
       unsafe_or_insufficient_volume_fails_closed},
      {"exact_uppercase_ok_is_mandatory", exact_uppercase_ok_is_mandatory},
      {"successful_transaction_reobserves_every_boundary",
       successful_transaction_reobserves_every_boundary},
      {"failed_shrink_is_classified_by_exact_readback",
       failed_shrink_is_classified_by_exact_readback},
      {"raw_layout_cleanup_binding_requires_every_partition",
       raw_layout_cleanup_binding_requires_every_partition},
      {"post_shrink_failure_rolls_back_exact",
       post_shrink_failure_rolls_back_exact},
      {"unsafe_post_shrink_volume_state_rolls_back_before_creation",
       unsafe_post_shrink_volume_state_rolls_back_before_creation},
      {"changed_final_layout_is_deleted_and_rolled_back",
       changed_final_layout_is_deleted_and_rolled_back},
      {"pre_mutation_drift_never_calls_shrink",
       pre_mutation_drift_never_calls_shrink},
  };
  for (const auto& [name, test] : tests) {
    test();
    std::cout << "PASS: " << name << '\n';
  }
  return 0;
}

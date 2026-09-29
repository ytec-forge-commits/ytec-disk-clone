#include "ytec/bootrepair/automatic_repair_plan.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::wstring_view kEfiPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kBasicDataPartitionType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error test_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::query_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = operation,
      .message = L"合成失敗",
  };
}

std::wstring volume_root(const std::uint32_t partition_number) {
  std::wstring suffix = std::to_wstring(partition_number);
  while (suffix.size() < 12U) {
    suffix.insert(suffix.begin(), L'0');
  }
  return L"\\\\?\\Volume{11111111-1111-1111-1111-" + suffix + L"}\\";
}

ytec::diskmodel::DiskInfo make_gpt_disk(
    const std::size_t system_partition_count = 1U,
    const std::size_t windows_partition_count = 1U) {
  using ytec::diskmodel::PartitionInfo;
  using ytec::diskmodel::PartitionStyle;
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 4U,
      .device_path = L"\\\\.\\PhysicalDrive4",
      .device_instance_id = L"MOCK\\BOOT\\GPT",
      .model = L"Tsumugi GPT",
      .size_bytes = 4ULL * 1024ULL * kMiB,
      .sector_count = 8ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"NVMe",
      .serial_suffix = "GPT00001",
      .partition_style = PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  std::uint64_t offset = kMiB;
  std::uint32_t number = 1U;
  for (std::size_t index = 0U; index < system_partition_count; ++index) {
    disk.partitions.push_back(PartitionInfo{
        .number = number++,
        .offset_bytes = offset,
        .size_bytes = 64ULL * kMiB,
        .style = PartitionStyle::gpt,
        .type = std::wstring(kEfiPartitionType),
        .identifier = L"{ESP-" + std::to_wstring(index + 1U) + L"}",
        .name = L"EFI " + std::to_wstring(index + 1U),
    });
    offset += 64ULL * kMiB;
  }
  for (std::size_t index = 0U; index < windows_partition_count; ++index) {
    disk.partitions.push_back(PartitionInfo{
        .number = number++,
        .offset_bytes = offset,
        .size_bytes = 512ULL * kMiB,
        .style = PartitionStyle::gpt,
        .type = std::wstring(kBasicDataPartitionType),
        .identifier = L"{WINDOWS-" + std::to_wstring(index + 1U) + L"}",
        .name = L"Windows " + std::to_wstring(index + 1U),
    });
    offset += 512ULL * kMiB;
  }
  return disk;
}

ytec::diskmodel::DiskInfo make_mbr_disk() {
  using ytec::diskmodel::PartitionInfo;
  using ytec::diskmodel::PartitionStyle;
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 5U,
      .device_path = L"\\\\.\\PhysicalDrive5",
      .device_instance_id = L"MOCK\\BOOT\\MBR",
      .model = L"Tsumugi MBR",
      .size_bytes = 2ULL * 1024ULL * kMiB,
      .sector_count = 4ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 512U,
      .bus_type = L"SATA",
      .serial_suffix = "MBR00001",
      .partition_style = PartitionStyle::mbr,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  disk.partitions.push_back(PartitionInfo{
      .number = 1U,
      .offset_bytes = kMiB,
      .size_bytes = 1536ULL * kMiB,
      .style = PartitionStyle::mbr,
      .type = L"0x07",
      .identifier = L"0x12345678:1",
      .name = L"Windows",
      .bootable = true,
  });
  return disk;
}

ytec::clonecore::StableDiskIdentity identity_for(
    const ytec::diskmodel::DiskInfo& disk) {
  const auto identity = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(identity.has_value(), "Fixture stable identity should build");
  return identity.value();
}

ytec::bootrepair::BootVolumeObservation make_volume(
    const ytec::diskmodel::DiskInfo& disk,
    const ytec::diskmodel::PartitionInfo& partition,
    std::wstring file_system) {
  return ytec::bootrepair::BootVolumeObservation{
      .volume_name = volume_root(partition.number),
      .location = ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = disk.disk_number,
          .starting_offset = partition.offset_bytes,
          .extent_length = partition.size_bytes,
          .file_system = std::move(file_system),
      },
  };
}

ytec::bootrepair::OfflineWindowsCandidateValidation supported_windows() {
  return {
      .state = ytec::bootrepair::OfflineWindowsCandidateState::
          present_supported,
      .version = ytec::bootrepair::OfflineWindowsVersion{
          .major = 10U,
          .build = 26100U,
          .installation_type = L"Client",
      },
  };
}

ytec::bootrepair::WinReDiagnosticReport trusted_winre(
    const std::uint32_t partition_number = 0U) {
  return {
      .source_state = partition_number != 0U
          ? ytec::bootrepair::WinReSourceState::registered_partition
          : ytec::bootrepair::WinReSourceState::image_available_in_windows,
      .registered_partition_number = partition_number,
      .winre_image_size_bytes = 640ULL * kMiB,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .registered_location_reported = partition_number != 0U,
      .registered_path_kind_reported = partition_number != 0U,
      .registered_location_matches_expected_disk = partition_number != 0U,
      .registered_image_present = partition_number != 0U,
      .fallback_image_present = partition_number == 0U,
  };
}

class Inventory final : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++calls;
    return ytec::clonecore::Result<
        ytec::diskmodel::InventoryReport>::success(report);
  }

  ytec::diskmodel::InventoryReport report;
  std::size_t calls{};
};

class Volumes final
    : public ytec::bootrepair::IBootRepairVolumeObservationProvider {
 public:
  ytec::clonecore::Result<std::vector<
      ytec::bootrepair::BootVolumeObservation>>
  observe_read_only(
      const ytec::diskmodel::DiskInfo& selected_disk) override {
    ++calls;
    last_disk_number = selected_disk.disk_number;
    return ytec::clonecore::Result<std::vector<
        ytec::bootrepair::BootVolumeObservation>>::success(observations);
  }

  std::vector<ytec::bootrepair::BootVolumeObservation> observations;
  std::size_t calls{};
  std::uint32_t last_disk_number{};
};

class WindowsValidator final
    : public ytec::bootrepair::IOfflineWindowsCandidateValidator {
 public:
  ytec::clonecore::Result<
      ytec::bootrepair::OfflineWindowsCandidateValidation>
  inspect_volume_read_only(const std::wstring& volume_root_value) override {
    calls.push_back(volume_root_value);
    const auto found = observations.find(volume_root_value);
    if (found == observations.end()) {
      return ytec::clonecore::Result<
          ytec::bootrepair::OfflineWindowsCandidateValidation>::success({});
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::OfflineWindowsCandidateValidation>::success(
        found->second);
  }

  std::map<
      std::wstring,
      ytec::bootrepair::OfflineWindowsCandidateValidation>
      observations;
  std::vector<std::wstring> calls;
};

class WinRe final : public ytec::bootrepair::IWinReDiagnosticService {
 public:
  ytec::clonecore::Result<ytec::bootrepair::WinReDiagnosticReport> inspect(
      const std::wstring& offline_windows_directory,
      const std::uint32_t expected_target_disk_number) override {
    calls.emplace_back(
        offline_windows_directory, expected_target_disk_number);
    const auto found = observations.find(offline_windows_directory);
    if (found == observations.end()) {
      return ytec::clonecore::Result<
          ytec::bootrepair::WinReDiagnosticReport>::failure(
          test_error(L"合成WinRE診断"));
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::WinReDiagnosticReport>::success(found->second);
  }

  std::map<std::wstring, ytec::bootrepair::WinReDiagnosticReport>
      observations;
  std::vector<std::pair<std::wstring, std::uint32_t>> calls;
};

class EfiOwnership final
    : public ytec::bootrepair::IEfiBootOwnershipInspector {
 public:
  ytec::clonecore::Result<ytec::bootrepair::EfiBootOwnershipEvidence>
  inspect_existing_esp_read_only(const std::wstring& volume_root_value)
      override {
    calls.push_back(volume_root_value);
    if (fail) {
      return ytec::clonecore::Result<
          ytec::bootrepair::EfiBootOwnershipEvidence>::failure(
          test_error(L"合成ESP EFI所有権診断"));
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::EfiBootOwnershipEvidence>::success(evidence);
  }

  ytec::bootrepair::EfiBootOwnershipEvidence evidence{
      .state = ytec::bootrepair::EfiBootOwnershipState::
          microsoft_only_or_empty,
      .efi_directory_present = true,
      .microsoft_namespace_present = true,
      .microsoft_signed_efi_loader_count = 4U,
  };
  bool fail{};
  std::vector<std::wstring> calls;
};

struct Fixture final {
  explicit Fixture(ytec::diskmodel::DiskInfo input_disk)
      : disk(std::move(input_disk)) {
    inventory.report.disks = {disk};
    for (const auto& partition : disk.partitions) {
      const bool fat32 = disk.partition_style ==
              ytec::diskmodel::PartitionStyle::gpt &&
          partition.type == kEfiPartitionType;
      volumes.observations.push_back(make_volume(
          disk, partition, fat32 ? L"FAT32" : L"NTFS"));
    }
  }

  void mark_windows(const std::size_t partition_index) {
    const auto& partition = disk.partitions.at(partition_index);
    const std::wstring root = volume_root(partition.number);
    windows.observations[root] = supported_windows();
    winre.observations[root + L"Windows"] = trusted_winre();
  }

  ytec::bootrepair::AutomaticBootRepairPlanner planner() {
    return ytec::bootrepair::AutomaticBootRepairPlanner(
        inventory, volumes, windows, winre, efi_ownership);
  }

  ytec::diskmodel::DiskInfo disk;
  Inventory inventory;
  Volumes volumes;
  WindowsValidator windows;
  WinRe winre;
  EfiOwnership efi_ownership;
};

void test_gpt_single_windows_plan_is_read_only_and_fresh() {
  Fixture fixture(make_gpt_disk());
  fixture.mark_windows(1U);
  fixture.winre.observations[volume_root(2U) + L"Windows"] =
      trusted_winre(3U);
  auto planner = fixture.planner();

  const auto result = planner.plan(identity_for(fixture.disk));
  check(result.has_value(), "GPT discovery should succeed");
  const auto& plan = result.value();
  check(
      plan.partition_style == ytec::diskmodel::PartitionStyle::gpt &&
          plan.firmware == ytec::bootrepair::BcdBootFirmware::uefi,
      "GPT should plan UEFI repair");
  check(
      plan.planned_bcd_store_policy ==
          ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh,
      "Automatic plan must require a fresh BCD store");
  check(
      plan.windows_installations.size() == 1U &&
          !plan.windows_not_found &&
          !plan.windows_selection_policy_needed,
      "One Windows installation should be explicit and unambiguous");
  check(
      plan.system_partition_candidates.size() == 1U &&
          !plan.system_partition_create_plan_needed,
      "Existing ESP should be discovered without a create plan");
  check(
      plan.windows_installations.front().winre.source_state ==
              ytec::bootrepair::WinReSourceState::registered_partition &&
          plan.windows_installations.front()
              .winre.registered_location_matches_selected_disk,
      "WinRE evidence should be preserved");
  check(
      fixture.inventory.calls == 1U && fixture.volumes.calls == 1U &&
          fixture.windows.calls.size() == 1U &&
          fixture.winre.calls.size() == 1U &&
          fixture.efi_ownership.calls.size() == 1U,
      "Discovery should only use the five read-only seams");
  check(
      plan.system_partition_candidates.front().efi_ownership.state ==
          ytec::bootrepair::EfiBootOwnershipState::
              microsoft_only_or_empty,
      "The existing ESP ownership evidence must be retained in the plan");
}

void test_multiple_windows_require_explicit_policy() {
  Fixture fixture(make_gpt_disk(1U, 2U));
  fixture.mark_windows(1U);
  fixture.mark_windows(2U);
  auto planner = fixture.planner();

  const auto result = planner.plan(identity_for(fixture.disk));
  check(result.has_value(), "Multiple Windows discovery should succeed");
  check(
      result.value().windows_installations.size() == 2U,
      "Every Windows installation must be returned");
  check(
      result.value().windows_selection_policy_needed,
      "Multiple Windows installations must require caller policy");
}

void test_missing_esp_reports_create_plan_needed_without_writes() {
  Fixture fixture(make_gpt_disk(0U, 1U));
  fixture.mark_windows(0U);
  auto planner = fixture.planner();

  const auto result = planner.plan(identity_for(fixture.disk));
  check(result.has_value(), "Missing ESP should remain a planning result");
  check(
      result.value().system_partition_candidates.empty() &&
          result.value().system_partition_create_plan_needed,
      "Missing ESP should request a separate create plan");
  check(
      result.value().required_system_partition_role ==
          ytec::bootrepair::BootSystemPartitionRole::efi_system,
      "The required missing role should be explicit");
}

void test_mbr_active_ntfs_selects_bios() {
  Fixture fixture(make_mbr_disk());
  fixture.mark_windows(0U);
  auto planner = fixture.planner();

  const auto result = planner.plan(identity_for(fixture.disk));
  check(result.has_value(), "MBR discovery should succeed");
  check(
      result.value().firmware == ytec::bootrepair::BcdBootFirmware::bios &&
          result.value().required_system_partition_role ==
              ytec::bootrepair::BootSystemPartitionRole::bios_active,
      "MBR should plan BIOS repair");
  check(
      result.value().system_partition_candidates.size() == 1U &&
          result.value().windows_installations.size() == 1U,
      "An active Windows partition may serve both roles");
}

void test_multiple_system_partitions_are_not_auto_selected() {
  Fixture fixture(make_gpt_disk(2U, 1U));
  fixture.mark_windows(2U);
  auto planner = fixture.planner();

  const auto result = planner.plan(identity_for(fixture.disk));
  check(result.has_value(), "Multiple ESP discovery should succeed");
  check(
      result.value().system_partition_candidates.size() == 2U &&
          result.value().system_partition_selection_policy_needed,
      "Multiple ESPs should be returned without automatic selection");
}

void test_unsupported_windows_is_returned_with_policy_flag() {
  Fixture fixture(make_gpt_disk());
  const std::wstring root = volume_root(2U);
  fixture.windows.observations[root] = {
      .state = ytec::bootrepair::OfflineWindowsCandidateState::
          present_unsupported,
      .version = ytec::bootrepair::OfflineWindowsVersion{
          .major = 6U,
          .build = 7601U,
          .installation_type = L"Client",
      },
  };
  fixture.winre.observations[root + L"Windows"] = trusted_winre();
  auto planner = fixture.planner();

  const auto result = planner.plan(identity_for(fixture.disk));
  check(result.has_value(), "Unsupported Windows should remain discoverable");
  check(
      result.value().windows_installations.size() == 1U &&
          !result.value().windows_installations.front().officially_supported &&
          result.value().unsupported_windows_policy_needed,
      "Unsupported Windows should require explicit caller policy");
}

void test_disk_info_input_is_reidentified_instead_of_trusting_disk_number() {
  const auto remembered = make_gpt_disk();
  Fixture fixture(remembered);
  fixture.disk.disk_number = 9U;
  fixture.disk.device_path = L"\\\\.\\PhysicalDrive9";
  fixture.inventory.report.disks = {fixture.disk};
  for (auto& volume : fixture.volumes.observations) {
    volume.location.disk_number = 9U;
  }
  fixture.mark_windows(1U);
  auto planner = fixture.planner();

  const auto result = planner.plan(remembered);
  check(result.has_value(), "DiskInfo overload should reidentify by stable data");
  check(
      result.value().selected_disk.disk_number == 9U &&
          fixture.volumes.last_disk_number == 9U,
      "Remembered disk number must not be trusted");
}

void test_identity_mismatch_stops_before_volume_observation() {
  Fixture fixture(make_gpt_disk());
  const auto selected_identity = identity_for(fixture.disk);
  fixture.inventory.report.disks.front().serial_suffix = "CHANGED1";
  auto planner = fixture.planner();

  const auto result = planner.plan(selected_identity);
  check(!result.has_value(), "Identity mismatch should fail closed");
  check(
      result.error().code == ytec::clonecore::ErrorCode::identity_mismatch,
      "Identity mismatch should remain distinguishable");
  check(
      fixture.volumes.calls == 0U && fixture.windows.calls.empty() &&
          fixture.winre.calls.empty() && fixture.efi_ownership.calls.empty(),
      "No volume or filesystem probe may run for the wrong disk");
}

void test_efi_ownership_probe_failure_stops_gpt_plan() {
  Fixture fixture(make_gpt_disk());
  fixture.mark_windows(1U);
  fixture.efi_ownership.fail = true;
  auto planner = fixture.planner();

  const auto result = planner.plan(identity_for(fixture.disk));
  check(!result.has_value(), "An unreadable ESP ownership state must stop");
  check(
      result.error().code == ytec::clonecore::ErrorCode::query_failed &&
          fixture.windows.calls.empty(),
      "ESP ownership failure must stop before Windows classification");
}

void test_untrusted_winre_result_is_rejected() {
  Fixture fixture(make_gpt_disk());
  fixture.mark_windows(1U);
  auto& report = fixture.winre.observations[volume_root(2U) + L"Windows"];
  report.read_only_command = false;
  auto planner = fixture.planner();

  const auto result = planner.plan(identity_for(fixture.disk));
  check(!result.has_value(), "A write-capable WinRE probe must be rejected");
  check(
      result.error().code == ytec::clonecore::ErrorCode::verification_failed,
      "WinRE trust failure should be explicit");
}

void test_multiboot_choices_preserve_explicit_priority() {
  Fixture fixture(make_gpt_disk(1U, 2U));
  fixture.mark_windows(1U);
  fixture.mark_windows(2U);
  auto planner = fixture.planner();
  const auto discovery = planner.plan(identity_for(fixture.disk));
  check(discovery.has_value(), "Multi-boot discovery should succeed");

  const auto reviewed =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          discovery.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {3U, 2U},
              .system_partition_number = 1U,
          });
  check(reviewed.has_value(), "Explicit multi-boot choices should pass");
  const auto windows = reviewed.value().windows_in_boot_priority();
  check(
      windows.size() == 2U && windows[0].partition.number == 3U &&
          windows[1].partition.number == 2U &&
          reviewed.value().windows_policy() == ytec::bootrepair::
              AutomaticWindowsRegistrationPolicy::
                  all_with_explicit_priority,
      "Every Windows installation must retain the reviewed boot priority");
  check(
      reviewed.value().system_partition().partition.number == 1U &&
          reviewed.value().bcd_store_policy() ==
              ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh,
      "The exact ESP and fresh-store policy must be immutable choices");
}

void test_selected_only_and_invalid_multiboot_choices_fail_closed() {
  Fixture fixture(make_gpt_disk(1U, 2U));
  fixture.mark_windows(1U);
  fixture.mark_windows(2U);
  auto planner = fixture.planner();
  const auto discovery = planner.plan(identity_for(fixture.disk));
  check(discovery.has_value(), "Multi-boot discovery should succeed");

  const auto selected =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          discovery.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
          });
  check(
      selected.has_value() &&
          selected.value().windows_in_boot_priority().size() == 1U &&
          selected.value()
                  .windows_in_boot_priority()
                  .front()
                  .partition.number == 2U,
      "Selected-only mode must retain exactly one reviewed Windows");

  const auto duplicate =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          discovery.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 2U},
              .system_partition_number = 1U,
          });
  check(
      !duplicate.has_value() && duplicate.error().code ==
          ytec::clonecore::ErrorCode::invalid_argument,
      "Duplicate Windows priority entries must fail closed");

  const auto omitted =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          discovery.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
          });
  check(
      !omitted.has_value() && omitted.error().code ==
          ytec::clonecore::ErrorCode::confirmation_required,
      "All-registration mode must not silently omit a discovered Windows");
}

void test_missing_or_unreviewed_efi_policy_cannot_form_choices() {
  Fixture missing_fixture(make_gpt_disk(0U, 1U));
  missing_fixture.mark_windows(0U);
  auto missing_planner = missing_fixture.planner();
  const auto missing =
      missing_planner.plan(identity_for(missing_fixture.disk));
  check(missing.has_value(), "Missing ESP discovery should succeed");
  const auto without_system =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          missing.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {1U},
              .system_partition_number = 0U,
          });
  check(
      !without_system.has_value() && without_system.error().code ==
          ytec::clonecore::ErrorCode::confirmation_required,
      "A missing ESP must defer to a separately reviewed creation plan");

  Fixture foreign_fixture(make_gpt_disk());
  foreign_fixture.mark_windows(1U);
  foreign_fixture.efi_ownership.evidence = {
      .state = ytec::bootrepair::EfiBootOwnershipState::
          non_microsoft_or_untrusted_present,
      .efi_directory_present = true,
      .microsoft_namespace_present = true,
      .non_microsoft_or_untrusted_entry_count = 1U,
      .top_level_non_microsoft_namespace_count = 1U,
  };
  auto foreign_planner = foreign_fixture.planner();
  const auto foreign =
      foreign_planner.plan(identity_for(foreign_fixture.disk));
  check(foreign.has_value(), "Third-party EFI must remain discoverable");
  const auto no_efi_policy =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          foreign.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
          });
  check(
      !no_efi_policy.has_value() && no_efi_policy.error().code ==
          ytec::clonecore::ErrorCode::confirmation_required,
      "Third-party EFI must await an explicit preserve/delete policy");

  const auto preserve =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          foreign.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
              .third_party_efi_policy = ytec::bootrepair::
                  AutomaticThirdPartyEfiPolicy::preserve,
          });
  check(
      preserve.has_value() &&
          preserve.value().third_party_efi_policy() ==
              ytec::bootrepair::AutomaticThirdPartyEfiPolicy::preserve,
      "An explicit preserve choice must be retained immutably");

  const auto delete_non_microsoft =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          foreign.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
              .third_party_efi_policy = ytec::bootrepair::
                  AutomaticThirdPartyEfiPolicy::delete_non_microsoft,
          });
  check(
      delete_non_microsoft.has_value() &&
          delete_non_microsoft.value().third_party_efi_policy() ==
              ytec::bootrepair::
                  AutomaticThirdPartyEfiPolicy::delete_non_microsoft,
      "An explicit delete choice must be retained without performing I/O");

  auto ambiguous = foreign.value();
  ambiguous.system_partition_candidates.front().efi_ownership.state =
      ytec::bootrepair::EfiBootOwnershipState::ambiguous;
  const auto ambiguous_preserve =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          ambiguous,
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
              .third_party_efi_policy = ytec::bootrepair::
                  AutomaticThirdPartyEfiPolicy::preserve,
          });
  check(
      !ambiguous_preserve.has_value() && ambiguous_preserve.error().code ==
          ytec::clonecore::ErrorCode::unsupported_layout,
      "Ambiguous EFI content must remain fail-closed even for preserve");

  Fixture microsoft_fixture(make_gpt_disk());
  microsoft_fixture.mark_windows(1U);
  auto microsoft_planner = microsoft_fixture.planner();
  const auto microsoft =
      microsoft_planner.plan(identity_for(microsoft_fixture.disk));
  check(microsoft.has_value(), "Microsoft-only EFI discovery should succeed");
  const auto extraneous_policy =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          microsoft.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
              .third_party_efi_policy = ytec::bootrepair::
                  AutomaticThirdPartyEfiPolicy::delete_non_microsoft,
          });
  check(
      !extraneous_policy.has_value() && extraneous_policy.error().code ==
          ytec::clonecore::ErrorCode::invalid_argument,
      "A delete choice must not be accepted when no third-party EFI exists");
}

void test_choice_revalidation_allows_disk_number_churn_only() {
  Fixture fixture(make_gpt_disk());
  fixture.mark_windows(1U);
  auto planner = fixture.planner();
  const auto discovery = planner.plan(identity_for(fixture.disk));
  check(discovery.has_value(), "Discovery should succeed");
  const auto reviewed =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          discovery.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
          });
  check(reviewed.has_value(), "Initial choices should pass");

  auto renumbered = discovery.value();
  renumbered.selected_disk.disk_number = 12U;
  renumbered.selected_disk.device_path = L"\\\\.\\PhysicalDrive12";
  renumbered.selected_identity.disk_number = 12U;
  std::reverse(
      renumbered.selected_disk.partitions.begin(),
      renumbered.selected_disk.partitions.end());
  for (auto& candidate : renumbered.windows_installations) {
    candidate.volume.location.disk_number = 12U;
  }
  for (auto& candidate : renumbered.system_partition_candidates) {
    candidate.volume.location.disk_number = 12U;
  }
  const auto refreshed =
      ytec::bootrepair::revalidate_automatic_boot_repair_choices(
          reviewed.value(), renumbered);
  check(
      refreshed.has_value() &&
          refreshed.value().selected_identity().disk_number == 12U &&
          refreshed.value()
                  .windows_in_boot_priority()
                  .front()
                  .volume.location.disk_number == 12U,
      "Stable reidentification may refresh only transient disk routing");
}

void test_choice_revalidation_rejects_candidate_evidence_drift() {
  Fixture fixture(make_gpt_disk());
  fixture.mark_windows(1U);
  auto planner = fixture.planner();
  const auto discovery = planner.plan(identity_for(fixture.disk));
  check(discovery.has_value(), "Discovery should succeed");
  const auto reviewed =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          discovery.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
          });
  check(reviewed.has_value(), "Initial choices should pass");

  auto version_drift = discovery.value();
  ++version_drift.windows_installations.front().version.build;
  const auto changed_windows =
      ytec::bootrepair::revalidate_automatic_boot_repair_choices(
          reviewed.value(), version_drift);
  check(
      !changed_windows.has_value() && changed_windows.error().code ==
          ytec::clonecore::ErrorCode::identity_mismatch,
      "Windows version drift must invalidate the review");

  auto ownership_drift = discovery.value();
  ++ownership_drift.system_partition_candidates.front()
        .efi_ownership.microsoft_signed_efi_loader_count;
  const auto changed_efi =
      ytec::bootrepair::revalidate_automatic_boot_repair_choices(
          reviewed.value(), ownership_drift);
  check(
      !changed_efi.has_value() && changed_efi.error().code ==
          ytec::clonecore::ErrorCode::identity_mismatch,
      "EFI ownership drift must invalidate the review");

  auto malformed = discovery.value();
  ++malformed.windows_installations.front().partition.offset_bytes;
  const auto malformed_review =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          malformed,
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
          });
  check(
      !malformed_review.has_value() && malformed_review.error().code ==
          ytec::clonecore::ErrorCode::invalid_data,
      "A public discovery object cannot substitute a different partition extent");
}

void test_winre_and_nvram_choices_are_explicit_and_fail_closed() {
  Fixture fixture(make_gpt_disk(1U, 2U));
  fixture.mark_windows(1U);
  fixture.mark_windows(2U);
  fixture.winre.observations[volume_root(2U) + L"Windows"] =
      trusted_winre(2U);
  fixture.winre.observations[volume_root(3U) + L"Windows"] =
      ytec::bootrepair::WinReDiagnosticReport{
          .exit_code = 0U,
          .source_state = ytec::bootrepair::
              WinReSourceState::image_available_in_windows,
          .winre_image_size_bytes = 640ULL * kMiB,
          .microsoft_signature_verified = true,
          .read_only_command = true,
          .fallback_image_present = true,
      };
  auto planner = fixture.planner();
  const auto discovery = planner.plan(identity_for(fixture.disk));
  check(discovery.has_value(), "WinRE choice discovery should succeed");

  const auto reviewed =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          discovery.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .system_partition_number = 1U,
              .nvram_policy = ytec::bootrepair::
                  AutomaticNvramRepairPolicy::
                      repair_current_pc_windows_boot_manager,
          });
  check(reviewed.has_value(), "Explicit UEFI NVRAM choice should review");
  const auto winre = reviewed.value().winre_choices_in_boot_priority();
  check(
      winre.size() == 2U &&
          winre[0].windows_partition_number == 2U &&
          winre[0].disposition == ytec::bootrepair::
              AutomaticWinReRepairDisposition::
                  verify_existing_registration &&
          winre[1].windows_partition_number == 3U &&
          winre[1].disposition == ytec::bootrepair::
              AutomaticWinReRepairDisposition::
                  register_verified_windows_image &&
          reviewed.value().nvram_policy() == ytec::bootrepair::
              AutomaticNvramRepairPolicy::
                  repair_current_pc_windows_boot_manager,
      "WinRE work and current-PC NVRAM intent must retain boot priority");

  auto missing = discovery.value();
  missing.windows_installations.back().winre = {};
  missing.windows_installations.back().winre.source_state =
      ytec::bootrepair::WinReSourceState::missing;
  const auto partial =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          missing,
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .system_partition_number = 1U,
          });
  check(
      partial.has_value() &&
          partial.value().winre_choices_in_boot_priority()[1].disposition ==
              ytec::bootrepair::AutomaticWinReRepairDisposition::
                  normal_boot_only_partial,
      "Missing WinRE must become an explicit partial normal-boot repair");

  auto unknown = discovery.value();
  unknown.windows_installations.back().winre = {};
  const auto unknown_partial =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          unknown,
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .system_partition_number = 1U,
          });
  check(
      unknown_partial.has_value() &&
          unknown_partial.value()
                  .winre_choices_in_boot_priority()[1]
                  .disposition == ytec::bootrepair::
              AutomaticWinReRepairDisposition::normal_boot_only_partial,
      "An unavailable WinRE diagnostic must retain a partial repair result");

  auto inconsistent = discovery.value();
  inconsistent.windows_installations.back().winre.image_size_bytes = 0U;
  const auto invalid =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          inconsistent,
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .system_partition_number = 1U,
          });
  check(
      !invalid.has_value() && invalid.error().code ==
          ytec::clonecore::ErrorCode::invalid_data,
      "An inconsistent WinRE image report must not become a repair plan");

  auto unselected_inconsistent = discovery.value();
  unselected_inconsistent.windows_installations.back()
      .winre.image_size_bytes = 0U;
  const auto invalid_unselected =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          unselected_inconsistent,
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .system_partition_number = 1U,
          });
  check(
      !invalid_unselected.has_value() && invalid_unselected.error().code ==
          ytec::clonecore::ErrorCode::invalid_data,
      "Unselected candidate evidence must remain part of plan integrity");

  Fixture bios_fixture(make_mbr_disk());
  bios_fixture.mark_windows(0U);
  auto bios_planner = bios_fixture.planner();
  const auto bios = bios_planner.plan(identity_for(bios_fixture.disk));
  check(bios.has_value(), "BIOS fixture discovery should succeed");
  const auto invalid_nvram =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          bios.value(),
          ytec::bootrepair::AutomaticBootRepairChoiceRequest{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {1U},
              .system_partition_number = 1U,
              .nvram_policy = ytec::bootrepair::
                  AutomaticNvramRepairPolicy::
                      repair_current_pc_windows_boot_manager,
          });
  check(
      !invalid_nvram.has_value() && invalid_nvram.error().code ==
          ytec::clonecore::ErrorCode::invalid_argument,
      "BIOS repair must reject a UEFI NVRAM update request");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests{
      {"gpt_single_windows_plan_is_read_only_and_fresh",
       test_gpt_single_windows_plan_is_read_only_and_fresh},
      {"multiple_windows_require_explicit_policy",
       test_multiple_windows_require_explicit_policy},
      {"missing_esp_reports_create_plan_needed_without_writes",
       test_missing_esp_reports_create_plan_needed_without_writes},
      {"mbr_active_ntfs_selects_bios", test_mbr_active_ntfs_selects_bios},
      {"multiple_system_partitions_are_not_auto_selected",
       test_multiple_system_partitions_are_not_auto_selected},
      {"unsupported_windows_is_returned_with_policy_flag",
       test_unsupported_windows_is_returned_with_policy_flag},
      {"disk_info_input_is_reidentified_instead_of_trusting_disk_number",
       test_disk_info_input_is_reidentified_instead_of_trusting_disk_number},
      {"identity_mismatch_stops_before_volume_observation",
       test_identity_mismatch_stops_before_volume_observation},
      {"untrusted_winre_result_is_rejected",
       test_untrusted_winre_result_is_rejected},
      {"efi_ownership_probe_failure_stops_gpt_plan",
       test_efi_ownership_probe_failure_stops_gpt_plan},
      {"multiboot_choices_preserve_explicit_priority",
       test_multiboot_choices_preserve_explicit_priority},
      {"selected_only_and_invalid_multiboot_choices_fail_closed",
       test_selected_only_and_invalid_multiboot_choices_fail_closed},
      {"missing_or_unreviewed_efi_policy_cannot_form_choices",
       test_missing_or_unreviewed_efi_policy_cannot_form_choices},
      {"choice_revalidation_allows_disk_number_churn_only",
       test_choice_revalidation_allows_disk_number_churn_only},
      {"choice_revalidation_rejects_candidate_evidence_drift",
       test_choice_revalidation_rejects_candidate_evidence_drift},
      {"winre_and_nvram_choices_are_explicit_and_fail_closed",
       test_winre_and_nvram_choices_are_explicit_and_fail_closed},
  };

  std::size_t failed = 0U;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  if (failed != 0U) {
    std::cerr << failed << " test(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " test(s) passed\n";
  return 0;
}

#include "ytec/winpeapp/automatic_boot_repair_ui.h"

#include <cstdlib>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::bootrepair::AutomaticBootRepairPlan mbr_plan() {
  using ytec::bootrepair::AutomaticBootRepairPlan;
  using ytec::bootrepair::BootRepairVolumeLocation;
  using ytec::bootrepair::BootSystemPartitionRole;
  using ytec::bootrepair::BootVolumeObservation;
  using ytec::bootrepair::DiscoveredSystemPartition;
  using ytec::bootrepair::DiscoveredWindowsInstallation;
  using ytec::bootrepair::OfflineWindowsVersion;
  using ytec::diskmodel::DiskInfo;
  using ytec::diskmodel::PartitionInfo;
  using ytec::diskmodel::PartitionStyle;

  const PartitionInfo system{
      .number = 1U,
      .offset_bytes = 1ULL * kMiB,
      .size_bytes = 100ULL * kMiB,
      .style = PartitionStyle::mbr,
      .type = L"0x07",
      .identifier = L"0x12345678:1",
      .name = L"System",
      .bootable = true,
  };
  const PartitionInfo windows{
      .number = 2U,
      .offset_bytes = 101ULL * kMiB,
      .size_bytes = 1024ULL * kMiB,
      .style = PartitionStyle::mbr,
      .type = L"0x07",
      .identifier = L"0x12345678:2",
      .name = L"Windows",
  };
  DiskInfo disk{
      .disk_number = 8U,
      .device_path = L"\\\\.\\PhysicalDrive8",
      .device_instance_id = L"MOCK\\AUTO-BOOT\\MBR",
      .model = L"Tsumugi Boot Disk",
      .size_bytes = 2ULL * 1024ULL * kMiB,
      .sector_count = 4ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 512U,
      .bus_type = L"SATA",
      .serial_suffix = "BOOT0008",
      .partition_style = PartitionStyle::mbr,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
      .partitions = {system, windows},
  };
  auto identity = ytec::diskmodel::make_stable_disk_identity(disk, false);
  check(identity.has_value(), "fixture identity must be valid");

  const BootVolumeObservation windows_volume{
      .volume_name = L"\\\\?\\Volume{00000000-0000-0000-0000-000000000008}\\",
      .location = BootRepairVolumeLocation{
          .disk_number = disk.disk_number,
          .starting_offset = windows.offset_bytes,
          .extent_length = windows.size_bytes,
          .file_system = L"NTFS",
      },
      .mount_points = {L"D:\\"},
  };
  const BootVolumeObservation system_volume{
      .volume_name = L"\\\\?\\Volume{00000000-0000-0000-0000-000000000001}\\",
      .location = BootRepairVolumeLocation{
          .disk_number = disk.disk_number,
          .starting_offset = system.offset_bytes,
          .extent_length = system.size_bytes,
          .file_system = L"NTFS",
      },
  };
  AutomaticBootRepairPlan plan{
      .selected_disk = disk,
      .selected_identity = identity.value(),
      .partition_style = PartitionStyle::mbr,
      .firmware = ytec::bootrepair::BcdBootFirmware::bios,
      .required_system_partition_role = BootSystemPartitionRole::bios_active,
      .planned_bcd_store_policy =
          ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh,
  };
  plan.windows_installations.push_back(DiscoveredWindowsInstallation{
      .partition = windows,
      .volume = windows_volume,
      .windows_directory = windows_volume.volume_name + L"Windows",
      .version = OfflineWindowsVersion{
          .major = 10U,
          .build = 26100U,
          .installation_type = L"Client",
      },
      .officially_supported = true,
  });
  plan.system_partition_candidates.push_back(DiscoveredSystemPartition{
      .partition = system,
      .volume = system_volume,
      .role = BootSystemPartitionRole::bios_active,
  });
  return plan;
}

ytec::bootrepair::AutomaticBootRepairPlan gpt_plan() {
  auto plan = mbr_plan();
  auto& system = plan.selected_disk.partitions.at(0U);
  auto& windows = plan.selected_disk.partitions.at(1U);
  plan.selected_disk.partition_style =
      ytec::diskmodel::PartitionStyle::gpt;
  plan.selected_disk.device_instance_id = L"MOCK\\AUTO-BOOT\\GPT";
  system.style = ytec::diskmodel::PartitionStyle::gpt;
  system.type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
  system.identifier = L"{ESP-0001}";
  system.name = L"EFI system";
  system.bootable = false;
  windows.style = ytec::diskmodel::PartitionStyle::gpt;
  windows.type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
  windows.identifier = L"{WINDOWS-0002}";
  auto identity = ytec::diskmodel::make_stable_disk_identity(
      plan.selected_disk, false);
  check(identity.has_value(), "GPT fixture identity must be valid");
  plan.selected_identity = identity.value();
  plan.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  plan.firmware = ytec::bootrepair::BcdBootFirmware::uefi;
  plan.required_system_partition_role =
      ytec::bootrepair::BootSystemPartitionRole::efi_system;

  auto& discovered_windows = plan.windows_installations.front();
  discovered_windows.partition = windows;
  discovered_windows.winre = {
      .source_state =
          ytec::bootrepair::WinReSourceState::registered_partition,
      .registered_location_reported = true,
      .registered_location_matches_selected_disk = true,
      .registered_partition_number = 3U,
      .registered_path_kind_reported = true,
      .registered_image_present = true,
      .image_size_bytes = 640ULL * kMiB,
  };
  auto& discovered_system = plan.system_partition_candidates.front();
  discovered_system.partition = system;
  discovered_system.volume.location.file_system = L"FAT32";
  discovered_system.role =
      ytec::bootrepair::BootSystemPartitionRole::efi_system;
  discovered_system.efi_ownership = {
      .state = ytec::bootrepair::EfiBootOwnershipState::
          microsoft_only_or_empty,
      .efi_directory_present = true,
      .microsoft_namespace_present = true,
      .boot_namespace_present = true,
      .fallback_loader_present = true,
      .fallback_loader_microsoft_signed = true,
      .microsoft_signed_efi_loader_count = 5U,
  };
  return plan;
}

ytec::bootrepair::AutomaticBootRepairPlan multi_gpt_plan(
    const bool partial_second,
    const bool third_party_efi) {
  auto plan = gpt_plan();
  auto second_partition = plan.windows_installations.front().partition;
  second_partition.number = 3U;
  second_partition.offset_bytes = 1200ULL * kMiB;
  second_partition.size_bytes = 512ULL * kMiB;
  second_partition.identifier = L"{WINDOWS-0003}";
  second_partition.name = L"Windows 2";
  plan.selected_disk.partitions.push_back(second_partition);

  auto second = plan.windows_installations.front();
  second.partition = second_partition;
  second.volume.volume_name =
      L"\\\\?\\Volume{00000000-0000-0000-0000-000000000003}\\";
  second.volume.location.starting_offset = second_partition.offset_bytes;
  second.volume.location.extent_length = second_partition.size_bytes;
  second.volume.mount_points = {L"E:\\"};
  second.windows_directory = second.volume.volume_name + L"Windows";
  second.winre = {};
  second.winre.source_state = partial_second
      ? ytec::bootrepair::WinReSourceState::missing
      : ytec::bootrepair::WinReSourceState::registered_partition;
  if (!partial_second) {
    second.winre.registered_location_reported = true;
    second.winre.registered_location_matches_selected_disk = true;
    second.winre.registered_partition_number = 4U;
    second.winre.registered_path_kind_reported = true;
    second.winre.registered_image_present = true;
    second.winre.image_size_bytes = 640ULL * kMiB;
  }
  plan.windows_installations.push_back(std::move(second));
  plan.windows_selection_policy_needed = true;
  auto identity = ytec::diskmodel::make_stable_disk_identity(
      plan.selected_disk, false);
  check(identity.has_value(), "multi GPT fixture identity must be valid");
  plan.selected_identity = identity.take_value();

  if (third_party_efi) {
    plan.system_partition_candidates.front().efi_ownership = {
        .state = ytec::bootrepair::EfiBootOwnershipState::
            non_microsoft_or_untrusted_present,
        .efi_directory_present = true,
        .microsoft_namespace_present = true,
        .boot_namespace_present = true,
        .fallback_loader_present = true,
        .fallback_loader_microsoft_signed = true,
        .microsoft_signed_efi_loader_count = 5U,
        .non_microsoft_or_untrusted_entry_count = 1U,
        .top_level_non_microsoft_namespace_count = 1U,
    };
  }
  return plan;
}

ytec::bootrepair::BootRepairTargetSelection inspected_selection(
    const ytec::bootrepair::AutomaticBootRepairPlan& plan) {
  return ytec::bootrepair::BootRepairTargetSelection{
      .disk = plan.selected_disk,
      .identity = plan.selected_identity,
      .windows_partition =
          plan.windows_installations.front().partition,
      .system_partition =
          plan.system_partition_candidates.front().partition,
  };
}

ytec::bootrepair::WinReRegistrationImageIdentity winre_identity(
    std::wstring requested_path,
    const std::uint64_t length,
    const std::byte marker = std::byte{0x41}) {
  ytec::bootrepair::WinReRegistrationImageIdentity identity{
      .requested_path = std::move(requested_path),
      .opened_final_path =
          L"\\\\?\\Volume{00000000-0000-0000-0000-000000000008}\\"
          L"Windows\\System32\\Recovery\\Winre.wim",
      .volume_serial_number = 0x12345678U,
      .length = length,
      .last_write_time = 100U,
      .change_time = 101U,
  };
  identity.file_id.front() = marker;
  identity.sha256.front() = marker;
  return identity;
}

bool inside(
    const ytec::winpeapp::UiRectangle& value,
    const int width,
    const int height) {
  return value.left >= 260 && value.top >= 94 && value.right <= width - 20 &&
      value.bottom <= height - 20 && value.width() > 0 && value.height() > 0;
}

void layouts_fit() {
  for (const auto [width, height] :
       {std::pair{960, 516},
        std::pair{1024, 600},
        std::pair{1280, 720}}) {
    const auto layout =
        ytec::winpeapp::build_winpe_automatic_boot_repair_layout(
            width, height);
    for (const auto& rectangle :
         {layout.target_disk,
          layout.inspect,
          layout.confirmation_token,
          layout.execute,
          layout.cancel_review,
          layout.output}) {
      check(inside(rectangle, width, height),
            "every automatic-repair control must fit");
    }
    check(layout.inspect.width() >= 160 && layout.execute.width() >= 160 &&
              layout.cancel_review.width() >= 160,
          "Japanese action labels must not be truncated");
    check(layout.target_disk.right + 10 <= layout.inspect.left &&
              layout.confirmation_token.right + 10 <=
                  layout.cancel_review.left &&
              layout.cancel_review.right + 10 <= layout.execute.left &&
              layout.cancel_review.bottom <= layout.output.top,
          "automatic-repair control groups must not overlap");
    check(
        layout.output.height() >= (height < 600 ? 70 : 100),
        "review output must retain useful scrollable height");
  }
}

void unambiguous_mbr_plan_builds_existing_transaction_request() {
  const auto plan = mbr_plan();
  const auto review =
      ytec::winpeapp::build_executable_automatic_boot_repair_review(
          plan);
  check(review.has_value(), "unambiguous MBR plan should be executable");
  check(review.value().request.disk_number == 8U &&
            review.value().request.windows_root == L"D:\\" &&
            review.value().request.system_root.empty() &&
            review.value().request.auto_mount_system_partition &&
            review.value().request.store_policy ==
                ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh,
        "review must feed the existing fresh-BCD transaction exactly");

  const auto inspected = inspected_selection(plan);
  check(ytec::winpeapp::validate_automatic_boot_repair_inspection(
            plan, review.value(), inspected).has_value(),
        "the complete matching standalone inspection must be retained");
}

void unambiguous_gpt_plan_builds_existing_esp_transaction_request() {
  const auto plan = gpt_plan();
  const auto review =
      ytec::winpeapp::build_executable_automatic_boot_repair_review(plan);
  check(review.has_value(),
        "unambiguous Microsoft-only GPT plan should be executable");
  check(
      review.value().request.firmware ==
              ytec::bootrepair::BcdBootFirmware::uefi &&
          review.value().request.store_policy ==
              ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh &&
          review.value().request.require_efi_ownership_recheck &&
          review.value().request.system_volume_identity_root ==
              plan.system_partition_candidates.front().volume.volume_name &&
          !review.value().request.update_current_pc_nvram &&
          ytec::bootrepair::efi_boot_ownership_allows_microsoft_rebuild(
              review.value().request.expected_efi_ownership),
      "GPT review must bind the exact ESP, /c policy, EFI recheck, and no NVRAM change");

  const auto inspected = inspected_selection(plan);
  check(ytec::winpeapp::validate_automatic_boot_repair_inspection(
            plan, review.value(), inspected).has_value(),
        "the complete GPT standalone inspection must match the review");
}

void initial_standalone_inspection_requires_every_reviewed_field() {
  const auto plan = mbr_plan();
  const auto review =
      ytec::winpeapp::build_executable_automatic_boot_repair_review(plan);
  check(review.has_value(), "inspection fixture review must be valid");

  auto inspected = inspected_selection(plan);
  inspected.windows_partition.type = L"0x27";
  check(!ytec::winpeapp::validate_automatic_boot_repair_inspection(
             plan, review.value(), inspected),
        "a changed Windows partition type must invalidate the review");

  inspected = inspected_selection(plan);
  inspected.disk.partitions.front().bootable = false;
  check(!ytec::winpeapp::validate_automatic_boot_repair_inspection(
             plan, review.value(), inspected),
        "a changed full-disk bootable flag must invalidate the review");

  inspected = inspected_selection(plan);
  inspected.identity.device_instance_id += L"-CHANGED";
  check(!ytec::winpeapp::validate_automatic_boot_repair_inspection(
             plan, review.value(), inspected),
        "a changed stable identity must invalidate the review");
}

void exact_ok_and_review_cancellation_are_explicit() {
  auto view = ytec::winpeapp::build_winpe_automatic_boot_repair_ui_view({
      .inventory_ready = true,
      .idle = true,
      .target_selected = true,
      .reviewed = true,
      .confirmation_text = L"ok",
  });
  check(view.confirmation_visible && view.cancel_review_visible &&
            view.cancel_review_enabled && !view.execute_enabled,
        "a reviewed plan may be cancelled but lowercase ok must not execute");

  view = ytec::winpeapp::build_winpe_automatic_boot_repair_ui_view({
      .inventory_ready = true,
      .idle = true,
      .target_selected = true,
      .reviewed = true,
      .confirmation_text = L"OK ",
  });
  check(!view.execute_enabled,
        "uppercase OK with trailing input must not execute");

  view = ytec::winpeapp::build_winpe_automatic_boot_repair_ui_view({
      .inventory_ready = true,
      .idle = true,
      .target_selected = true,
      .reviewed = true,
      .confirmation_text = L"OK",
  });
  check(view.execute_enabled, "exact uppercase OK should enable execution");

  view = ytec::winpeapp::build_winpe_automatic_boot_repair_ui_view({
      .inventory_ready = true,
      .idle = true,
      .target_selected = true,
      .reviewed = false,
  });
  check(view.target_enabled && view.inspect_enabled &&
            !view.confirmation_visible && !view.execute_visible &&
            !view.cancel_review_visible,
        "discarding a review must return to target-only analysis state");

  view = ytec::winpeapp::build_winpe_automatic_boot_repair_ui_view({
      .inventory_ready = true,
      .idle = false,
      .target_selected = true,
      .reviewed = true,
      .execution_active = true,
      .confirmation_text = L"OK",
  });
  check(!view.cancel_review_visible && !view.execute_visible,
        "the non-interruptible BCD transaction must hide review actions");
}

void ambiguous_and_separate_plans_fail_closed() {
  auto plan = mbr_plan();
  plan.windows_installations.push_back(plan.windows_installations.front());
  plan.windows_selection_policy_needed = true;
  auto result =
      ytec::winpeapp::build_executable_automatic_boot_repair_review(plan);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "multiple Windows installations must require explicit policy");

  plan = mbr_plan();
  plan.system_partition_create_plan_needed = true;
  plan.system_partition_candidates.clear();
  result = ytec::winpeapp::build_executable_automatic_boot_repair_review(plan);
  check(!result.has_value(),
        "missing system partition must not silently create one");

  plan = mbr_plan();
  plan.system_partition_candidates.push_back(
      plan.system_partition_candidates.front());
  plan.system_partition_selection_policy_needed = true;
  result = ytec::winpeapp::build_executable_automatic_boot_repair_review(plan);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "multiple system partitions must require explicit selection");

  plan = gpt_plan();
  plan.system_partition_candidates.front().efi_ownership = {
      .state = ytec::bootrepair::EfiBootOwnershipState::
          non_microsoft_or_untrusted_present,
      .efi_directory_present = true,
      .non_microsoft_or_untrusted_entry_count = 1U,
      .top_level_non_microsoft_namespace_count = 1U,
  };
  result = ytec::winpeapp::build_executable_automatic_boot_repair_review(plan);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "Third-party EFI must be preserved by refusing automatic repair");

  plan = gpt_plan();
  plan.system_partition_candidates.front().efi_ownership = {
      .state = ytec::bootrepair::EfiBootOwnershipState::ambiguous,
      .efi_directory_present = true,
  };
  result = ytec::winpeapp::build_executable_automatic_boot_repair_review(plan);
  check(!result.has_value(),
        "Ambiguous EFI ownership must refuse automatic repair");

  plan = gpt_plan();
  plan.windows_installations.front().winre.registered_image_present = false;
  result = ytec::winpeapp::build_executable_automatic_boot_repair_review(plan);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "the single-target shortcut must defer WinRE repair to the product choice route");
}

void execution_recheck_requires_the_complete_reviewed_plan() {
  const auto reviewed = mbr_plan();
  auto observed = reviewed;
  observed.windows_installations.front().volume.mount_points.front() =
      L"E:\\";
  check(!ytec::winpeapp::equivalent_automatic_boot_repair_plan(
            reviewed, observed),
        "a changed volume mapping must invalidate the review");

  observed = reviewed;
  ++observed.selected_disk.partitions.front().size_bytes;
  check(!ytec::winpeapp::equivalent_automatic_boot_repair_plan(
            reviewed, observed),
        "any reviewed partition-layout change must invalidate execution");

  auto gpt_reviewed = gpt_plan();
  auto gpt_observed = gpt_reviewed;
  ++gpt_observed.system_partition_candidates.front()
        .efi_ownership.microsoft_signed_efi_loader_count;
  check(!ytec::winpeapp::equivalent_automatic_boot_repair_plan(
            gpt_reviewed, gpt_observed),
        "a changed EFI ownership observation must invalidate execution");

  observed = reviewed;
  check(ytec::winpeapp::equivalent_automatic_boot_repair_plan(
            reviewed, observed),
        "an identical replan should remain executable");
}

void reviewed_multi_windows_partial_maps_to_one_safe_batch() {
  const auto plan = multi_gpt_plan(true, false);
  const auto request = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          plan,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .explicitly_approved = true,
          });
  check(request.has_value(), "product choices should support two Windows");
  check(
      request.value().windows_policy == ytec::bootrepair::
              AutomaticWindowsRegistrationPolicy::
                  all_with_explicit_priority &&
          request.value().windows_partition_priority ==
              std::vector<std::uint32_t>({2U, 3U}) &&
          request.value().system_partition_number == 1U &&
          request.value().nvram_policy == ytec::bootrepair::
              AutomaticNvramRepairPolicy::leave_unchanged,
      "product choices must expose every Windows in displayed priority order");

  const auto choices =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          plan, request.value());
  check(choices.has_value(), "pure core review should bind product choices");
  const auto execution = ytec::winpeapp::
      build_executable_reviewed_automatic_boot_repair(choices.value());
  check(execution.has_value(), "reviewed choices should map to execution");
  check(
      execution.value().requests_in_boot_priority.size() == 2U &&
          execution.value().requests_in_boot_priority[0].windows_root ==
              L"D:\\" &&
          execution.value().requests_in_boot_priority[1].windows_root ==
              L"E:\\" &&
          execution.value().requests_in_boot_priority[0].store_policy ==
              ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh &&
          execution.value().requests_in_boot_priority[1].store_policy ==
              ytec::bootrepair::BcdBootStorePolicy::preserve_existing &&
          execution.value().requests_in_boot_priority[0]
              .reviewed_multi_windows_batch &&
          execution.value().requests_in_boot_priority[1]
              .reviewed_multi_windows_batch &&
          execution.value().normal_boot_only_partial &&
          !execution.value().third_party_efi_preserved,
      "the batch must rebuild once, append in order, and retain partial WinRE");

  std::vector<ytec::bootrepair::BootRepairTargetSelection> inspected;
  for (const auto& windows : choices.value().windows_in_boot_priority()) {
    inspected.push_back(ytec::bootrepair::BootRepairTargetSelection{
        .disk = plan.selected_disk,
        .identity = plan.selected_identity,
        .windows_partition = windows.partition,
        .system_partition = choices.value().system_partition().partition,
    });
  }
  check(ytec::winpeapp::validate_reviewed_automatic_boot_repair_inspections(
            plan, choices.value(), execution.value(), inspected)
            .has_value(),
        "every ordered standalone inspection should bind to the review");
  std::swap(
      inspected[0].windows_partition,
      inspected[1].windows_partition);
  check(!ytec::winpeapp::validate_reviewed_automatic_boot_repair_inspections(
             plan, choices.value(), execution.value(), inspected),
        "a changed Windows priority must invalidate the batch");

  inspected.clear();
  for (const auto& windows : choices.value().windows_in_boot_priority()) {
    inspected.push_back(ytec::bootrepair::BootRepairTargetSelection{
        .disk = plan.selected_disk,
        .identity = plan.selected_identity,
        .windows_partition = windows.partition,
        .system_partition = choices.value().system_partition().partition,
    });
  }
  auto changed_plan = plan;
  changed_plan.windows_installations.back().winre.source_state =
      ytec::bootrepair::WinReSourceState::unknown;
  check(!ytec::winpeapp::validate_reviewed_automatic_boot_repair_inspections(
             changed_plan,
             choices.value(),
             execution.value(),
             inspected),
        "an unreflected full-plan WinRE drift must invalidate inspections");
}

void current_pc_nvram_requires_separate_explicit_uefi_choice() {
  const auto plan = multi_gpt_plan(false, false);
  auto choice = ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
      .windows_policy = ytec::bootrepair::
          AutomaticWindowsRegistrationPolicy::selected_only,
      .windows_partition_priority = {2U},
      .nvram_policy = ytec::bootrepair::AutomaticNvramRepairPolicy::
          repair_current_pc_windows_boot_manager,
      .explicitly_approved = true,
  };
  auto missing_ack = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(plan, choice);
  check(!missing_ack.has_value(),
        "current-PC NVRAM must require its own explicit choice");

  choice.current_pc_nvram_explicitly_approved = true;
  auto request = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(plan, choice);
  check(request.has_value() && request.value().nvram_policy ==
            ytec::bootrepair::AutomaticNvramRepairPolicy::
                repair_current_pc_windows_boot_manager,
        "explicit UEFI this-PC choice must be bound into the immutable request");
  if (!request) {
    return;
  }
  auto reviewed = ytec::bootrepair::review_automatic_boot_repair_choices(
      plan, request.value());
  check(reviewed.has_value(), "NVRAM request should pass pure review");
  if (!reviewed) {
    return;
  }
  auto execution = ytec::winpeapp::
      build_executable_reviewed_automatic_boot_repair(reviewed.value());
  check(execution.has_value() && execution.value().repair_current_pc_nvram,
        "the executable review must retain the exact NVRAM disposition");
  if (!execution) {
    return;
  }

  auto changed = execution.value();
  changed.repair_current_pc_nvram = false;
  const auto mismatch = ytec::winpeapp::
      validate_reviewed_automatic_boot_repair_inspections(
          plan,
          reviewed.value(),
          changed,
          std::vector<ytec::bootrepair::BootRepairTargetSelection>{
              inspected_selection(plan)});
  check(!mismatch.has_value(),
        "execution evidence must not silently change the reviewed NVRAM choice");
}

void windows_priority_requires_explicit_user_choice() {
  const auto plan = multi_gpt_plan(false, false);
  auto result = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          plan,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
          });
  check(!result.has_value() && result.error().code ==
            ytec::clonecore::ErrorCode::confirmation_required,
        "display order must never become priority without explicit approval");

  result = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          plan,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {3U},
              .explicitly_approved = true,
          });
  check(result.has_value() &&
            result.value().windows_policy == ytec::bootrepair::
                AutomaticWindowsRegistrationPolicy::selected_only &&
            result.value().windows_partition_priority ==
                std::vector<std::uint32_t>({3U}),
        "the user must be able to register one selected Windows only");

  result = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          plan,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {3U, 2U},
              .explicitly_approved = true,
          });
  check(result.has_value() &&
            result.value().windows_partition_priority ==
                std::vector<std::uint32_t>({3U, 2U}),
        "an explicitly reviewed alternative priority must remain ordered");

  auto unsupported = plan;
  unsupported.windows_installations.back().officially_supported = false;
  unsupported.windows_installations.back().version.major = 6U;
  unsupported.windows_installations.back().version.build = 7601U;
  unsupported.unsupported_windows_policy_needed = true;
  result = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          unsupported,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .explicitly_approved = true,
          });
  check(result.has_value(),
        "selected-only must remain available when another candidate is unsupported");
  const auto selected_review =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          unsupported, result.value());
  check(selected_review.has_value() &&
            selected_review.value().windows_in_boot_priority().size() == 1U,
        "a supported selected-only Windows should not register an unselected unsupported candidate");
}

void third_party_efi_defaults_to_preserve_and_delete_is_explicit() {
  const auto plan = multi_gpt_plan(false, true);
  check(ytec::winpeapp::
            automatic_boot_repair_allows_third_party_efi_delete(plan),
        "only independent top-level third-party EFI should expose delete");
  const auto request = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          plan,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .explicitly_approved = true,
          });
  check(request.has_value() &&
            request.value().third_party_efi_policy == ytec::bootrepair::
                AutomaticThirdPartyEfiPolicy::preserve,
        "the product must make its non-destructive preserve choice explicit");
  const auto choices =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          plan, request.value());
  check(choices.has_value(), "third-party preserve should pass pure review");
  const auto execution = ytec::winpeapp::
      build_executable_reviewed_automatic_boot_repair(choices.value());
  check(
      execution.has_value() && execution.value().third_party_efi_preserved &&
          execution.value().requests_in_boot_priority.front()
                  .third_party_efi_policy == ytec::bootrepair::
              BootRepairThirdPartyEfiPolicy::preserve &&
          ytec::bootrepair::validate_boot_repair_efi_ownership(
              execution.value().requests_in_boot_priority.front(),
              plan.system_partition_candidates.front().efi_ownership)
              .has_value(),
      "exact evidence plus reviewed preserve should allow only the /s batch");

  const auto missing_delete_ack = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          plan,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .third_party_efi_policy = ytec::bootrepair::
                  AutomaticThirdPartyEfiPolicy::delete_non_microsoft,
              .explicitly_approved = true,
          });
  check(!missing_delete_ack.has_value() &&
            missing_delete_ack.error().code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "dangerous delete must require a dedicated explicit approval");

  const auto delete_request = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          plan,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .third_party_efi_policy = ytec::bootrepair::
                  AutomaticThirdPartyEfiPolicy::delete_non_microsoft,
              .third_party_efi_delete_explicitly_approved = true,
              .explicitly_approved = true,
          });
  check(delete_request.has_value() &&
            delete_request.value().third_party_efi_policy ==
                ytec::bootrepair::AutomaticThirdPartyEfiPolicy::
                    delete_non_microsoft,
        "dedicated approval must bind delete into the immutable request");
  const auto delete_choices =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          plan, delete_request.value());
  check(delete_choices.has_value(), "pure review should retain delete intent");
  const auto deleted = ytec::winpeapp::
      build_executable_reviewed_automatic_boot_repair(
          delete_choices.value());
  check(deleted.has_value() &&
            deleted.value().third_party_efi_delete_requested &&
            !deleted.value().third_party_efi_preserved &&
            deleted.value().requests_in_boot_priority.front()
                    .third_party_efi_policy == ytec::bootrepair::
                BootRepairThirdPartyEfiPolicy::preserve,
        "delete must route separately while standalone inspection remains preserve-only");
  const auto esp_request = ytec::winpeapp::
      build_windows_efi_delete_esp_request(delete_choices.value());
  check(esp_request.has_value() &&
            esp_request.value().expected_partition_number == 1U &&
            esp_request.value().expected_disk.disk_number ==
                plan.selected_identity.disk_number,
        "delete review must bind stable disk and exact GPT ESP routing");

  auto unsafe = plan;
  auto& unsafe_ownership =
      unsafe.system_partition_candidates.front().efi_ownership;
  unsafe_ownership.top_level_non_microsoft_namespace_count = 0U;
  unsafe_ownership.boot_namespace_nonstandard_entry_count = 1U;
  check(!ytec::winpeapp::
             automatic_boot_repair_allows_third_party_efi_delete(unsafe),
        "EFI Boot or ambiguous content must hide the delete choice");
  const auto unsafe_request = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          unsafe,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::
                      all_with_explicit_priority,
              .windows_partition_priority = {2U, 3U},
              .explicitly_approved = true,
          });
  check(!unsafe_request.has_value() && unsafe_request.error().code ==
            ytec::clonecore::ErrorCode::unsupported_layout,
        "EFI\\Boot content must not be mislabeled as an independently preservable namespace");
}

void verified_winre_fallback_is_bound_to_the_registration_transaction() {
  auto plan = gpt_plan();
  auto& winre = plan.windows_installations.front().winre;
  winre = {};
  winre.source_state =
      ytec::bootrepair::WinReSourceState::image_available_in_windows;
  winre.fallback_image_present = true;
  winre.image_size_bytes = 640ULL * kMiB;
  const auto request = ytec::winpeapp::
      build_product_automatic_boot_repair_choice_request(
          plan,
          ytec::winpeapp::WinPeAutomaticBootRepairProductChoice{
              .windows_policy = ytec::bootrepair::
                  AutomaticWindowsRegistrationPolicy::selected_only,
              .windows_partition_priority = {2U},
              .explicitly_approved = true,
          });
  check(request.has_value(), "WinRE registration candidate should be reviewable");
  const auto choices =
      ytec::bootrepair::review_automatic_boot_repair_choices(
          plan, request.value());
  check(choices.has_value(), "pure review should retain WinRE registration intent");
  const auto execution = ytec::winpeapp::
      build_executable_reviewed_automatic_boot_repair(choices.value());
  check(execution.has_value() &&
            !execution.value().normal_boot_only_partial &&
            execution.value().winre_actions_in_boot_priority.size() == 1U &&
            execution.value().winre_actions_in_boot_priority.front()
                    .disposition == ytec::bootrepair::
                AutomaticWinReRepairDisposition::
                    register_verified_windows_image &&
            execution.value().winre_actions_in_boot_priority.front()
                    .candidate_directory ==
                L"D:\\Windows\\System32\\Recovery" &&
            execution.value().winre_actions_in_boot_priority.front()
                    .expected_target_partition_number == 2U &&
            execution.value().winre_actions_in_boot_priority.front()
                    .expected_registered_path_kind == ytec::bootrepair::
                WinReRegisteredPathKind::windows_system32_recovery,
        "the fallback image must remain an explicit per-Windows registration action");

  const auto identity = winre_identity(
      L"D:\\Windows\\System32\\Recovery\\Winre.wim",
      640ULL * kMiB);
  const auto bound = ytec::winpeapp::
      bind_reviewed_automatic_boot_repair_winre_images(
          execution.value(),
          std::vector<ytec::winpeapp::
              WinPeAutomaticBootRepairWinReImageBinding>{
              {.windows_partition_number = 2U, .identity = identity}});
  check(bound.has_value() &&
            bound.value().winre_actions_in_boot_priority.front()
                .reviewed_candidate.has_value(),
        "opened-handle File ID and SHA-256 evidence must bind before confirmation");
  check(!ytec::winpeapp::
             bind_reviewed_automatic_boot_repair_winre_images(
                 bound.value(),
                 std::vector<ytec::winpeapp::
                     WinPeAutomaticBootRepairWinReImageBinding>{
                     {.windows_partition_number = 2U,
                      .identity = identity}}),
        "a bound Winre.wim identity must not be replaceable");
  check(ytec::winpeapp::validate_reviewed_automatic_boot_repair_inspections(
            plan,
            choices.value(),
            bound.value(),
            std::vector<ytec::bootrepair::BootRepairTargetSelection>{
                inspected_selection(plan)})
            .has_value(),
        "a fully bound fallback action should pass the final read-only review");

  auto wrong_identity = identity;
  ++wrong_identity.length;
  check(!ytec::winpeapp::
             bind_reviewed_automatic_boot_repair_winre_images(
                 execution.value(),
                 std::vector<ytec::winpeapp::
                     WinPeAutomaticBootRepairWinReImageBinding>{
                     {.windows_partition_number = 2U,
                      .identity = wrong_identity}}),
        "a reviewed Winre.wim length mismatch must fail closed");

  auto wrong_path = identity;
  wrong_path.requested_path =
      L"E:\\Windows\\System32\\Recovery\\Winre.wim";
  check(!ytec::winpeapp::
             bind_reviewed_automatic_boot_repair_winre_images(
                 execution.value(),
                 std::vector<ytec::winpeapp::
                     WinPeAutomaticBootRepairWinReImageBinding>{
                     {.windows_partition_number = 2U,
                      .identity = wrong_path}}),
        "a Winre.wim from another Windows root must fail closed");

  auto oversized_execution = execution.value();
  auto oversized_identity = identity;
  oversized_identity.length = 8ULL * 1024ULL * kMiB + 1ULL;
  oversized_execution.winre_actions_in_boot_priority.front()
      .prior_diagnostic.winre_image_size_bytes = oversized_identity.length;
  check(!ytec::winpeapp::
             bind_reviewed_automatic_boot_repair_winre_images(
                 std::move(oversized_execution),
                 std::vector<ytec::winpeapp::
                     WinPeAutomaticBootRepairWinReImageBinding>{
                     {.windows_partition_number = 2U,
                      .identity = oversized_identity}}),
        "a Winre.wim above the bounded transaction envelope must fail before BCD mutation");

  auto changed_plan = plan;
  ++changed_plan.windows_installations.front().winre.image_size_bytes;
  check(!ytec::winpeapp::validate_reviewed_automatic_boot_repair_inspections(
             changed_plan,
             choices.value(),
             bound.value(),
             std::vector<ytec::bootrepair::BootRepairTargetSelection>{
                 inspected_selection(changed_plan)}),
        "WinRE evidence drift must invalidate the bound registration action");
}

void product_gui_routes_missing_system_partition_transaction() {
  std::ifstream input(YTEC_WINPE_GUI_SOURCE_PATH, std::ios::binary);
  check(input.is_open(), "WinPE product GUI source must be readable");
  const std::string source{
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>()};

  const auto prompt = source.find(
      "prompt_system_partition_creation_windows_choice(");
  const auto observation = source.find(
      "make_windows_system_partition_creation_platform()", prompt);
  const auto review = source.find(
      "review_system_partition_creation(", observation);
  const auto retained = source.find(
      "inspected_system_partition_creation", review);
  const auto exact_ok = source.find(
      "control_text(state.repair_token) != L\"OK\"", retained);
  const auto execute = source.find(
      "execute_system_partition_creation(", exact_ok);
  const auto completed = source.find(
      "completed_creation_plan", execute);
  const auto next_review = source.find(
      "start_boot_review(*state, std::move(plan), choice.value())",
      completed);

  check(prompt != std::string::npos,
        "missing-system route must expose a dedicated Windows shrink choice");
  check(observation != std::string::npos && review != std::string::npos,
        "product route must observe with VDS and retain the pure creation review");
  check(retained != std::string::npos && exact_ok != std::string::npos &&
            execute != std::string::npos,
        "product route must require exact uppercase OK before creation execution");
  check(completed != std::string::npos && next_review != std::string::npos,
        "a verified created system partition must feed the normal BCD review route");
  check(source.find(
            "rollback未確認のため、後続の起動修復を開始しません。",
            completed) != std::string::npos,
        "rollback-incomplete creation must fail closed before BCD repair");
}

}  // namespace

int main() {
  try {
    layouts_fit();
    unambiguous_mbr_plan_builds_existing_transaction_request();
    unambiguous_gpt_plan_builds_existing_esp_transaction_request();
    initial_standalone_inspection_requires_every_reviewed_field();
    exact_ok_and_review_cancellation_are_explicit();
    ambiguous_and_separate_plans_fail_closed();
    execution_recheck_requires_the_complete_reviewed_plan();
    reviewed_multi_windows_partial_maps_to_one_safe_batch();
    current_pc_nvram_requires_separate_explicit_uefi_choice();
    windows_priority_requires_explicit_user_choice();
    third_party_efi_defaults_to_preserve_and_delete_is_explicit();
    verified_winre_fallback_is_bound_to_the_registration_transaction();
    product_gui_routes_missing_system_partition_transaction();
    std::cout << "winpe automatic boot repair ui tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "winpe automatic boot repair ui tests: FAIL: "
              << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

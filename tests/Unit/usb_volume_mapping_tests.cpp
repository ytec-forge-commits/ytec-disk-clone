#include "rescue_media_test_fixture.h"

#include "ytec/windowsapp/usb_volume_mapping.h"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

ytec::diskmodel::DiskInfo safe_usb() {
  constexpr std::uint64_t disk_size =
      32ULL * 1024ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t partition_offset =
      1024ULL * 1024ULL;
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 7U;
  disk.device_instance_id =
      L"USB\\VID_1234&PID_5678\\MAPPING-MOCK";
  disk.model = L"Mock USB Memory";
  disk.size_bytes = disk_size;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 4096U;
  disk.bus_type = L"USB";
  disk.serial_suffix = "A1B2C3D4";
  disk.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = true;
  disk.is_system_disk = false;
  disk.partitions.push_back({
      .number = 1U,
      .offset_bytes = partition_offset,
      .size_bytes = disk_size - partition_offset,
      .style = ytec::diskmodel::PartitionStyle::mbr,
  });
  return disk;
}

ytec::windowsapp::DriveLetterVolume target_volume(
    const wchar_t letter = L'E') {
  const auto disk = safe_usb();
  return {
      .drive_letter = letter,
      .extents =
          {{
              .disk_number = disk.disk_number,
              .starting_offset =
                  disk.partitions.front().offset_bytes,
              .length = disk.partitions.front().size_bytes,
          }},
  };
}

ytec::windowsapp::RescueUsbStoragePlan initialization_plan(
    const ytec::diskmodel::DiskInfo& disk) {
  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = ytec::windowsapp::
          RescueUsbProvisioningMode::initialize_all,
      .data_file_system =
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
  });
  check(plan.has_value(), "Initialization plan fixture should succeed");
  return plan.value();
}

ytec::diskmodel::DiskInfo completed_usb() {
  auto disk = safe_usb();
  constexpr std::uint64_t offset = 1024ULL * 1024ULL;
  constexpr std::uint64_t boot =
      ytec::windowsapp::kRescueUsbBootPartitionBytes;
  disk.partitions = {
      {
          .number = 1U,
          .offset_bytes = offset,
          .size_bytes = boot,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x0C",
          .bootable = true,
      },
      {
          .number = 2U,
          .offset_bytes = offset + boot,
          .size_bytes = disk.size_bytes - offset - boot,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
      },
  };
  return disk;
}

std::vector<ytec::windowsapp::DriveLetterVolume>
completed_volumes(const ytec::diskmodel::DiskInfo& disk) {
  return {
      {
          .drive_letter = L'R',
          .extents = {{
              .disk_number = disk.disk_number,
              .starting_offset = disk.partitions[0].offset_bytes,
              .length = disk.partitions[0].size_bytes,
          }},
      },
      {
          .drive_letter = L'S',
          .extents = {{
              .disk_number = disk.disk_number,
              .starting_offset = disk.partitions[1].offset_bytes,
              .length = disk.partitions[1].size_bytes,
          }},
      },
  };
}

void test_unique_mapping_succeeds_without_write() {
  const auto disk = safe_usb();
  std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {
          .drive_letter = L'C',
          .extents =
              {{
                  .disk_number = 0U,
                  .starting_offset = 1024ULL * 1024ULL,
                  .length = 100ULL * 1024ULL * 1024ULL,
              }},
      },
      target_volume(L'e'),
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, volumes);
  check(static_cast<bool>(result), "Unique USB mapping should pass");
  check(result.value().drive_letter == L'E',
        "Drive letter should be normalized to uppercase");
  check(result.value().root_path == L"E:\\",
        "Root path should be canonical");
  check(result.value().partition_number == 1U,
        "Mapped partition should be preserved");
  check(result.value().target_identity.disk_number == 7U,
        "Stable target identity should be preserved");
  check(!result.value().physical_write_started,
        "Read-only mapping must never start a physical write");
  check(!result.value().drive_letter_was_unassigned,
        "Existing volume mapping must not be marked as proposed");
}

void test_unpartitioned_usb_receives_one_unused_letter_without_write() {
  auto disk = safe_usb();
  disk.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  disk.partitions.clear();
  const std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {.drive_letter = L'C', .extents = {}},
      {.drive_letter = L'D', .extents = {}},
      {.drive_letter = L'E', .extents = {}},
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, volumes);
  check(static_cast<bool>(result),
        "An unpartitioned safe USB should receive a proposed letter");
  check(result.value().drive_letter == L'F' &&
            result.value().root_path == L"F:\\" &&
            result.value().partition_number == 0U &&
            result.value().drive_letter_was_unassigned &&
            !result.value().physical_write_started,
        "The first unused letter must be proposed without any write");
}

void test_unpartitioned_usb_rejects_stale_extent() {
  auto disk = safe_usb();
  disk.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  disk.partitions.clear();
  const std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {
          .drive_letter = L'J',
          .extents =
              {{
                  .disk_number = disk.disk_number,
                  .starting_offset = 1024U * 1024U,
                  .length = 1024U * 1024U,
              }},
      },
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, volumes);
  check(!result,
        "An unpartitioned USB with a stale extent must fail closed");
}

void test_spanned_volume_is_rejected() {
  const auto disk = safe_usb();
  auto volume = target_volume();
  volume.extents.push_back({
      .disk_number = 8U,
      .starting_offset = 1024ULL * 1024ULL,
      .length = 1024ULL * 1024ULL,
  });
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{volume});
  check(!result, "Spanned USB volume must fail closed");
  check(
      result.error().code ==
          ytec::clonecore::ErrorCode::unsupported_layout,
      "Spanned volume should report unsupported layout");
}

void test_multiple_drive_letters_are_rejected() {
  const auto disk = safe_usb();
  const std::vector volumes{
      target_volume(L'E'),
      target_volume(L'F'),
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, volumes);
  check(!result, "Ambiguous USB letters must fail closed");
  check(result.error().code ==
            ytec::clonecore::ErrorCode::identity_mismatch,
        "Ambiguous letters should report identity mismatch");
}

void test_out_of_range_extent_is_rejected() {
  const auto disk = safe_usb();
  auto volume = target_volume();
  volume.extents.front().starting_offset = disk.size_bytes - 100U;
  volume.extents.front().length = 200U;
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{volume});
  check(!result, "Out-of-range extent must fail closed");
}

void test_partition_mismatch_is_rejected() {
  const auto disk = safe_usb();
  auto volume = target_volume();
  volume.extents.front().starting_offset += 512U;
  volume.extents.front().length -= 512U;
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{volume});
  check(!result, "Extent not matching a partition must fail closed");
}

void test_no_target_mapping_is_rejected() {
  const auto disk = safe_usb();
  const std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {
          .drive_letter = L'C',
          .extents =
              {{
                  .disk_number = 0U,
                  .starting_offset = 1024ULL * 1024ULL,
                  .length = 100ULL * 1024ULL * 1024ULL,
              }},
      },
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, volumes);
  check(!result, "Missing target letter must fail closed");
}

void test_unsafe_target_states_are_rejected() {
  auto disk = safe_usb();
  disk.is_system_disk = true;
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()}),
      "System disk must be rejected");

  disk = safe_usb();
  disk.bus_type = L"NVMe";
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()}),
      "Non-USB disk must be rejected");

  disk = safe_usb();
  disk.read_only = true;
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()}),
      "Read-only USB must be rejected as a write target");

  disk = safe_usb();
  disk.offline = true;
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()}),
      "Offline USB must be rejected");
}

void test_unstable_identity_is_rejected() {
  auto disk = safe_usb();
  disk.serial_suffix.clear();
  disk.device_instance_id.clear();
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(
          disk, std::vector{target_volume()});
  check(!result, "Unstable USB identity must fail closed");
}

void test_reviewed_initialization_accepts_current_multi_layout_only() {
  auto disk = safe_usb();
  disk.partitions.front().size_bytes =
      8ULL * 1024ULL * 1024ULL * 1024ULL;
  disk.partitions.push_back({
      .number = 2U,
      .offset_bytes =
          8ULL * 1024ULL * 1024ULL * 1024ULL + 1024ULL * 1024ULL,
      .size_bytes = disk.size_bytes -
          (8ULL * 1024ULL * 1024ULL * 1024ULL + 1024ULL * 1024ULL),
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
  });
  const auto plan = initialization_plan(disk);
  const std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {.drive_letter = L'C', .extents = {}},
      {.drive_letter = L'D', .extents = {}},
      {.drive_letter = L'E', .extents = {}},
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter_for_plan(
          disk, volumes, plan,
          ytec::windowsapp::
              RescueUsbDestinationVerificationPoint::before_write);
  check(result.has_value() && result.value().drive_letter == L'F' &&
            result.value().partition_number == 0U &&
            result.value().drive_letter_was_unassigned &&
            !result.value().physical_write_started,
        "Reviewed multi-partition initialization must propose only an unused letter without writing");

  auto drifted = disk;
  drifted.partitions[1].offset_bytes += 512U;
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter_for_plan(
          drifted, volumes, plan,
          ytec::windowsapp::
              RescueUsbDestinationVerificationPoint::before_write),
      "Any current full-layout drift must invalidate the reviewed plan");
}

void test_post_initialization_requires_exact_completed_layout() {
  const auto before = safe_usb();
  const auto plan = initialization_plan(before);
  const auto completed = completed_usb();
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter_for_plan(
          completed, completed_volumes(completed), plan,
          ytec::windowsapp::
              RescueUsbDestinationVerificationPoint::after_write);
  check(result.has_value() && result.value().drive_letter == L'R' &&
            result.value().partition_number == 1U &&
            !result.value().drive_letter_was_unassigned,
        "Post-initialization mapping must accept only the exact 4 GiB boot volume");

  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter_for_plan(
          before, std::vector{target_volume()}, plan,
          ytec::windowsapp::
              RescueUsbDestinationVerificationPoint::after_write),
      "An incomplete post-initialization layout must fail closed");
}

void test_owned_refresh_keeps_the_reviewed_layout_and_boot_root() {
  const auto disk = completed_usb();
  const auto evidence =
      ytec::windowsapp::build_rescue_usb_owned_media_evidence(
          disk,
          rescue_media_test_fixture::owned_manifest(disk),
          rescue_media_test_fixture::marker_bytes(),
          rescue_media_test_fixture::boot_files(),
          rescue_media_test_fixture::boot_directories(),
          rescue_media_test_fixture::data_identity());
  check(evidence.has_value(), "Owned-media evidence fixture should succeed");
  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = ytec::windowsapp::
          RescueUsbProvisioningMode::preserve_data_refresh,
      .data_file_system =
          ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
      .owned_media = &evidence.value().owned_media,
  });
  check(plan.has_value(), "Owned refresh plan fixture should succeed");
  for (const auto point : {
           ytec::windowsapp::
               RescueUsbDestinationVerificationPoint::before_write,
           ytec::windowsapp::
               RescueUsbDestinationVerificationPoint::after_write,
       }) {
    const auto result =
        ytec::windowsapp::resolve_rescue_usb_drive_letter_for_plan(
            disk, completed_volumes(disk), plan.value(), point);
    check(result.has_value() && result.value().drive_letter == L'R' &&
              result.value().partition_number == 1U,
          "Owned refresh must resolve only the reviewed boot root before and after writing");
  }
}

void test_owned_two_partition_media_resolves_only_boot_volume() {
  auto disk = safe_usb();
  constexpr std::uint64_t offset = 1024ULL * 1024ULL;
  constexpr std::uint64_t boot =
      4ULL * 1024ULL * 1024ULL * 1024ULL;
  disk.partitions = {
      {
          .number = 1U,
          .offset_bytes = offset,
          .size_bytes = boot,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x0C",
          .bootable = true,
      },
      {
          .number = 2U,
          .offset_bytes = offset + boot,
          .size_bytes = disk.size_bytes - offset - boot,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
      },
  };
  const std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {
          .drive_letter = L'R',
          .extents = {{
              .disk_number = disk.disk_number,
              .starting_offset = disk.partitions[0].offset_bytes,
              .length = disk.partitions[0].size_bytes,
          }},
      },
      {
          .drive_letter = L'S',
          .extents = {{
              .disk_number = disk.disk_number,
              .starting_offset = disk.partitions[1].offset_bytes,
              .length = disk.partitions[1].size_bytes,
          }},
      },
  };
  const auto result =
      ytec::windowsapp::resolve_rescue_usb_drive_letter(disk, volumes);
  check(static_cast<bool>(result),
        "Owned two-partition media should resolve its boot volume");
  check(result.value().drive_letter == L'R' &&
            result.value().partition_number == 1U,
        "The data volume must never be selected as the boot update root");

  auto ambiguous = volumes;
  ambiguous.push_back(volumes.front());
  ambiguous.back().drive_letter = L'T';
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(disk, ambiguous),
      "Two drive letters for the boot partition must fail closed");
}

void test_foreign_two_partition_layout_is_rejected() {
  auto disk = safe_usb();
  disk.partitions = {
      {
          .number = 1U,
          .offset_bytes = 1024ULL * 1024ULL,
          .size_bytes = 3ULL * 1024ULL * 1024ULL * 1024ULL,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .bootable = true,
      },
      {
          .number = 2U,
          .offset_bytes =
              3ULL * 1024ULL * 1024ULL * 1024ULL +
              1024ULL * 1024ULL,
          .size_bytes =
              disk.size_bytes -
              (3ULL * 1024ULL * 1024ULL * 1024ULL +
               1024ULL * 1024ULL),
          .style = ytec::diskmodel::PartitionStyle::mbr,
      },
  };
  const std::vector<ytec::windowsapp::DriveLetterVolume> volumes{
      {
          .drive_letter = L'R',
          .extents = {{
              .disk_number = disk.disk_number,
              .starting_offset = disk.partitions[0].offset_bytes,
              .length = disk.partitions[0].size_bytes,
          }},
      },
  };
  check(
      !ytec::windowsapp::resolve_rescue_usb_drive_letter(disk, volumes),
      "A foreign same-count layout must not enter the refresh mapping path");
}

}  // namespace

int main() {
  try {
    test_unique_mapping_succeeds_without_write();
    test_unpartitioned_usb_receives_one_unused_letter_without_write();
    test_unpartitioned_usb_rejects_stale_extent();
    test_spanned_volume_is_rejected();
    test_multiple_drive_letters_are_rejected();
    test_out_of_range_extent_is_rejected();
    test_partition_mismatch_is_rejected();
    test_no_target_mapping_is_rejected();
    test_unsafe_target_states_are_rejected();
    test_unstable_identity_is_rejected();
    test_reviewed_initialization_accepts_current_multi_layout_only();
    test_post_initialization_requires_exact_completed_layout();
    test_owned_refresh_keeps_the_reviewed_layout_and_boot_root();
    test_owned_two_partition_media_resolves_only_boot_volume();
    test_foreign_two_partition_layout_is_rejected();
    std::cout << "usb volume mapping tests passed\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << failure.message << '\n';
    return 1;
  }
}

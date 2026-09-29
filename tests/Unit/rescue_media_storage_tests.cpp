#include "ytec/windowsapp/rescue_media_storage.h"

#include "ytec/imageformat/sha256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::windowsapp::RescueMediaFileFingerprint;
using ytec::windowsapp::RescueUsbDataFileSystem;
using ytec::windowsapp::RescueUsbOwnedMediaInspection;
using ytec::windowsapp::RescueUsbProvisioningMode;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::imageformat::Sha256Digest digest(const std::string& text) {
  const auto bytes = std::as_bytes(std::span(text));
  const auto result = ytec::imageformat::sha256(bytes);
  check(result.has_value(), "SHA-256 fixture should succeed");
  return result.value();
}

std::string digest_hex(const ytec::imageformat::Sha256Digest& value) {
  constexpr char hexadecimal[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(value.size() * 2U);
  for (const std::byte byte : value) {
    const auto number = std::to_integer<unsigned int>(byte);
    output.push_back(hexadecimal[(number >> 4U) & 0xFU]);
    output.push_back(hexadecimal[number & 0xFU]);
  }
  return output;
}

RescueMediaFileFingerprint file(
    std::wstring path,
    const std::string& contents) {
  return {
      .relative_path = std::move(path),
      .length = static_cast<std::uint64_t>(contents.size()),
      .sha256 = digest(contents),
      .reparse_point = false,
      .hard_link_count = 1U,
  };
}

ytec::diskmodel::DiskInfo base_usb() {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 17U;
  disk.device_instance_id = L"USB\\VID_5954&PID_4543\\OWNED-MOCK";
  disk.model = L"Synthetic rescue USB";
  disk.size_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 4096U;
  disk.bus_type = L"USB";
  disk.serial_suffix = "AB12CD34";
  disk.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = true;
  disk.is_system_disk = false;
  return disk;
}

ytec::diskmodel::DiskInfo initializable_usb() {
  auto disk = base_usb();
  disk.partitions.push_back({
      .number = 1U,
      .offset_bytes = 1024ULL * 1024ULL,
      .size_bytes = disk.size_bytes - 1024ULL * 1024ULL,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
  });
  return disk;
}

ytec::diskmodel::DiskInfo owned_usb() {
  auto disk = base_usb();
  constexpr std::uint64_t offset = 1024ULL * 1024ULL;
  constexpr std::uint64_t boot =
      ytec::windowsapp::kRescueUsbBootPartitionBytes;
  disk.partitions = {
      {
          .number = 1U,
          .offset_bytes = offset,
          .size_bytes = boot,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"FAT32 LBA",
          .bootable = true,
      },
      {
          .number = 2U,
          .offset_bytes = offset + boot,
          .size_bytes = disk.size_bytes - offset - boot,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"IFS",
          .bootable = false,
      },
  };
  return disk;
}

RescueUsbOwnedMediaInspection owned_inspection(
    const ytec::diskmodel::DiskInfo& disk) {
  const std::wstring media_id =
      L"12345678-1234-1234-1234-123456789abc";
  std::vector<std::byte> media_id_bytes;
  for (const wchar_t character : media_id) {
    media_id_bytes.push_back(
        static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  const auto marker_hash = ytec::imageformat::sha256(media_id_bytes);
  check(marker_hash.has_value(), "Marker fixture hash should succeed");
  std::vector<RescueMediaFileFingerprint> boot_files{
      file(L"sources\\boot.wim", "boot-wim"),
      file(L"bootmgr", "boot-manager"),
      file(L"EFI\\BOOT\\bootx64.efi", "efi-manager"),
      {
          .relative_path =
              std::wstring(ytec::windowsapp::kRescueUsbMarkerRelativePath),
          .length = 36U,
          .sha256 = marker_hash.value(),
          .reparse_point = false,
          .hard_link_count = 1U,
      },
  };
  std::vector<std::wstring> boot_directories{
      L"sources",
      L"EFI",
      L"EFI\\BOOT",
      L"YtecDiskClone",
      std::wstring(ytec::windowsapp::kRescueUsbTransactionRelativePath),
  };
  const auto boot_tree =
      ytec::windowsapp::make_rescue_usb_boot_tree_identity(
          boot_files, boot_directories);
  check(boot_tree.has_value(), "Boot tree fixture should be valid");
  const std::vector<RescueMediaFileFingerprint> data_files{
      file(L"images\\backup.tsumugi", "preserved-image"),
      file(L"logs\\run.log", "preserved-log"),
  };
  const std::vector<std::wstring> data_directories{L"images", L"logs"};
  const auto data_tree =
      ytec::windowsapp::make_rescue_usb_private_data_tree_identity(
          data_files, data_directories);
  check(data_tree.has_value(), "Data tree fixture should be valid");
  return {
      .schema_version =
          ytec::windowsapp::kRescueUsbOwnershipSchemaVersion,
      .purpose =
          std::wstring(ytec::windowsapp::kRescueUsbOwnershipPurpose),
      .media_id = media_id,
      .boot_file_system = L"fat32",
      .data_file_system = RescueUsbDataFileSystem::ntfs,
      .boot_partition_bytes =
          ytec::windowsapp::kRescueUsbBootPartitionBytes,
      .manifest_layout =
          ytec::windowsapp::make_rescue_usb_canonical_layout(disk),
      .manifest_owned_boot_tree_identity = boot_tree.value(),
      .manifest_owned_boot_files = boot_files,
      .manifest_owned_boot_directories = boot_directories,
      .observed_boot_tree_identity = boot_tree.value(),
      .observed_boot_files = boot_files,
      .observed_boot_directories = boot_directories,
      .observed_data_tree_identity = data_tree.value(),
  };
}

void initialization_requires_eight_gib_and_plans_exact_split() {
  auto disk = initializable_usb();
  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = RescueUsbProvisioningMode::initialize_all,
      .data_file_system = RescueUsbDataFileSystem::ntfs,
  });
  check(plan.has_value(), "A safe 32 GiB USB should be initializable");
  check(
      plan.value().planned_boot_partition_bytes ==
              4ULL * 1024ULL * 1024ULL * 1024ULL &&
          plan.value().planned_data_partition_uses_remaining_space &&
          plan.value().data_file_system == RescueUsbDataFileSystem::ntfs,
      "Initialization must plan exact 4 GiB FAT32 plus remaining NTFS");

  const auto exfat = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = RescueUsbProvisioningMode::initialize_all,
      .data_file_system = RescueUsbDataFileSystem::exfat,
  });
  check(
      exfat.has_value() &&
          exfat.value().data_file_system == RescueUsbDataFileSystem::exfat,
      "exFAT must require an explicit planner selection");

  disk.size_bytes =
      ytec::windowsapp::kRescueUsbMinimumBytes - 1U;
  disk.partitions.front().size_bytes = disk.size_bytes - 1024ULL * 1024ULL;
  check(
      !ytec::windowsapp::plan_rescue_usb_storage({.target = &disk}),
      "A USB one byte below 8 GiB must fail closed");
}

void refresh_requires_owned_exact_layout_and_canonical_trees() {
  const auto disk = owned_usb();
  auto owned = owned_inspection(disk);
  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = RescueUsbProvisioningMode::preserve_data_refresh,
      .data_file_system = RescueUsbDataFileSystem::ntfs,
      .owned_media = &owned,
  });
  check(plan.has_value(), "Verified owned two-partition media should refresh");
  check(
      plan.value().reviewed_owned_media.has_value() &&
          !plan.value().physical_write_started,
      "Refresh plan must retain immutable ownership evidence before writes");

  std::reverse(
      owned.observed_boot_files.begin(),
      owned.observed_boot_files.end());
  std::reverse(
      owned.observed_boot_directories.begin(),
      owned.observed_boot_directories.end());
  check(
      static_cast<bool>(
          ytec::windowsapp::validate_rescue_usb_storage_plan(
              plan.value(), disk, &owned)),
      "Enumeration order alone must not invalidate canonical evidence");

  auto unowned = owned;
  unowned.observed_boot_files.push_back(file(L"notes.txt", "foreign"));
  check(
      !ytec::windowsapp::plan_rescue_usb_storage({
          .target = &disk,
          .mode = RescueUsbProvisioningMode::preserve_data_refresh,
          .owned_media = &unowned,
      }),
      "Unowned boot content must fail before staging");

  auto reparse = owned_inspection(disk);
  reparse.observed_boot_files.front().reparse_point = true;
  check(
      !ytec::windowsapp::plan_rescue_usb_storage({
          .target = &disk,
          .mode = RescueUsbProvisioningMode::preserve_data_refresh,
          .owned_media = &reparse,
      }),
      "Reparse points in observed boot data must fail closed");

  auto hardlink = owned_inspection(disk);
  hardlink.observed_boot_files.front().hard_link_count = 2U;
  check(
      !ytec::windowsapp::plan_rescue_usb_storage({
          .target = &disk,
          .mode = RescueUsbProvisioningMode::preserve_data_refresh,
          .owned_media = &hardlink,
      }),
      "Hard-linked boot content must fail closed");

  auto occupied_transaction = owned_inspection(disk);
  occupied_transaction.manifest_owned_boot_files.push_back(
      file(
          std::wstring(ytec::windowsapp::kRescueUsbTransactionRelativePath) +
              L"\\leftover.bin",
          "leftover"));
  occupied_transaction.observed_boot_files =
      occupied_transaction.manifest_owned_boot_files;
  const auto occupied_tree =
      ytec::windowsapp::make_rescue_usb_boot_tree_identity(
          occupied_transaction.manifest_owned_boot_files,
          occupied_transaction.manifest_owned_boot_directories);
  check(occupied_tree.has_value(), "Occupied tree fixture should be valid");
  occupied_transaction.manifest_owned_boot_tree_identity =
      occupied_tree.value();
  occupied_transaction.observed_boot_tree_identity = occupied_tree.value();
  check(
      !ytec::windowsapp::plan_rescue_usb_storage({
          .target = &disk,
          .mode = RescueUsbProvisioningMode::preserve_data_refresh,
          .owned_media = &occupied_transaction,
      }),
      "The reserved transaction directory must be empty");
}

void unknown_basic_multi_partition_media_can_be_initialized() {
  auto disk = initializable_usb();
  disk.partitions.front().size_bytes =
      4ULL * 1024ULL * 1024ULL * 1024ULL;
  disk.partitions.push_back({
      .number = 2U,
      .offset_bytes = 8ULL * 1024ULL * 1024ULL * 1024ULL,
      .size_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x0C",
  });
  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = RescueUsbProvisioningMode::initialize_all,
  });
  check(
      plan.has_value() && plan.value().reviewed_layout.partitions.size() == 2U,
      "Unknown basic multi-partition USB must be eligible for reviewed whole-disk initialization");
}

void layout_drift_and_plan_tamper_never_validate() {
  const auto disk = owned_usb();
  const auto owned = owned_inspection(disk);
  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = RescueUsbProvisioningMode::preserve_data_refresh,
      .owned_media = &owned,
  });
  check(plan.has_value(), "Fixture refresh plan should succeed");

  auto geometry_changed = disk;
  geometry_changed.partitions[1].offset_bytes += 512U;
  geometry_changed.partitions[1].size_bytes -= 512U;
  auto changed_owned = owned;
  changed_owned.manifest_layout =
      ytec::windowsapp::make_rescue_usb_canonical_layout(geometry_changed);
  check(
      !ytec::windowsapp::validate_rescue_usb_storage_plan(
          plan.value(), geometry_changed, &changed_owned),
      "Same-count geometry drift must fail exact layout binding");

  auto type_changed = disk;
  type_changed.partitions[1].type = L"0x06";
  changed_owned = owned;
  changed_owned.manifest_layout =
      ytec::windowsapp::make_rescue_usb_canonical_layout(type_changed);
  check(
      !ytec::windowsapp::validate_rescue_usb_storage_plan(
          plan.value(), type_changed, &changed_owned),
      "Same-count partition type drift must fail exact layout binding");

  auto tampered = plan.value();
  tampered.planned_boot_partition_bytes += 512U;
  check(
      !ytec::windowsapp::validate_rescue_usb_storage_plan(
          tampered, disk, &owned),
      "Plan tampering must fail its immutable digest before a writer");
}

void canonical_layout_digest_matches_powershell_wire_contract() {
  const auto layout =
      ytec::windowsapp::make_rescue_usb_canonical_layout(owned_usb());
  const auto expected =
      ytec::windowsapp::make_rescue_usb_canonical_layout_digest(layout);
  check(expected.has_value(), "Canonical layout digest should succeed");
  check(
      digest_hex(expected.value()) ==
          "FAEA8977FDFD7359207E3F4935AFE154"
          "2F59329B93F1089737517D7F0B00DBDD",
      "C++ canonical layout digest must match the audited PowerShell wire value");

  auto reordered = layout;
  std::reverse(reordered.partitions.begin(), reordered.partitions.end());
  const auto reordered_digest =
      ytec::windowsapp::make_rescue_usb_canonical_layout_digest(reordered);
  check(
      reordered_digest.has_value() &&
          reordered_digest.value() != expected.value(),
      "Digest helper requires its documented order-normalized layout input");

  auto geometry_changed = layout;
  geometry_changed.partitions[1].offset_bytes += 512U;
  const auto changed_digest =
      ytec::windowsapp::make_rescue_usb_canonical_layout_digest(
          geometry_changed);
  check(
      changed_digest.has_value() &&
          changed_digest.value() != expected.value(),
      "Same-count geometry drift must change the cross-process digest");
}

void preserved_data_requires_exact_before_after_hashes() {
  const std::vector<RescueMediaFileFingerprint> before{
      file(L"images\\one.tsumugi", "one"),
      file(L"logs\\two.log", "two"),
  };
  auto reordered = before;
  std::reverse(reordered.begin(), reordered.end());
  check(
      static_cast<bool>(
          ytec::windowsapp::validate_rescue_usb_data_unchanged(
              before, reordered)),
      "Unchanged data must compare independent of enumeration order");

  auto modified = before;
  modified.front().sha256 = digest("changed");
  check(
      !ytec::windowsapp::validate_rescue_usb_data_unchanged(
          before, modified),
      "Changed data content must fail the post-refresh gate");

  auto duplicate = before;
  duplicate.push_back(file(L"IMAGES\\ONE.TSUMUGI", "one"));
  check(
      !ytec::windowsapp::validate_rescue_usb_data_unchanged(
          duplicate, duplicate),
      "Case-insensitive duplicate paths must fail even on equal vectors");

  auto invalid_character = before;
  invalid_character.front().relative_path = L"images\\bad?.tsumugi";
  check(
      !ytec::windowsapp::validate_rescue_usb_data_unchanged(
          invalid_character, invalid_character),
      "Windows-forbidden path characters must fail closed");

  auto device_name = before;
  device_name.front().relative_path = L"images\\CON.txt";
  check(
      !ytec::windowsapp::validate_rescue_usb_data_unchanged(
          device_name, device_name),
      "Windows reserved device basenames must fail closed");

  auto oversized = before;
  oversized.front().length =
      2ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL + 1ULL;
  check(
      !ytec::windowsapp::validate_rescue_usb_data_unchanged(
          oversized, oversized),
      "Logical-byte totals above the bounded media limit must fail closed");

  std::vector<RescueMediaFileFingerprint> too_many(262'145U);
  check(
      !ytec::windowsapp::validate_rescue_usb_data_unchanged(
          too_many, too_many),
      "File counts above the bounded tree limit must fail before traversal");
}

}  // namespace

int main() {
  try {
    initialization_requires_eight_gib_and_plans_exact_split();
    refresh_requires_owned_exact_layout_and_canonical_trees();
    unknown_basic_multi_partition_media_can_be_initialized();
    layout_drift_and_plan_tamper_never_validate();
    canonical_layout_digest_matches_powershell_wire_contract();
    preserved_data_requires_exact_before_after_hashes();
    std::cout << "rescue media storage tests passed\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "rescue media storage tests failed: "
              << exception.what() << '\n';
    return 1;
  }
}

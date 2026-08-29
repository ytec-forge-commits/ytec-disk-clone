#include "ytec/bootrepair/system_partition_creation.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace ytec::bootrepair {
namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint64_t kAlignment = 1ULL * kMiB;
constexpr std::uint64_t kGptEspBytes = 260ULL * kMiB;
constexpr std::uint64_t kMbrSystemBytes = 100ULL * kMiB;
constexpr std::uint64_t kReclaimabilityMargin = 256ULL * kMiB;
constexpr std::uint64_t kMinimumRemainingWindowsBytes = 4ULL * kGiB;
constexpr std::wstring_view kGptBasicDataType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
constexpr std::wstring_view kGptEspType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";

clonecore::Error creation_error(
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

bool text_equal(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(), left.end(), right.begin(),
          [](const wchar_t l, const wchar_t r) {
            return std::towlower(l) == std::towlower(r);
          });
}

bool same_partition(
    const diskmodel::PartitionInfo& left,
    const diskmodel::PartitionInfo& right) noexcept {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes && left.style == right.style &&
      text_equal(left.type, right.type) &&
      text_equal(left.identifier, right.identifier) &&
      text_equal(left.name, right.name) && left.bootable == right.bootable;
}

bool same_disk_without_number(
    const diskmodel::DiskInfo& left,
    const diskmodel::DiskInfo& right) noexcept {
  return text_equal(left.device_interface_path, right.device_interface_path) &&
      text_equal(
          left.connection_location_path, right.connection_location_path) &&
      text_equal(left.device_instance_id, right.device_instance_id) &&
      text_equal(left.model, right.model) && left.size_bytes == right.size_bytes &&
      left.sector_count == right.sector_count &&
      left.logical_sector_size == right.logical_sector_size &&
      left.physical_sector_size == right.physical_sector_size &&
      text_equal(left.bus_type, right.bus_type) &&
      left.serial_suffix == right.serial_suffix &&
      left.partition_style == right.partition_style &&
      text_equal(left.disk_identifier, right.disk_identifier) &&
      left.offline == right.offline && left.read_only == right.read_only &&
      left.removable == right.removable &&
      left.is_system_disk == right.is_system_disk;
}

bool same_volume(
    const BootVolumeObservation& left,
    const BootVolumeObservation& right) noexcept {
  return text_equal(left.volume_name, right.volume_name) &&
      left.location.starting_offset == right.location.starting_offset &&
      left.location.extent_length == right.location.extent_length &&
      text_equal(left.location.file_system, right.location.file_system) &&
      left.mount_points.size() == right.mount_points.size() &&
      std::equal(
          left.mount_points.begin(), left.mount_points.end(),
          right.mount_points.begin(),
          [](const auto& l, const auto& r) { return text_equal(l, r); });
}

bool same_winre(
    const AutomaticBootRepairWinReEvidence& left,
    const AutomaticBootRepairWinReEvidence& right) noexcept {
  return left.source_state == right.source_state &&
      left.registered_location_reported ==
          right.registered_location_reported &&
      left.registered_location_matches_selected_disk ==
          right.registered_location_matches_selected_disk &&
      left.registered_partition_number == right.registered_partition_number &&
      left.registered_path_kind_reported ==
          right.registered_path_kind_reported &&
      left.registered_path_kind == right.registered_path_kind &&
      left.registered_image_present == right.registered_image_present &&
      left.fallback_image_present == right.fallback_image_present &&
      left.image_size_bytes == right.image_size_bytes;
}

bool same_windows(
    const DiscoveredWindowsInstallation& left,
    const DiscoveredWindowsInstallation& right) noexcept {
  return same_partition(left.partition, right.partition) &&
      same_volume(left.volume, right.volume) &&
      text_equal(left.windows_directory, right.windows_directory) &&
      left.version.major == right.version.major &&
      left.version.build == right.version.build &&
      text_equal(
          left.version.installation_type,
          right.version.installation_type) &&
      left.officially_supported == right.officially_supported &&
      same_winre(left.winre, right.winre);
}

const DiscoveredWindowsInstallation* find_windows(
    const AutomaticBootRepairPlan& plan,
    const std::uint32_t partition_number) noexcept {
  const auto found = std::find_if(
      plan.windows_installations.begin(),
      plan.windows_installations.end(),
      [partition_number](const auto& windows) {
        return windows.partition.number == partition_number;
      });
  return found == plan.windows_installations.end() ? nullptr : &*found;
}

clonecore::Status validate_target_attributes(
    const AutomaticBootRepairPlan& plan) {
  if (plan.selected_disk.is_system_disk ||
      !plan.selected_disk.offline.has_value() ||
      !plan.selected_disk.read_only.has_value() ||
      !plan.selected_disk.removable.has_value() ||
      plan.selected_disk.offline.value() ||
      plan.selected_disk.read_only.value() ||
      plan.selected_disk.removable.value() ||
      (plan.partition_style != diskmodel::PartitionStyle::gpt &&
       plan.partition_style != diskmodel::PartitionStyle::mbr) ||
      plan.selected_disk.logical_sector_size == 0U ||
      plan.selected_disk.size_bytes == 0U) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"起動修復システム領域作成の対象属性",
        L"オンライン、書込み可能、固定、非起動環境のGPT/MBRディスクだけを扱います"));
  }
  if (!plan.system_partition_create_plan_needed ||
      !plan.system_partition_candidates.empty() ||
      plan.system_partition_selection_policy_needed ||
      plan.windows_not_found || plan.windows_installations.empty()) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"起動修復システム領域作成の発動条件",
        L"既存システム領域がなく、Windowsを一意に診断できた計画ではありません"));
  }
  const bool gpt =
      plan.partition_style == diskmodel::PartitionStyle::gpt &&
      plan.firmware == BcdBootFirmware::uefi &&
      plan.required_system_partition_role ==
          BootSystemPartitionRole::efi_system;
  const bool mbr =
      plan.partition_style == diskmodel::PartitionStyle::mbr &&
      plan.firmware == BcdBootFirmware::bios &&
      plan.required_system_partition_role ==
          BootSystemPartitionRole::bios_active;
  if (!gpt && !mbr) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"起動修復システム領域作成の方式整合性",
        L"GPT/UEFI/ESPまたはMBR/BIOS/Activeの組合せが一致しません"));
  }
  if ((gpt && plan.selected_disk.partitions.size() >= 128U) ||
      (mbr && plan.selected_disk.partitions.size() >= 4U)) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"起動修復システム領域作成の区画上限",
        gpt ? L"GPT entryに新しいESPを安全に追加できません"
            : L"MBR primary partitionが4件のためシステム領域を追加できません"));
  }
  return clonecore::success_status();
}

bool safe_windows_volume_observation(
    const SystemPartitionCreationObservation& observation) noexcept {
  return observation.exact_windows_volume_found &&
      observation.simple_ntfs_volume && observation.volume_online &&
      observation.volume_transition_stable &&
      observation.volume_health_acceptable &&
      !observation.forbidden_volume_role_or_encryption;
}

bool exact_pre_mutation_observation(
    const ReviewedSystemPartitionCreation& reviewed,
    const SystemPartitionCreationObservation& fresh) {
  const auto& original = reviewed.discovery();
  if (!clonecore::validate_stable_identity(
           original.selected_identity,
           fresh.plan.selected_identity,
           L"起動修復システム領域作成対象") ||
      !same_disk_without_number(
          original.selected_disk, fresh.plan.selected_disk) ||
      original.partition_style != fresh.plan.partition_style ||
      original.firmware != fresh.plan.firmware ||
      original.required_system_partition_role !=
          fresh.plan.required_system_partition_role ||
      original.planned_bcd_store_policy !=
          fresh.plan.planned_bcd_store_policy ||
      original.selected_disk.partitions.size() !=
          fresh.plan.selected_disk.partitions.size() ||
      original.windows_installations.size() !=
          fresh.plan.windows_installations.size() ||
      original.windows_not_found != fresh.plan.windows_not_found ||
      original.windows_selection_policy_needed !=
          fresh.plan.windows_selection_policy_needed ||
      original.unsupported_windows_policy_needed !=
          fresh.plan.unsupported_windows_policy_needed ||
      original.system_partition_create_plan_needed !=
          fresh.plan.system_partition_create_plan_needed ||
      original.system_partition_selection_policy_needed !=
          fresh.plan.system_partition_selection_policy_needed ||
      !fresh.plan.system_partition_candidates.empty()) {
    return false;
  }
  for (std::size_t i = 0U; i < original.selected_disk.partitions.size(); ++i) {
    if (!same_partition(
            original.selected_disk.partitions[i],
            fresh.plan.selected_disk.partitions[i])) {
      return false;
    }
  }
  for (std::size_t i = 0U; i < original.windows_installations.size(); ++i) {
    if (!same_windows(
            original.windows_installations[i],
            fresh.plan.windows_installations[i])) {
      return false;
    }
  }
  return safe_windows_volume_observation(fresh) &&
      fresh.max_reclaimable_bytes >= reviewed.reclaim_bytes();
}

bool expected_shrunken_base_layout(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& observed,
    const bool system_partition_is_present) {
  const auto& original = reviewed.discovery();
  if (!clonecore::validate_stable_identity(
           original.selected_identity,
           observed.selected_identity,
           L"起動修復システム領域作成対象") ||
      !same_disk_without_number(original.selected_disk, observed.selected_disk) ||
      original.partition_style != observed.partition_style ||
      original.firmware != observed.firmware ||
      original.required_system_partition_role !=
          observed.required_system_partition_role ||
      original.planned_bcd_store_policy !=
          observed.planned_bcd_store_policy ||
      original.windows_not_found != observed.windows_not_found ||
      original.windows_selection_policy_needed !=
          observed.windows_selection_policy_needed ||
      original.unsupported_windows_policy_needed !=
          observed.unsupported_windows_policy_needed ||
      observed.system_partition_selection_policy_needed ||
      observed.selected_disk.partitions.size() !=
          original.selected_disk.partitions.size() +
              (system_partition_is_present ? 1U : 0U) ||
      observed.windows_installations.size() !=
          original.windows_installations.size()) {
    return false;
  }
  std::set<std::uint32_t> observed_partition_numbers;
  for (const auto& partition : observed.selected_disk.partitions) {
    if (partition.number == 0U ||
        !observed_partition_numbers.insert(partition.number).second) {
      return false;
    }
  }
  for (const auto& expected : original.selected_disk.partitions) {
    const auto current = std::find_if(
        observed.selected_disk.partitions.begin(),
        observed.selected_disk.partitions.end(),
        [&](const auto& value) { return value.number == expected.number; });
    if (current == observed.selected_disk.partitions.end()) {
      return false;
    }
    if (expected.number == reviewed.windows_partition().number) {
      auto adjusted = expected;
      adjusted.size_bytes = reviewed.shrunken_windows_size_bytes();
      if (!same_partition(adjusted, *current)) {
        return false;
      }
    } else if (!same_partition(expected, *current)) {
      return false;
    }
  }
  for (const auto& expected : original.windows_installations) {
    const auto* current = find_windows(
        observed, expected.partition.number);
    if (current == nullptr) {
      return false;
    }
    if (expected.partition.number == reviewed.windows_partition().number) {
      auto adjusted = expected;
      adjusted.partition.size_bytes = reviewed.shrunken_windows_size_bytes();
      adjusted.volume.location.extent_length =
          reviewed.shrunken_windows_size_bytes();
      if (!same_windows(adjusted, *current)) {
        return false;
      }
    } else if (!same_windows(expected, *current)) {
      return false;
    }
  }
  const auto selected = find_windows(observed, reviewed.windows_partition().number);
  return selected != nullptr &&
      selected->partition.size_bytes ==
          reviewed.shrunken_windows_size_bytes() &&
      selected->partition.offset_bytes ==
          reviewed.windows_partition().offset_bytes &&
      selected->volume.location.extent_length ==
          reviewed.shrunken_windows_size_bytes() &&
      text_equal(selected->volume.volume_name, reviewed.windows_volume_name());
}

bool expected_after_shrink(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& observed) {
  return observed.system_partition_candidates.empty() &&
      observed.system_partition_create_plan_needed &&
      !observed.system_partition_selection_policy_needed &&
      expected_shrunken_base_layout(reviewed, observed, false);
}

bool expected_raw_disk_layout(
    const ReviewedSystemPartitionCreation& reviewed,
    const diskmodel::DiskInfo& observed,
    const bool created_partition_is_present) {
  const auto& original = reviewed.discovery().selected_disk;
  auto observed_identity = diskmodel::make_stable_disk_identity(
      observed, observed.is_system_disk);
  if (!observed_identity ||
      !clonecore::validate_stable_identity(
          reviewed.selected_identity(),
          observed_identity.value(),
          L"起動修復システム領域作成対象") ||
      !same_disk_without_number(original, observed) ||
      observed.partitions.size() !=
          original.partitions.size() +
              (created_partition_is_present ? 1U : 0U)) {
    return false;
  }

  std::set<std::uint32_t> observed_numbers;
  for (const auto& partition : observed.partitions) {
    if (partition.number == 0U ||
        !observed_numbers.insert(partition.number).second) {
      return false;
    }
  }

  std::set<std::uint32_t> original_numbers;
  for (const auto& expected : original.partitions) {
    original_numbers.insert(expected.number);
    const auto current = std::find_if(
        observed.partitions.begin(),
        observed.partitions.end(),
        [&](const auto& value) { return value.number == expected.number; });
    if (current == observed.partitions.end()) {
      return false;
    }
    if (expected.number == reviewed.windows_partition().number) {
      if (current->offset_bytes != expected.offset_bytes ||
          current->size_bytes != reviewed.shrunken_windows_size_bytes() ||
          current->style != expected.style ||
          !text_equal(current->type, expected.type) ||
          !text_equal(current->identifier, expected.identifier) ||
          !text_equal(current->name, expected.name) ||
          current->bootable != expected.bootable) {
        return false;
      }
    } else if (!same_partition(expected, *current)) {
      return false;
    }
  }

  std::vector<const diskmodel::PartitionInfo*> additions;
  for (const auto& partition : observed.partitions) {
    if (!original_numbers.contains(partition.number)) {
      additions.push_back(&partition);
    }
  }
  if (!created_partition_is_present) {
    return additions.empty();
  }
  if (additions.size() != 1U) {
    return false;
  }
  const auto& created = *additions.front();
  if (created.offset_bytes != reviewed.system_partition_offset_bytes() ||
      created.size_bytes != reviewed.system_partition_size_bytes()) {
    return false;
  }
  if (reviewed.system_role() == BootSystemPartitionRole::efi_system) {
    return created.style == diskmodel::PartitionStyle::gpt &&
        text_equal(created.type, kGptEspType) &&
        !created.identifier.empty() && !created.bootable;
  }
  return created.style == diskmodel::PartitionStyle::mbr &&
      created.number <= 4U && text_equal(created.type, L"0x07") &&
      created.bootable;
}

bool expected_completed_plan(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& observed) {
  if (!expected_shrunken_base_layout(reviewed, observed, true)) {
    return false;
  }
  if (observed.system_partition_create_plan_needed ||
      observed.system_partition_selection_policy_needed ||
      observed.system_partition_candidates.size() != 1U ||
      observed.selected_disk.partitions.size() !=
          reviewed.discovery().selected_disk.partitions.size() + 1U) {
    return false;
  }
  const auto& system = observed.system_partition_candidates.front();
  const bool exact_geometry =
      system.role == reviewed.system_role() &&
      system.partition.offset_bytes ==
          reviewed.system_partition_offset_bytes() &&
      system.partition.size_bytes ==
          reviewed.system_partition_size_bytes() &&
      system.partition.number != 0U &&
      system.volume.location.starting_offset ==
          reviewed.system_partition_offset_bytes() &&
      system.volume.location.extent_length ==
          reviewed.system_partition_size_bytes() &&
      !system.volume.volume_name.empty();
  if (!exact_geometry) {
    return false;
  }
  const auto selected_disk_partition = std::find_if(
      observed.selected_disk.partitions.begin(),
      observed.selected_disk.partitions.end(),
      [&](const auto& partition) {
        return partition.number == system.partition.number;
      });
  if (selected_disk_partition == observed.selected_disk.partitions.end() ||
      !same_partition(*selected_disk_partition, system.partition)) {
    return false;
  }
  if (reviewed.system_role() == BootSystemPartitionRole::efi_system) {
    return system.partition.style == diskmodel::PartitionStyle::gpt &&
        text_equal(system.partition.type, kGptEspType) &&
        text_equal(system.volume.location.file_system, L"FAT32") &&
        system.efi_ownership.state ==
            EfiBootOwnershipState::microsoft_only_or_empty;
  }
  return system.partition.style == diskmodel::PartitionStyle::mbr &&
      system.partition.number <= 4U &&
      text_equal(system.partition.type, L"0x07") &&
      system.partition.bootable &&
      text_equal(system.volume.location.file_system, L"NTFS") &&
      system.efi_ownership.state == EfiBootOwnershipState::not_applicable;
}

void attempt_rollback(
    const ReviewedSystemPartitionCreation& reviewed,
    ISystemPartitionCreationPlatform& platform,
    const bool created,
    SystemPartitionCreationReport& report) {
  report.rollback_attempted = true;
  if (created) {
    const auto removed = platform.delete_created_system_exact(reviewed);
    report.created_partition_removed = removed.has_value();
    if (!removed && !report.rollback_error.has_value()) {
      report.rollback_error = removed.error();
    }
  } else {
    report.created_partition_removed = true;
  }
  if (report.created_partition_removed) {
    const auto extended = platform.extend_windows_exact(reviewed);
    report.windows_extent_restored = extended.has_value();
    if (!extended && !report.rollback_error.has_value()) {
      report.rollback_error = extended.error();
    }
  }
  if (report.windows_extent_restored) {
    auto restored = platform.observe_read_only(
        reviewed.selected_identity(), reviewed.windows_partition().number);
    if (restored && exact_pre_mutation_observation(reviewed, restored.value())) {
      report.original_layout_restored = true;
    } else if (!report.rollback_error.has_value()) {
      report.rollback_error = restored
          ? creation_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_DATA,
                L"起動修復システム領域作成のrollback読戻し",
                L"元のWindows extentと全レイアウトへ戻ったことを確認できません")
          : restored.error();
    }
  }
  report.outcome = report.created_partition_removed &&
          report.windows_extent_restored && report.original_layout_restored
      ? SystemPartitionCreationOutcome::rolled_back_exact
      : SystemPartitionCreationOutcome::rollback_incomplete;
}

}  // namespace

ReviewedSystemPartitionCreation::ReviewedSystemPartitionCreation(
    AutomaticBootRepairPlan discovery,
    DiscoveredWindowsInstallation windows,
    const std::uint64_t system_partition_size_bytes,
    const std::uint64_t reclaim_bytes,
    const std::uint64_t system_partition_offset_bytes,
    const std::uint64_t shrunken_windows_size_bytes)
    : discovery_(std::move(discovery)),
      windows_(std::move(windows)),
      system_partition_size_bytes_(system_partition_size_bytes),
      reclaim_bytes_(reclaim_bytes),
      system_partition_offset_bytes_(system_partition_offset_bytes),
      shrunken_windows_size_bytes_(shrunken_windows_size_bytes) {}

const AutomaticBootRepairPlan&
ReviewedSystemPartitionCreation::discovery() const noexcept {
  return discovery_;
}

const clonecore::StableDiskIdentity&
ReviewedSystemPartitionCreation::selected_identity() const noexcept {
  return discovery_.selected_identity;
}

const diskmodel::PartitionInfo&
ReviewedSystemPartitionCreation::windows_partition() const noexcept {
  return windows_.partition;
}

const std::wstring&
ReviewedSystemPartitionCreation::windows_volume_name() const noexcept {
  return windows_.volume.volume_name;
}

BootSystemPartitionRole
ReviewedSystemPartitionCreation::system_role() const noexcept {
  return discovery_.required_system_partition_role;
}

std::uint64_t
ReviewedSystemPartitionCreation::system_partition_size_bytes() const noexcept {
  return system_partition_size_bytes_;
}

std::uint64_t ReviewedSystemPartitionCreation::reclaim_bytes() const noexcept {
  return reclaim_bytes_;
}

std::uint64_t
ReviewedSystemPartitionCreation::system_partition_offset_bytes() const noexcept {
  return system_partition_offset_bytes_;
}

std::uint64_t
ReviewedSystemPartitionCreation::shrunken_windows_size_bytes() const noexcept {
  return shrunken_windows_size_bytes_;
}

clonecore::Result<ReviewedSystemPartitionCreation>
review_system_partition_creation(
    const AutomaticBootRepairPlan& discovery,
    const std::uint32_t selected_windows_partition_number,
    const SystemPartitionCreationObservation& observation) {
  const auto attributes = validate_target_attributes(discovery);
  if (!attributes) {
    return clonecore::Result<ReviewedSystemPartitionCreation>::failure(
        attributes.error());
  }
  const auto identity = clonecore::validate_stable_identity(
      discovery.selected_identity,
      observation.plan.selected_identity,
      L"起動修復システム領域作成対象");
  if (!identity || !same_disk_without_number(
                       discovery.selected_disk,
                       observation.plan.selected_disk)) {
    return clonecore::Result<ReviewedSystemPartitionCreation>::failure(
        identity ? creation_error(
                       clonecore::ErrorCode::identity_mismatch,
                       ERROR_DEVICE_NOT_CONNECTED,
                       L"起動修復システム領域作成の完全再識別",
                       L"読取り計画とVDS観測の対象ディスク属性が一致しません")
                 : identity.error());
  }
  const auto* windows = find_windows(
      discovery, selected_windows_partition_number);
  const auto* observed_windows = find_windows(
      observation.plan, selected_windows_partition_number);
  if (windows == nullptr || observed_windows == nullptr ||
      !same_windows(*windows, *observed_windows) ||
      windows->volume.volume_name.empty() ||
      !text_equal(windows->volume.location.file_system, L"NTFS") ||
      !observation.exact_windows_volume_found ||
      !observation.simple_ntfs_volume || !observation.volume_online ||
      !observation.volume_transition_stable ||
      !observation.volume_health_acceptable ||
      observation.forbidden_volume_role_or_encryption) {
    return clonecore::Result<ReviewedSystemPartitionCreation>::failure(
        creation_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"起動修復システム領域作成のNTFS候補",
            L"一意なsimple NTFS、online、stable、非暗号化Windows volumeを安全に拘束できません"));
  }
  const bool gpt = discovery.partition_style == diskmodel::PartitionStyle::gpt;
  const bool mbr_primary_only = gpt ||
      (windows->partition.number >= 1U &&
       windows->partition.number <= 4U &&
       std::all_of(
           discovery.selected_disk.partitions.begin(),
           discovery.selected_disk.partitions.end(),
           [](const auto& partition) {
             return partition.number >= 1U && partition.number <= 4U &&
                 !text_equal(partition.type, L"0x05") &&
                 !text_equal(partition.type, L"0x0F") &&
                 !text_equal(partition.type, L"0x85");
           }));
  const bool type_valid = gpt
      ? text_equal(windows->partition.type, kGptBasicDataType)
      : text_equal(windows->partition.type, L"0x07");
  const std::uint64_t system_bytes = gpt ? kGptEspBytes : kMbrSystemBytes;
  if (!type_valid || !mbr_primary_only ||
      windows->partition.offset_bytes % kAlignment != 0U ||
      windows->partition.size_bytes % kAlignment != 0U ||
      windows->partition.size_bytes <=
          system_bytes + kMinimumRemainingWindowsBytes ||
      observation.max_reclaimable_bytes < system_bytes ||
      observation.max_reclaimable_bytes - system_bytes <
          kReclaimabilityMargin) {
    return clonecore::Result<ReviewedSystemPartitionCreation>::failure(
        creation_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_DISK_FULL,
            L"起動修復システム領域作成の縮小余力",
            L"基本GPT/MBR primary構成、1 MiB整列、最小Windows寸法、またはNTFSの安全な縮小余力を満たしません"));
  }
  const std::uint64_t shrunken_size =
      windows->partition.size_bytes - system_bytes;
  if (windows->partition.offset_bytes >
      (std::numeric_limits<std::uint64_t>::max)() - shrunken_size) {
    return clonecore::Result<ReviewedSystemPartitionCreation>::failure(
        creation_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"起動修復システム領域作成の配置計算",
            L"Windows終端を64ビット範囲で表現できません"));
  }
  const std::uint64_t system_offset =
      windows->partition.offset_bytes + shrunken_size;
  if (system_offset % kAlignment != 0U ||
      system_offset > discovery.selected_disk.size_bytes ||
      system_bytes > discovery.selected_disk.size_bytes - system_offset) {
    return clonecore::Result<ReviewedSystemPartitionCreation>::failure(
        creation_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"起動修復システム領域作成の配置境界",
            L"作成予定領域が1 MiB境界または対象ディスク範囲と一致しません"));
  }
  return clonecore::Result<ReviewedSystemPartitionCreation>::success(
      ReviewedSystemPartitionCreation(
          discovery,
          *windows,
          system_bytes,
          system_bytes,
          system_offset,
          shrunken_size));
}

clonecore::Status revalidate_system_partition_creation_review(
    const ReviewedSystemPartitionCreation& reviewed,
    const SystemPartitionCreationObservation& fresh) {
  const auto identity = clonecore::validate_stable_identity(
      reviewed.selected_identity(),
      fresh.plan.selected_identity,
      L"起動修復システム領域作成対象");
  if (!identity) {
    return identity;
  }
  if (!exact_pre_mutation_observation(reviewed, fresh)) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"起動修復システム領域作成の実行直前再解析",
        L"ディスク、全区画、Windows/WinRE、NTFS状態、または縮小可能量がレビュー時から変化しました"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_system_partition_creation_shrunken_plan(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& observed) {
  if (!expected_after_shrink(reviewed, observed)) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"起動修復システム領域作成の縮小後完全拘束",
        L"安定対象、全区画、Windows/WinRE、またはmissing-system状態がレビュー済み縮小後計画と一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_system_partition_creation_completed_plan(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& observed) {
  if (!expected_completed_plan(reviewed, observed)) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"起動修復システム領域作成の完成後完全拘束",
        L"安定対象、全区画、Windows/WinRE、system extent、filesystem、またはEFI所有権が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_system_partition_creation_shrunken_disk(
    const ReviewedSystemPartitionCreation& reviewed,
    const diskmodel::DiskInfo& observed) {
  if (!expected_raw_disk_layout(reviewed, observed, false)) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"起動修復システム領域作成の縮小後raw layout拘束",
        L"安定対象または全パーティションがレビュー済み縮小後layoutと一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status
validate_system_partition_creation_created_disk_for_rollback(
    const ReviewedSystemPartitionCreation& reviewed,
    const diskmodel::DiskInfo& observed) {
  if (!expected_raw_disk_layout(reviewed, observed, true)) {
    return clonecore::Status::failure(creation_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"起動修復システム領域作成のrollback raw layout拘束",
        L"安定対象、全既存区画、またはexact created system partitionが一致しません"));
  }
  return clonecore::success_status();
}

SystemPartitionCreationReport execute_system_partition_creation(
    const ReviewedSystemPartitionCreation& reviewed,
    const clonecore::TargetConfirmation& confirmation,
    ISystemPartitionCreationPlatform& platform) {
  SystemPartitionCreationReport report;
  if (!confirmation.first_step_acknowledged ||
      confirmation.typed_token != L"OK") {
    report.primary_error = creation_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"起動修復システム領域作成の追加確認",
        L"変更内容の確認と大文字OKの完全一致が必要です");
    return report;
  }
  report.confirmation_verified = true;
  auto before = platform.observe_read_only(
      reviewed.selected_identity(), reviewed.windows_partition().number);
  if (!before) {
    report.primary_error = before.error();
    return report;
  }
  const auto rebound = revalidate_system_partition_creation_review(
      reviewed, before.value());
  if (!rebound) {
    report.primary_error = rebound.error();
    return report;
  }
  report.pre_mutation_revalidated = true;

  const auto shrunk = platform.shrink_windows_exact(reviewed);
  if (!shrunk) {
    report.primary_error = shrunk.error();
    auto after_failed_shrink = platform.observe_read_only(
        reviewed.selected_identity(), reviewed.windows_partition().number);
    if (after_failed_shrink && exact_pre_mutation_observation(
                                   reviewed, after_failed_shrink.value())) {
      return report;
    }
    if (after_failed_shrink &&
        safe_windows_volume_observation(after_failed_shrink.value()) &&
        expected_after_shrink(reviewed, after_failed_shrink.value().plan)) {
      report.windows_shrunk = true;
      report.shrunken_layout_verified = true;
      attempt_rollback(reviewed, platform, false, report);
      return report;
    }
    report.outcome = SystemPartitionCreationOutcome::rollback_incomplete;
    report.rollback_error = after_failed_shrink
        ? creation_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"起動修復システム領域作成の失敗したshrink読戻し",
              L"VDS shrink失敗後の状態が元layoutにもexact縮小後layoutにも一致しないため追加書込みを停止しました")
        : after_failed_shrink.error();
    return report;
  }
  report.windows_shrunk = true;
  auto after_shrink = platform.observe_read_only(
      reviewed.selected_identity(), reviewed.windows_partition().number);
  if (!after_shrink || !safe_windows_volume_observation(after_shrink.value()) ||
      !expected_after_shrink(reviewed, after_shrink.value().plan)) {
    report.primary_error = after_shrink
        ? creation_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"起動修復システム領域作成の縮小読戻し",
              L"Windows extentだけがexact寸法で縮小されたことを確認できません")
        : after_shrink.error();
    attempt_rollback(reviewed, platform, false, report);
    return report;
  }
  report.shrunken_layout_verified = true;

  const auto created = platform.create_and_format_system_exact(reviewed);
  if (!created) {
    report.primary_error = created.error();
    // The production adapter guarantees that a format failure first attempts
    // to remove its exact just-created partition.  Passing false here avoids
    // deleting an unrelated object when creation never committed.
    attempt_rollback(reviewed, platform, false, report);
    return report;
  }
  report.system_partition_created_and_formatted = true;
  auto completed = platform.observe_read_only(
      reviewed.selected_identity(), reviewed.windows_partition().number);
  if (!completed || !safe_windows_volume_observation(completed.value()) ||
      !expected_completed_plan(reviewed, completed.value().plan)) {
    report.primary_error = completed
        ? creation_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"起動修復システム領域作成の最終読戻し",
              L"新しいESP/Active領域、filesystem、Volume GUID、または全レイアウトを完全確認できません")
        : completed.error();
    attempt_rollback(reviewed, platform, true, report);
    return report;
  }
  report.completed_plan_verified = true;
  report.completed_plan = completed.take_value().plan;
  report.outcome = SystemPartitionCreationOutcome::committed;
  return report;
}

}  // namespace ytec::bootrepair

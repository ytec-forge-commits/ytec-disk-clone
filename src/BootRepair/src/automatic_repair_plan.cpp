#include "ytec/bootrepair/automatic_repair_plan.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

constexpr std::wstring_view kEfiPartitionType =
    L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
constexpr std::wstring_view kBasicDataPartitionType =
    L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
constexpr std::wstring_view kMbrNtfsPartitionType = L"0x07";

clonecore::Error planning_error(
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

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      std::equal(
          left.begin(),
          left.end(),
          right.begin(),
          [](const wchar_t left_character, const wchar_t right_character) {
            return std::towlower(left_character) ==
                std::towlower(right_character);
          });
}

clonecore::Status validate_partition_layout(
    const diskmodel::DiskInfo& disk) {
  if ((disk.partition_style != diskmodel::PartitionStyle::gpt &&
       disk.partition_style != diskmodel::PartitionStyle::mbr) ||
      disk.size_bytes == 0U || disk.partitions.empty()) {
    return clonecore::Status::failure(planning_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"自動起動修復計画のディスク形式確認",
        L"パーティションを持つGPTまたはMBRディスクだけを診断できます"));
  }

  std::vector<const diskmodel::PartitionInfo*> ordered;
  ordered.reserve(disk.partitions.size());
  std::set<std::uint32_t> numbers;
  for (const auto& partition : disk.partitions) {
    if (partition.number == 0U || partition.size_bytes == 0U ||
        partition.style != disk.partition_style ||
        partition.offset_bytes > disk.size_bytes ||
        partition.size_bytes > disk.size_bytes - partition.offset_bytes ||
        !numbers.insert(partition.number).second) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"自動起動修復計画のパーティション検証",
          L"対象ディスクに未確定、範囲外、形式不一致、または重複した区画があります"));
    }
    ordered.push_back(&partition);
  }
  std::sort(
      ordered.begin(),
      ordered.end(),
      [](const auto* left, const auto* right) {
        if (left->offset_bytes != right->offset_bytes) {
          return left->offset_bytes < right->offset_bytes;
        }
        return left->number < right->number;
      });
  for (std::size_t index = 1U; index < ordered.size(); ++index) {
    const auto* previous = ordered[index - 1U];
    const auto* current = ordered[index];
    if (previous->offset_bytes + previous->size_bytes >
        current->offset_bytes) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"自動起動修復計画のパーティション範囲確認",
          L"対象ディスクに重なったパーティションがあります"));
    }
  }
  return clonecore::success_status();
}

clonecore::Result<diskmodel::DiskInfo> resolve_selected_disk(
    const clonecore::StableDiskIdentity& selected_identity,
    diskmodel::IDiskInventoryProvider& inventory) {
  const clonecore::Status input_status = clonecore::validate_stable_identity(
      selected_identity,
      selected_identity,
      L"起動修復対象");
  if (!input_status) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(
        input_status.error());
  }

  auto report = inventory.enumerate();
  if (!report) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(report.error());
  }
  if (!report.value().issues.empty()) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(planning_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"自動起動修復計画の全ディスク再列挙",
        L"未解決の列挙診断があるため対象ディスクを確定できません"));
  }

  std::vector<const diskmodel::DiskInfo*> matches;
  for (const auto& disk : report.value().disks) {
    const auto observed_identity = diskmodel::make_stable_disk_identity(
        disk, disk.is_system_disk);
    if (!observed_identity) {
      continue;
    }
    if (clonecore::validate_stable_identity(
            selected_identity,
            observed_identity.value(),
            L"起動修復対象")) {
      matches.push_back(&disk);
    }
  }
  if (matches.size() != 1U) {
    return clonecore::Result<diskmodel::DiskInfo>::failure(planning_error(
        clonecore::ErrorCode::identity_mismatch,
        matches.empty() ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        L"自動起動修復計画の安定再識別",
        L"選択済みディスクを一意に再識別できません"));
  }
  return clonecore::Result<diskmodel::DiskInfo>::success(*matches.front());
}

bool same_partition_extent(
    const BootVolumeObservation& volume,
    const diskmodel::DiskInfo& disk,
    const diskmodel::PartitionInfo& partition) {
  return volume.location.disk_number == disk.disk_number &&
      volume.location.starting_offset == partition.offset_bytes &&
      volume.location.extent_length == partition.size_bytes;
}

clonecore::Result<std::vector<BootVolumeObservation>>
validate_volume_observations(
    const diskmodel::DiskInfo& disk,
    std::vector<BootVolumeObservation> observations) {
  std::vector<BootVolumeObservation> validated;
  validated.reserve(observations.size());
  for (auto& observation : observations) {
    if (observation.location.disk_number != disk.disk_number ||
        observation.location.extent_length == 0U ||
        observation.location.file_system.empty()) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(planning_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"自動起動修復計画のボリューム対応確認",
          L"対象外ディスクまたは不完全なボリューム観測が含まれています"));
    }
    const auto partition = std::find_if(
        disk.partitions.begin(),
        disk.partitions.end(),
        [&](const auto& candidate) {
          return same_partition_extent(observation, disk, candidate);
        });
    if (partition == disk.partitions.end()) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(planning_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"自動起動修復計画のボリューム範囲確認",
          L"パーティション範囲と完全一致しないボリューム観測があります"));
    }
    auto normalized =
        normalize_offline_windows_volume_root(observation.volume_name);
    if (!normalized) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(normalized.error());
    }
    observation.volume_name = normalized.take_value();

    const bool duplicate = std::any_of(
        validated.begin(),
        validated.end(),
        [&](const auto& existing) {
          return equals_case_insensitive(
                     existing.volume_name, observation.volume_name) ||
              (existing.location.starting_offset ==
                   observation.location.starting_offset &&
               existing.location.extent_length ==
                   observation.location.extent_length);
        });
    if (duplicate) {
      return clonecore::Result<
          std::vector<BootVolumeObservation>>::failure(planning_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"自動起動修復計画のボリューム一意性確認",
          L"同じVolume GUIDまたは区画範囲が複数回報告されました"));
    }
    validated.push_back(std::move(observation));
  }
  return clonecore::Result<std::vector<BootVolumeObservation>>::success(
      std::move(validated));
}

const BootVolumeObservation* volume_for_partition(
    const diskmodel::DiskInfo& disk,
    const diskmodel::PartitionInfo& partition,
    const std::vector<BootVolumeObservation>& volumes) {
  const auto found = std::find_if(
      volumes.begin(),
      volumes.end(),
      [&](const auto& volume) {
        return same_partition_extent(volume, disk, partition);
      });
  return found == volumes.end() ? nullptr : &*found;
}

bool is_gpt_system_partition(const diskmodel::PartitionInfo& partition) {
  return equals_case_insensitive(partition.type, kEfiPartitionType);
}

bool is_mbr_windows_partition(const diskmodel::PartitionInfo& partition) {
  return equals_case_insensitive(partition.type, kMbrNtfsPartitionType);
}

bool is_windows_partition_candidate(
    const diskmodel::PartitionInfo& partition,
    const diskmodel::PartitionStyle style) {
  return style == diskmodel::PartitionStyle::gpt
      ? equals_case_insensitive(partition.type, kBasicDataPartitionType)
      : is_mbr_windows_partition(partition);
}

clonecore::Status append_system_candidates(
    AutomaticBootRepairPlan& plan,
    const std::vector<const diskmodel::PartitionInfo*>& ordered_partitions,
    const std::vector<BootVolumeObservation>& volumes,
    IEfiBootOwnershipInspector& efi_ownership_inspector) {
  std::vector<const diskmodel::PartitionInfo*> structural_candidates;
  for (const auto* partition : ordered_partitions) {
    const bool matches = plan.partition_style == diskmodel::PartitionStyle::gpt
        ? is_gpt_system_partition(*partition)
        : partition->bootable;
    if (matches) {
      structural_candidates.push_back(partition);
    }
  }
  if (structural_candidates.empty()) {
    plan.system_partition_create_plan_needed = true;
    return clonecore::success_status();
  }

  const std::wstring_view expected_file_system =
      plan.firmware == BcdBootFirmware::uefi ? L"FAT32" : L"NTFS";
  for (const auto* partition : structural_candidates) {
    if (plan.partition_style == diskmodel::PartitionStyle::mbr &&
        !is_mbr_windows_partition(*partition)) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"BIOSシステム領域候補の形式確認",
          L"Active属性を持つ区画がWindows BIOS用のNTFS区画形式ではありません"));
    }
    const auto* volume = volume_for_partition(
        plan.selected_disk, *partition, volumes);
    if (volume == nullptr) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::query_failed,
          ERROR_NOT_FOUND,
          L"システム領域候補のボリューム確認",
          L"既存のESPまたはActive領域を読取り専用で確認できません"));
    }
    if (!equals_case_insensitive(
            volume->location.file_system, expected_file_system)) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"システム領域候補のファイルシステム確認",
          L"既存のシステム領域候補が必要なファイルシステムではありません"));
    }
    EfiBootOwnershipEvidence efi_ownership;
    if (plan.partition_style == diskmodel::PartitionStyle::gpt) {
      auto inspected =
          efi_ownership_inspector.inspect_existing_esp_read_only(
              volume->volume_name);
      if (!inspected) {
        return clonecore::Status::failure(inspected.error());
      }
      efi_ownership = inspected.take_value();
      if (efi_ownership.state == EfiBootOwnershipState::not_applicable) {
        return clonecore::Status::failure(planning_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"ESP EFI所有権診断結果",
            L"GPT/UEFIのESPに適用可能なEFI所有権診断結果がありません"));
      }
    }
    plan.system_partition_candidates.push_back(
        DiscoveredSystemPartition{
            .partition = *partition,
            .volume = *volume,
            .role = plan.required_system_partition_role,
            .efi_ownership = efi_ownership,
        });
  }
  plan.system_partition_selection_policy_needed =
      plan.system_partition_candidates.size() > 1U;
  return clonecore::success_status();
}

clonecore::Status append_windows_installations(
    AutomaticBootRepairPlan& plan,
    const std::vector<const diskmodel::PartitionInfo*>& ordered_partitions,
    const std::vector<BootVolumeObservation>& volumes,
    IOfflineWindowsCandidateValidator& windows_validator,
    IWinReDiagnosticService& winre_inspector) {
  for (const auto* partition : ordered_partitions) {
    if (!is_windows_partition_candidate(*partition, plan.partition_style)) {
      continue;
    }
    const auto* volume = volume_for_partition(
        plan.selected_disk, *partition, volumes);
    if (volume == nullptr || !equals_case_insensitive(
                                 volume->location.file_system, L"NTFS")) {
      continue;
    }

    auto validation =
        windows_validator.inspect_volume_read_only(volume->volume_name);
    if (!validation) {
      return clonecore::Status::failure(validation.error());
    }
    if (validation.value().state == OfflineWindowsCandidateState::absent) {
      continue;
    }

    const bool version_is_supported =
        is_supported_offline_windows_version(validation.value().version);
    const bool reported_supported = validation.value().state ==
        OfflineWindowsCandidateState::present_supported;
    const bool reported_unsupported = validation.value().state ==
        OfflineWindowsCandidateState::present_unsupported;
    if ((!reported_supported && !reported_unsupported) ||
        version_is_supported != reported_supported) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"オフラインWindows候補の検証結果確認",
          L"Windowsの版情報と対応状態が一致しません"));
    }

    const std::wstring windows_directory =
        volume->volume_name + L"Windows";
    auto winre = winre_inspector.inspect(
        windows_directory, plan.selected_disk.disk_number);
    if (!winre) {
      return clonecore::Status::failure(winre.error());
    }
    if (!winre.value().read_only_command ||
        !winre.value().microsoft_signature_verified) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"WinRE読取り専用診断の信頼確認",
          L"署名済みの読取り専用WinRE診断として確認できません"));
    }

    plan.windows_installations.push_back(
        DiscoveredWindowsInstallation{
            .partition = *partition,
            .volume = *volume,
            .windows_directory = windows_directory,
            .version = validation.value().version,
            .officially_supported = reported_supported,
            .winre = AutomaticBootRepairWinReEvidence{
                .source_state = winre.value().source_state,
                .registered_location_reported =
                    winre.value().registered_location_reported,
                .registered_location_matches_selected_disk =
                    winre.value()
                        .registered_location_matches_expected_disk,
                .registered_partition_number =
                    winre.value().registered_partition_number,
                .registered_path_kind_reported =
                    winre.value().registered_path_kind_reported,
                .registered_path_kind =
                    winre.value().registered_path_kind,
                .registered_image_present =
                    winre.value().registered_image_present,
                .fallback_image_present =
                    winre.value().fallback_image_present,
                .image_size_bytes =
                    winre.value().winre_image_size_bytes,
            },
        });
  }
  plan.windows_not_found = plan.windows_installations.empty();
  plan.windows_selection_policy_needed =
      plan.windows_installations.size() > 1U;
  plan.unsupported_windows_policy_needed = std::any_of(
      plan.windows_installations.begin(),
      plan.windows_installations.end(),
      [](const auto& installation) {
        return !installation.officially_supported;
      });
  return clonecore::success_status();
}

bool same_partition(
    const diskmodel::PartitionInfo& left,
    const diskmodel::PartitionInfo& right) {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes && left.style == right.style &&
      equals_case_insensitive(left.type, right.type) &&
      equals_case_insensitive(left.identifier, right.identifier) &&
      left.name == right.name && left.bootable == right.bootable;
}

bool same_volume(
    const BootVolumeObservation& left,
    const BootVolumeObservation& right) {
  // Disk number is a transient routing hint. Stable disk identity and the
  // exact partition extent are compared separately.
  return equals_case_insensitive(left.volume_name, right.volume_name) &&
      left.location.starting_offset == right.location.starting_offset &&
      left.location.extent_length == right.location.extent_length &&
      equals_case_insensitive(
          left.location.file_system, right.location.file_system);
}

bool same_winre(
    const AutomaticBootRepairWinReEvidence& left,
    const AutomaticBootRepairWinReEvidence& right) {
  return left.source_state == right.source_state &&
      left.registered_location_reported ==
          right.registered_location_reported &&
      left.registered_location_matches_selected_disk ==
          right.registered_location_matches_selected_disk &&
      left.registered_partition_number ==
          right.registered_partition_number &&
      left.registered_path_kind_reported ==
          right.registered_path_kind_reported &&
      left.registered_path_kind == right.registered_path_kind &&
      left.registered_image_present == right.registered_image_present &&
      left.fallback_image_present == right.fallback_image_present &&
      left.image_size_bytes == right.image_size_bytes;
}

bool same_windows_installation(
    const DiscoveredWindowsInstallation& left,
    const DiscoveredWindowsInstallation& right) {
  return same_partition(left.partition, right.partition) &&
      same_volume(left.volume, right.volume) &&
      equals_case_insensitive(
          left.windows_directory, right.windows_directory) &&
      left.version.major == right.version.major &&
      left.version.build == right.version.build &&
      equals_case_insensitive(
          left.version.installation_type,
          right.version.installation_type) &&
      left.officially_supported == right.officially_supported &&
      same_winre(left.winre, right.winre);
}

bool same_system_partition(
    const DiscoveredSystemPartition& left,
    const DiscoveredSystemPartition& right) {
  return same_partition(left.partition, right.partition) &&
      same_volume(left.volume, right.volume) && left.role == right.role &&
      equivalent_efi_boot_ownership(
          left.efi_ownership, right.efi_ownership);
}

bool same_disk_layout(
    const diskmodel::DiskInfo& left,
    const diskmodel::DiskInfo& right) {
  if (left.size_bytes != right.size_bytes ||
      left.logical_sector_size != right.logical_sector_size ||
      left.physical_sector_size != right.physical_sector_size ||
      left.partition_style != right.partition_style ||
      !equals_case_insensitive(
          left.disk_identifier, right.disk_identifier) ||
      left.partitions.size() != right.partitions.size()) {
    return false;
  }
  std::vector<const diskmodel::PartitionInfo*> left_ordered;
  std::vector<const diskmodel::PartitionInfo*> right_ordered;
  left_ordered.reserve(left.partitions.size());
  right_ordered.reserve(right.partitions.size());
  for (const auto& partition : left.partitions) {
    left_ordered.push_back(&partition);
  }
  for (const auto& partition : right.partitions) {
    right_ordered.push_back(&partition);
  }
  const auto by_extent = [](const auto* first, const auto* second) {
    if (first->offset_bytes != second->offset_bytes) {
      return first->offset_bytes < second->offset_bytes;
    }
    return first->number < second->number;
  };
  std::sort(left_ordered.begin(), left_ordered.end(), by_extent);
  std::sort(right_ordered.begin(), right_ordered.end(), by_extent);
  for (std::size_t index = 0U; index < left_ordered.size(); ++index) {
    if (!same_partition(*left_ordered[index], *right_ordered[index])) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_discovery_candidate_integrity(
    const AutomaticBootRepairPlan& discovery) {
  const auto observed_identity = diskmodel::make_stable_disk_identity(
      discovery.selected_disk,
      discovery.selected_disk.is_system_disk);
  if (!observed_identity) {
    return clonecore::Status::failure(observed_identity.error());
  }
  const auto identity = clonecore::validate_stable_identity(
      discovery.selected_identity,
      observed_identity.value(),
      L"起動修復診断対象");
  if (!identity) {
    return identity;
  }

  std::set<std::uint32_t> windows_numbers;
  for (const auto& installation : discovery.windows_installations) {
    const auto partition = std::find_if(
        discovery.selected_disk.partitions.begin(),
        discovery.selected_disk.partitions.end(),
        [&](const auto& candidate) {
          return same_partition(candidate, installation.partition);
        });
    auto normalized = normalize_offline_windows_volume_root(
        installation.volume.volume_name);
    if (partition == discovery.selected_disk.partitions.end() ||
        !windows_numbers.insert(installation.partition.number).second ||
        installation.volume.location.disk_number !=
            discovery.selected_disk.disk_number ||
        !same_partition_extent(
            installation.volume,
            discovery.selected_disk,
            installation.partition) ||
        !equals_case_insensitive(
            installation.volume.location.file_system, L"NTFS") ||
        !normalized ||
        !equals_case_insensitive(
            normalized.value(), installation.volume.volume_name) ||
        !equals_case_insensitive(
            installation.windows_directory,
            installation.volume.volume_name + L"Windows") ||
        installation.officially_supported !=
            is_supported_offline_windows_version(installation.version)) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"起動修復Windows候補整合性",
          L"Windows候補が対象ディスク、区画範囲、Volume GUID、版情報と一致しません"));
    }
  }

  std::set<std::uint32_t> system_numbers;
  const std::wstring_view required_file_system =
      discovery.firmware == BcdBootFirmware::uefi ? L"FAT32" : L"NTFS";
  for (const auto& system : discovery.system_partition_candidates) {
    const auto partition = std::find_if(
        discovery.selected_disk.partitions.begin(),
        discovery.selected_disk.partitions.end(),
        [&](const auto& candidate) {
          return same_partition(candidate, system.partition);
        });
    auto normalized = normalize_offline_windows_volume_root(
        system.volume.volume_name);
    const bool ownership_is_coherent =
        discovery.partition_style == diskmodel::PartitionStyle::gpt
        ? system.efi_ownership.state !=
            EfiBootOwnershipState::not_applicable
        : system.efi_ownership.state ==
            EfiBootOwnershipState::not_applicable;
    if (partition == discovery.selected_disk.partitions.end() ||
        !system_numbers.insert(system.partition.number).second ||
        system.role != discovery.required_system_partition_role ||
        system.volume.location.disk_number !=
            discovery.selected_disk.disk_number ||
        !same_partition_extent(
            system.volume, discovery.selected_disk, system.partition) ||
        !equals_case_insensitive(
            system.volume.location.file_system, required_file_system) ||
        !normalized ||
        !equals_case_insensitive(
            normalized.value(), system.volume.volume_name) ||
        !ownership_is_coherent) {
      return clonecore::Status::failure(planning_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"起動修復システム領域候補整合性",
          L"ESPまたはActive領域候補が対象ディスク、区画範囲、役割、所有権と一致しません"));
    }
  }
  return clonecore::success_status();
}

bool equivalent_discovery(
    const AutomaticBootRepairPlan& reviewed,
    const AutomaticBootRepairPlan& fresh) {
  if (!clonecore::validate_stable_identity(
          reviewed.selected_identity,
          fresh.selected_identity,
          L"起動修復レビュー再識別") ||
      !same_disk_layout(reviewed.selected_disk, fresh.selected_disk) ||
      reviewed.partition_style != fresh.partition_style ||
      reviewed.firmware != fresh.firmware ||
      reviewed.required_system_partition_role !=
          fresh.required_system_partition_role ||
      reviewed.planned_bcd_store_policy != fresh.planned_bcd_store_policy ||
      reviewed.windows_not_found != fresh.windows_not_found ||
      reviewed.windows_selection_policy_needed !=
          fresh.windows_selection_policy_needed ||
      reviewed.unsupported_windows_policy_needed !=
          fresh.unsupported_windows_policy_needed ||
      reviewed.system_partition_create_plan_needed !=
          fresh.system_partition_create_plan_needed ||
      reviewed.system_partition_selection_policy_needed !=
          fresh.system_partition_selection_policy_needed ||
      reviewed.windows_installations.size() !=
          fresh.windows_installations.size() ||
      reviewed.system_partition_candidates.size() !=
          fresh.system_partition_candidates.size()) {
    return false;
  }
  for (std::size_t index = 0U;
       index < reviewed.windows_installations.size(); ++index) {
    if (!same_windows_installation(
            reviewed.windows_installations[index],
            fresh.windows_installations[index])) {
      return false;
    }
  }
  for (std::size_t index = 0U;
       index < reviewed.system_partition_candidates.size(); ++index) {
    if (!same_system_partition(
            reviewed.system_partition_candidates[index],
            fresh.system_partition_candidates[index])) {
      return false;
    }
  }
  return true;
}

clonecore::Result<std::vector<DiscoveredWindowsInstallation>>
select_windows_installations(
    const AutomaticBootRepairPlan& discovery,
    const AutomaticBootRepairChoiceRequest& request) {
  if (request.windows_policy !=
          AutomaticWindowsRegistrationPolicy::selected_only &&
      request.windows_policy !=
          AutomaticWindowsRegistrationPolicy::all_with_explicit_priority) {
    return clonecore::Result<
        std::vector<DiscoveredWindowsInstallation>>::failure(
        planning_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"起動修復Windows登録方針",
            L"Windows登録方針が不正です"));
  }
  const std::size_t required_count =
      request.windows_policy ==
              AutomaticWindowsRegistrationPolicy::selected_only
          ? 1U
          : discovery.windows_installations.size();
  if (request.windows_partition_priority.size() != required_count ||
      required_count == 0U) {
    return clonecore::Result<
        std::vector<DiscoveredWindowsInstallation>>::failure(
        planning_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_INVALID_PARAMETER,
            L"起動修復Windows登録方針",
            request.windows_policy ==
                    AutomaticWindowsRegistrationPolicy::selected_only
                ? L"登録するWindowsを1件だけ明示してください"
                : L"検出した全Windowsを重複なく起動優先順に並べてください"));
  }

  std::set<std::uint32_t> selected_numbers;
  std::vector<DiscoveredWindowsInstallation> selected;
  selected.reserve(required_count);
  for (const auto partition_number :
       request.windows_partition_priority) {
    if (partition_number == 0U ||
        !selected_numbers.insert(partition_number).second) {
      return clonecore::Result<
          std::vector<DiscoveredWindowsInstallation>>::failure(
          planning_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_DUP_NAME,
              L"起動修復Windows優先順位",
              L"Windows区画番号が未指定または重複しています"));
    }
    const auto found = std::find_if(
        discovery.windows_installations.begin(),
        discovery.windows_installations.end(),
        [&](const auto& candidate) {
          return candidate.partition.number == partition_number;
        });
    if (found == discovery.windows_installations.end()) {
      return clonecore::Result<
          std::vector<DiscoveredWindowsInstallation>>::failure(
          planning_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_NOT_FOUND,
              L"起動修復Windows選択",
              L"選択したWindows区画は読取り専用診断の候補にありません"));
    }
    if (!found->officially_supported) {
      return clonecore::Result<
          std::vector<DiscoveredWindowsInstallation>>::failure(
          planning_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"起動修復Windows対応版",
              L"選択したWindowsは製品が対応するWindows 10/11 x64ではありません"));
    }
    selected.push_back(*found);
  }
  return clonecore::Result<
      std::vector<DiscoveredWindowsInstallation>>::success(
      std::move(selected));
}

clonecore::Result<std::vector<AutomaticWinReRepairChoice>>
derive_winre_choices(
    const std::vector<DiscoveredWindowsInstallation>& windows) {
  std::vector<AutomaticWinReRepairChoice> choices;
  choices.reserve(windows.size());
  for (const auto& installation : windows) {
    const auto& evidence = installation.winre;
    const bool has_registered_location =
        evidence.registered_location_reported &&
        evidence.registered_location_matches_selected_disk &&
        evidence.registered_path_kind_reported &&
        evidence.registered_partition_number != 0U;
    const bool has_registered_image =
        evidence.registered_image_present &&
        evidence.image_size_bytes != 0U;
    const bool has_fallback_image =
        evidence.fallback_image_present &&
        evidence.image_size_bytes != 0U;

    AutomaticWinReRepairDisposition disposition{};
    switch (evidence.source_state) {
      case WinReSourceState::registered_partition:
        if (!has_registered_location || !has_registered_image ||
            evidence.fallback_image_present) {
          return clonecore::Result<
              std::vector<AutomaticWinReRepairChoice>>::failure(
              planning_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_INVALID_DATA,
                  L"起動修復WinRE登録診断",
                  L"登録済みWinREの区画、対象ディスク、Winre.wimが一致しません"));
        }
        disposition = AutomaticWinReRepairDisposition::
            verify_existing_registration;
        break;
      case WinReSourceState::image_available_in_windows:
        if (!has_fallback_image || evidence.registered_image_present) {
          return clonecore::Result<
              std::vector<AutomaticWinReRepairChoice>>::failure(
              planning_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_INVALID_DATA,
                  L"起動修復WinRE再登録候補",
                  L"Windows内のWinre.wim診断結果が整合していません"));
        }
        disposition = AutomaticWinReRepairDisposition::
            register_verified_windows_image;
        break;
      case WinReSourceState::missing:
        if (evidence.registered_location_reported ||
            evidence.registered_location_matches_selected_disk ||
            evidence.registered_partition_number != 0U ||
            evidence.registered_path_kind_reported ||
            evidence.registered_image_present ||
            evidence.fallback_image_present ||
            evidence.image_size_bytes != 0U) {
          return clonecore::Result<
              std::vector<AutomaticWinReRepairChoice>>::failure(
              planning_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_INVALID_DATA,
                  L"起動修復WinRE欠損診断",
                  L"WinRE欠損と画像または登録先の診断結果が同時に報告されました"));
        }
        disposition = AutomaticWinReRepairDisposition::
            normal_boot_only_partial;
        break;
      case WinReSourceState::unknown:
        if (evidence.registered_image_present ||
            evidence.fallback_image_present ||
            evidence.image_size_bytes != 0U ||
            !((!evidence.registered_location_reported &&
               !evidence.registered_location_matches_selected_disk &&
               evidence.registered_partition_number == 0U &&
               !evidence.registered_path_kind_reported) ||
              (evidence.registered_location_reported &&
               evidence.registered_location_matches_selected_disk &&
               evidence.registered_partition_number != 0U &&
               evidence.registered_path_kind_reported))) {
          return clonecore::Result<
              std::vector<AutomaticWinReRepairChoice>>::failure(
              planning_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_INVALID_DATA,
                  L"起動修復WinRE不明診断",
                  L"Winre.wimを特定できない診断の登録先情報が整合しません"));
        }
        disposition = AutomaticWinReRepairDisposition::
            normal_boot_only_partial;
        break;
      default:
        return clonecore::Result<
            std::vector<AutomaticWinReRepairChoice>>::failure(
            planning_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"起動修復WinRE方針",
                L"未知のWinRE診断状態は実行計画にできません"));
    }
    choices.push_back(AutomaticWinReRepairChoice{
        .windows_partition_number = installation.partition.number,
        .disposition = disposition,
    });
  }
  return clonecore::Result<
      std::vector<AutomaticWinReRepairChoice>>::success(
      std::move(choices));
}

clonecore::Result<DiscoveredSystemPartition> select_system_partition(
    const AutomaticBootRepairPlan& discovery,
    const AutomaticBootRepairChoiceRequest& request) {
  if (discovery.system_partition_create_plan_needed ||
      discovery.system_partition_candidates.empty()) {
    return clonecore::Result<DiscoveredSystemPartition>::failure(
        planning_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_NOT_FOUND,
            L"起動修復システム領域選択",
            L"既存システム領域がないため、新規作成計画を先に確認する必要があります"));
  }
  const auto found = std::find_if(
      discovery.system_partition_candidates.begin(),
      discovery.system_partition_candidates.end(),
      [&](const auto& candidate) {
        return candidate.partition.number ==
            request.system_partition_number;
      });
  if (request.system_partition_number == 0U ||
      found == discovery.system_partition_candidates.end()) {
    return clonecore::Result<DiscoveredSystemPartition>::failure(
        planning_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_NOT_FOUND,
            L"起動修復システム領域選択",
            L"使用するESPまたはActive領域を診断候補から明示してください"));
  }
  if (discovery.partition_style == diskmodel::PartitionStyle::mbr) {
    if (request.third_party_efi_policy !=
        AutomaticThirdPartyEfiPolicy::not_applicable) {
      return clonecore::Result<DiscoveredSystemPartition>::failure(
          planning_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"起動修復第三者EFI方針",
              L"BIOS/MBR修復では第三者EFIの保持／削除方針を指定できません"));
    }
    return clonecore::Result<DiscoveredSystemPartition>::success(*found);
  }

  switch (found->efi_ownership.state) {
    case EfiBootOwnershipState::microsoft_only_or_empty:
      if (!efi_boot_ownership_allows_microsoft_rebuild(
              found->efi_ownership) ||
          request.third_party_efi_policy !=
              AutomaticThirdPartyEfiPolicy::not_applicable) {
        return clonecore::Result<DiscoveredSystemPartition>::failure(
            planning_error(
                clonecore::ErrorCode::invalid_argument,
                ERROR_INVALID_DATA,
                L"起動修復第三者EFI方針",
                L"第三者EFIがないESPでは保持／削除方針を指定できません"));
      }
      break;
    case EfiBootOwnershipState::non_microsoft_or_untrusted_present:
      if (found->efi_ownership.non_microsoft_or_untrusted_entry_count == 0U) {
        return clonecore::Result<DiscoveredSystemPartition>::failure(
            planning_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"起動修復第三者EFI診断",
                L"第三者EFIの分類と検出件数が一致しません"));
      }
      if (request.third_party_efi_policy ==
          AutomaticThirdPartyEfiPolicy::not_applicable) {
        return clonecore::Result<DiscoveredSystemPartition>::failure(
            planning_error(
                clonecore::ErrorCode::confirmation_required,
                ERROR_NOT_SUPPORTED,
                L"起動修復第三者EFI方針",
              L"検出した第三者EFIを保持するか削除するか明示してください"));
      }
      if (request.third_party_efi_policy ==
              AutomaticThirdPartyEfiPolicy::preserve &&
          !efi_boot_ownership_allows_third_party_preserve(
              found->efi_ownership)) {
        return clonecore::Result<DiscoveredSystemPartition>::failure(
            planning_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"起動修復第三者EFI保持境界",
                L"独立した通常ディレクトリの第三者EFI namespaceだけを保持対象にできます"));
      }
      break;
    case EfiBootOwnershipState::ambiguous:
      return clonecore::Result<DiscoveredSystemPartition>::failure(
          planning_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"起動修復第三者EFI診断",
              L"EFI内容を安全に分類できないため保持／削除のどちらも実行できません"));
    case EfiBootOwnershipState::not_applicable:
    default:
      return clonecore::Result<DiscoveredSystemPartition>::failure(
          planning_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"起動修復第三者EFI診断",
              L"UEFI修復に適用できるEFI所有権診断がありません"));
  }

  if (request.third_party_efi_policy !=
          AutomaticThirdPartyEfiPolicy::not_applicable &&
      request.third_party_efi_policy !=
          AutomaticThirdPartyEfiPolicy::preserve &&
      request.third_party_efi_policy !=
          AutomaticThirdPartyEfiPolicy::delete_non_microsoft) {
    return clonecore::Result<DiscoveredSystemPartition>::failure(
        planning_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"起動修復第三者EFI方針",
            L"第三者EFIの保持／削除方針が不正です"));
  }
  return clonecore::Result<DiscoveredSystemPartition>::success(*found);
}

}  // namespace

AutomaticBootRepairPlanner::AutomaticBootRepairPlanner(
    diskmodel::IDiskInventoryProvider& inventory,
    IBootRepairVolumeObservationProvider& volumes,
    IOfflineWindowsCandidateValidator& windows_validator,
    IWinReDiagnosticService& winre_inspector,
    IEfiBootOwnershipInspector& efi_ownership_inspector) noexcept
    : inventory_(inventory),
      volumes_(volumes),
      windows_validator_(windows_validator),
      winre_inspector_(winre_inspector),
      efi_ownership_inspector_(efi_ownership_inspector) {}

clonecore::Result<AutomaticBootRepairPlan>
AutomaticBootRepairPlanner::plan(
    const diskmodel::DiskInfo& selected_disk) {
  const auto identity = diskmodel::make_stable_disk_identity(
      selected_disk, selected_disk.is_system_disk);
  if (!identity) {
    return clonecore::Result<AutomaticBootRepairPlan>::failure(
        identity.error());
  }
  return plan(identity.value());
}

clonecore::Result<AutomaticBootRepairPlan>
AutomaticBootRepairPlanner::plan(
    const clonecore::StableDiskIdentity& selected_identity) {
  auto selected_disk = resolve_selected_disk(selected_identity, inventory_);
  if (!selected_disk) {
    return clonecore::Result<AutomaticBootRepairPlan>::failure(
        selected_disk.error());
  }
  const clonecore::Status layout =
      validate_partition_layout(selected_disk.value());
  if (!layout) {
    return clonecore::Result<AutomaticBootRepairPlan>::failure(layout.error());
  }

  auto observed_volumes = volumes_.observe_read_only(selected_disk.value());
  if (!observed_volumes) {
    return clonecore::Result<AutomaticBootRepairPlan>::failure(
        observed_volumes.error());
  }
  auto volumes = validate_volume_observations(
      selected_disk.value(), observed_volumes.take_value());
  if (!volumes) {
    return clonecore::Result<AutomaticBootRepairPlan>::failure(
        volumes.error());
  }

  const auto observed_identity = diskmodel::make_stable_disk_identity(
      selected_disk.value(), selected_disk.value().is_system_disk);
  if (!observed_identity) {
    return clonecore::Result<AutomaticBootRepairPlan>::failure(
        observed_identity.error());
  }

  AutomaticBootRepairPlan result{
      .selected_disk = selected_disk.take_value(),
      .selected_identity = observed_identity.value(),
  };
  result.partition_style = result.selected_disk.partition_style;
  if (result.partition_style == diskmodel::PartitionStyle::gpt) {
    result.firmware = BcdBootFirmware::uefi;
    result.required_system_partition_role =
        BootSystemPartitionRole::efi_system;
  } else {
    result.firmware = BcdBootFirmware::bios;
    result.required_system_partition_role =
        BootSystemPartitionRole::bios_active;
  }
  result.planned_bcd_store_policy = BcdBootStorePolicy::rebuild_fresh;

  std::vector<const diskmodel::PartitionInfo*> ordered_partitions;
  ordered_partitions.reserve(result.selected_disk.partitions.size());
  for (const auto& partition : result.selected_disk.partitions) {
    ordered_partitions.push_back(&partition);
  }
  std::sort(
      ordered_partitions.begin(),
      ordered_partitions.end(),
      [](const auto* left, const auto* right) {
        if (left->offset_bytes != right->offset_bytes) {
          return left->offset_bytes < right->offset_bytes;
        }
        return left->number < right->number;
      });

  const clonecore::Status system_status = append_system_candidates(
      result,
      ordered_partitions,
      volumes.value(),
      efi_ownership_inspector_);
  if (!system_status) {
    return clonecore::Result<AutomaticBootRepairPlan>::failure(
        system_status.error());
  }
  const clonecore::Status windows_status = append_windows_installations(
      result,
      ordered_partitions,
      volumes.value(),
      windows_validator_,
      winre_inspector_);
  if (!windows_status) {
    return clonecore::Result<AutomaticBootRepairPlan>::failure(
        windows_status.error());
  }
  return clonecore::Result<AutomaticBootRepairPlan>::success(
      std::move(result));
}

ReviewedAutomaticBootRepairChoices::ReviewedAutomaticBootRepairChoices(
    AutomaticBootRepairPlan discovery,
    AutomaticBootRepairChoiceRequest request,
    std::vector<DiscoveredWindowsInstallation> windows,
    std::vector<AutomaticWinReRepairChoice> winre_choices,
    DiscoveredSystemPartition system_partition)
    : discovery_(std::move(discovery)),
      request_(std::move(request)),
      windows_(std::move(windows)),
      winre_choices_(std::move(winre_choices)),
      system_partition_(std::move(system_partition)) {}

const clonecore::StableDiskIdentity&
ReviewedAutomaticBootRepairChoices::selected_identity() const noexcept {
  return discovery_.selected_identity;
}

diskmodel::PartitionStyle
ReviewedAutomaticBootRepairChoices::partition_style() const noexcept {
  return discovery_.partition_style;
}

BcdBootFirmware ReviewedAutomaticBootRepairChoices::firmware()
    const noexcept {
  return discovery_.firmware;
}

BcdBootStorePolicy ReviewedAutomaticBootRepairChoices::bcd_store_policy()
    const noexcept {
  return discovery_.planned_bcd_store_policy;
}

AutomaticWindowsRegistrationPolicy
ReviewedAutomaticBootRepairChoices::windows_policy() const noexcept {
  return request_.windows_policy;
}

AutomaticThirdPartyEfiPolicy
ReviewedAutomaticBootRepairChoices::third_party_efi_policy()
    const noexcept {
  return request_.third_party_efi_policy;
}

AutomaticNvramRepairPolicy
ReviewedAutomaticBootRepairChoices::nvram_policy() const noexcept {
  return request_.nvram_policy;
}

std::span<const DiscoveredWindowsInstallation>
ReviewedAutomaticBootRepairChoices::windows_in_boot_priority()
    const noexcept {
  return windows_;
}

std::span<const AutomaticWinReRepairChoice>
ReviewedAutomaticBootRepairChoices::winre_choices_in_boot_priority()
    const noexcept {
  return winre_choices_;
}

const DiscoveredSystemPartition&
ReviewedAutomaticBootRepairChoices::system_partition() const noexcept {
  return system_partition_;
}

clonecore::Result<ReviewedAutomaticBootRepairChoices>
review_automatic_boot_repair_choices(
    const AutomaticBootRepairPlan& discovery,
    const AutomaticBootRepairChoiceRequest& request) {
  const bool coherent_windows_flags =
      discovery.windows_not_found ==
          discovery.windows_installations.empty() &&
      discovery.windows_selection_policy_needed ==
          (discovery.windows_installations.size() > 1U) &&
      discovery.unsupported_windows_policy_needed ==
          std::any_of(
              discovery.windows_installations.begin(),
              discovery.windows_installations.end(),
              [](const auto& candidate) {
                return !candidate.officially_supported;
              });
  const bool coherent_system_flags =
      discovery.system_partition_create_plan_needed ==
          discovery.system_partition_candidates.empty() &&
      discovery.system_partition_selection_policy_needed ==
          (discovery.system_partition_candidates.size() > 1U);
  const bool coherent_firmware =
      (discovery.partition_style == diskmodel::PartitionStyle::gpt &&
       discovery.firmware == BcdBootFirmware::uefi &&
       discovery.required_system_partition_role ==
           BootSystemPartitionRole::efi_system) ||
      (discovery.partition_style == diskmodel::PartitionStyle::mbr &&
       discovery.firmware == BcdBootFirmware::bios &&
       discovery.required_system_partition_role ==
           BootSystemPartitionRole::bios_active);
  if (!coherent_windows_flags || !coherent_system_flags ||
      !coherent_firmware ||
      discovery.planned_bcd_store_policy !=
          BcdBootStorePolicy::rebuild_fresh) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        planning_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"起動修復読取り専用診断整合性",
            L"診断候補、選択要求フラグ、起動方式、またはBCD方針が一致しません"));
  }

  const auto identity = clonecore::validate_stable_identity(
      discovery.selected_identity,
      discovery.selected_identity,
      L"起動修復レビュー対象");
  if (!identity) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        identity.error());
  }
  const auto layout = validate_partition_layout(discovery.selected_disk);
  if (!layout) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        layout.error());
  }
  const auto candidates = validate_discovery_candidate_integrity(discovery);
  if (!candidates) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        candidates.error());
  }
  const auto complete_winre_integrity =
      derive_winre_choices(discovery.windows_installations);
  if (!complete_winre_integrity) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        complete_winre_integrity.error());
  }

  auto windows = select_windows_installations(discovery, request);
  if (!windows) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        windows.error());
  }
  auto winre_choices = derive_winre_choices(windows.value());
  if (!winre_choices) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        winre_choices.error());
  }
  auto system = select_system_partition(discovery, request);
  if (!system) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        system.error());
  }
  if (request.nvram_policy ==
          AutomaticNvramRepairPolicy::repair_current_pc_windows_boot_manager &&
      discovery.firmware != BcdBootFirmware::uefi) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        planning_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_NOT_SUPPORTED,
            L"起動修復NVRAM方針",
            L"現在PCのWindows Boot Manager登録修復はUEFI対象だけで選択できます"));
  }
  if (request.nvram_policy !=
          AutomaticNvramRepairPolicy::leave_unchanged &&
      request.nvram_policy !=
          AutomaticNvramRepairPolicy::repair_current_pc_windows_boot_manager) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        planning_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"起動修復NVRAM方針",
            L"NVRAM変更方針が不正です"));
  }
  return clonecore::Result<ReviewedAutomaticBootRepairChoices>::success(
      ReviewedAutomaticBootRepairChoices(
          discovery,
          request,
          windows.take_value(),
          winre_choices.take_value(),
          system.take_value()));
}

clonecore::Result<ReviewedAutomaticBootRepairChoices>
revalidate_automatic_boot_repair_choices(
    const ReviewedAutomaticBootRepairChoices& reviewed,
    const AutomaticBootRepairPlan& fresh_discovery) {
  if (!equivalent_discovery(reviewed.discovery_, fresh_discovery)) {
    return clonecore::Result<ReviewedAutomaticBootRepairChoices>::failure(
        planning_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_REVISION_MISMATCH,
            L"起動修復レビュー直前再診断",
            L"Windows、システム領域、レイアウト、WinRE、またはEFI所有権がレビュー時から変わりました"));
  }
  return review_automatic_boot_repair_choices(
      fresh_discovery, reviewed.request_);
}

}  // namespace ytec::bootrepair

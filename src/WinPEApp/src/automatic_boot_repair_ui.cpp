#include "ytec/winpeapp/automatic_boot_repair_ui.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace ytec::winpeapp {
namespace {

clonecore::Error review_error(
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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(review_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool is_drive_root(const std::wstring_view value) {
  return value.size() == 3U && std::iswalpha(value[0]) != 0 &&
      value[1] == L':' && value[2] == L'\\';
}

bool same_text(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
      _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

bool same_identity(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) {
  return left.disk_number == right.disk_number &&
      same_text(left.model, right.model) &&
      left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.serial_suffix == right.serial_suffix &&
      same_text(left.device_instance_id, right.device_instance_id) &&
      left.is_system_disk == right.is_system_disk;
}

bool same_partition(
    const diskmodel::PartitionInfo& left,
    const diskmodel::PartitionInfo& right) {
  return left.number == right.number &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes && left.style == right.style &&
      same_text(left.type, right.type) &&
      same_text(left.identifier, right.identifier) &&
      same_text(left.name, right.name) && left.bootable == right.bootable;
}

bool same_mount_points(
    const std::vector<std::wstring>& left,
    const std::vector<std::wstring>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  return std::all_of(
      left.begin(), left.end(), [&](const std::wstring& value) {
        return std::count_if(
                   left.begin(),
                   left.end(),
                   [&](const std::wstring& candidate) {
                     return same_text(value, candidate);
                   }) ==
            std::count_if(
                right.begin(),
                right.end(),
                [&](const std::wstring& candidate) {
                  return same_text(value, candidate);
                });
      });
}

bool same_volume(
    const bootrepair::BootVolumeObservation& left,
    const bootrepair::BootVolumeObservation& right) {
  return same_text(left.volume_name, right.volume_name) &&
      left.location.disk_number == right.location.disk_number &&
      left.location.starting_offset == right.location.starting_offset &&
      left.location.extent_length == right.location.extent_length &&
      same_text(left.location.file_system, right.location.file_system) &&
      same_mount_points(left.mount_points, right.mount_points);
}

bool same_disk(
    const diskmodel::DiskInfo& left,
    const diskmodel::DiskInfo& right) {
  if (left.disk_number != right.disk_number ||
      !same_text(left.device_path, right.device_path) ||
      !same_text(left.device_instance_id, right.device_instance_id) ||
      !same_text(left.model, right.model) ||
      left.size_bytes != right.size_bytes ||
      left.sector_count != right.sector_count ||
      left.logical_sector_size != right.logical_sector_size ||
      left.physical_sector_size != right.physical_sector_size ||
      !same_text(left.bus_type, right.bus_type) ||
      left.serial_suffix != right.serial_suffix ||
      left.partition_style != right.partition_style ||
      left.offline != right.offline || left.read_only != right.read_only ||
      left.removable != right.removable ||
      left.is_system_disk != right.is_system_disk ||
      left.partitions.size() != right.partitions.size()) {
    return false;
  }
  return std::equal(
      left.partitions.begin(),
      left.partitions.end(),
      right.partitions.begin(),
      same_partition);
}

clonecore::Result<std::wstring> one_drive_root(
    const std::vector<std::wstring>& mount_points,
    const std::wstring_view operation) {
  if (mount_points.size() != 1U || !is_drive_root(mount_points.front())) {
    return failure<std::wstring>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        std::wstring(operation),
        L"既存ドライブ文字を一つだけ安全に特定できません。"
        L"フォルダーマウント、未割当、または複数割当を自動選択しません");
  }
  std::wstring root = mount_points.front();
  root[0] = static_cast<wchar_t>(std::towupper(root[0]));
  return clonecore::Result<std::wstring>::success(std::move(root));
}

bool same_request(
    const bootrepair::BootRepairTargetRequest& left,
    const bootrepair::BootRepairTargetRequest& right) {
  return left.disk_number == right.disk_number &&
      same_text(left.windows_root, right.windows_root) &&
      same_text(left.system_root, right.system_root) &&
      left.firmware == right.firmware &&
      left.store_policy == right.store_policy &&
      left.auto_mount_system_partition ==
          right.auto_mount_system_partition &&
      same_text(
          left.system_volume_identity_root,
          right.system_volume_identity_root) &&
      left.require_efi_ownership_recheck ==
          right.require_efi_ownership_recheck &&
      bootrepair::equivalent_efi_boot_ownership(
          left.expected_efi_ownership,
          right.expected_efi_ownership) &&
      left.third_party_efi_policy == right.third_party_efi_policy &&
      left.reviewed_multi_windows_batch ==
          right.reviewed_multi_windows_batch &&
      left.update_current_pc_nvram == right.update_current_pc_nvram;
}

std::wstring fallback_winre_directory(
    const std::wstring_view offline_windows_directory) {
  return std::wstring(offline_windows_directory) +
      L"\\System32\\Recovery";
}

std::wstring winre_image_path(const std::wstring_view directory) {
  return std::wstring(directory) + L"\\Winre.wim";
}

bool valid_winre_image_identity_shape(
    const bootrepair::WinReRegistrationImageIdentity& identity) {
  constexpr std::uint64_t kMaximumReviewedWinReImageBytes =
      8ULL * 1024ULL * 1024ULL * 1024ULL;
  const bool file_id_present = std::any_of(
      identity.file_id.begin(), identity.file_id.end(),
      [](const std::byte value) { return value != std::byte{}; });
  const bool hash_present = std::any_of(
      identity.sha256.begin(), identity.sha256.end(),
      [](const std::byte value) { return value != std::byte{}; });
  return !identity.requested_path.empty() &&
      !identity.opened_final_path.empty() &&
      identity.volume_serial_number != 0U && file_id_present &&
      identity.length != 0U &&
      identity.length <= kMaximumReviewedWinReImageBytes && hash_present;
}

bootrepair::WinReDiagnosticReport rebuild_prior_winre_diagnostic(
    const bootrepair::DiscoveredWindowsInstallation& windows,
    const std::wstring& offline_windows_directory) {
  const auto& evidence = windows.winre;
  bootrepair::WinReDiagnosticReport report{
      .exit_code = 0U,
      .source_state = evidence.source_state,
      .registered_partition_number =
          evidence.registered_partition_number,
      .registered_path_kind = evidence.registered_path_kind,
      .winre_image_size_bytes = evidence.image_size_bytes,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .registered_location_reported =
          evidence.registered_location_reported,
      .registered_path_kind_reported =
          evidence.registered_path_kind_reported,
      .registered_location_matches_expected_disk =
          evidence.registered_location_matches_selected_disk,
      .registered_image_present = evidence.registered_image_present,
      .fallback_image_present = evidence.fallback_image_present,
  };
  if (evidence.source_state ==
      bootrepair::WinReSourceState::image_available_in_windows) {
    report.inspected_image_path = winre_image_path(
        fallback_winre_directory(offline_windows_directory));
  }
  return report;
}

bool same_winre_action_semantics(
    const WinPeReviewedAutomaticBootRepairExecution::WinReAction& left,
    const WinPeReviewedAutomaticBootRepairExecution::WinReAction& right) {
  const auto& left_prior = left.prior_diagnostic;
  const auto& right_prior = right.prior_diagnostic;
  return left.windows_partition_number == right.windows_partition_number &&
      left.disposition == right.disposition &&
      same_text(left.offline_windows_directory,
                right.offline_windows_directory) &&
      same_text(left.candidate_directory, right.candidate_directory) &&
      left.expected_target_partition_number ==
          right.expected_target_partition_number &&
      left.expected_registered_path_kind ==
          right.expected_registered_path_kind &&
      left_prior.exit_code == right_prior.exit_code &&
      left_prior.source_state == right_prior.source_state &&
      left_prior.registered_partition_number ==
          right_prior.registered_partition_number &&
      left_prior.registered_path_kind ==
          right_prior.registered_path_kind &&
      left_prior.winre_image_size_bytes ==
          right_prior.winre_image_size_bytes &&
      left_prior.microsoft_signature_verified ==
          right_prior.microsoft_signature_verified &&
      left_prior.read_only_command == right_prior.read_only_command &&
      left_prior.registered_location_reported ==
          right_prior.registered_location_reported &&
      left_prior.registered_path_kind_reported ==
          right_prior.registered_path_kind_reported &&
      left_prior.registered_location_matches_expected_disk ==
          right_prior.registered_location_matches_expected_disk &&
      left_prior.registered_location_mismatch_classified_as_cloned_source_stale ==
          right_prior.registered_location_mismatch_classified_as_cloned_source_stale &&
      left_prior.registered_image_present ==
          right_prior.registered_image_present &&
      left_prior.fallback_image_present ==
          right_prior.fallback_image_present &&
      same_text(left_prior.inspected_image_path,
                right_prior.inspected_image_path);
}

bool same_execution_review(
    const WinPeReviewedAutomaticBootRepairExecution& left,
    const WinPeReviewedAutomaticBootRepairExecution& right) {
  return left.windows_partition_numbers_in_boot_priority ==
          right.windows_partition_numbers_in_boot_priority &&
      left.system_partition_number == right.system_partition_number &&
      left.temporary_system_mount_required ==
          right.temporary_system_mount_required &&
      left.third_party_efi_preserved ==
          right.third_party_efi_preserved &&
      left.third_party_efi_delete_requested ==
          right.third_party_efi_delete_requested &&
      left.repair_current_pc_nvram == right.repair_current_pc_nvram &&
      left.normal_boot_only_partial ==
          right.normal_boot_only_partial &&
      left.winre_actions_in_boot_priority.size() ==
          right.winre_actions_in_boot_priority.size() &&
      std::equal(
          left.winre_actions_in_boot_priority.begin(),
          left.winre_actions_in_boot_priority.end(),
          right.winre_actions_in_boot_priority.begin(),
          [](const auto& left_action, const auto& right_action) {
            if (!same_winre_action_semantics(
                    left_action, right_action) ||
                left_action.reviewed_candidate.has_value() !=
                    right_action.reviewed_candidate.has_value()) {
              return false;
            }
            return !left_action.reviewed_candidate.has_value() ||
                bootrepair::equivalent_winre_registration_image_identity(
                    *left_action.reviewed_candidate,
                    *right_action.reviewed_candidate);
          }) &&
      left.requests_in_boot_priority.size() ==
          right.requests_in_boot_priority.size() &&
      std::equal(
          left.requests_in_boot_priority.begin(),
          left.requests_in_boot_priority.end(),
          right.requests_in_boot_priority.begin(),
          same_request);
}

}  // namespace

clonecore::Status validate_automatic_boot_repair_inspection(
    const bootrepair::AutomaticBootRepairPlan& plan,
    const WinPeAutomaticBootRepairReview& review,
    const bootrepair::BootRepairTargetSelection& inspected) {
  if (plan.windows_installations.size() != 1U ||
      plan.system_partition_candidates.size() != 1U) {
    return clonecore::Status::failure(review_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"自動起動修復の初回照合",
        L"実行可能な単一Windows／システム領域計画ではありません"));
  }
  const auto& windows = plan.windows_installations.front().partition;
  const auto& system =
      plan.system_partition_candidates.front().partition;
  const auto rebuilt =
      build_executable_automatic_boot_repair_review(plan);
  if (!rebuilt) {
    return clonecore::Status::failure(rebuilt.error());
  }
  const auto& expected_review = rebuilt.value();
  const bool review_mapping_matches =
      review.request.disk_number == expected_review.request.disk_number &&
      same_text(
          review.request.windows_root,
          expected_review.request.windows_root) &&
      same_text(
          review.request.system_root,
          expected_review.request.system_root) &&
      review.request.firmware == expected_review.request.firmware &&
      review.request.store_policy == expected_review.request.store_policy &&
      review.request.auto_mount_system_partition ==
          expected_review.request.auto_mount_system_partition &&
      same_text(
          review.request.system_volume_identity_root,
          expected_review.request.system_volume_identity_root) &&
      review.request.require_efi_ownership_recheck ==
          expected_review.request.require_efi_ownership_recheck &&
      bootrepair::equivalent_efi_boot_ownership(
          review.request.expected_efi_ownership,
          expected_review.request.expected_efi_ownership) &&
      review.request.update_current_pc_nvram ==
          expected_review.request.update_current_pc_nvram &&
      review.windows_partition_number ==
          expected_review.windows_partition_number &&
      review.system_partition_number ==
          expected_review.system_partition_number &&
      review.temporary_system_mount_required ==
          expected_review.temporary_system_mount_required;
  const bool inspection_matches =
      same_identity(plan.selected_identity, inspected.identity) &&
      same_disk(plan.selected_disk, inspected.disk) &&
      same_partition(windows, inspected.windows_partition) &&
      same_partition(system, inspected.system_partition);
  if (!review_mapping_matches || !inspection_matches) {
    return clonecore::Status::failure(review_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"自動解析と起動修復トランザクションの照合",
        L"ディスク識別、全レイアウト、または選択区画の全属性が一致しません"));
  }
  return clonecore::success_status();
}

bool equivalent_automatic_boot_repair_plan(
    const bootrepair::AutomaticBootRepairPlan& reviewed,
    const bootrepair::AutomaticBootRepairPlan& observed) noexcept {
  if (!same_disk(reviewed.selected_disk, observed.selected_disk) ||
      !same_identity(reviewed.selected_identity, observed.selected_identity) ||
      reviewed.partition_style != observed.partition_style ||
      reviewed.firmware != observed.firmware ||
      reviewed.required_system_partition_role !=
          observed.required_system_partition_role ||
      reviewed.planned_bcd_store_policy !=
          observed.planned_bcd_store_policy ||
      reviewed.windows_not_found != observed.windows_not_found ||
      reviewed.windows_selection_policy_needed !=
          observed.windows_selection_policy_needed ||
      reviewed.unsupported_windows_policy_needed !=
          observed.unsupported_windows_policy_needed ||
      reviewed.system_partition_create_plan_needed !=
          observed.system_partition_create_plan_needed ||
      reviewed.system_partition_selection_policy_needed !=
          observed.system_partition_selection_policy_needed ||
      reviewed.windows_installations.size() !=
          observed.windows_installations.size() ||
      reviewed.system_partition_candidates.size() !=
          observed.system_partition_candidates.size()) {
    return false;
  }
  for (std::size_t index = 0U;
       index < reviewed.windows_installations.size(); ++index) {
    const auto& left = reviewed.windows_installations[index];
    const auto& right = observed.windows_installations[index];
    if (!same_partition(left.partition, right.partition) ||
        !same_volume(left.volume, right.volume) ||
        !same_text(left.windows_directory, right.windows_directory) ||
        left.version.major != right.version.major ||
        left.version.build != right.version.build ||
        !same_text(
            left.version.installation_type,
            right.version.installation_type) ||
        left.officially_supported != right.officially_supported ||
        left.winre.source_state != right.winre.source_state ||
        left.winre.registered_location_reported !=
            right.winre.registered_location_reported ||
        left.winre.registered_location_matches_selected_disk !=
            right.winre.registered_location_matches_selected_disk ||
        left.winre.registered_partition_number !=
            right.winre.registered_partition_number ||
        left.winre.registered_path_kind_reported !=
            right.winre.registered_path_kind_reported ||
        left.winre.registered_path_kind !=
            right.winre.registered_path_kind ||
        left.winre.registered_image_present !=
            right.winre.registered_image_present ||
        left.winre.fallback_image_present !=
            right.winre.fallback_image_present ||
        left.winre.image_size_bytes != right.winre.image_size_bytes) {
      return false;
    }
  }
  for (std::size_t index = 0U;
       index < reviewed.system_partition_candidates.size(); ++index) {
    const auto& left = reviewed.system_partition_candidates[index];
    const auto& right = observed.system_partition_candidates[index];
    if (!same_partition(left.partition, right.partition) ||
        !same_volume(left.volume, right.volume) ||
        left.role != right.role ||
        !bootrepair::equivalent_efi_boot_ownership(
            left.efi_ownership, right.efi_ownership)) {
      return false;
    }
  }
  return true;
}

clonecore::Result<WinPeAutomaticBootRepairReview>
build_executable_automatic_boot_repair_review(
    const bootrepair::AutomaticBootRepairPlan& plan) {
  if (plan.selected_disk.is_system_disk ||
      !plan.selected_disk.offline.has_value() ||
      !plan.selected_disk.read_only.has_value() ||
      !plan.selected_disk.removable.has_value() ||
      plan.selected_disk.offline.value() ||
      plan.selected_disk.read_only.value() ||
      plan.selected_disk.removable.value()) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"自動起動修復対象の安全属性",
        L"オンライン、書込み可能、固定、非起動環境と確認できるディスクだけを修復できます");
  }
  if (plan.windows_not_found || plan.windows_installations.empty()) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_FOUND,
        L"自動起動修復のWindows選択",
        L"対象ディスクに対応Windowsを一つも確認できません");
  }
  if (plan.windows_selection_policy_needed ||
      plan.windows_installations.size() != 1U) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_DUP_NAME,
        L"自動起動修復のWindows選択",
        L"複数Windowsの優先順位を自動決定しません。選択UIの接続が必要です");
  }
  if (plan.unsupported_windows_policy_needed ||
      !plan.windows_installations.front().officially_supported) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_OLD_WIN_VERSION,
        L"自動起動修復のWindows対応確認",
        L"未保証Windowsは自動実行せず停止します");
  }
  if (plan.system_partition_create_plan_needed) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_NOT_FOUND,
        L"自動起動修復のシステム領域作成",
        L"ESPまたはBIOSシステム領域の新設は、独立した追加確認トランザクション完了後の再解析が必要です");
  }
  if (plan.system_partition_selection_policy_needed ||
      plan.system_partition_candidates.size() != 1U) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_DUP_NAME,
        L"自動起動修復のシステム領域選択",
        L"複数のESPまたはActive領域を自動選択しません");
  }
  const bool uefi =
      plan.partition_style == diskmodel::PartitionStyle::gpt &&
      plan.firmware == bootrepair::BcdBootFirmware::uefi &&
      plan.required_system_partition_role ==
          bootrepair::BootSystemPartitionRole::efi_system;
  const bool bios =
      plan.partition_style == diskmodel::PartitionStyle::mbr &&
      plan.firmware == bootrepair::BcdBootFirmware::bios &&
      plan.required_system_partition_role ==
          bootrepair::BootSystemPartitionRole::bios_active;
  if ((!uefi && !bios) ||
      plan.planned_bcd_store_policy !=
          bootrepair::BcdBootStorePolicy::rebuild_fresh) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"自動起動修復の形式整合性",
        L"GPT/UEFI/ESPまたはMBR/BIOS/Active領域と新規BCD再構築の判定が一致しません");
  }

  const auto& windows = plan.windows_installations.front();
  const auto& system = plan.system_partition_candidates.front();
  if (uefi &&
      (windows.winre.source_state !=
           bootrepair::WinReSourceState::registered_partition ||
       !windows.winre.registered_location_reported ||
       !windows.winre.registered_location_matches_selected_disk ||
       windows.winre.registered_partition_number == 0U ||
       !windows.winre.registered_path_kind_reported ||
       !windows.winre.registered_image_present ||
       windows.winre.image_size_bytes == 0U)) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_NOT_SUPPORTED,
        L"UEFI起動修復のWinRE方針",
        L"同じ対象ディスク内の既存WinRE登録とWinre.wimを完全確認できません。"
        L"この単一対象shortcutはWinRE再登録を所有しません。製品の選択・再登録経路を使用してください");
  }
  if (uefi && !bootrepair::efi_boot_ownership_allows_microsoft_rebuild(
                  system.efi_ownership)) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_NOT_SUPPORTED,
        L"UEFI第三者EFIローダー保護",
        system.efi_ownership.state ==
                bootrepair::EfiBootOwnershipState::
                    non_microsoft_or_untrusted_present
            ? L"第三者または未検証のEFI内容を検出しました。保持／削除選択が"
              L"ない単一対象shortcutでは修復を開始しません。製品の保持／専用削除経路を使用してください"
            : L"ESPのEFI所有権を一意に確認できません。曖昧な状態では修復を開始しません");
  }
  if (bios && system.efi_ownership.state !=
                  bootrepair::EfiBootOwnershipState::not_applicable) {
    return failure<WinPeAutomaticBootRepairReview>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"BIOS起動修復のEFI診断混在",
        L"BIOS計画へUEFI専用のEFI所有権診断が混在しています");
  }
  auto windows_root = one_drive_root(
      windows.volume.mount_points, L"自動起動修復のWindows割当確認");
  if (!windows_root) {
    return clonecore::Result<WinPeAutomaticBootRepairReview>::failure(
        windows_root.error());
  }

  bool temporary_system_mount = false;
  std::wstring system_root;
  if (system.volume.mount_points.empty()) {
    temporary_system_mount = true;
  } else {
    auto mounted_system_root = one_drive_root(
        system.volume.mount_points, L"自動起動修復のシステム領域割当確認");
    if (!mounted_system_root) {
      return clonecore::Result<WinPeAutomaticBootRepairReview>::failure(
          mounted_system_root.error());
    }
    system_root = mounted_system_root.take_value();
  }

  return clonecore::Result<WinPeAutomaticBootRepairReview>::success(
      WinPeAutomaticBootRepairReview{
          .request = bootrepair::BootRepairTargetRequest{
              .disk_number = plan.selected_disk.disk_number,
              .windows_root = windows_root.take_value(),
              .system_root = std::move(system_root),
              .firmware = plan.firmware,
              .store_policy = plan.planned_bcd_store_policy,
              .auto_mount_system_partition = temporary_system_mount,
              .system_volume_identity_root =
                  uefi ? system.volume.volume_name : L"",
              .require_efi_ownership_recheck = uefi,
              .expected_efi_ownership = system.efi_ownership,
              .update_current_pc_nvram = false,
          },
          .windows_partition_number = windows.partition.number,
          .system_partition_number = system.partition.number,
          .temporary_system_mount_required = temporary_system_mount,
      });
}

clonecore::Result<bootrepair::AutomaticBootRepairChoiceRequest>
build_product_automatic_boot_repair_choice_request(
    const bootrepair::AutomaticBootRepairPlan& plan,
    const WinPeAutomaticBootRepairProductChoice& choice) {
  if (plan.selected_disk.is_system_disk ||
      !plan.selected_disk.offline.has_value() ||
      !plan.selected_disk.read_only.has_value() ||
      !plan.selected_disk.removable.has_value() ||
      plan.selected_disk.offline.value() ||
      plan.selected_disk.read_only.value() ||
      plan.selected_disk.removable.value()) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"自動起動修復対象の安全属性",
        L"オンライン、書込み可能、固定、非起動環境と確認できるディスクだけを修復できます");
  }
  if (plan.windows_not_found || plan.windows_installations.empty()) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_FOUND,
        L"自動起動修復のWindows選択",
        L"対象ディスクに対応Windowsを一つも確認できません");
  }
  if (plan.system_partition_create_plan_needed ||
      plan.system_partition_candidates.empty()) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_NOT_FOUND,
        L"自動起動修復のシステム領域作成",
        L"既存ESPまたはActive領域がありません。先に独立した追加確認トランザクションを完了し、再解析してください");
  }
  if (plan.system_partition_candidates.size() != 1U ||
      plan.system_partition_selection_policy_needed) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_DUP_NAME,
        L"自動起動修復のシステム領域選択",
        L"既存ESPまたはActive領域を一意に選べません。曖昧な候補を自動選択しません");
  }
  if (!choice.explicitly_approved ||
      (choice.windows_policy !=
           bootrepair::AutomaticWindowsRegistrationPolicy::selected_only &&
       choice.windows_policy != bootrepair::
           AutomaticWindowsRegistrationPolicy::
               all_with_explicit_priority)) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"自動起動修復のWindows選択確認",
        L"全Windowsの登録順または登録する1つのWindowsを明示してください");
  }
  const std::size_t expected_count =
      choice.windows_policy == bootrepair::
              AutomaticWindowsRegistrationPolicy::selected_only
          ? 1U
          : plan.windows_installations.size();
  if (choice.windows_partition_priority.size() != expected_count ||
      expected_count == 0U) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_INVALID_PARAMETER,
        L"自動起動修復のWindows登録順",
        choice.windows_policy == bootrepair::
                AutomaticWindowsRegistrationPolicy::selected_only
            ? L"登録するWindowsを1件だけ明示してください"
            : L"検出した全Windowsを重複なく起動優先順に並べてください");
  }

  bootrepair::AutomaticBootRepairChoiceRequest request{
      .windows_policy = choice.windows_policy,
      .windows_partition_priority = choice.windows_partition_priority,
      .system_partition_number =
          plan.system_partition_candidates.front().partition.number,
      .nvram_policy = choice.nvram_policy,
  };
  if (choice.nvram_policy == bootrepair::AutomaticNvramRepairPolicy::
          repair_current_pc_windows_boot_manager &&
      (!choice.current_pc_nvram_explicitly_approved ||
       plan.firmware != bootrepair::BcdBootFirmware::uefi)) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"起動修復NVRAM方針",
        L"UEFI対象で「このPCで使用する」を明示した場合だけNVRAM修復を選べます");
  }
  if (choice.nvram_policy !=
          bootrepair::AutomaticNvramRepairPolicy::leave_unchanged &&
      choice.nvram_policy != bootrepair::AutomaticNvramRepairPolicy::
          repair_current_pc_windows_boot_manager) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"起動修復NVRAM方針",
        L"未知のNVRAM方針は製品計画へ接続しません");
  }
  const auto& system = plan.system_partition_candidates.front();
  if (plan.firmware == bootrepair::BcdBootFirmware::uefi) {
    switch (system.efi_ownership.state) {
      case bootrepair::EfiBootOwnershipState::microsoft_only_or_empty:
        if (choice.third_party_efi_policy ==
            bootrepair::AutomaticThirdPartyEfiPolicy::
                delete_non_microsoft) {
          return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
              clonecore::ErrorCode::invalid_argument,
              ERROR_NOT_FOUND,
              L"UEFI第三者EFI削除選択",
              L"第三者EFIがないESPへ削除方針を指定できません");
        }
        request.third_party_efi_policy =
            bootrepair::AutomaticThirdPartyEfiPolicy::not_applicable;
        break;
      case bootrepair::EfiBootOwnershipState::
          non_microsoft_or_untrusted_present:
        if (!bootrepair::efi_boot_ownership_allows_third_party_preserve(
                system.efi_ownership)) {
          return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"UEFI第三者EFI保持境界",
              L"独立した通常ディレクトリとして証明できない第三者EFIは保持修復できません");
        }
        if (choice.third_party_efi_policy ==
            bootrepair::AutomaticThirdPartyEfiPolicy::preserve) {
          request.third_party_efi_policy =
              bootrepair::AutomaticThirdPartyEfiPolicy::preserve;
        } else if (choice.third_party_efi_policy ==
                   bootrepair::AutomaticThirdPartyEfiPolicy::
                       delete_non_microsoft &&
                   choice.third_party_efi_delete_explicitly_approved) {
          request.third_party_efi_policy =
              bootrepair::AutomaticThirdPartyEfiPolicy::
                  delete_non_microsoft;
        } else if (choice.third_party_efi_policy ==
                  bootrepair::AutomaticThirdPartyEfiPolicy::
                      delete_non_microsoft) {
          return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
              clonecore::ErrorCode::confirmation_required,
              ERROR_CANCELLED,
              L"UEFI第三者EFI削除選択",
              L"「削除（危険）」の専用確認が明示されていません");
        } else {
          return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"UEFI第三者EFI方針",
              L"保持または明示承認済み削除だけを選択できます");
        }
        break;
      case bootrepair::EfiBootOwnershipState::ambiguous:
      case bootrepair::EfiBootOwnershipState::not_applicable:
      default:
        return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"UEFI第三者EFI診断",
            L"EFI内容を安全に分類できないため、保持／削除のどちらも実行しません");
    }
  } else if (choice.third_party_efi_policy ==
             bootrepair::AutomaticThirdPartyEfiPolicy::
                 delete_non_microsoft) {
    return failure<bootrepair::AutomaticBootRepairChoiceRequest>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"BIOS第三者EFI削除選択",
        L"第三者EFI削除はGPT/UEFI ESPだけで選択できます");
  }
  return clonecore::Result<
      bootrepair::AutomaticBootRepairChoiceRequest>::success(
      std::move(request));
}

bool automatic_boot_repair_allows_third_party_efi_delete(
    const bootrepair::AutomaticBootRepairPlan& plan) noexcept {
  if (plan.partition_style != diskmodel::PartitionStyle::gpt ||
      plan.firmware != bootrepair::BcdBootFirmware::uefi ||
      plan.system_partition_create_plan_needed ||
      plan.system_partition_selection_policy_needed ||
      plan.system_partition_candidates.size() != 1U) {
    return false;
  }
  const auto& system = plan.system_partition_candidates.front();
  return system.role == bootrepair::BootSystemPartitionRole::efi_system &&
      system.efi_ownership.state ==
          bootrepair::EfiBootOwnershipState::
              non_microsoft_or_untrusted_present &&
      bootrepair::efi_boot_ownership_allows_third_party_preserve(
          system.efi_ownership);
}

clonecore::Result<bootrepair::WindowsEfiDeleteEspRequest>
build_windows_efi_delete_esp_request(
    const bootrepair::ReviewedAutomaticBootRepairChoices& reviewed) {
  const auto& system = reviewed.system_partition();
  if (reviewed.partition_style() != diskmodel::PartitionStyle::gpt ||
      reviewed.firmware() != bootrepair::BcdBootFirmware::uefi ||
      reviewed.third_party_efi_policy() !=
          bootrepair::AutomaticThirdPartyEfiPolicy::
              delete_non_microsoft ||
      system.role != bootrepair::BootSystemPartitionRole::efi_system ||
      system.partition.style != diskmodel::PartitionStyle::gpt ||
      system.partition.number == 0U ||
      system.partition.size_bytes == 0U ||
      system.partition.identifier.empty() ||
      system.partition.type.empty() || system.volume.volume_name.empty() ||
      system.efi_ownership.state !=
          bootrepair::EfiBootOwnershipState::
              non_microsoft_or_untrusted_present ||
      !bootrepair::efi_boot_ownership_allows_third_party_preserve(
          system.efi_ownership)) {
    return failure<bootrepair::WindowsEfiDeleteEspRequest>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"第三者EFI削除ESP routing",
        L"レビュー済みGPT ESPと独立top-level第三者namespaceの証拠が揃っていません");
  }
  return clonecore::Result<
      bootrepair::WindowsEfiDeleteEspRequest>::success(
      bootrepair::WindowsEfiDeleteEspRequest{
          .expected_disk = reviewed.selected_identity(),
          .expected_partition_number = system.partition.number,
          .expected_offset_bytes = system.partition.offset_bytes,
          .expected_length_bytes = system.partition.size_bytes,
          .expected_partition_identifier = system.partition.identifier,
          .expected_partition_type_identifier = system.partition.type,
          .expected_volume_guid_root = system.volume.volume_name,
      });
}

std::wstring format_reviewed_efi_delete_plan(
    const bootrepair::ReviewedEfiDeletePlan& reviewed) {
  std::wostringstream output;
  output << L"\r\n【第三者EFI削除 専用レビュー（危険）】\r\n"
         << L"  対象ESP: partition #"
         << reviewed.expected_esp().partition_number << L" / "
         << reviewed.expected_esp().volume_guid_root << L"\r\n"
         << L"  削除候補: " << reviewed.candidates().size()
         << L" 個（EFI直下の独立directoryのみ）\r\n";
  for (const auto& candidate : reviewed.candidates()) {
    output << L"    - EFI\\" << candidate.relative_name << L" ("
           << candidate.entries.size() << L" entries)\r\n";
  }
  output << L"  manifest SHA-256: ";
  output << std::hex << std::setfill(L'0');
  for (const std::byte value : reviewed.manifest_sha256()) {
    output << std::setw(2)
           << static_cast<unsigned int>(std::to_integer<unsigned char>(value));
  }
  output << std::dec
         << L"\r\n  Microsoft/Boot/fallback/ESP root/EFI rootは対象外です。"
         << L"実行にはこのレビュー後の大文字 OK とfresh exact再照合が必要です。\r\n";
  return output.str();
}

std::wstring format_efi_delete_transaction_report(
    const bootrepair::EfiDeleteTransactionReport& report) {
  std::wstring classification;
  switch (report.outcome) {
    case bootrepair::EfiDeleteTransactionOutcome::committed:
      classification = L"COMMITTED";
      break;
    case bootrepair::EfiDeleteTransactionOutcome::stopped_before_mutation:
      classification = L"STOPPED_BEFORE_MUTATION";
      break;
    case bootrepair::EfiDeleteTransactionOutcome::rolled_back:
      classification = L"ROLLED_BACK_EXACT";
      break;
    case bootrepair::EfiDeleteTransactionOutcome::partial_rollback:
      classification = L"PARTIAL_ROLLBACK";
      break;
    case bootrepair::EfiDeleteTransactionOutcome::finalization_incomplete:
      classification = L"FINALIZATION_INCOMPLETE";
      break;
    case bootrepair::EfiDeleteTransactionOutcome::partial_delete:
      classification = L"PARTIAL_DELETE";
      break;
    case bootrepair::EfiDeleteTransactionOutcome::
        committed_bcd_cleanup_incomplete:
      classification = L"COMMITTED_BCD_CLEANUP_INCOMPLETE";
      break;
    case bootrepair::EfiDeleteTransactionOutcome::
        committed_quarantine_cleanup_incomplete:
      classification = L"COMMITTED_QUARANTINE_CLEANUP_INCOMPLETE";
      break;
  }
  std::wostringstream output;
  output << L"\r\n【第三者EFI削除transaction結果】 "
         << classification << L"\r\n"
         << L"  quarantine=" << report.quarantined_candidates
         << L" / rollback=" << report.rolled_back_candidates
         << L" / delete=" << report.deleted_candidates << L"\r\n"
         << L"  stable ESP="
         << (report.stable_target_reidentified ? L"OK" : L"NO")
         << L" / fresh manifest="
         << (report.fresh_manifest_verified ? L"OK" : L"NO")
         << L" / BCD readback="
         << (report.microsoft_bcd_rebuild_readback_verified ? L"OK" : L"NO")
         << L"\r\n";
  if (report.primary_error.has_value()) {
    output << L"  primary: " << report.primary_error->operation
           << L" - " << report.primary_error->message << L"\r\n";
  }
  if (report.rollback_error.has_value()) {
    output << L"  rollback: " << report.rollback_error->operation
           << L" - " << report.rollback_error->message << L"\r\n";
  }
  return output.str();
}

clonecore::Result<WinPeReviewedAutomaticBootRepairExecution>
build_executable_reviewed_automatic_boot_repair(
    const bootrepair::ReviewedAutomaticBootRepairChoices& reviewed) {
  const bool repair_current_pc_nvram =
      reviewed.nvram_policy() == bootrepair::AutomaticNvramRepairPolicy::
          repair_current_pc_windows_boot_manager;
  if (reviewed.nvram_policy() !=
          bootrepair::AutomaticNvramRepairPolicy::leave_unchanged &&
      !repair_current_pc_nvram) {
    return failure<WinPeReviewedAutomaticBootRepairExecution>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"起動修復NVRAM実行境界",
        L"レビュー済みNVRAM方針が未知の値です");
  }
  if (reviewed.windows_in_boot_priority().empty()) {
    return failure<WinPeReviewedAutomaticBootRepairExecution>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"起動修復Windows実行順",
        L"レビュー済みWindowsがありません");
  }

  const auto& system = reviewed.system_partition();
  const bool uefi =
      reviewed.partition_style() == diskmodel::PartitionStyle::gpt &&
      reviewed.firmware() == bootrepair::BcdBootFirmware::uefi &&
      system.role == bootrepair::BootSystemPartitionRole::efi_system;
  const bool bios =
      reviewed.partition_style() == diskmodel::PartitionStyle::mbr &&
      reviewed.firmware() == bootrepair::BcdBootFirmware::bios &&
      system.role == bootrepair::BootSystemPartitionRole::bios_active;
  if ((!uefi && !bios) || reviewed.bcd_store_policy() !=
          bootrepair::BcdBootStorePolicy::rebuild_fresh) {
    return failure<WinPeReviewedAutomaticBootRepairExecution>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"起動修復レビュー形式",
        L"レビュー済み起動方式、システム領域、またはBCD方針が一致しません");
  }
  if (repair_current_pc_nvram && !uefi) {
    return failure<WinPeReviewedAutomaticBootRepairExecution>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"起動修復NVRAM実行境界",
        L"現在PCのNVRAM修復はレビュー済みGPT ESPのUEFI計画だけで実行できます");
  }

  bootrepair::BootRepairThirdPartyEfiPolicy execution_efi_policy =
      bootrepair::BootRepairThirdPartyEfiPolicy::not_applicable;
  bool preserves_third_party = false;
  bool deletes_third_party = false;
  if (reviewed.third_party_efi_policy() ==
          bootrepair::AutomaticThirdPartyEfiPolicy::preserve ||
      reviewed.third_party_efi_policy() ==
          bootrepair::AutomaticThirdPartyEfiPolicy::
              delete_non_microsoft) {
    if (!uefi || system.efi_ownership.state !=
            bootrepair::EfiBootOwnershipState::
                non_microsoft_or_untrusted_present ||
        !bootrepair::efi_boot_ownership_allows_third_party_preserve(
            system.efi_ownership)) {
      return failure<WinPeReviewedAutomaticBootRepairExecution>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"起動修復第三者EFItransaction境界",
          L"第三者EFI方針と、Microsoftが変更しない独立namespaceの"
          L"証拠が一致しません");
    }
    // The standalone BCD inspection seam must continue to preserve the
    // namespace. Actual deletion is owned only by EfiDeleteTransaction.
    execution_efi_policy =
        bootrepair::BootRepairThirdPartyEfiPolicy::preserve;
    preserves_third_party = reviewed.third_party_efi_policy() ==
        bootrepair::AutomaticThirdPartyEfiPolicy::preserve;
    deletes_third_party = !preserves_third_party;
  } else if (uefi && system.efi_ownership.state !=
      bootrepair::EfiBootOwnershipState::microsoft_only_or_empty) {
    return failure<WinPeReviewedAutomaticBootRepairExecution>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_NOT_SUPPORTED,
        L"起動修復第三者EFI保持境界",
        L"第三者EFIを検出しましたが、保持方針がレビューされていません");
  }

  bool partial = false;
  if (reviewed.winre_choices_in_boot_priority().size() !=
      reviewed.windows_in_boot_priority().size()) {
    return failure<WinPeReviewedAutomaticBootRepairExecution>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"起動修復WinRE方針件数",
        L"レビュー済みWindowsとWinRE方針の件数が一致しません");
  }
  for (const auto& winre : reviewed.winre_choices_in_boot_priority()) {
    if (winre.disposition == bootrepair::
            AutomaticWinReRepairDisposition::normal_boot_only_partial) {
      partial = true;
    } else if (winre.disposition != bootrepair::
                   AutomaticWinReRepairDisposition::
                       verify_existing_registration &&
               winre.disposition != bootrepair::
                   AutomaticWinReRepairDisposition::
                       register_verified_windows_image) {
      return failure<WinPeReviewedAutomaticBootRepairExecution>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"起動修復WinRE方針",
          L"未知のWinRE方針は実行できません");
    }
  }

  bool temporary_system_mount = false;
  std::wstring system_root;
  if (system.volume.mount_points.empty()) {
    temporary_system_mount = true;
  } else {
    auto mounted = one_drive_root(
        system.volume.mount_points,
        L"自動起動修復のシステム領域割当確認");
    if (!mounted) {
      return clonecore::Result<
          WinPeReviewedAutomaticBootRepairExecution>::failure(
          mounted.error());
    }
    system_root = mounted.take_value();
  }

  WinPeReviewedAutomaticBootRepairExecution result{
      .system_partition_number = system.partition.number,
      .temporary_system_mount_required = temporary_system_mount,
      .third_party_efi_preserved = preserves_third_party,
      .third_party_efi_delete_requested = deletes_third_party,
      .repair_current_pc_nvram = repair_current_pc_nvram,
      .normal_boot_only_partial = partial,
  };
  const auto windows = reviewed.windows_in_boot_priority();
  result.requests_in_boot_priority.reserve(windows.size());
  result.windows_partition_numbers_in_boot_priority.reserve(windows.size());
  result.winre_actions_in_boot_priority.reserve(windows.size());
  const auto winre_choices = reviewed.winre_choices_in_boot_priority();
  for (std::size_t index = 0U; index < windows.size(); ++index) {
    auto windows_root = one_drive_root(
        windows[index].volume.mount_points,
        L"自動起動修復のWindows割当確認");
    if (!windows_root) {
      return clonecore::Result<
          WinPeReviewedAutomaticBootRepairExecution>::failure(
          windows_root.error());
    }
    const std::wstring resolved_windows_root = windows_root.value();
    const std::wstring offline_windows_directory =
        resolved_windows_root + L"Windows";
    if (winre_choices[index].windows_partition_number !=
        windows[index].partition.number) {
      return failure<WinPeReviewedAutomaticBootRepairExecution>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"起動修復WinRE方針順",
          L"WinRE方針がレビュー済みWindows優先順と一致しません");
    }
    result.windows_partition_numbers_in_boot_priority.push_back(
        windows[index].partition.number);
    result.requests_in_boot_priority.push_back(
        bootrepair::BootRepairTargetRequest{
            .disk_number = reviewed.selected_identity().disk_number,
            .windows_root = resolved_windows_root,
            .system_root = system_root,
            .firmware = reviewed.firmware(),
            .store_policy = index == 0U
                ? bootrepair::BcdBootStorePolicy::rebuild_fresh
                : bootrepair::BcdBootStorePolicy::preserve_existing,
            .auto_mount_system_partition = temporary_system_mount,
            .system_volume_identity_root =
                uefi ? system.volume.volume_name : L"",
            .require_efi_ownership_recheck = uefi,
            .expected_efi_ownership = system.efi_ownership,
            .third_party_efi_policy = execution_efi_policy,
            .reviewed_multi_windows_batch = true,
            .update_current_pc_nvram = false,
        });
    const bool register_fallback =
        winre_choices[index].disposition == bootrepair::
            AutomaticWinReRepairDisposition::
                register_verified_windows_image;
    result.winre_actions_in_boot_priority.push_back(
        WinPeReviewedAutomaticBootRepairExecution::WinReAction{
            .windows_partition_number = windows[index].partition.number,
            .disposition = winre_choices[index].disposition,
            .offline_windows_directory = offline_windows_directory,
            .candidate_directory = register_fallback
                ? fallback_winre_directory(offline_windows_directory)
                : L"",
            .expected_target_partition_number = register_fallback
                ? windows[index].partition.number
                : 0U,
            .expected_registered_path_kind = bootrepair::
                WinReRegisteredPathKind::windows_system32_recovery,
            .prior_diagnostic = rebuild_prior_winre_diagnostic(
                windows[index], offline_windows_directory),
        });
  }
  return clonecore::Result<
      WinPeReviewedAutomaticBootRepairExecution>::success(
      std::move(result));
}

clonecore::Result<WinPeReviewedAutomaticBootRepairExecution>
bind_reviewed_automatic_boot_repair_winre_images(
    WinPeReviewedAutomaticBootRepairExecution execution,
    const std::span<const WinPeAutomaticBootRepairWinReImageBinding>
        bindings) {
  std::size_t required_bindings{};
  for (auto& action : execution.winre_actions_in_boot_priority) {
    if (action.disposition != bootrepair::AutomaticWinReRepairDisposition::
            register_verified_windows_image) {
      if (action.reviewed_candidate.has_value()) {
        return failure<WinPeReviewedAutomaticBootRepairExecution>(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_DATA,
            L"起動修復WinRE画像束縛",
            L"変更しないWinRE方針へ画像証拠を束縛できません");
      }
      continue;
    }
    ++required_bindings;
    if (action.reviewed_candidate.has_value()) {
      return failure<WinPeReviewedAutomaticBootRepairExecution>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_ALREADY_EXISTS,
          L"起動修復WinRE画像再束縛",
          L"確認済みWinre.wimを別の証拠で上書きできません");
    }
    const auto first = std::find_if(
        bindings.begin(), bindings.end(), [&](const auto& binding) {
          return binding.windows_partition_number ==
              action.windows_partition_number;
        });
    if (first == bindings.end() ||
        std::count_if(
            bindings.begin(), bindings.end(), [&](const auto& binding) {
              return binding.windows_partition_number ==
                  action.windows_partition_number;
            }) != 1 ||
        !valid_winre_image_identity_shape(first->identity) ||
        !same_text(
            first->identity.requested_path,
            winre_image_path(action.candidate_directory)) ||
        first->identity.length !=
            action.prior_diagnostic.winre_image_size_bytes) {
      return failure<WinPeReviewedAutomaticBootRepairExecution>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"起動修復WinRE画像束縛",
          L"確認済みWindows区画、候補パス、寸法、File ID、SHA-256を一意に束縛できません");
    }
    action.reviewed_candidate = first->identity;
  }
  if (bindings.size() != required_bindings) {
    return failure<WinPeReviewedAutomaticBootRepairExecution>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"起動修復WinRE画像束縛件数",
        L"WinRE再登録方針と確認済み画像証拠の件数が一致しません");
  }
  return clonecore::Result<
      WinPeReviewedAutomaticBootRepairExecution>::success(
      std::move(execution));
}

clonecore::Status validate_reviewed_automatic_boot_repair_inspections(
    const bootrepair::AutomaticBootRepairPlan& plan,
    const bootrepair::ReviewedAutomaticBootRepairChoices& choices,
    const WinPeReviewedAutomaticBootRepairExecution& execution,
    const std::span<const bootrepair::BootRepairTargetSelection> inspected) {
  auto rebound = bootrepair::revalidate_automatic_boot_repair_choices(
      choices, plan);
  if (!rebound) {
    return clonecore::Status::failure(rebound.error());
  }
  auto rebuilt = build_executable_reviewed_automatic_boot_repair(
      rebound.value());
  if (!rebuilt) {
    return clonecore::Status::failure(rebuilt.error());
  }
  std::vector<WinPeAutomaticBootRepairWinReImageBinding> bindings;
  for (const auto& action : execution.winre_actions_in_boot_priority) {
    if (action.reviewed_candidate.has_value()) {
      bindings.push_back(WinPeAutomaticBootRepairWinReImageBinding{
          .windows_partition_number = action.windows_partition_number,
          .identity = *action.reviewed_candidate,
      });
    }
  }
  auto bound_rebuilt = bind_reviewed_automatic_boot_repair_winre_images(
      rebuilt.take_value(), bindings);
  if (!bound_rebuilt) {
    return clonecore::Status::failure(bound_rebuilt.error());
  }
  if (!same_execution_review(execution, bound_rebuilt.value()) ||
      inspected.empty() ||
      inspected.size() != execution.requests_in_boot_priority.size() ||
      plan.partition_style != choices.partition_style() ||
      plan.firmware != choices.firmware() ||
      !same_identity(plan.selected_identity, choices.selected_identity()) ||
      !same_disk(plan.selected_disk, inspected.front().disk)) {
    return clonecore::Status::failure(review_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"レビュー済み起動修復の初回照合",
        L"計画、実行要求、対象ディスク、または候補件数が一致しません"));
  }
  const auto windows = choices.windows_in_boot_priority();
  if (windows.size() != inspected.size()) {
    return clonecore::Status::failure(review_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"レビュー済みWindows起動順照合",
        L"レビュー済みWindowsと直前診断の件数が一致しません"));
  }
  for (std::size_t index = 0U; index < inspected.size(); ++index) {
    if (!same_identity(choices.selected_identity(), inspected[index].identity) ||
        !same_disk(plan.selected_disk, inspected[index].disk) ||
        !same_partition(
            windows[index].partition,
            inspected[index].windows_partition) ||
        !same_partition(
            choices.system_partition().partition,
            inspected[index].system_partition)) {
      return clonecore::Status::failure(review_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"レビュー済み起動修復候補照合",
          L"Windows優先順、システム領域、全レイアウト、または安定識別が一致しません"));
    }
  }
  return clonecore::success_status();
}

WinPeAutomaticBootRepairLayout
build_winpe_automatic_boot_repair_layout(
    const int client_width,
    const int client_height) noexcept {
  constexpr int kContentLeft = 260;
  const int content_right = (std::max)(client_width - 28, 800);
  const int field_left = kContentLeft + 22;
  const int field_right = content_right - 22;
  constexpr int kActionWidth = 170;
  constexpr int kGap = 10;
  const bool compact_height = client_height < 600;
  const int confirmation_top = compact_height ? 358 : 378;
  const int confirmation_bottom = confirmation_top + 32;
  const int output_top = compact_height ? 410 : 462;
  const int output_bottom = compact_height
      ? (std::max)(client_height - 32, output_top + 1)
      : (std::max)(client_height - 32, 562);
  const int execute_left = field_right - kActionWidth;
  const int cancel_left = execute_left - kGap - kActionWidth;
  return WinPeAutomaticBootRepairLayout{
      .target_disk = {
          field_left,
          202,
          field_right - kActionWidth - kGap,
          234,
      },
      .inspect = {
          field_right - kActionWidth,
          202,
          field_right,
          234,
      },
      .confirmation_token = {
          field_left,
          confirmation_top,
          cancel_left - kGap,
          confirmation_bottom,
      },
      .execute = {
          execute_left,
          confirmation_top,
          field_right,
          confirmation_bottom,
      },
      .cancel_review = {
          cancel_left,
          confirmation_top,
          execute_left - kGap,
          confirmation_bottom,
      },
      .output = {
          field_left,
          output_top,
          field_right,
          output_bottom,
      },
  };
}

WinPeAutomaticBootRepairUiView
build_winpe_automatic_boot_repair_ui_view(
    const WinPeAutomaticBootRepairUiInput& input) noexcept {
  const bool selectable = input.idle && !input.execution_active;
  const bool confirmation = input.reviewed && !input.execution_active;
  return WinPeAutomaticBootRepairUiView{
      .target_enabled = selectable,
      .inspect_enabled = selectable && input.inventory_ready &&
          input.target_selected,
      .confirmation_visible = confirmation,
      .confirmation_enabled = selectable && input.reviewed,
      .execute_visible = confirmation,
      .execute_enabled = selectable && input.reviewed &&
          input.confirmation_text == L"OK",
      .cancel_review_visible = confirmation,
      .cancel_review_enabled = selectable && input.reviewed,
  };
}

}  // namespace ytec::winpeapp

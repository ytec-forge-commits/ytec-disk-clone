#include "ytec/bootrepair/mbr2gpt_execution.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace ytec::bootrepair {
namespace {

constexpr std::uint64_t kRecoveryGptAttributes =
    0x8000000000000001ULL;

clonecore::Error preparation_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"MBR→GPT別ターゲット実行準備",
      .message = std::move(message),
  };
}

const clonecore::GptGuid& expected_type_guid(
    const PlannedGptPartitionRole role) noexcept {
  switch (role) {
    case PlannedGptPartitionRole::efi_system:
      return clonecore::gpt_type_efi_system();
    case PlannedGptPartitionRole::microsoft_reserved:
      return clonecore::gpt_type_microsoft_reserved();
    case PlannedGptPartitionRole::windows:
    case PlannedGptPartitionRole::data:
      return clonecore::gpt_type_basic_data();
    case PlannedGptPartitionRole::recovery:
      return clonecore::gpt_type_windows_recovery();
  }
  return clonecore::gpt_type_basic_data();
}

bool action_matches_role(
    const PlannedGptPartition& partition) noexcept {
  switch (partition.role) {
    case PlannedGptPartitionRole::efi_system:
      return partition.action ==
                 PlannedGptPartitionAction::create_fat32 &&
             partition.source_partition_number == 0U;
    case PlannedGptPartitionRole::microsoft_reserved:
      return partition.action ==
                 PlannedGptPartitionAction::create_reserved &&
             partition.source_partition_number == 0U;
    case PlannedGptPartitionRole::windows:
    case PlannedGptPartitionRole::data:
      return partition.action ==
                 PlannedGptPartitionAction::copy_source_contents &&
             partition.source_partition_number != 0U;
    case PlannedGptPartitionRole::recovery:
      return (partition.action ==
                  PlannedGptPartitionAction::copy_source_contents &&
              partition.source_partition_number != 0U) ||
             (partition.action ==
                  PlannedGptPartitionAction::create_and_stage_winre &&
              partition.source_partition_number == 0U);
  }
  return false;
}

bool metadata_write_sequence_is_exact(
    const clonecore::GptWritePlan& metadata) noexcept {
  constexpr std::array<clonecore::GptMetadataKind, 5U> expected{
      clonecore::GptMetadataKind::primary_entries,
      clonecore::GptMetadataKind::backup_entries,
      clonecore::GptMetadataKind::backup_header,
      clonecore::GptMetadataKind::protective_mbr,
      clonecore::GptMetadataKind::primary_header_commit,
  };
  if (metadata.writes.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    if (metadata.writes[index].kind != expected[index] ||
        metadata.writes[index].bytes.empty()) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_layout_and_metadata(
    const Mbr2GptRebuildPlan& layout,
    const clonecore::GptWritePlan& metadata) {
  const auto& disk = metadata.target_disk;
  if (!layout.source_disk_remains_unchanged ||
      !layout.official_mbr2gpt_validate_still_required ||
      !layout.bcdboot_required ||
      layout.alignment_bytes == 0U ||
      layout.target_partitions.size() < 3U ||
      layout.target_partitions.size() != disk.partitions.size() ||
      (disk.logical_sector_size != 512U &&
       disk.logical_sector_size != 4096U) ||
      disk.sector_count == 0U ||
      disk.disk_guid.is_zero() ||
      !metadata_write_sequence_is_exact(metadata)) {
    return clonecore::Status::failure(preparation_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"配置計画またはGPTメタデータの必須安全条件が揃っていません"));
  }
  if (disk.sector_count >
      (std::numeric_limits<std::uint64_t>::max)() /
          disk.logical_sector_size) {
    return clonecore::Status::failure(preparation_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"GPT媒体寸法が64ビット範囲を超えています"));
  }
  const std::uint64_t disk_bytes =
      disk.sector_count * disk.logical_sector_size;
  for (const auto& write : metadata.writes) {
    if (write.offset > disk_bytes ||
        write.bytes.size() > disk_bytes - write.offset) {
      return clonecore::Status::failure(preparation_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"GPTメタデータ書込み範囲がコピー先容量外です"));
    }
  }

  bool recovery_seen = false;
  bool data_seen = false;
  std::vector<clonecore::GptGuid> unique_guids;
  unique_guids.reserve(disk.partitions.size());
  for (std::size_t index = 0U;
       index < layout.target_partitions.size();
       ++index) {
    const auto& planned = layout.target_partitions[index];
    const auto& gpt = disk.partitions[index];
    const auto required_role =
        index == 0U
            ? PlannedGptPartitionRole::efi_system
            : index == 1U
                  ? PlannedGptPartitionRole::microsoft_reserved
                  : index == 2U
                        ? PlannedGptPartitionRole::windows
                        : planned.role;
    if (planned.target_number != index + 1U ||
        planned.role != required_role ||
        !action_matches_role(planned) ||
        gpt.entry_index != index ||
        gpt.type_guid != expected_type_guid(planned.role) ||
        gpt.unique_guid.is_zero() ||
        std::find(
            unique_guids.begin(),
            unique_guids.end(),
            gpt.unique_guid) != unique_guids.end() ||
        planned.offset_bytes % disk.logical_sector_size != 0U ||
        planned.size_bytes == 0U ||
        planned.size_bytes % disk.logical_sector_size != 0U) {
      return clonecore::Status::failure(preparation_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"GPT区画の順序、動作、GUIDまたは寸法が計画と一致しません"));
    }
    const std::uint64_t first_lba =
        planned.offset_bytes / disk.logical_sector_size;
    const std::uint64_t lba_count =
        planned.size_bytes / disk.logical_sector_size;
    if (lba_count == 0U ||
        first_lba >
            (std::numeric_limits<std::uint64_t>::max)() -
                (lba_count - 1U) ||
        gpt.first_lba != first_lba ||
        gpt.last_lba != first_lba + lba_count - 1U) {
      return clonecore::Status::failure(preparation_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"GPT区画LBAが配置計画と一致しません"));
    }
    const std::uint64_t expected_attributes =
        planned.role == PlannedGptPartitionRole::recovery
            ? kRecoveryGptAttributes
            : 0U;
    if (gpt.attributes != expected_attributes ||
        (recovery_seen &&
         planned.role == PlannedGptPartitionRole::recovery) ||
        (data_seen &&
         planned.role == PlannedGptPartitionRole::recovery)) {
      return clonecore::Status::failure(preparation_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"回復区画の属性またはWindows直後という順序が不正です"));
    }
    recovery_seen =
        recovery_seen ||
        planned.role == PlannedGptPartitionRole::recovery;
    data_seen =
        data_seen ||
        planned.role == PlannedGptPartitionRole::data;
    unique_guids.push_back(gpt.unique_guid);
  }
  if (recovery_seen != layout.winre_registration_required) {
    return clonecore::Status::failure(preparation_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"回復区画とWinRE登録要件が一致しません"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<Mbr2GptExecutionPreparation>
prepare_mbr2gpt_target_build_execution(
    const Mbr2GptRebuildPlan& layout,
    const clonecore::GptWritePlan& metadata) {
  const auto validation =
      validate_layout_and_metadata(layout, metadata);
  if (!validation) {
    return clonecore::Result<
        Mbr2GptExecutionPreparation>::failure(validation.error());
  }

  Mbr2GptExecutionPreparation preparation{
      .layout_and_metadata_cross_checked = true,
      .source_writes_permitted = false,
      .official_mbr2gpt_validate_required = true,
      .bcdboot_signature_check_required = true,
      .winre_registration_required =
          layout.winre_registration_required,
      .final_cold_uefi_boot_required = true,
      .execution_adapter_connected = false,
      .physical_write_started = false,
  };
  const auto append =
      [&preparation](
          const Mbr2GptTargetBuildStepKind kind,
          const std::uint32_t source_partition,
          const std::uint32_t target_partition,
          const bool mutates_target) {
        preparation.steps.push_back({
            .sequence = static_cast<std::uint32_t>(
                preparation.steps.size() + 1U),
            .kind = kind,
            .source_partition_number = source_partition,
            .target_partition_number = target_partition,
            .mutates_target = mutates_target,
            .source_remains_read_only = true,
        });
      };

  append(
      Mbr2GptTargetBuildStepKind::
          verify_source_identity_read_only,
      0U,
      0U,
      false);
  append(
      Mbr2GptTargetBuildStepKind::
          run_official_mbr2gpt_validate,
      0U,
      0U,
      false);
  append(
      Mbr2GptTargetBuildStepKind::
          reidentify_source_after_validate,
      0U,
      0U,
      false);
  append(
      Mbr2GptTargetBuildStepKind::
          verify_target_identity_and_empty,
      0U,
      0U,
      false);
  append(
      Mbr2GptTargetBuildStepKind::
          verify_two_step_target_confirmation,
      0U,
      0U,
      false);
  append(
      Mbr2GptTargetBuildStepKind::verify_microsoft_tools,
      0U,
      0U,
      false);

  preparation.first_target_mutation_index =
      preparation.steps.size();
  append(
      Mbr2GptTargetBuildStepKind::
          write_protective_mbr_and_gpt,
      0U,
      0U,
      true);
  append(
      Mbr2GptTargetBuildStepKind::create_esp_fat32,
      0U,
      1U,
      true);

  for (const auto& partition : layout.target_partitions) {
    if (partition.action ==
        PlannedGptPartitionAction::copy_source_contents) {
      append(
          Mbr2GptTargetBuildStepKind::
              copy_source_partition_contents,
          partition.source_partition_number,
          partition.target_number,
          true);
      if (partition.role ==
          PlannedGptPartitionRole::windows) {
        append(
            Mbr2GptTargetBuildStepKind::
                extend_windows_file_system,
            0U,
            partition.target_number,
            true);
      }
    } else if (
        partition.action ==
        PlannedGptPartitionAction::create_and_stage_winre) {
      append(
          Mbr2GptTargetBuildStepKind::create_recovery_ntfs,
          0U,
          partition.target_number,
          true);
      append(
          Mbr2GptTargetBuildStepKind::stage_winre_image,
          0U,
          partition.target_number,
          true);
    }
  }
  append(
      Mbr2GptTargetBuildStepKind::run_bcdboot_uefi,
      0U,
      1U,
      true);
  if (layout.winre_registration_required) {
    const auto recovery = std::find_if(
        layout.target_partitions.begin(),
        layout.target_partitions.end(),
        [](const auto& partition) {
          return partition.role ==
                 PlannedGptPartitionRole::recovery;
        });
    append(
        Mbr2GptTargetBuildStepKind::register_winre,
        0U,
        recovery->target_number,
        true);
  }
  append(
      Mbr2GptTargetBuildStepKind::verify_gpt_readback,
      0U,
      0U,
      false);
  append(
      Mbr2GptTargetBuildStepKind::verify_partition_contents,
      0U,
      0U,
      false);
  append(
      Mbr2GptTargetBuildStepKind::verify_uefi_boot_files,
      0U,
      1U,
      false);
  if (layout.winre_registration_required) {
    append(
        Mbr2GptTargetBuildStepKind::
            verify_winre_registration,
        0U,
        0U,
        false);
  }
  append(
      Mbr2GptTargetBuildStepKind::
          require_cold_uefi_boot_test,
      0U,
      0U,
      false);

  return clonecore::Result<
      Mbr2GptExecutionPreparation>::success(
      std::move(preparation));
}

std::wstring_view mbr2gpt_target_build_step_name(
    const Mbr2GptTargetBuildStepKind kind) noexcept {
  switch (kind) {
    case Mbr2GptTargetBuildStepKind::
        verify_source_identity_read_only:
      return L"コピー元を読み取り専用で再識別";
    case Mbr2GptTargetBuildStepKind::
        run_official_mbr2gpt_validate:
      return L"Microsoft MBR2GPT /validate";
    case Mbr2GptTargetBuildStepKind::
        reidentify_source_after_validate:
      return L"validate後にコピー元を再識別";
    case Mbr2GptTargetBuildStepKind::
        verify_target_identity_and_empty:
      return L"コピー先の再識別と空媒体確認";
    case Mbr2GptTargetBuildStepKind::
        verify_two_step_target_confirmation:
      return L"コピー先の二段階確認";
    case Mbr2GptTargetBuildStepKind::verify_microsoft_tools:
      return L"Microsoftツールの署名と版を確認";
    case Mbr2GptTargetBuildStepKind::
        write_protective_mbr_and_gpt:
      return L"保護MBRと主副GPTを作成";
    case Mbr2GptTargetBuildStepKind::create_esp_fat32:
      return L"EFIシステム区画をFAT32で作成";
    case Mbr2GptTargetBuildStepKind::
        copy_source_partition_contents:
      return L"コピー元区画内容を移行";
    case Mbr2GptTargetBuildStepKind::
        extend_windows_file_system:
      return L"Windowsファイルシステムを拡張";
    case Mbr2GptTargetBuildStepKind::create_recovery_ntfs:
      return L"回復区画をNTFSで作成";
    case Mbr2GptTargetBuildStepKind::stage_winre_image:
      return L"確認済みWinREイメージを配置";
    case Mbr2GptTargetBuildStepKind::run_bcdboot_uefi:
      return L"BCDBootでUEFI起動ファイルを構築";
    case Mbr2GptTargetBuildStepKind::register_winre:
      return L"Windows REを登録";
    case Mbr2GptTargetBuildStepKind::verify_gpt_readback:
      return L"主副GPTを全量読戻し検証";
    case Mbr2GptTargetBuildStepKind::
        verify_partition_contents:
      return L"移行区画内容を検証";
    case Mbr2GptTargetBuildStepKind::verify_uefi_boot_files:
      return L"UEFI起動ファイルを検証";
    case Mbr2GptTargetBuildStepKind::
        verify_winre_registration:
      return L"Windows RE登録を検証";
    case Mbr2GptTargetBuildStepKind::
        require_cold_uefi_boot_test:
      return L"コピー先単独のコールドUEFI起動を要求";
  }
  return L"不明";
}

}  // namespace ytec::bootrepair

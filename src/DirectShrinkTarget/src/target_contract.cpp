#include "ytec/directshrink/target_contract.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace ytec::directshrink {
namespace {

clonecore::Error contract_error(
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
  return clonecore::Result<T>::failure(contract_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool stable_identity_exactly_matches(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  return left.model == right.model &&
      left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.serial_suffix == right.serial_suffix &&
      left.device_instance_id == right.device_instance_id &&
      left.is_system_disk == right.is_system_disk;
}

bool is_archive_task(const PartitionTask& task) noexcept {
  return task.kind == PartitionTaskKind::apply_ntfs_wim;
}

bool is_ntfs_reconstruction_task(const PartitionTask& task) noexcept {
  return is_archive_task(task) ||
      task.kind == PartitionTaskKind::create_empty_ntfs;
}

bool is_generated_task(const PartitionTask& task) noexcept {
  return task.kind == PartitionTaskKind::recreate_efi_system ||
      task.kind == PartitionTaskKind::recreate_microsoft_reserved;
}

bool is_raw_task(const PartitionTask& task) noexcept {
  return task.kind == PartitionTaskKind::copy_exact_raw;
}

bool ranges_overlap(
    const std::uint64_t left_begin,
    const std::uint64_t left_end,
    const std::uint64_t right_begin,
    const std::uint64_t right_end) noexcept {
  return left_begin < right_end && right_begin < left_end;
}

}  // namespace

clonecore::Result<TargetPlan> make_target_plan(TargetPlanData data) {
  auto operation_status = operationcore::validate_operation_plan(
      data.operation_plan);
  auto source_status = data.operation_plan.source.has_value()
      ? clonecore::validate_stable_identity(
            data.expected_source,
            *data.operation_plan.source,
            L"直接縮小target contractコピー元")
      : clonecore::Status::failure(contract_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小target contractコピー元",
            L"OperationPlanにコピー元がありません"));
  auto target_status = data.operation_plan.target.has_value()
      ? clonecore::validate_stable_identity(
            data.expected_target,
            *data.operation_plan.target,
            L"直接縮小target contractコピー先")
      : clonecore::Status::failure(contract_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小target contractコピー先",
            L"OperationPlanにコピー先がありません"));
  auto selection_status = clonecore::validate_clone_selection(
      data.expected_source,
      data.expected_source,
      data.expected_target,
      data.expected_target,
      false);
  if (!operation_status || !source_status || !target_status ||
      !selection_status ||
      !stable_identity_exactly_matches(
          data.expected_source, *data.operation_plan.source) ||
      !stable_identity_exactly_matches(
          data.expected_target, *data.operation_plan.target) ||
      data.operation_plan.kind != operationcore::OperationKind::clone ||
      data.operation_plan.expected_work_bytes == 0U ||
      all_zero(data.operation_plan.immutable_payload_hash) ||
      data.expected_source.logical_sector_size != 512U ||
      data.expected_target.logical_sector_size != 512U ||
      data.expected_target.is_system_disk ||
      data.target_is_active_rescue_media ||
      all_zero(data.expected_source_layout_hash) ||
      all_zero(data.expected_target_layout_hash) ||
      all_zero(data.final_layout_hash) || data.tasks.empty() ||
      data.tasks.size() > 128U || data.archive_task_count == 0U ||
      data.checkpoint_offset_bytes != kCheckpointOffsetBytes ||
      data.maximum_archive_upper_bound_bytes == 0U ||
      data.maximum_archive_upper_bound_bytes !=
          data.staging.archive_capacity_bytes) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::unsupported_layout,
        !operation_status
            ? operation_status.error().native_code
            : !source_status
                ? source_status.error().native_code
                : !target_status
                    ? target_status.error().native_code
                    : !selection_status
                        ? selection_status.error().native_code
                        : ERROR_NOT_SUPPORTED,
        L"直接縮小target contract共通条件",
        !operation_status
            ? operation_status.error().message
            : !source_status
                ? source_status.error().message
                : !target_status
                    ? target_status.error().message
                    : !selection_status
                        ? selection_status.error().message
                        : L"clone OperationPlan、512-byte sector、固定非system target、Hash、checkpoint、staging、またはtask件数がtarget transaction条件外です");
  }

  const bool preserve_gpt =
      data.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      data.source_partition_style ==
          migrationcore::MigrationPartitionStyle::gpt &&
      data.partition_style == migrationcore::MigrationPartitionStyle::gpt;
  const bool preserve_mbr =
      data.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      data.source_partition_style ==
          migrationcore::MigrationPartitionStyle::mbr &&
      data.partition_style == migrationcore::MigrationPartitionStyle::mbr;
  const bool mbr_to_gpt =
      data.partition_style_choice ==
          migrationcore::DirectClonePartitionStyleChoice::mbr_to_gpt &&
      data.source_partition_style ==
          migrationcore::MigrationPartitionStyle::mbr &&
      data.partition_style == migrationcore::MigrationPartitionStyle::gpt;
  const bool mbr_binding_valid = preserve_mbr
      ? data.mbr_preserve_binding.has_value() &&
          !all_zero(data.mbr_preserve_binding->source_sector0_hash) &&
          !all_zero(
              data.mbr_preserve_binding->planning_signature_inventory_hash) &&
          data.mbr_preserve_binding->source_disk_signature != 0U &&
          data.mbr_preserve_binding->target_disk_signature != 0U &&
          data.mbr_preserve_binding->source_disk_signature !=
              data.mbr_preserve_binding->target_disk_signature
      : !data.mbr_preserve_binding.has_value();
  if ((!preserve_gpt && !preserve_mbr && !mbr_to_gpt) ||
      (preserve_mbr && data.tasks.size() > 4U) ||
      !mbr_binding_valid ||
      ((preserve_mbr || mbr_to_gpt) &&
       all_zero(data.source_partition_snapshot_hash))) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小target contract partition style",
        L"GPT/MBR preserveまたは認証済みMBR-to-GPTと、必要なraw snapshot/MBR bindingだけを扱います");
  }

  const bool leave_unallocated = data.surplus_allocation ==
      migrationcore::ShrinkSurplusAllocation::leave_unallocated;
  const bool automatic = data.surplus_allocation ==
      migrationcore::ShrinkSurplusAllocation::automatic_proportional;
  const bool selected_data = data.surplus_allocation ==
      migrationcore::ShrinkSurplusAllocation::selected_data_partition;
  if ((!leave_unallocated && !automatic && !selected_data) ||
      (selected_data !=
       data.surplus_target_source_table_index.has_value()) ||
      (data.surplus_target_source_table_index.has_value() &&
       *data.surplus_target_source_table_index == 0U)) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"直接縮小target contract余剰policy",
        L"未割当、比率自動配分、または指定data partitionの3 policyだけを扱います");
  }

  const std::uint64_t checkpoint_end =
      data.checkpoint_offset_bytes + kCheckpointRecordBytes;
  std::map<std::uint32_t, bool> target_numbers;
  std::map<std::uint32_t, bool> source_task_indexes;
  std::uint64_t archive_count{};
  std::uint64_t extension_count{};
  std::uint64_t maximum_archive{};
  std::uint64_t final_end{};
  std::uint64_t windows_task_count{};
  std::uint64_t bios_system_task_count{};
  std::uint64_t efi_system_task_count{};
  std::uint64_t microsoft_reserved_task_count{};
  std::uint64_t recovery_task_count{};
  std::uint64_t active_task_count{};
  const PartitionTask* active_task = nullptr;
  for (const auto& task : data.tasks) {
    std::uint64_t target_end{};
    std::uint64_t source_end{};
    if (task.target_number == 0U ||
        (preserve_mbr && task.target_number > 4U) ||
        !target_numbers.emplace(task.target_number, true).second ||
        task.target_offset_bytes < kStagingAlignmentBytes ||
        task.target_offset_bytes % kStagingAlignmentBytes != 0U ||
        task.construction_size_bytes == 0U ||
        task.target_size_bytes == 0U ||
        task.construction_size_bytes > task.target_size_bytes ||
        task.construction_size_bytes % 512U != 0U ||
        task.target_size_bytes % 512U != 0U ||
        !checked_add(
            task.target_offset_bytes, task.target_size_bytes, target_end) ||
        target_end > data.expected_target.size_bytes ||
        (task.target_offset_bytes < checkpoint_end &&
         data.checkpoint_offset_bytes < target_end)) {
      return failure<TargetPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小target contract task配置",
          L"target番号、offset、construction/final容量、整列、またはcheckpoint分離が不正です");
    }
    for (const auto& previous : data.tasks) {
      if (&previous == &task) {
        break;
      }
      std::uint64_t previous_end{};
      if (!checked_add(
              previous.target_offset_bytes,
              previous.target_size_bytes,
              previous_end) ||
          (task.target_offset_bytes < previous_end &&
           previous.target_offset_bytes < target_end)) {
        return failure<TargetPlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小target contract task重複",
            L"最終target partition extentが重複しています");
      }
    }
    final_end = (std::max)(final_end, target_end);

    windows_task_count += task.role ==
        migrationcore::MigrationPartitionRole::windows;
    bios_system_task_count += task.role ==
        migrationcore::MigrationPartitionRole::bios_system;
    efi_system_task_count += task.role ==
        migrationcore::MigrationPartitionRole::efi_system;
    microsoft_reserved_task_count += task.role ==
        migrationcore::MigrationPartitionRole::microsoft_reserved;
    recovery_task_count += task.role ==
        migrationcore::MigrationPartitionRole::recovery;
    if (task.active) {
      ++active_task_count;
      active_task = &task;
    }

    if (is_archive_task(task)) {
      if (!task.source_table_index.has_value() ||
          *task.source_table_index == 0U ||
          !source_task_indexes.emplace(
              *task.source_table_index, true).second ||
          task.source_size_bytes == 0U || task.source_used_bytes == 0U ||
          task.source_used_bytes > task.source_size_bytes ||
          !checked_add(
              task.source_offset_bytes,
              task.source_size_bytes,
              source_end) ||
          source_end > data.expected_source.size_bytes ||
          !all_zero(task.source_partition_type) ||
          task.original_volume_guid_path.empty() ||
          task.archive_upper_bound_bytes == 0U ||
          task.archive_upper_bound_bytes >
              data.staging.archive_capacity_bytes) {
        return failure<TargetPlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小target contract NTFS task",
            L"source table index/extent、使用量、Volume GUID、またはarchive上限が不正です");
      }
      ++archive_count;
      maximum_archive = (std::max)(
          maximum_archive, task.archive_upper_bound_bytes);
      if (task.construction_size_bytes < task.target_size_bytes) {
        ++extension_count;
      }
    } else if (task.kind == PartitionTaskKind::create_empty_ntfs) {
      if (!task.source_table_index.has_value() ||
          *task.source_table_index == 0U ||
          !source_task_indexes.emplace(
              *task.source_table_index, true).second ||
          task.role != migrationcore::MigrationPartitionRole::data ||
          task.source_size_bytes == 0U || task.source_used_bytes != 0U ||
          !checked_add(
              task.source_offset_bytes,
              task.source_size_bytes,
              source_end) ||
          source_end > data.expected_source.size_bytes ||
          !all_zero(task.source_partition_type) ||
          !task.original_volume_guid_path.empty() ||
          task.archive_upper_bound_bytes != 0U) {
        return failure<TargetPlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小target contract empty NTFS task",
            L"空NTFS taskは一意なsource data extent、used=0、archiveなしでなければなりません");
      }
      if (task.construction_size_bytes < task.target_size_bytes) {
        ++extension_count;
      }
    } else if (is_raw_task(task)) {
      if (!task.source_table_index.has_value() ||
          *task.source_table_index == 0U ||
          !source_task_indexes.emplace(
              *task.source_table_index, true).second ||
          task.role != migrationcore::MigrationPartitionRole::data ||
          task.source_size_bytes == 0U ||
          task.source_used_bytes > task.source_size_bytes ||
          !checked_add(
              task.source_offset_bytes,
              task.source_size_bytes,
              source_end) ||
          source_end > data.expected_source.size_bytes ||
          task.construction_size_bytes != task.source_size_bytes ||
          task.target_size_bytes != task.source_size_bytes ||
          all_zero(task.source_partition_type) ||
          !task.original_volume_guid_path.empty() ||
          task.archive_upper_bound_bytes != 0U) {
        return failure<TargetPlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小target contract exact RAW task",
            L"exact RAWは一意なsource data extent、元区画と同一容量、元partition type、およびarchiveなしでなければなりません");
      }
    } else if (!is_generated_task(task) || task.source_table_index.has_value() ||
               task.source_offset_bytes != 0U ||
               task.source_size_bytes != 0U ||
               task.source_used_bytes != 0U ||
               !all_zero(task.source_partition_type) ||
               !task.original_volume_guid_path.empty() ||
               task.archive_upper_bound_bytes != 0U || task.active ||
               (task.kind == PartitionTaskKind::recreate_efi_system &&
                task.role !=
                    migrationcore::MigrationPartitionRole::efi_system) ||
               (task.kind ==
                    PartitionTaskKind::recreate_microsoft_reserved &&
                task.role != migrationcore::MigrationPartitionRole::
                    microsoft_reserved)) {
      return failure<TargetPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小target contract generated task",
          L"生成partition taskがsource payloadまたはarchiveへ結合されています");
    }
  }
  if (archive_count != data.archive_task_count ||
      extension_count != data.ntfs_extension_task_count ||
      maximum_archive != data.maximum_archive_upper_bound_bytes ||
      final_end > data.expected_target.size_bytes) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"直接縮小target contract task集計",
        L"archive/extension件数、最大archive容量、またはfinal extentが不変集計と一致しません");
  }

  const bool invalid_mbr_boot_roles =
      data.partition_style == migrationcore::MigrationPartitionStyle::mbr &&
      (windows_task_count != 1U || active_task_count != 1U ||
       bios_system_task_count > 1U || efi_system_task_count != 0U ||
       microsoft_reserved_task_count != 0U || active_task == nullptr ||
       !active_task->source_table_index.has_value() ||
       (bios_system_task_count == 1U
            ? active_task->role !=
                migrationcore::MigrationPartitionRole::bios_system
            : active_task->role !=
                migrationcore::MigrationPartitionRole::windows));
  const bool invalid_gpt_boot_roles =
      data.partition_style == migrationcore::MigrationPartitionStyle::gpt &&
      (windows_task_count != 1U || active_task_count != 0U ||
       bios_system_task_count != 0U || efi_system_task_count != 1U ||
       microsoft_reserved_task_count != 1U);
  const bool invalid_data_only_roles = !data.boot_finalization_required &&
      (windows_task_count != 0U || active_task_count != 0U ||
       bios_system_task_count != 0U || recovery_task_count != 0U ||
       efi_system_task_count != 0U ||
       (data.partition_style == migrationcore::MigrationPartitionStyle::mbr
            ? microsoft_reserved_task_count != 0U
            : microsoft_reserved_task_count > 1U));
  if ((data.boot_finalization_required &&
       (invalid_mbr_boot_roles || invalid_gpt_boot_roles ||
        recovery_task_count > 1U)) ||
      invalid_data_only_roles) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小target contract起動役割",
        L"Windows/ESP/MSR/Active/WinREまたはdata-onlyの起動役割集合が一意ではありません");
  }

  std::uint64_t staging_end{};
  std::uint64_t expected_archive_offset{};
  std::uint64_t archive_end{};
  if (data.staging.offset_bytes < kStagingAlignmentBytes ||
      data.staging.offset_bytes % kStagingAlignmentBytes != 0U ||
      data.staging.length_bytes == 0U ||
      data.staging.length_bytes % kStagingAlignmentBytes != 0U ||
      data.staging.control_reserve_bytes != kStagingControlReserveBytes ||
      data.staging.archive_capacity_bytes <
          kStagingFileSystemReserveBytes ||
      data.staging.archive_capacity_bytes % kStagingAlignmentBytes != 0U ||
      !checked_add(
          data.staging.offset_bytes,
          data.staging.length_bytes,
          staging_end) ||
      !checked_add(
          data.staging.offset_bytes,
          data.staging.control_reserve_bytes,
          expected_archive_offset) ||
      !checked_add(
          data.staging.archive_offset_bytes,
          data.staging.archive_capacity_bytes,
          archive_end) ||
      staging_end > data.expected_target.size_bytes ||
      data.expected_target.size_bytes - staging_end <
          kStagingAlignmentBytes ||
      ranges_overlap(
          data.staging.offset_bytes,
          staging_end,
          data.checkpoint_offset_bytes,
          checkpoint_end) ||
      data.staging.archive_offset_bytes != expected_archive_offset ||
      archive_end != staging_end ||
      data.staging.archive_capacity_bytes !=
          data.staging.length_bytes - data.staging.control_reserve_bytes) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小target contract staging",
        L"target-owned staging/control/archiveの範囲または整列が不正です");
  }

  for (const auto& task : data.tasks) {
    std::uint64_t task_end{};
    if (!checked_add(
            task.target_offset_bytes, task.target_size_bytes, task_end)) {
      return failure<TargetPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"直接縮小target contract staging/task終端",
          L"target task終端を再検証できません");
    }
    const bool is_growth_owner =
        data.staging.final_growth_owner_target_number == task.target_number;
    if ((!is_growth_owner &&
         ranges_overlap(
             data.staging.offset_bytes,
             staging_end,
             task.target_offset_bytes,
             task_end)) ||
        (is_growth_owner &&
         (data.staging.offset_bytes !=
              task.target_offset_bytes + task.construction_size_bytes ||
          staging_end != task_end))) {
      return failure<TargetPlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"直接縮小target contract staging/task分離",
          L"stagingは全final taskと非重複、または一意なgrowth ownerのconstruction後端と完全一致しなければなりません");
    }
  }

  if (leave_unallocated &&
      data.staging.final_growth_owner_target_number.has_value()) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"直接縮小target contract未割当policy",
        L"余剰未割当policyにgrowth-owner stagingを設定できません");
  }
  if (!leave_unallocated &&
      !data.staging.final_growth_owner_target_number.has_value()) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"直接縮小target contract余剰growth owner",
        L"余剰配分policyのtarget-owned staging growth ownerがありません");
  }
  if (data.staging.final_growth_owner_target_number.has_value()) {
    const auto owner = std::find_if(
        data.tasks.begin(),
        data.tasks.end(),
        [&](const PartitionTask& task) {
          return task.target_number ==
              *data.staging.final_growth_owner_target_number;
        });
    if (owner == data.tasks.end() || !is_ntfs_reconstruction_task(*owner) ||
        owner->construction_size_bytes >= owner->target_size_bytes ||
        data.staging.offset_bytes !=
            owner->target_offset_bytes + owner->construction_size_bytes ||
        data.staging.length_bytes !=
            owner->target_size_bytes - owner->construction_size_bytes) {
      return failure<TargetPlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"直接縮小target contract staging owner",
          L"stagingが不変NTFS growth extentを正確に所有していません");
    }
  }

  std::map<std::uint32_t, bool> mapped_source_indexes;
  std::map<std::uint32_t, bool> mapped_target_numbers;
  std::map<std::uint32_t, std::uint64_t> transferred_task_mapping_counts;
  std::map<std::uint32_t, std::uint64_t> archive_task_mapping_counts;
  bool selected_surplus_owner_mapped = !selected_data;
  for (const auto& mapping : data.source_partition_mappings) {
    const auto target = mapping.target_number.has_value()
        ? std::find_if(
              data.tasks.begin(),
              data.tasks.end(),
              [&](const PartitionTask& task) {
                return task.target_number == *mapping.target_number;
              })
        : data.tasks.end();
    const bool omitted = mapping.disposition ==
        SourcePartitionDisposition::omitted_unselected;
    const bool transferred = mapping.disposition ==
        SourcePartitionDisposition::transferred_to_target;
    const bool recreated = mapping.disposition ==
        SourcePartitionDisposition::recreated_as_generated_system_partition;
    const bool replaced = mapping.disposition ==
        SourcePartitionDisposition::replaced_by_generated_uefi_boot;
    const auto valid_disposition_count =
        static_cast<unsigned int>(omitted) +
        static_cast<unsigned int>(transferred) +
        static_cast<unsigned int>(recreated) +
        static_cast<unsigned int>(replaced);
    if (mapping.source_table_index == 0U ||
        !mapped_source_indexes.emplace(
            mapping.source_table_index, true).second ||
        (mapping.required && !mapping.selected) ||
        mapping.selected != (mapping.requested || mapping.required) ||
        valid_disposition_count != 1U ||
        (omitted != !mapping.selected) ||
        (!mapping.selected && mapping.target_number.has_value()) ||
        (mapping.target_number.has_value() && target == data.tasks.end()) ||
        (mapping.target_number.has_value() &&
         !mapped_target_numbers.emplace(
             *mapping.target_number, true).second) ||
        (transferred &&
         (!mapping.selected || target == data.tasks.end() ||
          target->source_table_index != mapping.source_table_index ||
          target->role != mapping.role || is_generated_task(*target))) ||
        (recreated &&
         (!mapping.selected || target == data.tasks.end() ||
          target->source_table_index.has_value() ||
          target->role != mapping.role || !is_generated_task(*target) ||
          (mapping.role !=
               migrationcore::MigrationPartitionRole::efi_system &&
           mapping.role != migrationcore::MigrationPartitionRole::
               microsoft_reserved))) ||
        (replaced &&
         (!mapping.selected || mapping.target_number.has_value() ||
          !mbr_to_gpt ||
          mapping.role !=
              migrationcore::MigrationPartitionRole::bios_system))) {
      return failure<TargetPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小target contract source mapping",
          L"source table index、required選択、またはtarget対応が不正です");
    }
    if (transferred) {
      ++transferred_task_mapping_counts[target->target_number];
      if (is_archive_task(*target)) {
        ++archive_task_mapping_counts[target->target_number];
      }
    }
    if (selected_data &&
        mapping.source_table_index ==
            *data.surplus_target_source_table_index) {
      selected_surplus_owner_mapped = mapping.selected &&
          mapping.role == migrationcore::MigrationPartitionRole::data &&
          mapping.target_number ==
              data.staging.final_growth_owner_target_number;
    }
  }
  for (const auto& task : data.tasks) {
    if (task.source_table_index.has_value() &&
        transferred_task_mapping_counts[task.target_number] != 1U) {
      return failure<TargetPlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"直接縮小target contract task/source mapping",
          L"source-bound target taskはexactly one transferred mappingを持たなければなりません");
    }
    if (is_archive_task(task) &&
        archive_task_mapping_counts[task.target_number] != 1U) {
      return failure<TargetPlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"直接縮小target contract archive/source mapping",
          L"全NTFS archive taskはexactly one selected source mappingを持たなければなりません");
    }
  }
  if (!selected_surplus_owner_mapped) {
    return failure<TargetPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"直接縮小target contract指定余剰owner",
        L"reviewed source table indexを選択data targetへ一意に対応できません");
  }

  return clonecore::Result<TargetPlan>::success(TargetPlan(std::move(data)));
}

}  // namespace ytec::directshrink

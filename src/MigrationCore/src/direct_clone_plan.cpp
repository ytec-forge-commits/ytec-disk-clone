#include "ytec/migrationcore/direct_clone_plan.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::migrationcore {
namespace {

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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(planning_error(
      code, native_code, std::move(operation), std::move(message)));
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

bool known_mode_choice(const DirectCloneModeChoice choice) noexcept {
  switch (choice) {
    case DirectCloneModeChoice::automatic:
    case DirectCloneModeChoice::exact:
    case DirectCloneModeChoice::shrink:
      return true;
  }
  return false;
}

bool known_style_choice(
    const DirectClonePartitionStyleChoice choice) noexcept {
  switch (choice) {
    case DirectClonePartitionStyleChoice::preserve:
    case DirectClonePartitionStyleChoice::mbr_to_gpt:
      return true;
  }
  return false;
}

bool known_partition_style(const MigrationPartitionStyle style) noexcept {
  switch (style) {
    case MigrationPartitionStyle::mbr:
    case MigrationPartitionStyle::gpt:
      return true;
  }
  return false;
}

bool known_file_system(const MigrationFileSystem file_system) noexcept {
  switch (file_system) {
    case MigrationFileSystem::none:
    case MigrationFileSystem::ntfs:
    case MigrationFileSystem::fat32:
    case MigrationFileSystem::exfat:
    case MigrationFileSystem::unsupported:
      return true;
  }
  return false;
}

bool known_partition_role(const MigrationPartitionRole role) noexcept {
  switch (role) {
    case MigrationPartitionRole::efi_system:
    case MigrationPartitionRole::microsoft_reserved:
    case MigrationPartitionRole::bios_system:
    case MigrationPartitionRole::windows:
    case MigrationPartitionRole::recovery:
    case MigrationPartitionRole::data:
      return true;
  }
  return false;
}

bool known_surplus_allocation(
    const ShrinkSurplusAllocation allocation) noexcept {
  switch (allocation) {
    case ShrinkSurplusAllocation::automatic_proportional:
    case ShrinkSurplusAllocation::leave_unallocated:
    case ShrinkSurplusAllocation::selected_data_partition:
      return true;
  }
  return false;
}

bool valid_logical_sector_size(const std::uint32_t bytes) noexcept {
  return bytes == 512U || bytes == 4096U;
}

bool is_content_role(const MigrationPartitionRole role) noexcept {
  return role == MigrationPartitionRole::bios_system ||
      role == MigrationPartitionRole::windows ||
      role == MigrationPartitionRole::recovery ||
      role == MigrationPartitionRole::data;
}

struct NormalizedPlanningInput final {
  MigrationPartitionStyle target_style{MigrationPartitionStyle::gpt};
  bool windows_selected{};
  std::vector<DirectClonePartitionSelection> selections;
};

clonecore::Result<NormalizedPlanningInput> normalize_request(
    const DirectClonePlanningRequest& request) {
  if (!known_mode_choice(request.mode_choice) ||
      !known_style_choice(request.partition_style_choice) ||
      !known_partition_style(request.source_style) ||
      !known_surplus_allocation(request.surplus_allocation) ||
      !valid_logical_sector_size(request.source_logical_sector_size) ||
      !valid_logical_sector_size(request.target_logical_sector_size) ||
      request.source_size_bytes == 0U || request.target_size_bytes == 0U ||
      request.source_size_bytes % request.source_logical_sector_size != 0U ||
      request.target_size_bytes % request.target_logical_sector_size != 0U ||
      request.source_partitions.empty() ||
      request.source_partitions.size() > 128U) {
    return failure<NormalizedPlanningInput>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接クローン計画の共通条件",
        L"方式、形式、容量、論理セクター、またはパーティション数が不正です");
  }
  const bool targets_selected_data = request.surplus_allocation ==
      ShrinkSurplusAllocation::selected_data_partition;
  if (targets_selected_data !=
      request.surplus_target_source_table_index.has_value()) {
    return failure<NormalizedPlanningInput>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"直接クローン計画の余剰容量対象",
        L"指定データ領域への配分にはコピー元パーティション表番号が1つ必要です");
  }

  MigrationPartitionStyle target_style = request.source_style;
  if (request.partition_style_choice ==
      DirectClonePartitionStyleChoice::mbr_to_gpt) {
    if (request.source_style != MigrationPartitionStyle::mbr ||
        !request.mbr_to_gpt_eligible) {
      return failure<NormalizedPlanningInput>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"直接クローン計画のMBRからGPT変換",
          L"MBRコピー元と、読取り専用解析で確認済みの変換適格性が必要です");
    }
    target_style = MigrationPartitionStyle::gpt;
  }

  constexpr std::uint64_t kMbrAddressableSectors =
      static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) +
      1ULL;
  if (request.source_style == MigrationPartitionStyle::mbr &&
      request.source_size_bytes / request.source_logical_sector_size >
          kMbrAddressableSectors) {
    return failure<NormalizedPlanningInput>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接クローン計画のMBRコピー元",
        L"コピー元がMBRの32bit LBA範囲を超えています");
  }

  std::size_t windows_count = 0U;
  std::size_t efi_count = 0U;
  std::size_t msr_count = 0U;
  std::size_t bios_system_count = 0U;
  std::size_t active_count = 0U;
  std::uint64_t total_partition_bytes = 0U;
  bool windows_selected = false;

  for (std::size_t index = 0U;
       index < request.source_partitions.size();
       ++index) {
    const auto& entry = request.source_partitions[index];
    const auto& partition = entry.partition;
    if (!known_partition_role(partition.role) ||
        !known_file_system(partition.file_system) ||
        partition.source_size_bytes == 0U ||
        partition.source_size_bytes % request.source_logical_sector_size !=
            0U ||
        partition.used_bytes > partition.source_size_bytes ||
        (partition.minimum_target_bytes != 0U &&
         partition.minimum_target_bytes < partition.used_bytes) ||
        partition.label.size() > 32U ||
        partition.label.find(L'\0') != std::wstring::npos ||
        (entry.required_for_windows &&
         partition.role != MigrationPartitionRole::recovery)) {
      return failure<NormalizedPlanningInput>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接クローン計画のコピー元パーティション",
          L"役割、ファイルシステム、容量、ラベル、または必須領域指定が不正です");
    }
    if (!checked_add(
            total_partition_bytes,
            partition.source_size_bytes,
            total_partition_bytes)) {
      return failure<NormalizedPlanningInput>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"直接クローン計画のコピー元容量",
          L"パーティション容量の合計がオーバーフローしました");
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (request.source_partitions[previous]
              .partition.source_table_index ==
          partition.source_table_index) {
        return failure<NormalizedPlanningInput>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"直接クローン計画のパーティション番号",
            L"コピー元パーティション表番号が重複しています");
      }
    }

    switch (partition.role) {
      case MigrationPartitionRole::windows:
        ++windows_count;
        windows_selected = windows_selected || entry.selected;
        break;
      case MigrationPartitionRole::efi_system:
        ++efi_count;
        break;
      case MigrationPartitionRole::microsoft_reserved:
        ++msr_count;
        break;
      case MigrationPartitionRole::bios_system:
        ++bios_system_count;
        break;
      case MigrationPartitionRole::recovery:
      case MigrationPartitionRole::data:
        break;
    }
    if (is_content_role(partition.role) && partition.active) {
      ++active_count;
    }
  }

  if (total_partition_bytes > request.source_size_bytes) {
    return failure<NormalizedPlanningInput>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接クローン計画のコピー元容量",
        L"パーティション容量の合計がコピー元ディスク容量を超えています");
  }

  const bool windows_layout_invalid = request.source_is_windows_system &&
      (windows_count != 1U ||
       (request.source_style == MigrationPartitionStyle::gpt
            ? efi_count != 1U || msr_count != 1U ||
                  bios_system_count != 0U
            : efi_count != 0U || msr_count != 0U ||
                  bios_system_count > 1U || active_count != 1U));
  const bool data_layout_invalid = !request.source_is_windows_system &&
      (windows_count != 0U || bios_system_count != 0U || efi_count != 0U ||
       active_count != 0U ||
       (request.source_style == MigrationPartitionStyle::gpt
            ? msr_count > 1U
            : msr_count != 0U));
  if (windows_layout_invalid || data_layout_invalid) {
    return failure<NormalizedPlanningInput>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接クローン計画のディスク役割",
        L"Windows起動ディスクまたはデータ専用ディスクの役割を一意に確定できません");
  }

  NormalizedPlanningInput normalized{
      .target_style = target_style,
      .windows_selected = windows_selected,
  };
  normalized.selections.reserve(request.source_partitions.size());
  std::size_t selected_count = 0U;
  for (const auto& entry : request.source_partitions) {
    const auto role = entry.partition.role;
    const bool required = windows_selected &&
        (role == MigrationPartitionRole::windows ||
         (request.source_style == MigrationPartitionStyle::gpt &&
          (role == MigrationPartitionRole::efi_system ||
           role == MigrationPartitionRole::microsoft_reserved)) ||
         (request.source_style == MigrationPartitionStyle::mbr &&
          role == MigrationPartitionRole::bios_system) ||
         (role == MigrationPartitionRole::recovery &&
          entry.required_for_windows));
    const bool selected = entry.selected || required;
    if (selected) {
      ++selected_count;
    }
    normalized.selections.push_back(DirectClonePartitionSelection{
        .source_table_index = entry.partition.source_table_index,
        .role = role,
        .file_system = entry.partition.file_system,
        .requested = entry.selected,
        .selected = selected,
        .required = required,
    });
  }
  if (selected_count == 0U) {
    return failure<NormalizedPlanningInput>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"直接クローン計画のパーティション選択",
        L"コピー対象パーティションが選択されていません");
  }
  if (targets_selected_data) {
    const auto source = std::find_if(
        request.source_partitions.begin(),
        request.source_partitions.end(),
        [&request](const DirectCloneSourcePartition& candidate) {
          return candidate.partition.source_table_index ==
              *request.surplus_target_source_table_index;
        });
    const auto selection = std::find_if(
        normalized.selections.begin(),
        normalized.selections.end(),
        [&request](const DirectClonePartitionSelection& candidate) {
          return candidate.source_table_index ==
              *request.surplus_target_source_table_index;
        });
    if (source == request.source_partitions.end() ||
        selection == normalized.selections.end() || !selection->selected) {
      return failure<NormalizedPlanningInput>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"直接クローン計画の余剰容量対象",
          L"余剰容量の指定先は選択済みパーティション表番号と一致する必要があります");
    }
    if (source->partition.role != MigrationPartitionRole::data ||
        classify_shrink_file_system(source->partition.file_system) !=
            ShrinkFileSystemDisposition::file_archive) {
      return failure<NormalizedPlanningInput>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"直接クローン計画の余剰容量対象",
          L"余剰容量の指定先は選択済みのNTFS・exFAT・FAT32データ領域に限ります");
    }
  }
  return clonecore::Result<NormalizedPlanningInput>::success(
      std::move(normalized));
}

clonecore::Result<ShrinkMigrationPlan> build_layout(
    const DirectClonePlanningRequest& request,
    const NormalizedPlanningInput& normalized,
    const DirectCloneMode mode,
    const ShrinkSurplusAllocation surplus_allocation) {
  if (mode == DirectCloneMode::exact &&
      request.source_logical_sector_size !=
          request.target_logical_sector_size) {
    return failure<ShrinkMigrationPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接クローン通常モードの論理セクター",
        L"異なる論理セクターサイズは縮小移行モードだけで扱えます");
  }

  ShrinkMigrationRequest layout_request{
      .source_style = request.source_style,
      .target_style = normalized.target_style,
      .target_size_bytes = request.target_size_bytes,
      .target_logical_sector_size = request.target_logical_sector_size,
      .source_is_windows_system = normalized.windows_selected,
      .windows_is_amd64 = request.windows_is_amd64,
      .bitlocker_fully_decrypted = request.bitlocker_fully_decrypted,
      .surplus_allocation = surplus_allocation,
      .surplus_target_source_table_index = surplus_allocation ==
              ShrinkSurplusAllocation::selected_data_partition
          ? request.surplus_target_source_table_index
          : std::nullopt,
  };
  layout_request.source_partitions.reserve(request.source_partitions.size());
  for (std::size_t index = 0U;
       index < request.source_partitions.size();
       ++index) {
    if (!normalized.selections[index].selected) {
      continue;
    }
    auto partition = request.source_partitions[index].partition;
    if (mode == DirectCloneMode::exact && is_content_role(partition.role)) {
      partition.used_bytes = 0U;
      partition.minimum_target_bytes = partition.source_size_bytes;
      if (partition.file_system == MigrationFileSystem::ntfs ||
          partition.file_system == MigrationFileSystem::fat32 ||
          partition.file_system == MigrationFileSystem::exfat) {
        partition.cluster_size = request.target_logical_sector_size;
      }
    }
    layout_request.source_partitions.push_back(std::move(partition));
  }
  return plan_shrink_migration(layout_request);
}

const DirectCloneSourcePartition* find_source_partition(
    const DirectClonePlanningRequest& request,
    const std::uint32_t source_table_index) noexcept {
  const auto found = std::find_if(
      request.source_partitions.begin(),
      request.source_partitions.end(),
      [source_table_index](const DirectCloneSourcePartition& candidate) {
        return candidate.partition.source_table_index == source_table_index;
      });
  return found == request.source_partitions.end() ? nullptr : &*found;
}

const DirectClonePartitionSelection* find_selection(
    const NormalizedPlanningInput& normalized,
    const std::uint32_t source_table_index) noexcept {
  const auto found = std::find_if(
      normalized.selections.begin(),
      normalized.selections.end(),
      [source_table_index](const DirectClonePartitionSelection& candidate) {
        return candidate.source_table_index == source_table_index;
      });
  return found == normalized.selections.end() ? nullptr : &*found;
}

}  // namespace

clonecore::Result<DirectClonePlan> plan_direct_clone(
    const DirectClonePlanningRequest& request) {
  auto normalized_result = normalize_request(request);
  if (!normalized_result) {
    return clonecore::Result<DirectClonePlan>::failure(
        normalized_result.error());
  }
  const auto normalized = normalized_result.take_value();

  std::optional<ShrinkMigrationPlan> exact_layout;
  std::optional<clonecore::Error> exact_error;
  auto exact_result = build_layout(
      request,
      normalized,
      DirectCloneMode::exact,
      request.surplus_allocation);
  if (exact_result) {
    exact_layout.emplace(exact_result.take_value());
  } else {
    exact_error.emplace(exact_result.error());
  }

  DirectCloneMode recommended_mode = DirectCloneMode::exact;
  DirectCloneMode selected_mode = DirectCloneMode::exact;
  std::optional<ShrinkMigrationPlan> shrink_layout;

  const bool needs_shrink_layout =
      request.mode_choice == DirectCloneModeChoice::shrink ||
      (request.mode_choice == DirectCloneModeChoice::automatic &&
       !exact_layout.has_value());
  if (needs_shrink_layout) {
    auto shrink_result = build_layout(
        request,
        normalized,
        DirectCloneMode::shrink,
        request.surplus_allocation);
    if (!shrink_result) {
      return clonecore::Result<DirectClonePlan>::failure(
          shrink_result.error());
    }
    shrink_layout.emplace(shrink_result.take_value());
  }

  switch (request.mode_choice) {
    case DirectCloneModeChoice::automatic:
      if (exact_layout) {
        recommended_mode = DirectCloneMode::exact;
        selected_mode = DirectCloneMode::exact;
      } else {
        recommended_mode = DirectCloneMode::shrink;
        selected_mode = DirectCloneMode::shrink;
      }
      break;
    case DirectCloneModeChoice::exact:
      if (!exact_layout) {
        return clonecore::Result<DirectClonePlan>::failure(*exact_error);
      }
      recommended_mode = DirectCloneMode::exact;
      selected_mode = DirectCloneMode::exact;
      break;
    case DirectCloneModeChoice::shrink:
      recommended_mode = exact_layout ? DirectCloneMode::exact
                                      : DirectCloneMode::shrink;
      selected_mode = DirectCloneMode::shrink;
      break;
  }

  const ShrinkMigrationPlan& selected_layout =
      selected_mode == DirectCloneMode::exact ? *exact_layout : *shrink_layout;
  auto minimum_layout_result = build_layout(
      request,
      normalized,
      selected_mode,
      ShrinkSurplusAllocation::leave_unallocated);
  if (!minimum_layout_result) {
    return clonecore::Result<DirectClonePlan>::failure(
        minimum_layout_result.error());
  }
  const auto minimum_layout = minimum_layout_result.take_value();
  if (minimum_layout.target_partitions.size() !=
      selected_layout.target_partitions.size()) {
    return failure<DirectClonePlan>(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"直接クローン計画の余剰容量配分",
        L"最小配置と最終配置のパーティション数が一致しません");
  }

  DirectClonePlan plan;
  plan.requested_mode_choice_ = request.mode_choice;
  plan.recommended_mode_ = recommended_mode;
  plan.mode_ = selected_mode;
  plan.mode_was_overridden_ =
      request.mode_choice != DirectCloneModeChoice::automatic &&
      selected_mode != recommended_mode;
  plan.partition_style_choice_ = request.partition_style_choice;
  plan.source_style_ = request.source_style;
  plan.target_style_ = normalized.target_style;
  plan.source_size_bytes_ = request.source_size_bytes;
  plan.source_logical_sector_size_ = request.source_logical_sector_size;
  plan.target_logical_sector_size_ = request.target_logical_sector_size;
  plan.surplus_allocation_ = request.surplus_allocation;
  plan.surplus_target_source_table_index_ =
      request.surplus_target_source_table_index;
  plan.minimum_target_size_bytes_ =
      selected_layout.minimum_target_size_bytes;
  plan.target_size_bytes_ = selected_layout.target_size_bytes;
  plan.unallocated_tail_bytes_ = selected_layout.unallocated_tail_bytes;
  plan.source_remains_unchanged_ = selected_layout.source_remains_unchanged;
  plan.boot_finalization_required_ =
      selected_layout.boot_finalization_required;
  plan.partition_selection_ = normalized.selections;
  plan.target_partitions_.reserve(selected_layout.target_partitions.size());

  for (std::size_t index = 0U;
       index < selected_layout.target_partitions.size();
       ++index) {
    const auto& partition = selected_layout.target_partitions[index];
    const auto& minimum = minimum_layout.target_partitions[index];
    if (partition.target_number != minimum.target_number ||
        partition.source_table_index != minimum.source_table_index ||
        partition.role != minimum.role ||
        partition.file_system != minimum.file_system ||
        partition.offset_bytes < minimum.offset_bytes ||
        partition.size_bytes < minimum.size_bytes) {
      return failure<DirectClonePlan>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"直接クローン計画の余剰容量配分",
          L"最小配置と最終配置の対応関係が一致しません");
    }

    const DirectCloneSourcePartition* source = nullptr;
    const DirectClonePartitionSelection* selection = nullptr;
    if (partition.source_table_index) {
      source = find_source_partition(request, *partition.source_table_index);
      selection = find_selection(normalized, *partition.source_table_index);
      if (source == nullptr || selection == nullptr || !selection->selected) {
        return failure<DirectClonePlan>(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_DATA,
            L"直接クローン計画のパーティション対応",
            L"コピー元選択とコピー先配置を一意に対応付けできません");
      }
    }

    plan.target_partitions_.push_back(DirectClonePlannedPartition{
        .target_number = partition.target_number,
        .source_table_index = partition.source_table_index,
        .role = partition.role,
        .file_system = partition.file_system,
        .transfer = source == nullptr
            ? DirectClonePartitionTransfer::recreate
            : selected_mode == DirectCloneMode::exact ||
                    partition.action == MigrationPartitionAction::copy_exact_raw
                ? DirectClonePartitionTransfer::exact_content
                : DirectClonePartitionTransfer::file_system_content,
        .offset_bytes = partition.offset_bytes,
        .minimum_size_bytes = minimum.size_bytes,
        .size_bytes = partition.size_bytes,
        .source_size_bytes = source == nullptr
            ? 0U
            : source->partition.source_size_bytes,
        .source_used_bytes = source == nullptr
            ? 0U
            : source->partition.used_bytes,
        .label = partition.label,
        .active = partition.active,
        .required = selection == nullptr
            ? normalized.windows_selected &&
                (partition.role == MigrationPartitionRole::efi_system ||
                 partition.role ==
                     MigrationPartitionRole::microsoft_reserved)
            : selection->required,
    });
  }

  return clonecore::Result<DirectClonePlan>::success(std::move(plan));
}

}  // namespace ytec::migrationcore

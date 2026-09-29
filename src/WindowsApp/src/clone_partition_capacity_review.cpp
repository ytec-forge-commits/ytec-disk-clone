#include "ytec/windowsapp/clone_partition_capacity_review.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::uintptr_t kAutomaticPolicyItemData = 1U;
constexpr std::uintptr_t kSelectedDataPolicyItemData = 2U;
constexpr std::uintptr_t kUnallocatedPolicyItemData = 3U;

constexpr std::array<WindowsCloneSurplusPolicyOption, 3U> kPolicyOptions{
    WindowsCloneSurplusPolicyOption{
        .allocation =
            migrationcore::ShrinkSurplusAllocation::automatic_proportional,
        .item_data = kAutomaticPolicyItemData,
        .label = L"自動（推奨）",
    },
    WindowsCloneSurplusPolicyOption{
        .allocation =
            migrationcore::ShrinkSurplusAllocation::selected_data_partition,
        .item_data = kSelectedDataPolicyItemData,
        .label = L"指定したデータ領域へ配分",
    },
    WindowsCloneSurplusPolicyOption{
        .allocation =
            migrationcore::ShrinkSurplusAllocation::leave_unallocated,
        .item_data = kUnallocatedPolicyItemData,
        .label = L"未割当のまま残す",
    },
};

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

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool known_partition_style(
    const migrationcore::MigrationPartitionStyle style) noexcept {
  switch (style) {
    case migrationcore::MigrationPartitionStyle::mbr:
    case migrationcore::MigrationPartitionStyle::gpt:
      return true;
  }
  return false;
}

bool known_partition_role(
    const migrationcore::MigrationPartitionRole role) noexcept {
  switch (role) {
    case migrationcore::MigrationPartitionRole::efi_system:
    case migrationcore::MigrationPartitionRole::microsoft_reserved:
    case migrationcore::MigrationPartitionRole::bios_system:
    case migrationcore::MigrationPartitionRole::windows:
    case migrationcore::MigrationPartitionRole::recovery:
    case migrationcore::MigrationPartitionRole::data:
      return true;
  }
  return false;
}

bool known_file_system(
    const migrationcore::MigrationFileSystem file_system) noexcept {
  switch (file_system) {
    case migrationcore::MigrationFileSystem::none:
    case migrationcore::MigrationFileSystem::ntfs:
    case migrationcore::MigrationFileSystem::fat32:
    case migrationcore::MigrationFileSystem::exfat:
    case migrationcore::MigrationFileSystem::unsupported:
      return true;
  }
  return false;
}

bool is_content_role(
    const migrationcore::MigrationPartitionRole role) noexcept {
  return role == migrationcore::MigrationPartitionRole::bios_system ||
      role == migrationcore::MigrationPartitionRole::windows ||
      role == migrationcore::MigrationPartitionRole::recovery ||
      role == migrationcore::MigrationPartitionRole::data;
}

bool is_required_row(
    const WindowsClonePartitionCapacityBinding& binding,
    const WindowsClonePartitionCapacityCandidate& candidate) noexcept {
  if (!binding.source.is_system_disk) {
    return false;
  }
  const auto role = candidate.partition.role;
  return role == migrationcore::MigrationPartitionRole::windows ||
      (binding.source_partition_style ==
               migrationcore::MigrationPartitionStyle::gpt &&
       (role == migrationcore::MigrationPartitionRole::efi_system ||
        role == migrationcore::MigrationPartitionRole::microsoft_reserved)) ||
      (binding.source_partition_style ==
               migrationcore::MigrationPartitionStyle::mbr &&
       role == migrationcore::MigrationPartitionRole::bios_system) ||
      (role == migrationcore::MigrationPartitionRole::recovery &&
       candidate.required_for_windows);
}

}  // namespace

std::span<const WindowsCloneSurplusPolicyOption>
windows_clone_surplus_policy_options() noexcept {
  return kPolicyOptions;
}

std::optional<std::uintptr_t>
encode_windows_clone_surplus_policy_item_data(
    const migrationcore::ShrinkSurplusAllocation allocation) noexcept {
  for (const auto& option : kPolicyOptions) {
    if (option.allocation == allocation) {
      return option.item_data;
    }
  }
  return std::nullopt;
}

std::optional<migrationcore::ShrinkSurplusAllocation>
decode_windows_clone_surplus_policy_item_data(
    const std::uintptr_t item_data) noexcept {
  for (const auto& option : kPolicyOptions) {
    if (option.item_data == item_data) {
      return option.allocation;
    }
  }
  return std::nullopt;
}

std::optional<std::uintptr_t> encode_windows_clone_partition_item_data(
    const std::uint32_t source_table_index) noexcept {
  if (source_table_index == 0U) {
    return std::nullopt;
  }
  return static_cast<std::uintptr_t>(source_table_index);
}

std::optional<std::uint32_t> decode_windows_clone_partition_item_data(
    const std::uintptr_t item_data) noexcept {
  if (item_data == 0U ||
      item_data > (std::numeric_limits<std::uint32_t>::max)()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(item_data);
}

clonecore::Result<WindowsClonePartitionCapacityReview>
build_windows_clone_partition_capacity_review(
    const WindowsClonePartitionCapacityBinding& binding,
    const std::span<const WindowsClonePartitionCapacityCandidate> candidates) {
  const auto identity = clonecore::validate_stable_identity(
      binding.source, binding.source, L"パーティション選択コピー元");
  if (!identity) {
    return clonecore::Result<WindowsClonePartitionCapacityReview>::failure(
        identity.error());
  }
  if (!known_partition_style(binding.source_partition_style) ||
      all_zero(binding.source_layout_hash) ||
      all_zero(binding.source_analysis_hash) || candidates.empty() ||
      candidates.size() > 128U) {
    return failure<WindowsClonePartitionCapacityReview>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"パーティション選択レビューの解析binding",
        L"コピー元形式、layout Hash、analysis Hash、またはパーティション数が不正です");
  }

  std::size_t windows_count{};
  std::size_t efi_count{};
  std::size_t msr_count{};
  std::size_t bios_system_count{};
  std::size_t active_count{};
  for (std::size_t index = 0U; index < candidates.size(); ++index) {
    const auto& candidate = candidates[index];
    const auto& partition = candidate.partition;
    if (partition.source_table_index == 0U ||
        !known_partition_role(partition.role) ||
        !known_file_system(partition.file_system) ||
        partition.source_size_bytes == 0U ||
        partition.used_bytes > partition.source_size_bytes ||
        partition.label.size() > 32U ||
        partition.label.find(L'\0') != std::wstring::npos ||
        (candidate.required_for_windows &&
         partition.role !=
             migrationcore::MigrationPartitionRole::recovery)) {
      return failure<WindowsClonePartitionCapacityReview>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"パーティション選択レビューの候補",
          L"table index、役割、filesystem、容量、ラベル、または必須指定が不正です");
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (candidates[previous].partition.source_table_index ==
          partition.source_table_index) {
        return failure<WindowsClonePartitionCapacityReview>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"パーティション選択レビューのtable index",
            L"source table indexが重複しています");
      }
    }

    switch (partition.role) {
      case migrationcore::MigrationPartitionRole::windows:
        ++windows_count;
        break;
      case migrationcore::MigrationPartitionRole::efi_system:
        ++efi_count;
        break;
      case migrationcore::MigrationPartitionRole::microsoft_reserved:
        ++msr_count;
        break;
      case migrationcore::MigrationPartitionRole::bios_system:
        ++bios_system_count;
        break;
      case migrationcore::MigrationPartitionRole::recovery:
      case migrationcore::MigrationPartitionRole::data:
        break;
    }
    if (is_content_role(partition.role) && partition.active) {
      ++active_count;
    }
  }

  const bool system_layout_invalid = binding.source.is_system_disk &&
      (windows_count != 1U ||
       (binding.source_partition_style ==
                migrationcore::MigrationPartitionStyle::gpt
            ? efi_count != 1U || msr_count != 1U ||
                  bios_system_count != 0U
            : efi_count != 0U || msr_count != 0U ||
                  bios_system_count > 1U || active_count != 1U));
  const bool data_layout_invalid = !binding.source.is_system_disk &&
      (windows_count != 0U || efi_count != 0U ||
       bios_system_count != 0U || active_count != 0U ||
       (binding.source_partition_style ==
                migrationcore::MigrationPartitionStyle::gpt
            ? msr_count > 1U
            : msr_count != 0U));
  if (system_layout_invalid || data_layout_invalid) {
    return failure<WindowsClonePartitionCapacityReview>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"パーティション選択レビューのWindows役割",
        L"Windows起動ディスクまたはデータ専用ディスクの役割を一意に確定できません");
  }

  WindowsClonePartitionCapacityReview review;
  review.binding_ = binding;
  review.rows_.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    review.rows_.push_back(WindowsClonePartitionCapacityRow{
        .partition = candidate.partition,
        .selected_by_default = true,
        .required = is_required_row(binding, candidate),
        .eligible_surplus_target = candidate.partition.role ==
                migrationcore::MigrationPartitionRole::data &&
            candidate.partition.file_system ==
                migrationcore::MigrationFileSystem::ntfs,
    });
  }
  return clonecore::Result<WindowsClonePartitionCapacityReview>::success(
      std::move(review));
}

clonecore::Result<WindowsClonePartitionCapacityDecision>
complete_windows_clone_partition_capacity_review(
    const WindowsClonePartitionCapacityReview& review,
    const WindowsClonePartitionCapacitySubmission& submission) {
  auto identity = clonecore::validate_stable_identity(
      review.binding().source,
      submission.revalidated_binding.source,
      L"パーティション選択コピー元");
  if (!identity) {
    return clonecore::Result<WindowsClonePartitionCapacityDecision>::failure(
        identity.error());
  }
  if (review.binding().source.is_system_disk !=
          submission.revalidated_binding.source.is_system_disk ||
      review.binding().source_partition_style !=
          submission.revalidated_binding.source_partition_style ||
      review.binding().source_layout_hash !=
          submission.revalidated_binding.source_layout_hash ||
      review.binding().source_analysis_hash !=
          submission.revalidated_binding.source_analysis_hash) {
    return failure<WindowsClonePartitionCapacityDecision>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"パーティション選択レビューの再検証",
        L"表示後にコピー元のsystem属性、形式、layout、またはanalysisが変化しました");
  }

  std::vector<std::uint32_t> selected_indexes;
  selected_indexes.reserve(submission.selected_partition_item_data.size());
  for (const auto item_data : submission.selected_partition_item_data) {
    const auto decoded =
        decode_windows_clone_partition_item_data(item_data);
    if (!decoded ||
        std::find(selected_indexes.begin(), selected_indexes.end(), *decoded) !=
            selected_indexes.end() ||
        std::find_if(
            review.rows().begin(),
            review.rows().end(),
            [&decoded](const WindowsClonePartitionCapacityRow& row) {
              return row.partition.source_table_index == *decoded;
            }) == review.rows().end()) {
      return failure<WindowsClonePartitionCapacityDecision>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"パーティション選択レビューのitem data",
          L"選択されたsource table indexが無効、重複、またはレビュー対象外です");
    }
    selected_indexes.push_back(*decoded);
  }
  if (selected_indexes.empty()) {
    return failure<WindowsClonePartitionCapacityDecision>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"パーティション選択レビューの選択件数",
        L"コピー対象パーティションが選択されていません");
  }
  for (const auto& row : review.rows()) {
    if (row.required &&
        std::find(
            selected_indexes.begin(),
            selected_indexes.end(),
            row.partition.source_table_index) == selected_indexes.end()) {
      return failure<WindowsClonePartitionCapacityDecision>(
          clonecore::ErrorCode::confirmation_required,
          ERROR_CANCELLED,
          L"パーティション選択レビューの必須領域",
          L"Windows、起動、または必須回復領域の選択は解除できません");
    }
  }

  const auto allocation = decode_windows_clone_surplus_policy_item_data(
      submission.surplus_policy_item_data);
  if (!allocation) {
    return failure<WindowsClonePartitionCapacityDecision>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"パーティション選択レビューの余剰容量方針",
        L"余剰容量方針のitem dataを解釈できません");
  }
  const bool targets_selected_data = *allocation ==
      migrationcore::ShrinkSurplusAllocation::selected_data_partition;
  if (targets_selected_data !=
      submission.surplus_target_partition_item_data.has_value()) {
    return failure<WindowsClonePartitionCapacityDecision>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"パーティション選択レビューの余剰容量対象",
        L"指定データ領域への配分時だけ対象パーティションが必要です");
  }

  std::optional<std::uint32_t> target_index;
  if (targets_selected_data) {
    target_index = decode_windows_clone_partition_item_data(
        *submission.surplus_target_partition_item_data);
    const auto row = target_index
        ? std::find_if(
              review.rows().begin(),
              review.rows().end(),
              [&target_index](const WindowsClonePartitionCapacityRow& item) {
                return item.partition.source_table_index == *target_index;
              })
        : review.rows().end();
    if (!target_index || row == review.rows().end() ||
        !row->eligible_surplus_target ||
        std::find(
            selected_indexes.begin(), selected_indexes.end(), *target_index) ==
            selected_indexes.end()) {
      return failure<WindowsClonePartitionCapacityDecision>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"パーティション選択レビューの余剰容量対象",
          L"余剰容量の指定先は選択済みのNTFSデータ領域に限ります");
    }
  }

  return clonecore::Result<WindowsClonePartitionCapacityDecision>::success(
      WindowsClonePartitionCapacityDecision{
          .binding = review.binding(),
          .selected_source_table_indexes = std::move(selected_indexes),
          .surplus_allocation = *allocation,
          .surplus_target_source_table_index = target_index,
      });
}

}  // namespace ytec::windowsapp

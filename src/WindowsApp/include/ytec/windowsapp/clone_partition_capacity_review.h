#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/migrationcore/shrink_layout.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

struct WindowsClonePartitionCapacityBinding final {
  clonecore::StableDiskIdentity source;
  migrationcore::MigrationPartitionStyle source_partition_style{
      migrationcore::MigrationPartitionStyle::gpt};
  imageformat::Sha256Digest source_layout_hash{};
  imageformat::Sha256Digest source_analysis_hash{};
};

struct WindowsClonePartitionCapacityCandidate final {
  migrationcore::ShrinkSourcePartition partition;
  // Only a Recovery row may carry this analysis-derived flag. Windows and
  // boot metadata are always derived as required from their authenticated
  // roles and cannot be weakened by the UI.
  bool required_for_windows{};
};

struct WindowsClonePartitionCapacityRow final {
  migrationcore::ShrinkSourcePartition partition;
  bool selected_by_default{true};
  bool required{};
  bool eligible_surplus_target{};
};

struct WindowsCloneSurplusPolicyOption final {
  migrationcore::ShrinkSurplusAllocation allocation{
      migrationcore::ShrinkSurplusAllocation::automatic_proportional};
  std::uintptr_t item_data{};
  std::wstring_view label;
};

// Construction is restricted to the pure builder. The review has no mutating
// API, so the source identity and the two read-only analysis digests shown to
// the user remain the exact values checked when the choice is completed.
class WindowsClonePartitionCapacityReview final {
 public:
  WindowsClonePartitionCapacityReview(
      const WindowsClonePartitionCapacityReview&) = default;
  WindowsClonePartitionCapacityReview(
      WindowsClonePartitionCapacityReview&&) noexcept = default;
  WindowsClonePartitionCapacityReview& operator=(
      const WindowsClonePartitionCapacityReview&) = delete;
  WindowsClonePartitionCapacityReview& operator=(
      WindowsClonePartitionCapacityReview&&) = delete;

  [[nodiscard]] const WindowsClonePartitionCapacityBinding& binding()
      const noexcept {
    return binding_;
  }
  [[nodiscard]] std::span<const WindowsClonePartitionCapacityRow> rows()
      const noexcept {
    return rows_;
  }
  [[nodiscard]] migrationcore::ShrinkSurplusAllocation
  default_surplus_allocation() const noexcept {
    return migrationcore::ShrinkSurplusAllocation::automatic_proportional;
  }

 private:
  WindowsClonePartitionCapacityReview() = default;

  WindowsClonePartitionCapacityBinding binding_;
  std::vector<WindowsClonePartitionCapacityRow> rows_;

  friend clonecore::Result<WindowsClonePartitionCapacityReview>
  build_windows_clone_partition_capacity_review(
      const WindowsClonePartitionCapacityBinding& binding,
      std::span<const WindowsClonePartitionCapacityCandidate> candidates);
};

struct WindowsClonePartitionCapacitySubmission final {
  WindowsClonePartitionCapacityBinding revalidated_binding;
  // These are decoded from Win32 list item data, not row positions.
  std::vector<std::uintptr_t> selected_partition_item_data;
  std::uintptr_t surplus_policy_item_data{};
  std::optional<std::uintptr_t> surplus_target_partition_item_data;
};

struct WindowsClonePartitionCapacityDecision final {
  WindowsClonePartitionCapacityBinding binding;
  std::vector<std::uint32_t> selected_source_table_indexes;
  migrationcore::ShrinkSurplusAllocation surplus_allocation{
      migrationcore::ShrinkSurplusAllocation::automatic_proportional};
  std::optional<std::uint32_t> surplus_target_source_table_index;
};

[[nodiscard]] std::span<const WindowsCloneSurplusPolicyOption>
windows_clone_surplus_policy_options() noexcept;

[[nodiscard]] std::optional<std::uintptr_t>
encode_windows_clone_surplus_policy_item_data(
    migrationcore::ShrinkSurplusAllocation allocation) noexcept;

[[nodiscard]] std::optional<migrationcore::ShrinkSurplusAllocation>
decode_windows_clone_surplus_policy_item_data(
    std::uintptr_t item_data) noexcept;

[[nodiscard]] std::optional<std::uintptr_t>
encode_windows_clone_partition_item_data(
    std::uint32_t source_table_index) noexcept;

[[nodiscard]] std::optional<std::uint32_t>
decode_windows_clone_partition_item_data(std::uintptr_t item_data) noexcept;

// Pure functions: no disk enumeration, handle open, analysis, UAC, or write.
[[nodiscard]] clonecore::Result<WindowsClonePartitionCapacityReview>
build_windows_clone_partition_capacity_review(
    const WindowsClonePartitionCapacityBinding& binding,
    std::span<const WindowsClonePartitionCapacityCandidate> candidates);

[[nodiscard]] clonecore::Result<WindowsClonePartitionCapacityDecision>
complete_windows_clone_partition_capacity_review(
    const WindowsClonePartitionCapacityReview& review,
    const WindowsClonePartitionCapacitySubmission& submission);

}  // namespace ytec::windowsapp

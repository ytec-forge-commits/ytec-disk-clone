#pragma once

#include "ytec/migrationcore/shrink_layout.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::migrationcore {

enum class DirectCloneModeChoice : std::uint8_t {
  automatic,
  exact,
  shrink,
};

enum class DirectCloneMode : std::uint8_t {
  exact,
  shrink,
};

enum class DirectClonePartitionStyleChoice : std::uint8_t {
  preserve,
  mbr_to_gpt,
};

enum class DirectClonePartitionTransfer : std::uint8_t {
  recreate,
  exact_content,
  file_system_content,
};

struct DirectCloneSourcePartition final {
  ShrinkSourcePartition partition;
  bool selected{true};
  // Only recovery partitions may use this flag. Boot/system roles are derived
  // from the authenticated role and cannot be downgraded by a caller.
  bool required_for_windows{};
};

struct DirectClonePlanningRequest final {
  DirectCloneModeChoice mode_choice{DirectCloneModeChoice::automatic};
  DirectClonePartitionStyleChoice partition_style_choice{
      DirectClonePartitionStyleChoice::preserve};
  MigrationPartitionStyle source_style{MigrationPartitionStyle::gpt};
  std::uint64_t source_size_bytes{};
  std::uint32_t source_logical_sector_size{};
  std::uint64_t target_size_bytes{};
  std::uint32_t target_logical_sector_size{};
  bool source_is_windows_system{};
  bool windows_is_amd64{};
  bool bitlocker_fully_decrypted{};
  // Set only after the read-only source analysis has established every
  // prerequisite needed by the later conversion executor.
  bool mbr_to_gpt_eligible{};
  ShrinkSurplusAllocation surplus_allocation{
      ShrinkSurplusAllocation::automatic_proportional};
  std::optional<std::uint32_t> surplus_target_source_table_index;
  std::vector<DirectCloneSourcePartition> source_partitions;
};

struct DirectClonePartitionSelection final {
  std::uint32_t source_table_index{};
  MigrationPartitionRole role{MigrationPartitionRole::data};
  MigrationFileSystem file_system{MigrationFileSystem::ntfs};
  bool requested{};
  bool selected{};
  bool required{};
};

struct DirectClonePlannedPartition final {
  std::uint32_t target_number{};
  std::optional<std::uint32_t> source_table_index;
  MigrationPartitionRole role{MigrationPartitionRole::data};
  MigrationFileSystem file_system{MigrationFileSystem::ntfs};
  DirectClonePartitionTransfer transfer{
      DirectClonePartitionTransfer::recreate};
  std::uint64_t offset_bytes{};
  std::uint64_t minimum_size_bytes{};
  std::uint64_t size_bytes{};
  std::uint64_t source_size_bytes{};
  std::uint64_t source_used_bytes{};
  std::wstring label;
  bool active{};
  bool required{};
};

// Construction is restricted to plan_direct_clone(). The resulting value has
// no mutating API, so WindowsApp and WinPEApp can review and hash the same
// normalized partition choices before any I/O adapter is invoked.
class DirectClonePlan final {
 public:
  DirectClonePlan(const DirectClonePlan&) = default;
  DirectClonePlan(DirectClonePlan&&) noexcept = default;
  DirectClonePlan& operator=(const DirectClonePlan&) = delete;
  DirectClonePlan& operator=(DirectClonePlan&&) = delete;

  [[nodiscard]] DirectCloneModeChoice requested_mode_choice() const noexcept {
    return requested_mode_choice_;
  }
  [[nodiscard]] DirectCloneMode recommended_mode() const noexcept {
    return recommended_mode_;
  }
  [[nodiscard]] DirectCloneMode mode() const noexcept { return mode_; }
  [[nodiscard]] bool mode_was_overridden() const noexcept {
    return mode_was_overridden_;
  }
  [[nodiscard]] DirectClonePartitionStyleChoice partition_style_choice()
      const noexcept {
    return partition_style_choice_;
  }
  [[nodiscard]] MigrationPartitionStyle source_style() const noexcept {
    return source_style_;
  }
  [[nodiscard]] MigrationPartitionStyle target_style() const noexcept {
    return target_style_;
  }
  [[nodiscard]] std::uint64_t source_size_bytes() const noexcept {
    return source_size_bytes_;
  }
  [[nodiscard]] std::uint32_t source_logical_sector_size() const noexcept {
    return source_logical_sector_size_;
  }
  [[nodiscard]] std::uint32_t target_logical_sector_size() const noexcept {
    return target_logical_sector_size_;
  }
  [[nodiscard]] ShrinkSurplusAllocation surplus_allocation() const noexcept {
    return surplus_allocation_;
  }
  [[nodiscard]] std::optional<std::uint32_t>
  surplus_target_source_table_index() const noexcept {
    return surplus_target_source_table_index_;
  }
  [[nodiscard]] std::uint64_t minimum_target_size_bytes() const noexcept {
    return minimum_target_size_bytes_;
  }
  [[nodiscard]] std::uint64_t target_size_bytes() const noexcept {
    return target_size_bytes_;
  }
  [[nodiscard]] std::uint64_t unallocated_tail_bytes() const noexcept {
    return unallocated_tail_bytes_;
  }
  [[nodiscard]] bool source_remains_unchanged() const noexcept {
    return source_remains_unchanged_;
  }
  [[nodiscard]] bool boot_finalization_required() const noexcept {
    return boot_finalization_required_;
  }
  [[nodiscard]] const std::vector<DirectClonePartitionSelection>&
  partition_selection() const noexcept {
    return partition_selection_;
  }
  [[nodiscard]] const std::vector<DirectClonePlannedPartition>&
  target_partitions() const noexcept {
    return target_partitions_;
  }

 private:
  DirectClonePlan() = default;

  DirectCloneModeChoice requested_mode_choice_{
      DirectCloneModeChoice::automatic};
  DirectCloneMode recommended_mode_{DirectCloneMode::exact};
  DirectCloneMode mode_{DirectCloneMode::exact};
  bool mode_was_overridden_{};
  DirectClonePartitionStyleChoice partition_style_choice_{
      DirectClonePartitionStyleChoice::preserve};
  MigrationPartitionStyle source_style_{MigrationPartitionStyle::gpt};
  MigrationPartitionStyle target_style_{MigrationPartitionStyle::gpt};
  std::uint64_t source_size_bytes_{};
  std::uint32_t source_logical_sector_size_{};
  std::uint32_t target_logical_sector_size_{};
  ShrinkSurplusAllocation surplus_allocation_{
      ShrinkSurplusAllocation::automatic_proportional};
  std::optional<std::uint32_t> surplus_target_source_table_index_;
  std::uint64_t minimum_target_size_bytes_{};
  std::uint64_t target_size_bytes_{};
  std::uint64_t unallocated_tail_bytes_{};
  bool source_remains_unchanged_{true};
  bool boot_finalization_required_{};
  std::vector<DirectClonePartitionSelection> partition_selection_;
  std::vector<DirectClonePlannedPartition> target_partitions_;

  friend clonecore::Result<DirectClonePlan> plan_direct_clone(
      const DirectClonePlanningRequest& request);
};

// Pure planner: performs no enumeration, handle open, UAC request, disk read,
// or disk write. A selected unsupported filesystem is retained at its exact
// source size and marked exact_content; product executors must use their
// verified read-only RAW path for that partition.
[[nodiscard]] clonecore::Result<DirectClonePlan> plan_direct_clone(
    const DirectClonePlanningRequest& request);

}  // namespace ytec::migrationcore

#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::migrationcore {

enum class MigrationPartitionStyle : std::uint8_t {
  mbr,
  gpt,
};

enum class MigrationFileSystem : std::uint8_t {
  none,
  ntfs,
  fat32,
  exfat,
  unsupported,
};

enum class ShrinkFileSystemDisposition : std::uint8_t {
  metadata_only,
  file_archive,
  exact_raw_only,
};

enum class ShrinkSurplusAllocation : std::uint8_t {
  automatic_proportional = 0,
  leave_unallocated = 1,
  selected_data_partition = 2,
};

enum class MigrationPartitionRole : std::uint8_t {
  efi_system,
  microsoft_reserved,
  bios_system,
  windows,
  recovery,
  data,
};

enum class MigrationPartitionAction : std::uint8_t {
  create_fat32,
  create_reserved,
  apply_file_image,
  create_empty_ntfs,
  create_empty_exfat,
  create_empty_fat32,
  copy_exact_raw,
};

struct ShrinkSourcePartition final {
  std::uint32_t source_table_index{};
  MigrationPartitionRole role{MigrationPartitionRole::data};
  MigrationFileSystem file_system{MigrationFileSystem::ntfs};
  std::uint64_t source_size_bytes{};
  std::uint64_t used_bytes{};
  // An authenticated image manifest may carry a stricter filesystem-specific
  // floor than the generic safety reserve calculated by MigrationCore. Zero
  // keeps the generic policy. A non-zero value may only raise that floor.
  std::uint64_t minimum_target_bytes{};
  std::uint64_t cluster_size{};
  std::wstring label;
  bool active{};
};

struct ShrinkMigrationRequest final {
  MigrationPartitionStyle source_style{MigrationPartitionStyle::gpt};
  MigrationPartitionStyle target_style{MigrationPartitionStyle::gpt};
  std::uint64_t target_size_bytes{};
  std::uint32_t target_logical_sector_size{};
  bool source_is_windows_system{};
  bool windows_is_amd64{};
  bool bitlocker_fully_decrypted{};
  ShrinkSurplusAllocation surplus_allocation{
      ShrinkSurplusAllocation::automatic_proportional};
  // Required only for selected_data_partition. It is the immutable source
  // partition-table index, never a transient UI row position.
  std::optional<std::uint32_t> surplus_target_source_table_index;
  std::vector<ShrinkSourcePartition> source_partitions;
};

struct ShrinkPlannedPartition final {
  std::uint32_t target_number{};
  std::optional<std::uint32_t> source_table_index;
  MigrationPartitionRole role{MigrationPartitionRole::data};
  MigrationFileSystem file_system{MigrationFileSystem::ntfs};
  MigrationPartitionAction action{
      MigrationPartitionAction::apply_file_image};
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
  std::uint64_t source_size_bytes{};
  std::uint64_t source_used_bytes{};
  std::wstring label;
  bool active{};
};

struct ShrinkMigrationPlan final {
  MigrationPartitionStyle target_style{MigrationPartitionStyle::gpt};
  std::uint64_t alignment_bytes{};
  std::uint64_t minimum_target_size_bytes{};
  std::uint64_t target_size_bytes{};
  std::uint64_t unallocated_tail_bytes{};
  std::optional<std::uint32_t> surplus_target_source_table_index;
  bool source_remains_unchanged{true};
  bool boot_finalization_required{};
  std::vector<ShrinkPlannedPartition> target_partitions;
  std::vector<std::wstring> notes;
};

// Builds a target-only reconstruction plan. It performs no I/O and never
// proposes shrinking or modifying the source. NTFS, exFAT, and FAT32 content
// volumes use a file-archive action. An unsupported filesystem is accepted
// only when it fits unchanged and is represented by an exact RAW action.
[[nodiscard]] clonecore::Result<ShrinkMigrationPlan>
plan_shrink_migration(const ShrinkMigrationRequest& request);

[[nodiscard]] ShrinkFileSystemDisposition
classify_shrink_file_system(MigrationFileSystem file_system) noexcept;

// Returns the canonical per-partition floor used by plan_shrink_migration().
// Source analysis uses this same function when sealing a target-independent
// shrink image manifest, so restore cannot silently apply a weaker reserve.
[[nodiscard]] clonecore::Result<std::uint64_t>
minimum_shrink_partition_bytes(const ShrinkSourcePartition& source);

[[nodiscard]] std::wstring_view migration_partition_role_name(
    MigrationPartitionRole role) noexcept;

}  // namespace ytec::migrationcore

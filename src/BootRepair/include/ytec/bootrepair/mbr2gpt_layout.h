#pragma once

#include "ytec/clonecore/gpt.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::bootrepair {

enum class Mbr2GptSourceRole : std::uint8_t {
  system_reserved,
  windows,
  recovery,
  data,
  unsupported,
};

enum class Mbr2GptFileSystem : std::uint8_t {
  ntfs,
  fat32,
  unknown,
  unsupported,
};

enum class WinReSourceState : std::uint8_t {
  registered_partition,
  image_available_in_windows,
  missing,
  unknown,
};

struct Mbr2GptSourcePartition final {
  std::uint32_t number{};
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
  Mbr2GptSourceRole role{Mbr2GptSourceRole::unsupported};
  Mbr2GptFileSystem file_system{Mbr2GptFileSystem::unknown};
  bool primary{};
  bool active{};
};

struct Mbr2GptRebuildRequest final {
  std::uint64_t source_disk_size_bytes{};
  std::uint64_t target_disk_size_bytes{};
  std::uint32_t logical_sector_size{};
  std::vector<Mbr2GptSourcePartition> source_partitions;
  WinReSourceState winre_state{WinReSourceState::unknown};
  std::uint32_t registered_winre_partition_number{};
  std::uint64_t winre_image_size_bytes{};
  bool firmware_supports_uefi{};
  bool windows_is_amd64{};
  bool bitlocker_fully_decrypted{};
  bool require_recovery_tools{true};
};

enum class PlannedGptPartitionRole : std::uint8_t {
  efi_system,
  microsoft_reserved,
  windows,
  recovery,
  data,
};

enum class PlannedGptPartitionAction : std::uint8_t {
  create_fat32,
  create_reserved,
  copy_source_contents,
  create_and_stage_winre,
};

struct PlannedGptPartition final {
  std::uint32_t target_number{};
  PlannedGptPartitionRole role{PlannedGptPartitionRole::data};
  PlannedGptPartitionAction action{
      PlannedGptPartitionAction::copy_source_contents};
  std::uint32_t source_partition_number{};
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
};

struct Mbr2GptRebuildPlan final {
  std::vector<PlannedGptPartition> target_partitions;
  std::uint64_t alignment_bytes{};
  std::uint64_t unallocated_tail_bytes{};
  bool microsoft_in_place_precheck_passed{};
  bool official_mbr2gpt_validate_still_required{true};
  bool source_disk_remains_unchanged{true};
  bool system_partition_replaced_by_esp{};
  bool recovery_partition_created{};
  bool recovery_order_changed{};
  bool bcdboot_required{true};
  bool winre_registration_required{};
  std::vector<std::wstring> notes;
};

// Builds a write-free plan for migration to a separate empty target. It never
// moves or edits the source MBR disk. The plan uses the current Microsoft
// deployment order: ESP, MSR, Windows, recovery tools, then optional data.
[[nodiscard]] clonecore::Result<Mbr2GptRebuildPlan>
plan_mbr2gpt_rebuild_to_new_target(
    const Mbr2GptRebuildRequest& request);

// Materializes the validated semantic layout as protective-MBR/GPT metadata
// bytes in memory. This function performs no device I/O and never applies the
// returned writes.
[[nodiscard]] clonecore::Result<clonecore::GptWritePlan>
make_mbr2gpt_gpt_metadata_plan(
    const Mbr2GptRebuildPlan& layout,
    std::uint64_t target_disk_size_bytes,
    std::uint32_t target_sector_size,
    clonecore::IGuidGenerator& guid_generator);

[[nodiscard]] std::wstring_view planned_gpt_partition_role_name(
    PlannedGptPartitionRole role) noexcept;

}  // namespace ytec::bootrepair

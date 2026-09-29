#pragma once

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/migrationcore/shrink_layout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::windowsshrink {

struct WindowsSourceVersion final {
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t build{};
  std::string architecture;
};

struct ShrinkSourceAnalysisContext final {
  clonecore::StableDiskIdentity source_identity;
  std::uint32_t physical_sector_size{};
  std::string created_utc;
  std::string app_version;
  std::optional<WindowsSourceVersion> known_windows_version;
};

struct AnalyzedShrinkVolume final {
  std::uint32_t source_table_index{};
  std::wstring volume_guid_path;
};

struct AnalyzedShrinkPartition final {
  std::uint32_t source_table_index{};
  migrationcore::MigrationPartitionRole role{
      migrationcore::MigrationPartitionRole::data};
  migrationcore::MigrationFileSystem file_system{
      migrationcore::MigrationFileSystem::ntfs};
  std::uint64_t source_offset_bytes{};
  std::uint64_t source_size_bytes{};
  std::uint64_t used_bytes{};
  std::uint64_t cluster_size{};
  bool active{};
  std::wstring label;
  std::wstring name;
  std::array<std::byte, 16U> type_id{};
  std::array<std::byte, 16U> unique_id{};
};

struct ShrinkSourceAnalysis final {
  clonecore::StableDiskIdentity source;
  std::uint32_t physical_sector_size{};
  migrationcore::MigrationPartitionStyle partition_style{
      migrationcore::MigrationPartitionStyle::gpt};
  std::optional<WindowsSourceVersion> windows_version;
  bool bitlocker_fully_decrypted{};
  std::string created_utc;
  std::string app_version;
  std::array<std::byte, 440U> mbr_bootstrap{};
  std::vector<std::byte> partition_snapshot;
  std::vector<AnalyzedShrinkPartition> partitions;
  std::vector<AnalyzedShrinkVolume> content_volumes;
};

// These functions are read-only. They bind exact partition offsets to
// single-disk Volume GUID paths, require basic NTFS content, calculate used
// bytes from the volume, and reject BitLocker or ambiguous Windows layouts.
[[nodiscard]] clonecore::Result<ShrinkSourceAnalysis>
analyze_gpt_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::GptDisk& layout,
    const ShrinkSourceAnalysisContext& context);

[[nodiscard]] clonecore::Result<ShrinkSourceAnalysis>
analyze_mbr_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::MbrDisk& layout,
    const ShrinkSourceAnalysisContext& context);

}  // namespace ytec::windowsshrink

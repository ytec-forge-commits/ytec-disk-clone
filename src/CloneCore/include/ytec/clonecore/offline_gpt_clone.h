#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/operation_progress.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ytec::clonecore {

struct NtfsGeometry final {
  std::uint32_t bytes_per_sector{};
  std::uint32_t sectors_per_cluster{};
  std::uint64_t total_sectors{};

  [[nodiscard]] std::uint64_t cluster_size() const noexcept {
    return static_cast<std::uint64_t>(bytes_per_sector) * sectors_per_cluster;
  }

  [[nodiscard]] std::uint64_t complete_cluster_count() const noexcept {
    return sectors_per_cluster == 0 ? 0 : total_sectors / sectors_per_cluster;
  }
};

struct Fat32Geometry final {
  std::uint32_t bytes_per_sector{};
  std::uint32_t sectors_per_cluster{};
  std::uint64_t total_sectors{};

  [[nodiscard]] std::uint64_t cluster_size() const noexcept {
    return static_cast<std::uint64_t>(bytes_per_sector) *
        sectors_per_cluster;
  }
};

struct ExFatGeometry final {
  std::uint32_t bytes_per_sector{};
  std::uint32_t sectors_per_cluster{};
  std::uint64_t total_sectors{};
  std::uint32_t fat_offset_sectors{};
  std::uint32_t fat_length_sectors{};
  std::uint32_t cluster_heap_offset_sectors{};
  std::uint32_t cluster_count{};
  std::uint32_t root_directory_cluster{};
};

enum class BasicDataFileSystem : std::uint8_t {
  ntfs,
  fat32,
  exfat,
};

class INtfsUsedRangeProvider {
 public:
  virtual ~INtfsUsedRangeProvider() = default;

  [[nodiscard]] virtual Result<std::vector<ByteRange>> query_used_ranges(
      std::uint32_t partition_index,
      const NtfsGeometry& geometry) = 0;
};

enum class PartitionCopyMode : std::uint8_t {
  efi_fat32_raw,
  microsoft_reserved_recreate,
  ntfs_used_clusters,
  basic_fat32_raw,
  basic_exfat_raw,
  recovery_ntfs_raw,
};

struct PlannedPartitionCopy final {
  std::uint32_t entry_index{};
  PartitionCopyMode mode{PartitionCopyMode::ntfs_used_clusters};
  std::vector<ByteRange> source_ranges;
};

struct OfflineGptClonePlan final {
  GptDisk source_gpt;
  GptWritePlan target_gpt;
  std::vector<PlannedPartitionCopy> partition_copies;
};

struct OfflineGptCloneRequest final {
  StableDiskIdentity expected_source;
  StableDiskIdentity observed_source;
  StableDiskIdentity expected_target;
  StableDiskIdentity observed_target;
  TargetConfirmation confirmation;
  std::size_t maximum_chunk_bytes{1024U * 1024U};
  DiskOperationCallbacks callbacks;
};

struct OfflineGptCloneReport final {
  std::uint64_t copied_data_bytes{};
  std::uint32_t copied_partition_count{};
  std::uint32_t recreated_partition_count{};
  GptGuid source_disk_guid;
  GptGuid target_disk_guid;
  std::array<std::byte, 32> verified_write_digest{};
  bool read_back_verified{};
  bool primary_gpt_committed{};
};

[[nodiscard]] Result<NtfsGeometry> parse_ntfs_geometry(
    std::span<const std::byte> boot_sector,
    std::uint32_t expected_sector_size,
    std::uint64_t partition_size_bytes);

[[nodiscard]] Status validate_fat32_boot_sector(
    std::span<const std::byte> boot_sector,
    std::uint32_t expected_sector_size,
    std::uint64_t partition_size_bytes);

[[nodiscard]] Result<Fat32Geometry> parse_fat32_geometry(
    std::span<const std::byte> boot_sector,
    std::uint32_t expected_sector_size,
    std::uint64_t partition_size_bytes);

[[nodiscard]] Result<ExFatGeometry> parse_exfat_geometry(
    std::span<const std::byte> boot_sector,
    std::uint32_t expected_sector_size,
    std::uint64_t partition_size_bytes);

// Re-identifies a Microsoft basic-data filesystem from the on-disk boot
// sector.  A partition-table type alone is never sufficient because GPT basic
// data and MBR type 0x07 can each carry more than one filesystem.
[[nodiscard]] Result<BasicDataFileSystem>
classify_basic_data_file_system(
    std::span<const std::byte> boot_sector,
    std::uint32_t expected_sector_size,
    std::uint64_t partition_size_bytes);

[[nodiscard]] Result<OfflineGptClonePlan> build_offline_gpt_clone_plan(
    const ISourceDiskReader& source,
    const ITargetDiskWriter& target,
    INtfsUsedRangeProvider& used_range_provider,
    IGuidGenerator& guid_generator);

[[nodiscard]] Result<OfflineGptCloneReport> execute_offline_gpt_clone(
    const OfflineGptCloneRequest& request,
    const ISourceDiskReader& source,
    ITargetDiskWriter& target,
    INtfsUsedRangeProvider& used_range_provider,
    IGuidGenerator& guid_generator);

}  // namespace ytec::clonecore

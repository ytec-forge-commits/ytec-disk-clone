#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/offline_gpt_clone.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ytec::clonecore {

enum class MbrPartitionCopyMode : std::uint8_t {
  fat32_raw,
  exfat_raw,
  ntfs_used_clusters,
  recovery_ntfs_raw,
};

struct PlannedMbrPartitionCopy final {
  std::uint8_t table_index{};
  MbrPartitionCopyMode mode{MbrPartitionCopyMode::ntfs_used_clusters};
  std::vector<ByteRange> source_ranges;
};

struct OfflineMbrClonePlan final {
  MbrDisk source_mbr;
  MbrWritePlan target_mbr;
  std::vector<PlannedMbrPartitionCopy> partition_copies;
};

struct OfflineMbrCloneRequest final {
  StableDiskIdentity expected_source;
  StableDiskIdentity observed_source;
  StableDiskIdentity expected_target;
  StableDiskIdentity observed_target;
  TargetConfirmation confirmation;
  std::size_t maximum_chunk_bytes{1024U * 1024U};
  std::vector<std::uint32_t> connected_mbr_signatures;
  DiskOperationCallbacks callbacks;
};

struct OfflineMbrCloneReport final {
  std::uint64_t copied_data_bytes{};
  std::uint32_t copied_partition_count{};
  std::uint32_t source_disk_signature{};
  std::uint32_t target_disk_signature{};
  std::array<std::byte, 32> verified_write_digest{};
  bool read_back_verified{};
  bool target_mbr_committed{};
};

[[nodiscard]] Result<OfflineMbrClonePlan> build_offline_mbr_clone_plan(
    const ISourceDiskReader& source,
    const ITargetDiskWriter& target,
    INtfsUsedRangeProvider& used_range_provider,
    IMbrSignatureGenerator& signature_generator,
    std::span<const std::uint32_t> disallowed_signatures = {});

[[nodiscard]] Result<OfflineMbrCloneReport> execute_offline_mbr_clone(
    const OfflineMbrCloneRequest& request,
    const ISourceDiskReader& source,
    ITargetDiskWriter& target,
    INtfsUsedRangeProvider& used_range_provider,
    IMbrSignatureGenerator& signature_generator);

}  // namespace ytec::clonecore

#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/sha256.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::uint32_t kPartitionSnapshotHeaderSize = 64;
inline constexpr std::uint32_t kPartitionSnapshotRecordSize = 64;

enum class PartitionTableStyle : std::uint16_t {
  mbr = 1,
  gpt = 2,
};

struct PartitionTableRegion final {
  std::uint64_t disk_offset{};
  std::vector<std::byte> data;
  Sha256Digest sha256{};
};

struct PartitionSnapshot final {
  PartitionTableStyle style{PartitionTableStyle::gpt};
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::vector<PartitionTableRegion> regions;
};

// Encodes only partition-table sectors. Data chunks must not overlap these
// regions so a restore can keep the target unrecognizable until final commit.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
build_partition_snapshot_v1(const PartitionSnapshot& snapshot);

// Reads and validates only partition-table metadata from a read-only source.
// MBR captures sector 0. GPT captures the protective MBR, both headers, and
// both entry arrays. Partition contents are never read by this function.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
capture_partition_snapshot_v1(
    const clonecore::ISourceDiskReader& source,
    PartitionTableStyle style);

// Treats the complete snapshot as untrusted and verifies canonical layout,
// bounds, reserved bytes, and every region hash.
[[nodiscard]] clonecore::Result<PartitionSnapshot>
inspect_partition_snapshot_v1(std::span<const std::byte> bytes);

}  // namespace ytec::imageformat

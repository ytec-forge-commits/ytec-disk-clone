#pragma once

#include "ytec/clonecore/block_device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ytec::clonecore {

struct MbrPartition final {
  std::uint8_t table_index{};
  bool active{};
  std::array<std::byte, 3> first_chs{};
  std::uint8_t type{};
  std::array<std::byte, 3> last_chs{};
  std::uint32_t first_lba{};
  std::uint32_t sector_count{};
};

struct MbrDisk final {
  std::uint32_t logical_sector_size{};
  std::uint64_t sector_count{};
  std::uint32_t disk_signature{};
  std::array<std::byte, 440> bootstrap{};
  std::vector<MbrPartition> partitions;
};

struct MbrWritePlan final {
  MbrDisk target_disk;
  std::vector<std::byte> sector;
};

struct MbrAddPartitionRequest final {
  std::uint32_t first_lba{};
  std::uint32_t sector_count{};
  std::uint8_t type{};
};

class IMbrSignatureGenerator {
 public:
  virtual ~IMbrSignatureGenerator() = default;
  [[nodiscard]] virtual Result<std::uint32_t> next_signature() = 0;
};

[[nodiscard]] std::unique_ptr<IMbrSignatureGenerator>
make_windows_mbr_signature_generator();

[[nodiscard]] Result<MbrDisk> parse_mbr(const ISourceDiskReader& reader);

[[nodiscard]] Result<MbrWritePlan> make_mbr_write_plan(
    const MbrDisk& source,
    std::uint64_t target_size_bytes,
    std::uint32_t target_sector_size,
    IMbrSignatureGenerator& signature_generator,
    std::span<const std::uint32_t> disallowed_signatures = {},
    bool require_single_active_partition = true);

// Adds one non-active primary partition to the first empty entry while
// preserving bootstrap code, disk signature, and all existing entries.
[[nodiscard]] Result<MbrWritePlan> make_mbr_add_partition_plan(
    const MbrDisk& current,
    const MbrAddPartitionRequest& request);

}  // namespace ytec::clonecore

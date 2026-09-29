#pragma once

#include "ytec/clonecore/block_device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ytec::clonecore {

struct GptGuid final {
  std::array<std::byte, 16> bytes{};

  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] bool operator==(const GptGuid&) const noexcept = default;
};

struct GptPartition final {
  std::uint32_t entry_index{};
  GptGuid type_guid;
  GptGuid unique_guid;
  std::uint64_t first_lba{};
  std::uint64_t last_lba{};
  std::uint64_t attributes{};
  std::u16string name;
};

struct GptDisk final {
  std::uint32_t logical_sector_size{};
  std::uint64_t sector_count{};
  GptGuid disk_guid;
  std::uint64_t first_usable_lba{};
  std::uint64_t last_usable_lba{};
  std::uint32_t partition_entry_count{};
  std::uint32_t partition_entry_size{};
  std::vector<GptPartition> partitions;
};

enum class GptMetadataKind : std::uint8_t {
  protective_mbr,
  primary_entries,
  backup_entries,
  backup_header,
  primary_header_commit,
};

struct GptMetadataWrite final {
  GptMetadataKind kind{GptMetadataKind::primary_entries};
  std::uint64_t offset{};
  std::vector<std::byte> bytes;
};

struct GptWritePlan final {
  GptDisk target_disk;
  std::vector<GptMetadataWrite> writes;
};

struct GptAddPartitionRequest final {
  std::uint64_t first_lba{};
  std::uint64_t sector_count{};
  GptGuid type_guid;
  std::uint64_t attributes{};
  std::u16string name;
};

class IGuidGenerator {
 public:
  virtual ~IGuidGenerator() = default;
  [[nodiscard]] virtual Result<GptGuid> next_guid() = 0;
};

[[nodiscard]] std::unique_ptr<IGuidGenerator> make_windows_guid_generator();

[[nodiscard]] Result<GptDisk> parse_gpt(const ISourceDiskReader& reader);

[[nodiscard]] Result<GptWritePlan> make_gpt_write_plan(
    const GptDisk& source,
    std::uint64_t target_size_bytes,
    std::uint32_t target_sector_size,
    IGuidGenerator& guid_generator);

// Adds exactly one entry while preserving the existing disk GUID, every
// existing partition GUID/entry index, usable-LBA bounds, and protective MBR.
// The returned writes intentionally omit the protective MBR and are ordered
// as primary entries, backup entries, backup header, primary header.
[[nodiscard]] Result<GptWritePlan> make_gpt_add_partition_plan(
    const GptDisk& current,
    const GptAddPartitionRequest& request,
    IGuidGenerator& guid_generator);

[[nodiscard]] const GptGuid& gpt_type_efi_system() noexcept;
[[nodiscard]] const GptGuid& gpt_type_microsoft_reserved() noexcept;
[[nodiscard]] const GptGuid& gpt_type_basic_data() noexcept;
[[nodiscard]] const GptGuid& gpt_type_windows_recovery() noexcept;

}  // namespace ytec::clonecore

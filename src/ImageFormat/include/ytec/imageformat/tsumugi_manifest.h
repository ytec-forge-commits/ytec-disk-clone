#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::uint16_t kTsumugiManifestMajorVersion = 1U;
inline constexpr std::uint16_t kTsumugiManifestMinorVersion = 0U;
inline constexpr std::uint32_t kTsumugiManifestHeaderSize = 256U;
inline constexpr std::uint32_t kTsumugiManifestPartitionRecordSize = 384U;
inline constexpr std::size_t kTsumugiManifestMaximumPartitions = 128U;

enum class TsumugiManifestMode : std::uint16_t {
  exact = 1U,
  shrink = 2U,
  rescue = 3U,
};

enum class TsumugiManifestPartitionStyle : std::uint16_t {
  mbr = 1U,
  gpt = 2U,
};

enum class TsumugiManifestPartitionRole : std::uint16_t {
  other = 0U,
  efi_system = 1U,
  microsoft_reserved = 2U,
  bios_system = 3U,
  windows = 4U,
  recovery = 5U,
  data = 6U,
};

enum class TsumugiManifestFileSystem : std::uint16_t {
  unknown = 0U,
  none = 1U,
  ntfs = 2U,
  exfat = 3U,
  fat32 = 4U,
};

// Shrink images never reinterpret an archive as target filesystem sectors.
// exact_raw is the only encoding accepted by the normal block restore path.
// microsoft_wim_single_image requires the dedicated, versioned shrink
// archive adapter; until that adapter is supplied, product execution must
// fail closed after verification and before target begin/write.
enum class TsumugiManifestPayloadEncoding : std::uint16_t {
  exact_raw = 0U,
  microsoft_wim_single_image = 1U,
};

inline constexpr std::uint16_t kTsumugiWimPayloadFormatVersion = 1U;

enum class TsumugiManifestFlags : std::uint32_t {
  none = 0U,
  source_contains_windows = 1U << 0U,
  bitlocker_source_was_unlocked = 1U << 1U,
  partition_selection = 1U << 2U,
  automatic_surplus_allocation = 1U << 3U,
};

enum class TsumugiManifestPartitionFlags : std::uint32_t {
  none = 0U,
  selected = 1U << 0U,
  required = 1U << 1U,
  active = 1U << 2U,
  contains_windows = 1U << 3U,
  bitlocker_was_unlocked = 1U << 4U,
};

[[nodiscard]] constexpr TsumugiManifestFlags operator|(
    const TsumugiManifestFlags left,
    const TsumugiManifestFlags right) noexcept {
  return static_cast<TsumugiManifestFlags>(
      static_cast<std::uint32_t>(left) |
      static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr TsumugiManifestPartitionFlags operator|(
    const TsumugiManifestPartitionFlags left,
    const TsumugiManifestPartitionFlags right) noexcept {
  return static_cast<TsumugiManifestPartitionFlags>(
      static_cast<std::uint32_t>(left) |
      static_cast<std::uint32_t>(right));
}

struct TsumugiManifestPartition final {
  std::uint32_t source_table_index{};
  std::uint32_t source_partition_number{};
  TsumugiManifestPartitionRole role{TsumugiManifestPartitionRole::other};
  TsumugiManifestFileSystem file_system{TsumugiManifestFileSystem::unknown};
  TsumugiManifestPartitionFlags flags{TsumugiManifestPartitionFlags::none};
  std::uint64_t source_offset{};
  std::uint64_t source_size{};
  std::uint64_t used_bytes{};
  std::uint64_t minimum_target_bytes{};
  std::uint64_t planned_target_bytes{};
  std::uint64_t payload_logical_offset{};
  std::uint64_t payload_logical_length{};
  TsumugiManifestPayloadEncoding payload_encoding{
      TsumugiManifestPayloadEncoding::exact_raw};
  std::uint16_t payload_format_version{};
  std::uint64_t cluster_size{};
  // GPT uses the raw 16-byte type/unique GUID representation. MBR stores its
  // one-byte partition type in type_id[0] and requires every remaining ID byte
  // to be zero.
  std::array<std::byte, 16> type_id{};
  std::array<std::byte, 16> unique_id{};
  std::string name_utf8;
  std::string label_utf8;
};

struct TsumugiManifest final {
  TsumugiManifestMode mode{TsumugiManifestMode::exact};
  TsumugiManifestPartitionStyle partition_style{
      TsumugiManifestPartitionStyle::gpt};
  TsumugiManifestFlags flags{TsumugiManifestFlags::none};
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  Sha256Digest source_model_hash{};
  Sha256Digest source_serial_hash{};
  // Binds every payload chunk to one immutable source session. For an online
  // image this includes the VSS Snapshot Set and selected snapshot IDs; for PE
  // it includes the stable disk identity and the captured layout generation.
  Sha256Digest source_state_hash{};
  std::string created_utc;
  std::string app_version;
  std::vector<TsumugiManifestPartition> partitions;
  // Canonical PartitionSnapshot v1 bytes. Keeping the exact source table in
  // the authenticated manifest lets restore planning remain independent from
  // the current host's disk numbering and mount state.
  std::vector<std::byte> partition_snapshot;
};

// Canonical binary manifest embedded in the authenticated .tsumugi metadata.
// The format is fixed header, fixed records, then the exact partition snapshot
// with no gaps or trailing bytes.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
build_tsumugi_manifest_v1(const TsumugiManifest& manifest);

// Treats all fields as untrusted, rejects unknown required bits/reserved bytes,
// verifies strict UTF-8 and canonical re-encoding, and inspects the embedded
// partition snapshot before returning it.
[[nodiscard]] clonecore::Result<TsumugiManifest>
inspect_tsumugi_manifest_v1(std::span<const std::byte> bytes);

[[nodiscard]] bool tsumugi_manifest_requires_shrink_archive_adapter(
    const TsumugiManifest& manifest) noexcept;

}  // namespace ytec::imageformat

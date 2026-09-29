#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/migrationcore/shrink_layout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::migrationcore {

// This contract is intentionally narrower than a generic filesystem copier.
// It is the pure/in-memory boundary between an opened-handle enumerator and a
// later target-only formatter/writer.  Only format-preserving FAT32 and exFAT
// recreation is accepted.  NTFS remains on its separately audited WIM path.
//
// Production connection is deliberately NOT part of this slice.  A future
// source adapter must bind the opened volume-root identity and exact partition
// extent into enumeration_epoch_sha256, enumerate the whole namespace without
// following reparse points, reopen every entry by handle, prove its identity
// stayed stable, and hash each regular unnamed stream through stable EOF.  A
// future target adapter must independently re-identify the reviewed target,
// report the formatter's actual sector/cluster/extent geometry, repeat the
// same complete opened-handle enumeration and only then call the verifier.
// Neither a formatter exit code nor pathname-only enumeration satisfies this
// contract.

using FileSystemRecreateSha256 = std::array<std::byte, 32U>;

inline constexpr std::size_t kMaximumFileSystemRecreateEntries = 1'000'000U;
inline constexpr std::uint32_t kMaximumFileSystemRecreatePathUtf16Units =
    32'767U;
inline constexpr std::uint32_t kMaximumFileSystemRecreateComponentUtf16Units =
    255U;
inline constexpr std::uint64_t kFileSystemRecreateTimestampQuantum100ns =
    20'000'000ULL;  // Conservative two-second common FAT/exFAT precision.
inline constexpr std::uint64_t kFat32MaximumRecreatedFileBytes =
    0xFFFF'FFFFULL;
// The filesystem can encode a larger unsigned length, but the supported
// Windows opened-handle adapters use signed LARGE_INTEGER lengths.
inline constexpr std::uint64_t kExfatMaximumRecreatedFileBytes =
    0x7FFF'FFFF'FFFF'FFFFULL;

enum class FileSystemRecreateEntryKind : std::uint8_t {
  directory = 1U,
  regular_file = 2U,
};

enum FileSystemRecreatePortableAttribute : std::uint32_t {
  recreate_attribute_read_only = 1U << 0U,
  recreate_attribute_hidden = 1U << 1U,
  recreate_attribute_system = 1U << 2U,
  recreate_attribute_archive = 1U << 3U,
};

inline constexpr std::uint32_t kFileSystemRecreatePortableAttributeMask =
    recreate_attribute_read_only | recreate_attribute_hidden |
    recreate_attribute_system | recreate_attribute_archive;

struct CanonicalFileSystemTreeEntry final {
  // UTF-16 relative path using '\\' separators.  The first component is
  // relative to the opened volume root; root itself is not an entry.
  std::wstring relative_path;
  FileSystemRecreateEntryKind kind{
      FileSystemRecreateEntryKind::regular_file};
  std::uint64_t size_bytes{};
  std::uint32_t portable_attributes{};

  // The opened-handle adapter canonicalizes both times down to the common
  // two-second quantum before sealing the source tree.  Access time, ACLs,
  // owner, audit data, EAs, compression, sparse state and encryption are not
  // represented by this v1 contract and must not be claimed as preserved.
  std::uint64_t creation_time_utc_100ns{};
  std::uint64_t last_write_time_utc_100ns{};

  // Directories require the all-zero digest.  Regular files require SHA-256
  // of the complete unnamed data stream, read through stable EOF.
  FileSystemRecreateSha256 content_sha256{};

  // These are explicit fail-closed evidence from the opened-handle adapter.
  // The adapter must query the opened object itself; pathname-only metadata is
  // insufficient.  No reparse target is followed by this contract.
  std::uint32_t hard_link_count{1U};
  std::uint32_t alternate_data_stream_count{};
  std::uint32_t reparse_tag{};
  bool opened_handle_identity_stable{};
  bool unnamed_stream_hashed_to_stable_eof{};
  bool namespace_supported{};
};

struct CanonicalFileSystemTree final {
  MigrationFileSystem file_system{MigrationFileSystem::fat32};
  // Positive, adapter-normalized partition-table number.  Zero is reserved as
  // "unbound" and is rejected before a recreate plan can be produced.
  std::uint32_t source_table_index{};

  // A non-zero adapter-defined SHA-256 binding of the opened root identity,
  // source partition extent and one frozen/read-only enumeration epoch.  It is
  // bound into the immutable recreate plan but intentionally excluded from
  // the target tree-equivalence digest.
  FileSystemRecreateSha256 enumeration_epoch_sha256{};

  bool namespace_fully_enumerated{};
  bool opened_handles_only{};
  bool every_regular_file_hashed_to_stable_eof{};
  bool short_name_aliases_collision_free{};
  std::vector<CanonicalFileSystemTreeEntry> entries;
};

struct FileSystemRecreateFormatGeometry final {
  MigrationFileSystem file_system{MigrationFileSystem::fat32};
  std::uint64_t target_volume_bytes{};
  std::uint32_t logical_sector_size{};
  std::uint64_t cluster_size{};
  std::uint32_t maximum_path_utf16_units{
      kMaximumFileSystemRecreatePathUtf16Units};
  std::uint32_t maximum_component_utf16_units{
      kMaximumFileSystemRecreateComponentUtf16Units};
};

struct FileSystemRecreatePlanningRequest final {
  std::uint32_t target_partition_number{};
  std::uint64_t target_partition_offset_bytes{};
  FileSystemRecreateFormatGeometry target_geometry;
  CanonicalFileSystemTree source_tree;
};

struct FileSystemRecreateCapacity final {
  std::uint64_t total_content_bytes{};
  std::uint64_t regular_file_allocation_bytes{};
  std::uint64_t directory_allocation_bytes{};
  std::uint64_t namespace_record_bytes{};
  std::uint64_t conservative_format_overhead_bytes{};
  std::uint64_t minimum_free_reserve_bytes{};
  std::uint64_t minimum_required_volume_bytes{};
};

// The plan has no mutating API.  It binds the exact target partition, format
// geometry, canonical source tree, source epoch and all capacity arithmetic
// before a production formatter or writer may be constructed.
class FileSystemRecreatePlan final {
 public:
  FileSystemRecreatePlan(const FileSystemRecreatePlan&) = default;
  FileSystemRecreatePlan(FileSystemRecreatePlan&&) noexcept = default;
  FileSystemRecreatePlan& operator=(const FileSystemRecreatePlan&) = delete;
  FileSystemRecreatePlan& operator=(FileSystemRecreatePlan&&) = delete;

  [[nodiscard]] std::uint32_t source_table_index() const noexcept {
    return source_table_index_;
  }
  [[nodiscard]] std::uint32_t target_partition_number() const noexcept {
    return target_partition_number_;
  }
  [[nodiscard]] std::uint64_t target_partition_offset_bytes() const noexcept {
    return target_partition_offset_bytes_;
  }
  [[nodiscard]] const FileSystemRecreateFormatGeometry& target_geometry()
      const noexcept {
    return target_geometry_;
  }
  [[nodiscard]] const FileSystemRecreateSha256& source_epoch_sha256()
      const noexcept {
    return source_epoch_sha256_;
  }
  [[nodiscard]] const FileSystemRecreateSha256& canonical_manifest_sha256()
      const noexcept {
    return canonical_manifest_sha256_;
  }
  [[nodiscard]] const FileSystemRecreateSha256& plan_sha256() const noexcept {
    return plan_sha256_;
  }
  [[nodiscard]] const FileSystemRecreateCapacity& capacity() const noexcept {
    return capacity_;
  }
  [[nodiscard]] std::span<const CanonicalFileSystemTreeEntry> entries()
      const noexcept {
    return entries_;
  }
  [[nodiscard]] bool source_remains_unchanged() const noexcept {
    return true;
  }

 private:
  FileSystemRecreatePlan() = default;

  std::uint32_t source_table_index_{};
  std::uint32_t target_partition_number_{};
  std::uint64_t target_partition_offset_bytes_{};
  FileSystemRecreateFormatGeometry target_geometry_;
  FileSystemRecreateSha256 source_epoch_sha256_{};
  FileSystemRecreateSha256 canonical_manifest_sha256_{};
  FileSystemRecreateSha256 plan_sha256_{};
  FileSystemRecreateCapacity capacity_;
  std::vector<CanonicalFileSystemTreeEntry> entries_;

  friend clonecore::Result<FileSystemRecreatePlan>
  plan_file_system_recreation(
      const FileSystemRecreatePlanningRequest& request);
};

struct FileSystemRecreateTargetReadback final {
  std::uint32_t target_partition_number{};
  std::uint64_t target_partition_offset_bytes{};
  FileSystemRecreateFormatGeometry actual_geometry;
  CanonicalFileSystemTree target_tree;
};

struct FileSystemRecreateVerification final {
  FileSystemRecreateSha256 expected_manifest_sha256{};
  FileSystemRecreateSha256 observed_manifest_sha256{};
  FileSystemRecreateSha256 target_epoch_sha256{};
  std::uint64_t directory_count{};
  std::uint64_t regular_file_count{};
  std::uint64_t regular_file_bytes_read{};
  bool exact_tree_and_content_equivalence{};
  bool namespace_fully_enumerated{};
  bool every_regular_file_hashed_to_stable_eof{};
};

// Validates the opened-handle evidence, canonical UTF-16 paths, strict
// case-insensitive order, parents, file-system limits and entry metadata, then
// hashes the canonical v1 encoding.  It performs no filesystem or disk I/O.
[[nodiscard]] clonecore::Result<FileSystemRecreateSha256>
hash_canonical_file_system_tree(
    const CanonicalFileSystemTree& tree,
    const FileSystemRecreateFormatGeometry& geometry);

// Conservative target-only planner.  FAT/exFAT format structures, directory
// records, cluster rounding and a bounded free-space reserve are included.
[[nodiscard]] clonecore::Result<FileSystemRecreatePlan>
plan_file_system_recreation(
    const FileSystemRecreatePlanningRequest& request);

// Revalidates the exact target binding and actual format geometry, then
// compares every canonical entry (including content hash, portable metadata
// and canonicalized times).  Hash equality alone is not treated as evidence.
// The caller is responsible for source/target stable-disk re-identification,
// destructive confirmation, target-only formatting/writes, flushes and for
// keeping the source opened read-only; none of that production I/O exists in
// this pure module.
[[nodiscard]] clonecore::Result<FileSystemRecreateVerification>
verify_recreated_file_system_tree(
    const FileSystemRecreatePlan& plan,
    const FileSystemRecreateTargetReadback& readback);

}  // namespace ytec::migrationcore

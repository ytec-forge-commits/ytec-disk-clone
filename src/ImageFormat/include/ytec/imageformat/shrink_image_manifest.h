#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/migrationcore/shrink_layout.h"

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::uint16_t kShrinkImageManifestMajorVersion = 1;
inline constexpr std::uint16_t kShrinkImageManifestMinorVersion = 0;
inline constexpr std::uint32_t kShrinkImageManifestHeaderSize = 256;
inline constexpr std::uint32_t kShrinkImageManifestRecordSize = 160;
inline constexpr std::size_t kMaximumShrinkImageManifestBytes =
    16U * 1024U * 1024U;

struct ShrinkImagePartition final {
  std::uint32_t source_table_index{};
  migrationcore::MigrationPartitionRole role{
      migrationcore::MigrationPartitionRole::data};
  migrationcore::MigrationFileSystem file_system{
      migrationcore::MigrationFileSystem::ntfs};
  std::uint64_t source_size_bytes{};
  std::uint64_t used_bytes{};
  std::uint64_t cluster_size{};
  bool active{};
  std::wstring label;
  std::string payload_file_name;
  std::uint64_t payload_length_bytes{};
  Sha256Digest payload_sha256{};
};

struct ShrinkImageManifest final {
  clonecore::StableDiskIdentity source;
  std::uint32_t physical_sector_size{};
  migrationcore::MigrationPartitionStyle partition_style{
      migrationcore::MigrationPartitionStyle::gpt};
  std::uint32_t windows_major{};
  std::uint32_t windows_minor{};
  std::uint32_t windows_build{};
  std::string windows_architecture;
  bool bitlocker_fully_decrypted{};
  std::string created_utc;
  std::string app_version;
  // Exact source MBR bootstrap code. It is never executed during inspection;
  // it is bound into the manifest so BIOS targets can be reconstructed.
  // GPT manifests require this field to remain zero.
  std::array<std::byte, 440> mbr_bootstrap{};
  std::vector<ShrinkImagePartition> partitions;
};

// Canonical metadata for a .dcmig directory bundle. WIM payload files are
// separate regular files whose exact length and SHA-256 are bound here.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
build_shrink_image_manifest_v1(const ShrinkImageManifest& manifest);

// Treats all bytes as untrusted and requires exact canonical encoding.
[[nodiscard]] clonecore::Result<ShrinkImageManifest>
inspect_shrink_image_manifest_v1(std::span<const std::byte> bytes);

}  // namespace ytec::imageformat

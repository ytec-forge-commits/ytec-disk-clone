#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/dcimg.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::uint16_t kBackupManifestMajorVersion = 1;
inline constexpr std::uint16_t kLegacyBackupManifestMinorVersion = 0;
inline constexpr std::uint16_t kBackupManifestMinorVersion = 1;
inline constexpr std::uint32_t kBackupManifestHeaderSize = 192;
inline constexpr std::uint32_t kBackupManifestPartitionRecordSize = 80;
inline constexpr std::size_t kMaximumBackupManifestBytes =
    16U * 1024U * 1024U;

enum class BackupPartitionStyle : std::uint16_t {
  mbr = 1,
  gpt = 2,
};

enum class BackupBootMode : std::uint16_t {
  none = 0,
  legacy_bios = 1,
  uefi = 2,
};

enum class BackupPartitionRole : std::uint16_t {
  efi_system = 1,
  microsoft_reserved = 2,
  windows_ntfs = 3,
  recovery_ntfs = 4,
  fat32_data = 5,
  ntfs_data = 6,
};

enum class BackupFileSystem : std::uint16_t {
  none = 0,
  ntfs = 1,
  fat32 = 2,
};

struct BackupManifestPartition final {
  std::uint32_t table_index{};
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  BackupPartitionRole role{BackupPartitionRole::windows_ntfs};
  BackupFileSystem file_system{BackupFileSystem::ntfs};
  std::uint64_t cluster_size{};
  std::wstring name;
};

struct BackupImageManifest final {
  std::uint16_t format_minor{kLegacyBackupManifestMinorVersion};
  clonecore::StableDiskIdentity source;
  std::uint32_t physical_sector_size{};
  BackupPartitionStyle partition_style{BackupPartitionStyle::gpt};
  BackupBootMode boot_mode{BackupBootMode::uefi};
  std::uint32_t windows_major{};
  std::uint32_t windows_minor{};
  std::uint32_t windows_build{};
  std::string windows_architecture;
  bool bitlocker_fully_decrypted{};
  DcimgCompression compression{DcimgCompression::none};
  std::uint16_t compression_version{};
  std::uint32_t chunk_size{kDcimgChunkSize16MiB};
  std::string created_utc;
  std::string app_version;
  std::vector<BackupManifestPartition> partitions;
};

// Produces a canonical little-endian v1 binary manifest. The enclosing dcimg
// hashes these exact bytes.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
build_backup_manifest_v1(const BackupImageManifest& manifest);

// Treats every byte as untrusted, validates all offsets, reserved bytes,
// UTF-8, supported values, partition semantics, and canonical re-encoding.
[[nodiscard]] clonecore::Result<BackupImageManifest>
inspect_backup_manifest_v1(std::span<const std::byte> bytes);

}  // namespace ytec::imageformat

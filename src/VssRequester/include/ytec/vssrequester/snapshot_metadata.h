#pragma once

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/imageformat/backup_manifest.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ytec::vssrequester {

struct SnapshotMetadataContext final {
  clonecore::StableDiskIdentity source;
  std::uint32_t physical_sector_size{};
  std::uint32_t windows_major{};
  std::uint32_t windows_minor{};
  std::uint32_t windows_build{};
  std::string windows_architecture;
  std::string created_utc;
  std::string app_version;
};

struct GptSnapshotMetadata final {
  clonecore::GptDisk layout;
  std::vector<std::byte> backup_manifest;
  std::vector<std::byte> partition_table_snapshot;
};

struct MbrSnapshotMetadata final {
  clonecore::MbrDisk layout;
  std::vector<std::byte> backup_manifest;
  std::vector<std::byte> partition_table_snapshot;
};

// Re-parses the current read-only source and builds all mandatory dcimg
// metadata. Raw NTFS boot-sector parsing is the fail-closed BitLocker gate:
// an on-disk FVE signature or suspended protection is not accepted as fully
// decrypted.
[[nodiscard]] clonecore::Result<GptSnapshotMetadata>
build_gpt_snapshot_metadata(
    const clonecore::ISourceDiskReader& read_only_source,
    const SnapshotMetadataContext& context);

[[nodiscard]] clonecore::Result<MbrSnapshotMetadata>
build_mbr_snapshot_metadata(
    const clonecore::ISourceDiskReader& read_only_source,
    const SnapshotMetadataContext& context);

}  // namespace ytec::vssrequester

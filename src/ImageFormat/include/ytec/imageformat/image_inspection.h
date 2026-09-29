#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/partition_snapshot.h"

#include <cstddef>
#include <span>

namespace ytec::imageformat {

struct DcimgMetadataInspection final {
  BackupImageManifest manifest;
  PartitionSnapshot partition_snapshot;
};

// Validates both embedded metadata formats and their cross-section
// consistency with the already verified dcimg header.
[[nodiscard]] clonecore::Result<DcimgMetadataInspection>
inspect_dcimg_metadata_v1(
    const DcimgHeader& header,
    std::span<const std::byte> manifest_bytes,
    std::span<const std::byte> partition_snapshot_bytes);

// Confirms that every data chunk belongs to one declared, writable
// partition and never overlaps partition-table sectors committed last.
[[nodiscard]] clonecore::Status validate_dcimg_restore_layout_v1(
    const DcimgInspection& container,
    const DcimgMetadataInspection& metadata);

}  // namespace ytec::imageformat

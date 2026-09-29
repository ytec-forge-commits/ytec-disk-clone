#pragma once

#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/vssrequester/snapshot_plan.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ytec::vssrequester {

struct VssSnapshotImageVolume final {
  std::uint32_t partition_entry_index{};
  std::uint64_t disk_offset{};
  std::uint64_t partition_length{};
  clonecore::NtfsGeometry geometry;
  const clonecore::ISourceDiskReader* snapshot_reader{};
};

struct VssSnapshotImageRequest final {
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{imageformat::kDcimgChunkSize16MiB};
  imageformat::DcimgCompression compression{
      imageformat::DcimgCompression::none};
  std::size_t verification_block_bytes{1024U * 1024U};
  std::vector<std::byte> manifest;
  std::vector<std::byte> partition_table_snapshot;
  std::vector<VssSnapshotImageVolume> volumes;
  // EFI FAT32 and Windows Recovery are not sourced from the live Windows
  // volume bitmap. Their complete, fixed partition ranges are read through a
  // separately identity-verified, read-only source-disk handle.
  std::vector<VssSnapshotImageRawRegion> raw_regions;
  clonecore::DiskOperationCallbacks callbacks;
};

// VSS Snapshot Readerと同じSnapshotパスへ束縛されたBitmap Providerから
// NTFS使用範囲を得て、ディスク論理位置へ変換する。通常Volumeや物理
// ディスクを直接開かず、全読戻し後だけStagingTargetを確定する。
[[nodiscard]] clonecore::Result<imageformat::DcimgStreamBuildReport>
write_vss_snapshot_dcimg_v1(
    const VssSnapshotImageRequest& request,
    clonecore::INtfsUsedRangeProvider& bitmap_provider,
    imageformat::IDcimgStagingTarget& target);

}  // namespace ytec::vssrequester

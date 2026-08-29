#pragma once

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/imageformat/image_primitives.h"
#include "ytec/vssrequester/workflow.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ytec::vssrequester {

struct SnapshotImageVolumePlan final {
  std::uint32_t partition_entry_index{};
  std::uint64_t disk_offset{};
  std::uint64_t partition_length{};
};

struct VssSnapshotImageRawRegion final {
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  std::uint64_t source_offset{};
  const clonecore::ISourceDiskReader* source_reader{};
};

// Read-plan metadata shared by the current .tsumugi path and the quarantined
// legacy image writer. It deliberately contains no dcimg writer or staging
// interface, so planning a current image cannot pull the legacy format into a
// product link.
struct SnapshotImageCopyRequest final {
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{imageformat::kImageChunkSize16MiB};
  imageformat::ImageCompression compression{
      imageformat::ImageCompression::none};
  std::size_t verification_block_bytes{1024U * 1024U};
  std::vector<std::byte> manifest;
  std::vector<std::byte> partition_table_snapshot;
  std::vector<SnapshotImageVolumePlan> volumes;
  std::vector<VssSnapshotImageRawRegion> raw_regions;
  clonecore::DiskOperationCallbacks callbacks;
};

struct SnapshotImagePlanOptions final {
  bool administrator{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{imageformat::kImageChunkSize16MiB};
  imageformat::ImageCompression compression{
      imageformat::ImageCompression::none};
  std::size_t verification_block_bytes{1024U * 1024U};
  std::vector<std::byte> manifest;
  std::vector<std::byte> partition_table_snapshot;
  // Empty preserves the legacy whole-disk plan. Otherwise these are the
  // canonical zero-based GPT entry/MBR table indexes whose contents may be
  // read. The planner still revalidates the complete partition table, but it
  // must not probe boot sectors or create VSS/raw readers for unselected
  // entries.
  std::vector<std::uint32_t> selected_partition_entry_indices;
};

struct PreparedSnapshotImagePlan final {
  WorkflowRequest workflow;
  SnapshotImageCopyRequest image_copy;
  std::size_t raw_partition_count{};
  std::size_t snapshot_partition_count{};
  std::size_t recreated_partition_count{};
};

// Builds an online image plan without writing. Basic-data/0x07 NTFS
// partitions are mapped to VSS. EFI FAT32 and Recovery partitions use the
// already identity-verified read-only source. MSR is represented by the
// partition table and recreated without copying contents.
[[nodiscard]] clonecore::Result<PreparedSnapshotImagePlan>
prepare_gpt_snapshot_image_plan(
    const clonecore::GptDisk& source_gpt,
    const clonecore::ISourceDiskReader& read_only_source,
    std::span<const clonecore::VolumeBitmapBinding> ntfs_bindings,
    const SnapshotImagePlanOptions& options);

[[nodiscard]] clonecore::Result<PreparedSnapshotImagePlan>
prepare_mbr_snapshot_image_plan(
    const clonecore::MbrDisk& source_mbr,
    const clonecore::ISourceDiskReader& read_only_source,
    std::span<const clonecore::VolumeBitmapBinding> ntfs_bindings,
    const SnapshotImagePlanOptions& options);

}  // namespace ytec::vssrequester

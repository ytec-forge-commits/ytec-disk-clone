#pragma once

#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/imageformat/tsumugi_image_service.h"
#include "ytec/vssrequester/snapshot_reader.h"
#include "ytec/vssrequester/workflow.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ytec::vssrequester {

struct TsumugiSnapshotVolumePlan final {
  std::uint32_t partition_entry_index{};
  std::uint64_t disk_offset{};
  std::uint64_t partition_length{};
  std::wstring original_volume_guid_path;
};

struct TsumugiSnapshotRawRegion final {
  std::uint32_t partition_entry_index{};
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  std::uint64_t source_offset{};
};

struct TsumugiSnapshotImageRequest final {
  // chunks and source_session must be empty. This adapter derives both from
  // the revalidated Snapshot devices and the locked read-only source handle.
  imageformat::TsumugiImageCreateRequest image;
  std::vector<TsumugiSnapshotVolumePlan> volumes;
  std::vector<TsumugiSnapshotRawRegion> raw_regions;
  const clonecore::ISourceDiskReader* locked_raw_source{};
  // Stable observation of the still-open physical source handle and layout.
  // It is mixed with all Snapshot device paths into manifest.source_state_hash.
  imageformat::Sha256Digest locked_source_state_hash{};
  // Re-reads GPT/MBR bytes through the same locked read-only physical handle
  // and compares them with manifest.partition_snapshot. Called immediately
  // before source reads and again after complete .partial verification.
  std::function<clonecore::Status()> revalidate_locked_layout;
  // Called after Bitmap/raw chunk planning but before the first .partial byte
  // is created. The value is a conservative maximum completed-image length.
  std::function<clonecore::Status(std::uint64_t)>
      validate_destination_capacity;
};

using TsumugiSnapshotReaderOpenCallback = std::function<clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>(
    const SnapshotVolumeOpenRequest&)>;

// Opens every VSS-revalidated Snapshot path, queries its own volume bitmap,
// and exposes one composite immutable source session to the .tsumugi service.
// Only exact-mode images are accepted by this online block-copy adapter.
[[nodiscard]] clonecore::Result<imageformat::TsumugiStagedImageV1>
prepare_tsumugi_snapshot_image_v1(
    const TsumugiSnapshotImageRequest& request,
    const SnapshotCopyContext& snapshot_context,
    const TsumugiSnapshotReaderOpenCallback& open_reader,
    clonecore::INtfsUsedRangeProvider& bitmap_provider,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Product adapter using Snapshot-only Windows handles and
// FSCTL_GET_VOLUME_BITMAP. Live Volume GUID paths are not accepted here.
[[nodiscard]] clonecore::Result<imageformat::TsumugiStagedImageV1>
prepare_tsumugi_snapshot_image_v1_with_windows_apis(
    const TsumugiSnapshotImageRequest& request,
    const SnapshotCopyContext& snapshot_context,
    const clonecore::DiskOperationCallbacks& callbacks = {});

}  // namespace ytec::vssrequester

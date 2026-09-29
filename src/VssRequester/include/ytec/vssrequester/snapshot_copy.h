#pragma once

#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/vssrequester/snapshot_plan.h"
#include "ytec/vssrequester/snapshot_reader.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ytec::vssrequester {

using SnapshotReaderOpenCallback = std::function<clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>(
    const SnapshotVolumeOpenRequest&)>;

// Connects VSS-revalidated Snapshot device paths to the dcimg stream writer.
// The supplied opener and bitmap provider make this boundary fully mockable.
// Live Volume GUID paths and physical disks are not accepted here.
[[nodiscard]] clonecore::Result<imageformat::DcimgStreamBuildReport>
copy_snapshot_devices_to_dcimg_v1(
    const SnapshotImageCopyRequest& request,
    std::span<const std::wstring> snapshot_device_paths,
    const SnapshotReaderOpenCallback& open_reader,
    clonecore::INtfsUsedRangeProvider& bitmap_provider,
    imageformat::IDcimgStagingTarget& target);

// Product adapter using the audited Windows Snapshot reader and Snapshot-only
// FSCTL_GET_VOLUME_BITMAP provider.
[[nodiscard]] clonecore::Result<imageformat::DcimgStreamBuildReport>
copy_snapshot_devices_to_dcimg_v1_with_windows_apis(
    const SnapshotImageCopyRequest& request,
    std::span<const std::wstring> snapshot_device_paths,
    imageformat::IDcimgStagingTarget& target);

}  // namespace ytec::vssrequester

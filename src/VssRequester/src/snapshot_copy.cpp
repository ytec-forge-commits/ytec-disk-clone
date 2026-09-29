#include "ytec/vssrequester/snapshot_copy.h"

#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/vssrequester/snapshot_image.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ytec::vssrequester {
namespace {

clonecore::Error copy_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

clonecore::Status validate_request(
    const SnapshotImageCopyRequest& request,
    const std::span<const std::wstring> snapshot_device_paths,
    const SnapshotReaderOpenCallback& open_reader) {
  if (!open_reader ||
      request.source_disk_size == 0 ||
      !imageformat::is_supported_sector_size_pair(
          request.logical_sector_size,
          request.physical_sector_size) ||
      request.source_disk_size % request.logical_sector_size != 0 ||
      request.manifest.empty() ||
      request.partition_table_snapshot.empty() ||
      request.volumes.empty() ||
      request.volumes.size() > 128 ||
      snapshot_device_paths.size() != request.volumes.size()) {
    return clonecore::Status::failure(copy_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS Snapshotイメージ接続要求",
        L"ディスク寸法、メタデータ、Volume、またはSnapshot件数が不正です"));
  }

  std::uint64_t previous_end = 0;
  for (std::size_t index = 0; index < request.volumes.size(); ++index) {
    const auto& volume = request.volumes[index];
    std::uint64_t end{};
    if (volume.partition_length == 0 ||
        volume.disk_offset % request.logical_sector_size != 0 ||
        volume.partition_length % request.logical_sector_size != 0 ||
        !checked_add(volume.disk_offset, volume.partition_length, end) ||
        end > request.source_disk_size ||
        (index != 0 && volume.disk_offset < previous_end)) {
      return clonecore::Status::failure(copy_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"VSS SnapshotイメージVolume境界",
          L"パーティションが空、未整列、重複、またはディスク境界外です"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (request.volumes[previous].partition_entry_index ==
              volume.partition_entry_index ||
          _wcsicmp(
              snapshot_device_paths[previous].c_str(),
              snapshot_device_paths[index].c_str()) == 0) {
        return clonecore::Status::failure(copy_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"VSS Snapshotイメージ対応",
            L"パーティション番号またはSnapshotデバイスが重複しています"));
      }
    }
    SnapshotVolumeOpenRequest snapshot_request{};
    snapshot_request.snapshot_device_path = snapshot_device_paths[index];
    snapshot_request.expected_size_bytes = volume.partition_length;
    snapshot_request.logical_sector_size = request.logical_sector_size;
    const auto valid_path =
        validate_snapshot_volume_open_request(snapshot_request);
    if (!valid_path) {
      return valid_path;
    }
    previous_end = end;
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<imageformat::DcimgStreamBuildReport>
copy_snapshot_devices_to_dcimg_v1(
    const SnapshotImageCopyRequest& request,
    const std::span<const std::wstring> snapshot_device_paths,
    const SnapshotReaderOpenCallback& open_reader,
    clonecore::INtfsUsedRangeProvider& bitmap_provider,
    imageformat::IDcimgStagingTarget& target) {
  const auto valid =
      validate_request(request, snapshot_device_paths, open_reader);
  if (!valid) {
    return clonecore::Result<
        imageformat::DcimgStreamBuildReport>::failure(valid.error());
  }

  std::vector<std::unique_ptr<clonecore::ISourceDiskReader>> readers;
  readers.reserve(request.volumes.size());
  std::vector<VssSnapshotImageVolume> image_volumes;
  image_volumes.reserve(request.volumes.size());
  for (std::size_t index = 0; index < request.volumes.size(); ++index) {
    const auto& plan = request.volumes[index];
    auto reader = open_reader(SnapshotVolumeOpenRequest{
        .snapshot_device_path = snapshot_device_paths[index],
        .expected_size_bytes = plan.partition_length,
        .logical_sector_size = request.logical_sector_size,
    });
    if (!reader) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(reader.error());
    }
    if (!reader.value() ||
        reader.value()->size_bytes() != plan.partition_length ||
        reader.value()->logical_sector_size() !=
            request.logical_sector_size) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(copy_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"VSS Snapshot Reader再確認",
              L"Snapshot Readerの容量または論理セクターが計画と一致しません"));
    }
    const auto boot_sector =
        reader.value()->read(0, request.logical_sector_size);
    if (!boot_sector) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(
              boot_sector.error());
    }
    if (boot_sector.value().size() != request.logical_sector_size) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(copy_error(
              clonecore::ErrorCode::io_failed,
              ERROR_HANDLE_EOF,
              L"VSS Snapshot NTFSブートセクター",
              L"Snapshotから論理セクターを完全に読み取れませんでした"));
    }
    const auto geometry = clonecore::parse_ntfs_geometry(
        boot_sector.value(),
        request.logical_sector_size,
        plan.partition_length);
    if (!geometry) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(geometry.error());
    }
    const std::uint64_t geometry_bytes =
        geometry.value().total_sectors *
        geometry.value().bytes_per_sector;
    // NTFS may occupy fewer sectors than its containing partition.  The
    // unused tail is not part of the filesystem bitmap and is represented by
    // omitted (zero) image ranges; only a filesystem extending past the
    // trusted partition boundary is an identity failure.
    if (geometry_bytes > plan.partition_length) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(copy_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"VSS Snapshot NTFS容量",
              L"NTFSブートセクターの容量がパーティション計画を超えています"));
    }
    readers.push_back(reader.take_value());
    image_volumes.push_back(VssSnapshotImageVolume{
        .partition_entry_index = plan.partition_entry_index,
        .disk_offset = plan.disk_offset,
        .partition_length = plan.partition_length,
        .geometry = geometry.value(),
        .snapshot_reader = readers.back().get(),
    });
  }

  return write_vss_snapshot_dcimg_v1(
      VssSnapshotImageRequest{
          .source_disk_size = request.source_disk_size,
          .logical_sector_size = request.logical_sector_size,
          .physical_sector_size = request.physical_sector_size,
          .chunk_size = request.chunk_size,
          .compression = request.compression,
          .verification_block_bytes =
              request.verification_block_bytes,
          .manifest = request.manifest,
          .partition_table_snapshot =
              request.partition_table_snapshot,
          .volumes = std::move(image_volumes),
          .raw_regions = request.raw_regions,
          .callbacks = request.callbacks,
      },
      bitmap_provider,
      target);
}

clonecore::Result<imageformat::DcimgStreamBuildReport>
copy_snapshot_devices_to_dcimg_v1_with_windows_apis(
    const SnapshotImageCopyRequest& request,
    const std::span<const std::wstring> snapshot_device_paths,
    imageformat::IDcimgStagingTarget& target) {
  if (snapshot_device_paths.size() != request.volumes.size()) {
    return clonecore::Result<
        imageformat::DcimgStreamBuildReport>::failure(copy_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Windows VSS Snapshot件数",
            L"VSSが返したSnapshot件数がイメージ計画と一致しません"));
  }
  std::vector<clonecore::SnapshotVolumeBitmapBinding> bindings;
  bindings.reserve(request.volumes.size());
  for (std::size_t index = 0; index < request.volumes.size(); ++index) {
    bindings.push_back(clonecore::SnapshotVolumeBitmapBinding{
        .partition_entry_index =
            request.volumes[index].partition_entry_index,
        .snapshot_device_path = snapshot_device_paths[index],
    });
  }
  clonecore::WindowsSnapshotVolumeBitmapProvider bitmap_provider(
      std::move(bindings));
  return copy_snapshot_devices_to_dcimg_v1(
      request,
      snapshot_device_paths,
      [](const SnapshotVolumeOpenRequest& open_request) {
        return open_snapshot_volume_reader_with_windows_apis(open_request);
      },
      bitmap_provider,
      target);
}

}  // namespace ytec::vssrequester

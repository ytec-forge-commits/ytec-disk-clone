#include "ytec/vssrequester/snapshot_image.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace ytec::vssrequester {
namespace {

clonecore::Error image_error(
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

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

clonecore::Status validate_request(
    const VssSnapshotImageRequest& request) {
  if (request.source_disk_size == 0 ||
      !imageformat::is_supported_sector_size_pair(
          request.logical_sector_size, request.physical_sector_size) ||
      request.source_disk_size % request.logical_sector_size != 0 ||
      (request.chunk_size != imageformat::kDcimgChunkSize16MiB &&
       request.chunk_size != imageformat::kDcimgChunkSize32MiB) ||
      (request.compression != imageformat::DcimgCompression::none &&
       request.compression !=
           imageformat::DcimgCompression::zstandard) ||
      request.verification_block_bytes == 0 ||
      request.verification_block_bytes >
          imageformat::kDcimgChunkSize32MiB ||
      request.manifest.empty() ||
      request.partition_table_snapshot.empty() ||
      request.volumes.empty() ||
      request.volumes.size() > 128 ||
      request.raw_regions.size() > 128) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS dcimg作成要求",
        L"ディスク寸法、メタデータ、チャンク、またはVolume件数が不正です"));
  }

  std::uint64_t previous_partition_end = 0;
  bool first = true;
  for (std::size_t index = 0; index < request.volumes.size(); ++index) {
    const auto& volume = request.volumes[index];
    std::uint64_t partition_end{};
    std::uint64_t geometry_bytes{};
    const std::uint64_t cluster_size = volume.geometry.cluster_size();
    if (volume.snapshot_reader == nullptr ||
        volume.partition_length == 0 ||
        volume.disk_offset % request.logical_sector_size != 0 ||
        volume.partition_length % request.logical_sector_size != 0 ||
        !checked_add(
            volume.disk_offset,
            volume.partition_length,
            partition_end) ||
        partition_end > request.source_disk_size ||
        (!first && volume.disk_offset < previous_partition_end) ||
        volume.snapshot_reader->size_bytes() != volume.partition_length ||
        volume.snapshot_reader->logical_sector_size() !=
            request.logical_sector_size ||
        volume.geometry.bytes_per_sector != request.logical_sector_size ||
        volume.geometry.sectors_per_cluster == 0 ||
        volume.geometry.total_sectors == 0 ||
        cluster_size == 0 ||
        request.chunk_size % cluster_size != 0 ||
        !checked_multiply(
            volume.geometry.total_sectors,
            volume.geometry.bytes_per_sector,
            geometry_bytes) ||
        geometry_bytes > volume.partition_length) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"VSS dcimg Volume対応",
          L"Snapshot Reader、パーティション、またはNTFS Geometryが一致しません"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (request.volumes[previous].partition_entry_index ==
          volume.partition_entry_index) {
        return clonecore::Status::failure(image_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"VSS dcimg Volume重複",
            L"同じパーティションへ複数のSnapshot Readerを割り当てられません"));
      }
    }
    previous_partition_end = partition_end;
    first = false;
  }

  struct LogicalRegion final {
    std::uint64_t offset{};
    std::uint64_t length{};
  };
  std::vector<LogicalRegion> all_regions;
  all_regions.reserve(request.volumes.size() + request.raw_regions.size());
  for (const auto& volume : request.volumes) {
    all_regions.push_back(LogicalRegion{
        .offset = volume.disk_offset,
        .length = volume.partition_length,
    });
  }
  for (const auto& region : request.raw_regions) {
    std::uint64_t disk_end{};
    std::uint64_t source_end{};
    if (region.source_reader == nullptr ||
        region.length == 0 ||
        region.disk_offset % request.logical_sector_size != 0 ||
        region.length % request.logical_sector_size != 0 ||
        region.source_offset % request.logical_sector_size != 0 ||
        region.source_reader->logical_sector_size() !=
            request.logical_sector_size ||
        !checked_add(region.disk_offset, region.length, disk_end) ||
        disk_end > request.source_disk_size ||
        !checked_add(region.source_offset, region.length, source_end) ||
        source_end > region.source_reader->size_bytes()) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"VSS dcimg固定領域",
          L"読取り専用固定領域が空、未整列、またはReader境界外です"));
    }
    all_regions.push_back(LogicalRegion{
        .offset = region.disk_offset,
        .length = region.length,
    });
  }
  std::sort(
      all_regions.begin(),
      all_regions.end(),
      [](const auto& left, const auto& right) {
        return left.offset < right.offset;
      });
  std::uint64_t previous_end = 0;
  for (std::size_t index = 0; index < all_regions.size(); ++index) {
    std::uint64_t end{};
    if (!checked_add(
            all_regions[index].offset,
            all_regions[index].length,
            end) ||
        (index != 0 && all_regions[index].offset < previous_end)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"VSS dcimg論理領域重複",
          L"Snapshot領域と固定読取り領域が重複しています"));
    }
    previous_end = end;
  }
  return clonecore::success_status();
}

clonecore::Status append_volume_chunks(
    const VssSnapshotImageVolume& volume,
    const std::uint32_t chunk_size,
    const std::vector<clonecore::ByteRange>& ranges,
    std::vector<imageformat::DcimgStreamChunk>& chunks) {
  if (ranges.empty()) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"VSS Snapshot Bitmap",
        L"NTFS使用範囲が0件のためSnapshotイメージを作成できません"));
  }

  const std::uint64_t cluster_size = volume.geometry.cluster_size();
  std::uint64_t geometry_bytes{};
  if (!checked_multiply(
          volume.geometry.total_sectors,
          volume.geometry.bytes_per_sector,
          geometry_bytes)) {
    return clonecore::Status::failure(image_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"VSS Snapshot NTFS容量",
        L"NTFS容量の計算がオーバーフローしました"));
  }
  std::uint64_t previous_end = 0;
  bool first = true;
  for (const auto& range : ranges) {
    std::uint64_t range_end{};
    if (range.length == 0 ||
        range.offset % cluster_size != 0 ||
        range.length % cluster_size != 0 ||
        !checked_add(range.offset, range.length, range_end) ||
        range_end > geometry_bytes ||
        (!first && range.offset < previous_end)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"VSS Snapshot Bitmap境界",
          L"使用範囲が空、重複、非整列、またはNTFS境界外です"));
    }

    std::uint64_t consumed = 0;
    while (consumed < range.length) {
      const std::uint64_t length = std::min<std::uint64_t>(
          range.length - consumed, chunk_size);
      std::uint64_t source_offset{};
      std::uint64_t logical_offset{};
      if (!checked_add(range.offset, consumed, source_offset) ||
          !checked_add(volume.disk_offset, source_offset, logical_offset)) {
        return clonecore::Status::failure(image_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"VSS dcimg論理位置",
            L"Snapshot使用範囲のディスク論理位置がオーバーフローしました"));
      }
      if (chunks.size() >= imageformat::kDcimgMaximumChunkCount) {
        return clonecore::Status::failure(image_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"VSS dcimgチャンク数",
            L"Snapshot使用範囲がdcimg v1のチャンク数上限を超えています"));
      }
      chunks.push_back(imageformat::DcimgStreamChunk{
          .logical_offset = logical_offset,
          .logical_length = length,
          .source_offset = source_offset,
          .zero_filled = false,
          .source = volume.snapshot_reader,
      });
      consumed += length;
    }
    previous_end = range_end;
    first = false;
  }
  return clonecore::success_status();
}

clonecore::Status append_raw_chunks(
    const VssSnapshotImageRawRegion& region,
    const std::uint32_t chunk_size,
    std::vector<imageformat::DcimgStreamChunk>& chunks) {
  std::uint64_t consumed = 0;
  while (consumed < region.length) {
    const std::uint64_t length = std::min<std::uint64_t>(
        region.length - consumed, chunk_size);
    std::uint64_t logical_offset{};
    std::uint64_t source_offset{};
    if (!checked_add(region.disk_offset, consumed, logical_offset) ||
        !checked_add(region.source_offset, consumed, source_offset)) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"VSS dcimg固定領域位置",
          L"固定読取り領域の位置がオーバーフローしました"));
    }
    if (chunks.size() >= imageformat::kDcimgMaximumChunkCount) {
      return clonecore::Status::failure(image_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"VSS dcimgチャンク数",
          L"固定読取り領域がdcimg v1のチャンク数上限を超えています"));
    }
    chunks.push_back(imageformat::DcimgStreamChunk{
        .logical_offset = logical_offset,
        .logical_length = length,
        .source_offset = source_offset,
        .zero_filled = false,
        .source = region.source_reader,
    });
    consumed += length;
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<imageformat::DcimgStreamBuildReport>
write_vss_snapshot_dcimg_v1(
    const VssSnapshotImageRequest& request,
    clonecore::INtfsUsedRangeProvider& bitmap_provider,
    imageformat::IDcimgStagingTarget& target) {
  const auto valid = validate_request(request);
  if (!valid) {
    return clonecore::Result<
        imageformat::DcimgStreamBuildReport>::failure(valid.error());
  }

  imageformat::DcimgStreamBuildRequest stream;
  stream.source_disk_size = request.source_disk_size;
  stream.logical_sector_size = request.logical_sector_size;
  stream.physical_sector_size = request.physical_sector_size;
  stream.chunk_size = request.chunk_size;
  stream.compression = request.compression;
  stream.verification_block_bytes = request.verification_block_bytes;
  stream.manifest = request.manifest;
  stream.partition_table_snapshot = request.partition_table_snapshot;

  for (const auto& volume : request.volumes) {
    auto ranges = bitmap_provider.query_used_ranges(
        volume.partition_entry_index, volume.geometry);
    if (!ranges) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(ranges.error());
    }
    const auto appended = append_volume_chunks(
        volume, request.chunk_size, ranges.value(), stream.chunks);
    if (!appended) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(appended.error());
    }
  }
  for (const auto& region : request.raw_regions) {
    const auto appended =
        append_raw_chunks(region, request.chunk_size, stream.chunks);
    if (!appended) {
      return clonecore::Result<
          imageformat::DcimgStreamBuildReport>::failure(
              appended.error());
    }
  }
  std::sort(
      stream.chunks.begin(),
      stream.chunks.end(),
      [](const auto& left, const auto& right) {
        return left.logical_offset < right.logical_offset;
      });

  return imageformat::write_verified_dcimg_v1(
      stream, target, request.callbacks);
}

}  // namespace ytec::vssrequester

#include "ytec/imageformat/image_inspection.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace ytec::imageformat {
namespace {

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

bool overlaps(
    const std::uint64_t left_offset,
    const std::uint64_t left_length,
    const std::uint64_t right_offset,
    const std::uint64_t right_length) noexcept {
  std::uint64_t left_end{};
  std::uint64_t right_end{};
  return !checked_add(left_offset, left_length, left_end) ||
         !checked_add(right_offset, right_length, right_end) ||
         (left_offset < right_end && right_offset < left_end);
}

clonecore::Status layout_failure(
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(clonecore::Error{
      .code = clonecore::ErrorCode::invalid_data,
      .native_code = ERROR_INVALID_DATA,
      .operation = std::move(operation),
      .message = std::move(message),
  });
}

}  // namespace

clonecore::Result<DcimgMetadataInspection>
inspect_dcimg_metadata_v1(
    const DcimgHeader& header,
    const std::span<const std::byte> manifest_bytes,
    const std::span<const std::byte> partition_snapshot_bytes) {
  auto manifest = inspect_backup_manifest_v1(manifest_bytes);
  if (!manifest) {
    return clonecore::Result<DcimgMetadataInspection>::failure(
        manifest.error());
  }
  auto snapshot =
      inspect_partition_snapshot_v1(partition_snapshot_bytes);
  if (!snapshot) {
    return clonecore::Result<DcimgMetadataInspection>::failure(
        snapshot.error());
  }

  if (snapshot.value().source_disk_size != header.source_disk_size ||
      snapshot.value().logical_sector_size !=
          header.logical_sector_size ||
      manifest.value().source.size_bytes != header.source_disk_size ||
      manifest.value().source.logical_sector_size !=
          header.logical_sector_size ||
      manifest.value().physical_sector_size !=
          header.physical_sector_size ||
      manifest.value().chunk_size != header.chunk_size ||
      manifest.value().compression != header.compression ||
      (snapshot.value().style == PartitionTableStyle::gpt) !=
          (manifest.value().partition_style ==
           BackupPartitionStyle::gpt)) {
    return clonecore::Result<DcimgMetadataInspection>::failure(
        clonecore::Error{
            .code = clonecore::ErrorCode::invalid_data,
            .native_code = ERROR_INVALID_DATA,
            .operation = L"dcimgメタデータ整合",
            .message =
                L"コンテナ、マニフェスト、パーティション表の寸法または形式が一致しません",
        });
  }

  return clonecore::Result<DcimgMetadataInspection>::success(
      DcimgMetadataInspection{
          .manifest = manifest.take_value(),
          .partition_snapshot = snapshot.take_value(),
      });
}

clonecore::Status validate_dcimg_restore_layout_v1(
    const DcimgInspection& container,
    const DcimgMetadataInspection& metadata) {
  for (const auto& chunk : container.chunks) {
    const auto declared_partition = std::find_if(
        metadata.manifest.partitions.begin(),
        metadata.manifest.partitions.end(),
        [&chunk](const auto& partition) {
          return overlaps(
                     chunk.logical_offset,
                     chunk.uncompressed_length,
                     partition.offset_bytes,
                     partition.length_bytes) &&
                 chunk.logical_offset >= partition.offset_bytes &&
                 chunk.uncompressed_length <=
                     partition.length_bytes -
                         (chunk.logical_offset -
                          partition.offset_bytes);
        });
    if (declared_partition == metadata.manifest.partitions.end() ||
        declared_partition->role ==
            BackupPartitionRole::microsoft_reserved) {
      return layout_failure(
          L"dcimgデータとマニフェスト領域",
          L"データチャンクが宣言済みパーティション内に収まっていません");
    }
    for (const auto& region : metadata.partition_snapshot.regions) {
      if (overlaps(
              chunk.logical_offset,
              chunk.uncompressed_length,
              region.disk_offset,
              region.data.size())) {
        return layout_failure(
            L"dcimg復元領域分離",
            L"データチャンクと最後に確定するパーティション表領域が重複しています");
      }
    }
  }
  return clonecore::success_status();
}

}  // namespace ytec::imageformat

#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/image_primitives.h"
#include "ytec/imageformat/sha256.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::uint32_t kDcimgHeaderSize = 256;
inline constexpr std::uint32_t kDcimgChunkRecordSize = 80;
inline constexpr std::uint32_t kDcimgFooterSize = 64;
inline constexpr std::uint32_t kDcimgChunkSize16MiB = kImageChunkSize16MiB;
inline constexpr std::uint32_t kDcimgChunkSize32MiB = kImageChunkSize32MiB;
inline constexpr std::uint64_t kDcimgMaximumChunkCount = 1'048'576;
inline constexpr std::uint16_t kDcimgZstandardProfileVersion = 1;

using DcimgCompression = ImageCompression;

enum class DcimgChunkFlags : std::uint32_t {
  none = 0,
  zero_filled = 1,
};

using DcimgSection = ImageSection;

struct DcimgHeader final {
  std::uint16_t major_version{};
  std::uint16_t minor_version{};
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{};
  DcimgCompression compression{DcimgCompression::none};
  std::uint16_t compression_version{};
  std::uint64_t chunk_count{};
  DcimgSection manifest;
  DcimgSection partition_table_snapshot;
  DcimgSection chunk_index;
  DcimgSection data;
  DcimgSection hash_table;
  DcimgSection footer;
  Sha256Digest manifest_hash{};
};

struct DcimgChunkRecord final {
  std::uint64_t logical_offset{};
  std::uint64_t uncompressed_length{};
  std::uint64_t stored_offset{};
  std::uint64_t stored_length{};
  DcimgChunkFlags flags{DcimgChunkFlags::none};
  DcimgCompression compression{DcimgCompression::none};
  Sha256Digest sha256{};
};

struct DcimgInspection final {
  DcimgHeader header;
  std::vector<DcimgChunkRecord> chunks;
  Sha256Digest global_hash{};
  bool manifest_hash_verified{};
  bool chunk_hashes_verified{};
  bool global_hash_verified{};
};

struct DcimgReadInspection final {
  DcimgInspection container;
  std::vector<std::byte> manifest;
  std::vector<std::byte> partition_table_snapshot;
};

struct DcimgReadInspectionProgress final {
  std::uint64_t verified_bytes{};
  std::uint64_t total_verify_bytes{};
};

struct DcimgReadInspectionCallbacks final {
  std::function<void(const DcimgReadInspectionProgress&)> progress;
};

struct DcimgBuildChunk final {
  std::uint64_t logical_offset{};
  std::uint64_t logical_length{};
  bool zero_filled{};
  std::vector<std::byte> data;
};

struct DcimgBuildRequest final {
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{kDcimgChunkSize16MiB};
  DcimgCompression compression{DcimgCompression::none};
  std::vector<std::byte> manifest;
  std::vector<std::byte> partition_table_snapshot;
  std::vector<DcimgBuildChunk> chunks;
};

class IDcimgStagingTarget {
 public:
  virtual ~IDcimgStagingTarget() = default;

  // 最終成果物とは分離された未完了領域を新規準備する。
  [[nodiscard]] virtual clonecore::Status begin(
      std::uint64_t expected_length) = 0;
  [[nodiscard]] virtual clonecore::Status write_at(
      std::uint64_t offset,
      std::span<const std::byte> bytes) = 0;
  // begin may reserve a safe upper bound. Before read-back verification, the
  // writer must shrink it exactly once to the canonical final image length.
  [[nodiscard]] virtual clonecore::Status resize_before_verification(
      std::uint64_t final_length) = 0;
  [[nodiscard]] virtual clonecore::Result<std::vector<std::byte>> read_at(
      std::uint64_t offset,
      std::size_t length) const = 0;
  [[nodiscard]] virtual clonecore::Status flush() = 0;

  // 全読戻し検証後だけ呼ぶ。失敗時はabort_incompleteを続けて試す。
  [[nodiscard]] virtual clonecore::Status commit_verified() = 0;
  [[nodiscard]] virtual clonecore::Status abort_incomplete() = 0;
};

struct DcimgStreamChunk final {
  std::uint64_t logical_offset{};
  std::uint64_t logical_length{};
  std::uint64_t source_offset{};
  bool zero_filled{};
  const clonecore::ISourceDiskReader* source{};
};

struct DcimgStreamBuildRequest final {
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{kDcimgChunkSize16MiB};
  DcimgCompression compression{DcimgCompression::none};
  std::size_t verification_block_bytes{1024U * 1024U};
  std::vector<std::byte> manifest;
  std::vector<std::byte> partition_table_snapshot;
  std::vector<DcimgStreamChunk> chunks;
};

struct DcimgStreamBuildReport final {
  std::uint64_t image_length{};
  std::uint64_t stored_data_bytes{};
  std::uint64_t zero_filled_bytes{};
  std::uint64_t chunk_count{};
  bool all_chunks_read_back_verified{};
  bool global_hash_read_back_verified{};
  bool committed{};
};

// Builds an in-memory v1 container. Zstandard profile 1 is used per chunk only
// when it is strictly smaller; incompressible chunks remain uncompressed.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
build_dcimg_v1(const DcimgBuildRequest& request);

// Compatibility entry point that rejects non-none compression.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
build_uncompressed_dcimg_v1(const DcimgBuildRequest& request);

// 大容量Snapshot用v1段階出力。Zstandardはチャンク単位で圧縮し、
// 圧縮後が小さくならないチャンクだけ非圧縮に戻す。全チャンクと全体SHA-256を
// StagingTargetから読み戻し、成功後だけcommit_verifiedを呼ぶ。
[[nodiscard]] clonecore::Result<DcimgStreamBuildReport>
write_verified_dcimg_v1(
    const DcimgStreamBuildRequest& request,
    IDcimgStagingTarget& target,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// 非圧縮互換API。圧縮指定は拒否する。
// 全チャンクと全体SHA-256を
// StagingTargetから読み戻し、成功後だけcommit_verifiedを呼ぶ。
// 失敗時は開始済みの未完了出力へabort_incompleteを必ず試す。
[[nodiscard]] clonecore::Result<DcimgStreamBuildReport>
write_verified_uncompressed_dcimg_v1(
    const DcimgStreamBuildRequest& request,
    IDcimgStagingTarget& target,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Treats every byte as untrusted.  A successful result means the implemented
// v1 structure and hashes are valid; it does not authorize restoration.
[[nodiscard]] clonecore::Result<DcimgInspection> inspect_dcimg_v1(
    std::span<const std::byte> image);

// Fully verifies a large image through bounded read-only callbacks without
// loading the data section into memory. Header/footer are re-read at the end
// so a file changed during inspection is rejected.
[[nodiscard]] clonecore::Result<DcimgReadInspection>
inspect_dcimg_v1_from_reader(
    std::uint64_t image_length,
    std::size_t maximum_block_bytes,
    const Sha256ReadCallback& reader,
    const DcimgReadInspectionCallbacks& callbacks = {});

}  // namespace ytec::imageformat

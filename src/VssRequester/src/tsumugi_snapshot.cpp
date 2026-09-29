#include "ytec/vssrequester/tsumugi_snapshot.h"

#include "ytec/clonecore/windows_volume_bitmap.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::vssrequester {
namespace {

clonecore::Error snapshot_error(
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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(snapshot_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0U &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool is_zero_digest(const imageformat::Sha256Digest& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

void append_u32(
    std::vector<std::byte>& bytes,
    const std::uint32_t value) {
  for (std::size_t index = 0U; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::byte>(
        (value >> (index * 8U)) & 0xFFU));
  }
}

void append_u64(
    std::vector<std::byte>& bytes,
    const std::uint64_t value) {
  for (std::size_t index = 0U; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::byte>(
        (value >> (index * 8U)) & 0xFFU));
  }
}

void append_digest(
    std::vector<std::byte>& bytes,
    const imageformat::Sha256Digest& digest) {
  bytes.insert(bytes.end(), digest.begin(), digest.end());
}

struct RawFingerprint final {
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  imageformat::Sha256Digest digest{};
};

clonecore::Result<imageformat::Sha256Digest> derive_source_state_hash(
    const TsumugiSnapshotImageRequest& request,
    const SnapshotCopyContext& snapshot_context,
    const std::span<const RawFingerprint> raw_fingerprints) {
  constexpr std::array<std::byte, 24> domain{
      std::byte{'Y'}, std::byte{'T'}, std::byte{'E'}, std::byte{'C'},
      std::byte{'-'}, std::byte{'T'}, std::byte{'S'}, std::byte{'U'},
      std::byte{'M'}, std::byte{'U'}, std::byte{'G'}, std::byte{'I'},
      std::byte{'-'}, std::byte{'V'}, std::byte{'S'}, std::byte{'S'},
      std::byte{'-'}, std::byte{'S'}, std::byte{'T'}, std::byte{'A'},
      std::byte{'T'}, std::byte{'E'}, std::byte{'-'}, std::byte{'1'}};
  std::vector<std::byte> state(domain.begin(), domain.end());
  append_digest(state, request.image.manifest.source_model_hash);
  append_digest(state, request.image.manifest.source_serial_hash);
  append_digest(state, request.locked_source_state_hash);
  const auto layout_hash = imageformat::sha256(
      request.image.manifest.partition_snapshot);
  if (!layout_hash) {
    return clonecore::Result<imageformat::Sha256Digest>::failure(
        layout_hash.error());
  }
  append_digest(state, layout_hash.value());
  append_u64(state, request.image.manifest.source_disk_size);
  append_u32(state, request.image.manifest.logical_sector_size);
  append_u32(state, request.image.manifest.physical_sector_size);
  const auto append_wide = [&](const std::wstring& value) {
    if (value.empty() || value.size() > 32U * 1024U) {
      return false;
    }
    append_u32(state, static_cast<std::uint32_t>(value.size()));
    const auto value_bytes = std::as_bytes(std::span(value));
    state.insert(state.end(), value_bytes.begin(), value_bytes.end());
    return true;
  };
  if (!append_wide(snapshot_context.snapshot_set_id)) {
    return failure<imageformat::Sha256Digest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_NAME,
        L"Tsumugi VSS Snapshot Set識別",
        L"Snapshot Set IDが空または上限超過です");
  }
  append_u32(state, static_cast<std::uint32_t>(request.volumes.size()));
  for (std::size_t index = 0U; index < request.volumes.size(); ++index) {
    const auto& mapping = snapshot_context.mappings[index];
    if (!append_wide(mapping.original_volume_guid_path) ||
        !append_wide(mapping.snapshot_id) ||
        !append_wide(mapping.snapshot_device_path)) {
      return failure<imageformat::Sha256Digest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_NAME,
          L"Tsumugi VSS状態識別",
          L"元Volume、Snapshot ID、またはデバイスパスが空か上限超過です");
    }
    append_u32(state, request.volumes[index].partition_entry_index);
    append_u64(state, request.volumes[index].disk_offset);
    append_u64(state, request.volumes[index].partition_length);
  }
  append_u32(state, static_cast<std::uint32_t>(request.raw_regions.size()));
  for (const auto& region : request.raw_regions) {
    append_u32(state, region.partition_entry_index);
    append_u64(state, region.disk_offset);
    append_u64(state, region.length);
    append_u64(state, region.source_offset);
  }
  append_u32(state, static_cast<std::uint32_t>(raw_fingerprints.size()));
  for (const auto& fingerprint : raw_fingerprints) {
    append_u64(state, fingerprint.disk_offset);
    append_u64(state, fingerprint.length);
    append_digest(state, fingerprint.digest);
  }
  return imageformat::sha256(state);
}

struct CompositeBinding final {
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  std::uint64_t source_offset{};
  const clonecore::ISourceDiskReader* reader{};
  bool verify_fingerprint{};
};

class CompositeSnapshotSourceSession final
    : public imageformat::ITsumugiImageSourceSession {
 public:
  CompositeSnapshotSourceSession(
      const std::uint64_t disk_size,
      const std::uint32_t logical_sector,
      imageformat::Sha256Digest model_hash,
      imageformat::Sha256Digest serial_hash,
      imageformat::Sha256Digest state_hash,
      std::vector<std::unique_ptr<clonecore::ISourceDiskReader>> owners,
      std::vector<CompositeBinding> bindings,
      std::vector<RawFingerprint> raw_fingerprints) noexcept
      : disk_size_(disk_size),
        logical_sector_(logical_sector),
        model_hash_(model_hash),
        serial_hash_(serial_hash),
        state_hash_(state_hash),
        owners_(std::move(owners)),
        bindings_(std::move(bindings)),
        raw_fingerprints_(std::move(raw_fingerprints)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return disk_size_;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return logical_sector_;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    std::uint64_t end{};
    if (length == 0U ||
        !checked_add(offset, static_cast<std::uint64_t>(length), end) ||
        end > disk_size_) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"Tsumugi複合Snapshot読取り",
          L"要求範囲が空、オーバーフロー、またはディスク境界外です");
    }
    const CompositeBinding* selected = nullptr;
    for (const auto& binding : bindings_) {
      std::uint64_t binding_end{};
      if (!checked_add(binding.disk_offset, binding.length, binding_end)) {
        continue;
      }
      if (offset >= binding.disk_offset && end <= binding_end) {
        if (selected != nullptr) {
          return failure<std::vector<std::byte>>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DUP_NAME,
              L"Tsumugi複合Snapshot対応",
              L"同じ読取り範囲に複数のSourceが対応します");
        }
        selected = &binding;
      }
    }
    if (selected == nullptr || selected->reader == nullptr) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Tsumugi複合Snapshot対応",
          L"読取り範囲に対応するSnapshotまたは固定領域がありません");
    }
    std::uint64_t relative{};
    std::uint64_t source_offset{};
    relative = offset - selected->disk_offset;
    if (!checked_add(selected->source_offset, relative, source_offset)) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi複合Snapshot Source位置",
          L"Source読取り位置が64bit上限を超えます");
    }
    auto bytes = selected->reader->read(source_offset, length);
    if (!bytes) {
      return bytes;
    }
    if (bytes.value().size() != length) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::io_failed,
          ERROR_HANDLE_EOF,
          L"Tsumugi複合Snapshot完全読取り",
          L"Sourceが要求長を完全に返しませんでした");
    }
    if (selected->verify_fingerprint) {
      const auto expected = std::find_if(
          raw_fingerprints_.begin(),
          raw_fingerprints_.end(),
          [&](const auto& fingerprint) {
            return fingerprint.disk_offset == offset &&
                fingerprint.length == length;
          });
      if (expected == raw_fingerprints_.end()) {
        return failure<std::vector<std::byte>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"Tsumugi固定領域Fingerprint対応",
            L"固定領域チャンクに事前Fingerprintがありません");
      }
      const auto actual = imageformat::sha256(bytes.value());
      if (!actual || actual.value() != expected->digest) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            actual ? snapshot_error(
                         clonecore::ErrorCode::identity_mismatch,
                         ERROR_CRC,
                         L"Tsumugi固定領域不変性検証",
                         L"VSS作成後にESP／回復等の固定領域が変化しました。PEから実行してください")
                   : actual.error());
      }
    }
    return bytes;
  }

  [[nodiscard]] imageformat::Sha256Digest
  source_model_hash() const noexcept override {
    return model_hash_;
  }

  [[nodiscard]] imageformat::Sha256Digest
  source_serial_hash() const noexcept override {
    return serial_hash_;
  }

  [[nodiscard]] imageformat::Sha256Digest
  source_state_hash() const noexcept override {
    return state_hash_;
  }

 private:
  std::uint64_t disk_size_{};
  std::uint32_t logical_sector_{};
  imageformat::Sha256Digest model_hash_{};
  imageformat::Sha256Digest serial_hash_{};
  imageformat::Sha256Digest state_hash_{};
  // Keeps every Snapshot handle alive for the complete stream and read-back.
  std::vector<std::unique_ptr<clonecore::ISourceDiskReader>> owners_;
  std::vector<CompositeBinding> bindings_;
  std::vector<RawFingerprint> raw_fingerprints_;
};

struct OpenedVolume final {
  TsumugiSnapshotVolumePlan plan;
  clonecore::NtfsGeometry geometry;
  const clonecore::ISourceDiskReader* reader{};
  std::vector<clonecore::ByteRange> used_ranges;
};

clonecore::Status append_piece(
    const std::uint64_t offset,
    const std::uint64_t length,
    const bool zero,
    const std::uint32_t chunk_size,
    const imageformat::ITsumugiImageSourceSession* source,
    std::vector<imageformat::TsumugiStreamBuildChunk>& chunks) {
  std::uint64_t completed = 0U;
  while (completed < length) {
    const std::uint64_t amount = std::min<std::uint64_t>(
        length - completed, chunk_size);
    std::uint64_t logical_offset{};
    if (!checked_add(offset, completed, logical_offset) ||
        chunks.size() >= imageformat::kTsumugiMaximumChunkCount) {
      return clonecore::Status::failure(snapshot_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi Snapshotチャンク追加",
          L"チャンク位置またはチャンク数が形式上限を超えます"));
    }
    chunks.push_back(imageformat::TsumugiStreamBuildChunk{
        .logical_offset = logical_offset,
        .logical_length = amount,
        .source_offset = zero ? 0U : logical_offset,
        .flags = zero ? imageformat::TsumugiChunkFlags::zero_filled
                      : imageformat::TsumugiChunkFlags::none,
        .source = zero ? nullptr : source,
    });
    completed += amount;
  }
  return clonecore::success_status();
}

clonecore::Result<std::uint64_t> append_ntfs_partition(
    const OpenedVolume& volume,
    const std::uint32_t chunk_size,
    const imageformat::ITsumugiImageSourceSession* source,
    std::vector<imageformat::TsumugiStreamBuildChunk>& chunks) {
  const std::uint64_t cluster_size = volume.geometry.cluster_size();
  std::uint64_t geometry_bytes{};
  if (cluster_size == 0U || chunk_size % cluster_size != 0U ||
      !checked_multiply(
          volume.geometry.total_sectors,
          volume.geometry.bytes_per_sector,
          geometry_bytes) ||
      geometry_bytes > volume.plan.partition_length) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi Snapshot NTFS寸法",
        L"NTFS Geometryとパーティション境界が一致しません");
  }
  std::uint64_t cursor = 0U;
  std::uint64_t used_bytes = 0U;
  for (const auto& range : volume.used_ranges) {
    std::uint64_t range_end{};
    if (range.length == 0U || range.offset < cursor ||
        range.offset % cluster_size != 0U ||
        range.length % cluster_size != 0U ||
        !checked_add(range.offset, range.length, range_end) ||
        range_end > geometry_bytes ||
        !checked_add(used_bytes, range.length, used_bytes)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"Tsumugi Snapshot Bitmap境界",
          L"NTFS使用範囲が空、重複、非整列、または境界外です");
    }
    if (range.offset > cursor) {
      std::uint64_t gap_offset{};
      if (!checked_add(volume.plan.disk_offset, cursor, gap_offset)) {
        return failure<std::uint64_t>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"Tsumugi Snapshot未使用範囲",
            L"未使用範囲の位置が64bit上限を超えます");
      }
      const auto zeroed = append_piece(
          gap_offset,
          range.offset - cursor,
          true,
          chunk_size,
          source,
          chunks);
      if (!zeroed) {
        return clonecore::Result<std::uint64_t>::failure(zeroed.error());
      }
    }
    std::uint64_t used_offset{};
    if (!checked_add(volume.plan.disk_offset, range.offset, used_offset)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi Snapshot使用範囲",
          L"使用範囲の位置が64bit上限を超えます");
    }
    const auto copied = append_piece(
        used_offset,
        range.length,
        false,
        chunk_size,
        source,
        chunks);
    if (!copied) {
      return clonecore::Result<std::uint64_t>::failure(copied.error());
    }
    cursor = range_end;
  }
  if (cursor < volume.plan.partition_length) {
    std::uint64_t tail_offset{};
    if (!checked_add(volume.plan.disk_offset, cursor, tail_offset)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi Snapshot末尾範囲",
          L"末尾範囲の位置が64bit上限を超えます");
    }
    const auto zeroed = append_piece(
        tail_offset,
        volume.plan.partition_length - cursor,
        true,
        chunk_size,
        source,
        chunks);
    if (!zeroed) {
      return clonecore::Result<std::uint64_t>::failure(zeroed.error());
    }
  }
  return clonecore::Result<std::uint64_t>::success(used_bytes);
}

bool selected(
    const imageformat::TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
          imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
}

clonecore::Result<std::vector<RawFingerprint>> fingerprint_raw_regions(
    const TsumugiSnapshotImageRequest& request) {
  std::vector<RawFingerprint> fingerprints;
  for (const auto& region : request.raw_regions) {
    std::uint64_t disk_end{};
    std::uint64_t source_end{};
    if (region.length == 0U || request.locked_raw_source == nullptr ||
        region.disk_offset % request.image.manifest.logical_sector_size != 0U ||
        region.length % request.image.manifest.logical_sector_size != 0U ||
        region.source_offset % request.image.manifest.logical_sector_size != 0U ||
        !checked_add(region.disk_offset, region.length, disk_end) ||
        disk_end > request.image.manifest.source_disk_size ||
        !checked_add(region.source_offset, region.length, source_end) ||
        source_end > request.locked_raw_source->size_bytes()) {
      return failure<std::vector<RawFingerprint>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi固定領域Fingerprint計画",
          L"固定領域が空、未整列、またはSource境界外です");
    }
    std::uint64_t completed = 0U;
    while (completed < region.length) {
      const std::uint64_t amount64 = std::min<std::uint64_t>(
          region.length - completed, request.image.chunk_size);
      if (amount64 > (std::numeric_limits<std::size_t>::max)()) {
        return failure<std::vector<RawFingerprint>>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"Tsumugi固定領域Fingerprint寸法",
            L"Fingerprintチャンクがsize_t上限を超えます");
      }
      std::uint64_t disk_offset{};
      std::uint64_t source_offset{};
      if (!checked_add(region.disk_offset, completed, disk_offset) ||
          !checked_add(region.source_offset, completed, source_offset)) {
        return failure<std::vector<RawFingerprint>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"Tsumugi固定領域Fingerprint位置",
            L"Fingerprint位置が64bit上限を超えます");
      }
      auto bytes = request.locked_raw_source->read(
          source_offset, static_cast<std::size_t>(amount64));
      if (!bytes) {
        return clonecore::Result<std::vector<RawFingerprint>>::failure(
            bytes.error());
      }
      if (bytes.value().size() != amount64) {
        return failure<std::vector<RawFingerprint>>(
            clonecore::ErrorCode::io_failed,
            ERROR_HANDLE_EOF,
            L"Tsumugi固定領域Fingerprint読取り",
            L"固定領域を要求長どおりに読み取れませんでした");
      }
      auto digest = imageformat::sha256(bytes.value());
      if (!digest) {
        return clonecore::Result<std::vector<RawFingerprint>>::failure(
            digest.error());
      }
      fingerprints.push_back(RawFingerprint{
          .disk_offset = disk_offset,
          .length = amount64,
          .digest = digest.take_value(),
      });
      completed += amount64;
    }
  }
  return clonecore::Result<std::vector<RawFingerprint>>::success(
      std::move(fingerprints));
}

clonecore::Result<std::uint64_t> estimate_maximum_image_bytes(
    const imageformat::TsumugiImageCreateRequest& image) {
  auto manifest = imageformat::build_tsumugi_manifest_v1(image.manifest);
  if (!manifest) {
    return clonecore::Result<std::uint64_t>::failure(manifest.error());
  }
  std::uint64_t result = imageformat::kTsumugiHeaderSize;
  std::uint64_t records_bytes{};
  if (!checked_multiply(
          image.chunks.size(),
          imageformat::kTsumugiChunkRecordSize,
          records_bytes) ||
      !checked_add(result, manifest.value().size(), result) ||
      !checked_add(result, imageformat::kTsumugiMetadataHeaderSize, result) ||
      !checked_add(result, records_bytes, result)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Tsumugi保存容量見積り",
        L"メタデータ寸法が64bit上限を超えます");
  }
  for (const auto& chunk : image.chunks) {
    if (chunk.flags == imageformat::TsumugiChunkFlags::zero_filled ||
        chunk.flags ==
            imageformat::TsumugiChunkFlags::unreadable_zero_filled) {
      continue;
    }
    std::uint64_t payload_bound = chunk.logical_length;
    if (image.compression == imageformat::ImageCompression::zstandard) {
      const std::uint64_t headroom =
          chunk.logical_length / 64U + 256U;
      if (!checked_add(payload_bound, headroom, payload_bound)) {
        return failure<std::uint64_t>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_ARITHMETIC_OVERFLOW,
            L"Tsumugi圧縮容量見積り",
            L"ペイロード見積りが64bit上限を超えます");
      }
    }
    if (!checked_add(result, payload_bound, result)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi保存容量見積り",
          L"完成イメージ見積りが64bit上限を超えます");
    }
  }
  constexpr std::uint64_t kSafetyMargin = 64ULL * 1024ULL * 1024ULL;
  if (!checked_add(result, imageformat::kTsumugiFooterSize, result) ||
      !checked_add(result, kSafetyMargin, result)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Tsumugi保存容量安全余裕",
        L"保存容量の安全余裕を加算できません");
  }
  return clonecore::Result<std::uint64_t>::success(result);
}

}  // namespace

clonecore::Result<imageformat::TsumugiStagedImageV1>
prepare_tsumugi_snapshot_image_v1(
    const TsumugiSnapshotImageRequest& request,
    const SnapshotCopyContext& snapshot_context,
    const TsumugiSnapshotReaderOpenCallback& open_reader,
    clonecore::INtfsUsedRangeProvider& bitmap_provider,
    const clonecore::DiskOperationCallbacks& callbacks) {
  const auto& manifest = request.image.manifest;
  if (!open_reader || request.image.source_session != nullptr ||
      !request.image.chunks.empty() ||
      !request.revalidate_locked_layout ||
      !request.validate_destination_capacity ||
      manifest.mode != imageformat::TsumugiManifestMode::exact ||
      manifest.source_disk_size == 0U ||
      manifest.partition_snapshot.empty() ||
      (request.image.chunk_size != imageformat::kImageChunkSize16MiB &&
       request.image.chunk_size != imageformat::kImageChunkSize32MiB) ||
      snapshot_context.snapshot_set_id.empty() ||
      request.volumes.size() != snapshot_context.mappings.size() ||
      request.volumes.size() > 128U || request.raw_regions.size() > 128U ||
      is_zero_digest(manifest.source_model_hash) ||
      is_zero_digest(manifest.source_serial_hash) ||
      is_zero_digest(request.locked_source_state_hash) ||
      (!request.raw_regions.empty() && request.locked_raw_source == nullptr)) {
    return failure<imageformat::TsumugiStagedImageV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi Snapshot作成要求",
        L"作成元、exact形式、Snapshot件数、または安定識別が不正です");
  }
  if (request.locked_raw_source != nullptr &&
      (request.locked_raw_source->size_bytes() !=
           manifest.source_disk_size ||
       request.locked_raw_source->logical_sector_size() !=
           manifest.logical_sector_size)) {
    return failure<imageformat::TsumugiStagedImageV1>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi固定領域Source再確認",
        L"読取り専用物理Sourceの容量または論理セクターが変化しました");
  }

  const auto initial_layout = request.revalidate_locked_layout();
  if (!initial_layout) {
    return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
        initial_layout.error());
  }
  auto raw_fingerprints = fingerprint_raw_regions(request);
  if (!raw_fingerprints) {
    return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
        raw_fingerprints.error());
  }
  auto state_hash = derive_source_state_hash(
      request, snapshot_context, raw_fingerprints.value());
  if (!state_hash) {
    return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
        state_hash.error());
  }

  std::vector<std::unique_ptr<clonecore::ISourceDiskReader>> owners;
  std::vector<CompositeBinding> bindings;
  std::vector<OpenedVolume> opened_volumes;
  owners.reserve(request.volumes.size());
  bindings.reserve(request.volumes.size() + request.raw_regions.size());
  opened_volumes.reserve(request.volumes.size());

  for (std::size_t index = 0U; index < request.volumes.size(); ++index) {
    const auto& plan = request.volumes[index];
    const auto& mapping = snapshot_context.mappings[index];
    std::uint64_t plan_end{};
    if (plan.partition_entry_index == 0U || plan.partition_length == 0U ||
        plan.original_volume_guid_path.empty() ||
        mapping.snapshot_id.empty() ||
        mapping.snapshot_device_path.empty() ||
        _wcsicmp(
            plan.original_volume_guid_path.c_str(),
            mapping.original_volume_guid_path.c_str()) != 0 ||
        plan.disk_offset % manifest.logical_sector_size != 0U ||
        plan.partition_length % manifest.logical_sector_size != 0U ||
        !checked_add(plan.disk_offset, plan.partition_length, plan_end) ||
        plan_end > manifest.source_disk_size) {
      return failure<imageformat::TsumugiStagedImageV1>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi Snapshot Volume計画",
          L"Volume計画が空、未整列、オーバーフロー、またはディスク境界外です");
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (_wcsicmp(
              snapshot_context.mappings[previous].snapshot_id.c_str(),
              mapping.snapshot_id.c_str()) == 0 ||
          _wcsicmp(
              snapshot_context.mappings[previous].snapshot_device_path.c_str(),
              mapping.snapshot_device_path.c_str()) == 0) {
        return failure<imageformat::TsumugiStagedImageV1>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"Tsumugi Snapshot ID重複",
            L"Snapshot IDまたはデバイスパスが重複しています");
      }
    }
    auto opened = open_reader(SnapshotVolumeOpenRequest{
        .snapshot_device_path = mapping.snapshot_device_path,
        .expected_size_bytes = plan.partition_length,
        .logical_sector_size = manifest.logical_sector_size,
    });
    if (!opened) {
      return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
          opened.error());
    }
    if (!opened.value() ||
        opened.value()->size_bytes() != plan.partition_length ||
        opened.value()->logical_sector_size() !=
            manifest.logical_sector_size) {
      return failure<imageformat::TsumugiStagedImageV1>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi Snapshot Reader再確認",
          L"Snapshot Readerの寸法が計画と一致しません");
    }
    const auto boot = opened.value()->read(0U, manifest.logical_sector_size);
    if (!boot || boot.value().size() != manifest.logical_sector_size) {
      return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
          boot ? snapshot_error(
                     clonecore::ErrorCode::io_failed,
                     ERROR_HANDLE_EOF,
                     L"Tsumugi Snapshot NTFSブートセクター",
                     L"Snapshotから論理セクターを完全に読めません")
               : boot.error());
    }
    auto geometry = clonecore::parse_ntfs_geometry(
        boot.value(), manifest.logical_sector_size, plan.partition_length);
    if (!geometry) {
      return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
          geometry.error());
    }
    auto ranges = bitmap_provider.query_used_ranges(
        plan.partition_entry_index, geometry.value());
    if (!ranges) {
      return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
          ranges.error());
    }
    const auto* reader = opened.value().get();
    bindings.push_back(CompositeBinding{
        .disk_offset = plan.disk_offset,
        .length = plan.partition_length,
        .source_offset = 0U,
        .reader = reader,
        .verify_fingerprint = false,
    });
    owners.push_back(opened.take_value());
    opened_volumes.push_back(OpenedVolume{
        .plan = plan,
        .geometry = geometry.take_value(),
        .reader = reader,
        .used_ranges = ranges.take_value(),
    });
  }

  for (const auto& region : request.raw_regions) {
    std::uint64_t disk_end{};
    std::uint64_t source_end{};
    if (region.partition_entry_index == 0U || region.length == 0U ||
        region.disk_offset % manifest.logical_sector_size != 0U ||
        region.length % manifest.logical_sector_size != 0U ||
        region.source_offset % manifest.logical_sector_size != 0U ||
        !checked_add(region.disk_offset, region.length, disk_end) ||
        disk_end > manifest.source_disk_size ||
        !checked_add(region.source_offset, region.length, source_end) ||
        source_end > request.locked_raw_source->size_bytes()) {
      return failure<imageformat::TsumugiStagedImageV1>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi Snapshot固定領域",
          L"固定領域が空、未整列、またはSource境界外です");
    }
    bindings.push_back(CompositeBinding{
        .disk_offset = region.disk_offset,
        .length = region.length,
        .source_offset = region.source_offset,
        .reader = request.locked_raw_source,
        .verify_fingerprint = true,
    });
  }
  std::sort(bindings.begin(), bindings.end(), [](const auto& left,
                                                  const auto& right) {
    return left.disk_offset < right.disk_offset;
  });
  std::uint64_t previous_end = 0U;
  for (std::size_t index = 0U; index < bindings.size(); ++index) {
    std::uint64_t end{};
    if (!checked_add(bindings[index].disk_offset, bindings[index].length, end) ||
        (index != 0U && bindings[index].disk_offset < previous_end)) {
      return failure<imageformat::TsumugiStagedImageV1>(
          clonecore::ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"Tsumugi Snapshot Source領域重複",
          L"Snapshot領域と固定読取り領域が重複しています");
    }
    previous_end = end;
  }

  CompositeSnapshotSourceSession session(
      manifest.source_disk_size,
      manifest.logical_sector_size,
      manifest.source_model_hash,
      manifest.source_serial_hash,
      state_hash.value(),
      std::move(owners),
      std::move(bindings),
      raw_fingerprints.take_value());

  auto image = request.image;
  image.manifest.source_state_hash = state_hash.value();
  image.source_session = &session;
  std::map<std::uint32_t, bool> used_volume;
  std::map<std::uint32_t, bool> used_raw;
  for (const auto& volume : request.volumes) {
    if (!used_volume.emplace(volume.partition_entry_index, false).second) {
      return failure<imageformat::TsumugiStagedImageV1>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Tsumugi Snapshot Volume重複",
          L"同じパーティションへ複数のSnapshotを割り当てられません");
    }
  }
  for (const auto& region : request.raw_regions) {
    if (!used_raw.emplace(region.partition_entry_index, false).second ||
        used_volume.contains(region.partition_entry_index)) {
      return failure<imageformat::TsumugiStagedImageV1>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Tsumugi Snapshot固定領域重複",
          L"同じパーティションへ複数のSourceを割り当てられません");
    }
  }

  for (auto& partition : image.manifest.partitions) {
    if (!selected(partition)) {
      continue;
    }
    const auto volume = std::find_if(
        opened_volumes.begin(), opened_volumes.end(), [&](const auto& item) {
          return item.plan.partition_entry_index ==
              partition.source_table_index;
        });
    const auto raw = std::find_if(
        request.raw_regions.begin(),
        request.raw_regions.end(),
        [&](const auto& item) {
          return item.partition_entry_index == partition.source_table_index;
        });
    if (volume != opened_volumes.end()) {
      if (partition.file_system !=
              imageformat::TsumugiManifestFileSystem::ntfs ||
          partition.source_offset != volume->plan.disk_offset ||
          partition.source_size != volume->plan.partition_length) {
        return failure<imageformat::TsumugiStagedImageV1>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi Snapshotマニフェスト対応",
            L"NTFSパーティションとSnapshot Volume計画が一致しません");
      }
      auto appended = append_ntfs_partition(
          *volume,
          image.chunk_size,
          &session,
          image.chunks);
      if (!appended) {
        return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
            appended.error());
      }
      partition.used_bytes = appended.value();
      used_volume.at(partition.source_table_index) = true;
      continue;
    }
    if (raw != request.raw_regions.end()) {
      if (partition.source_offset != raw->disk_offset ||
          partition.source_size != raw->length) {
        return failure<imageformat::TsumugiStagedImageV1>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi固定領域マニフェスト対応",
            L"パーティションと固定読取り領域の寸法が一致しません");
      }
      const auto appended = append_piece(
          partition.payload_logical_offset,
          partition.payload_logical_length,
          false,
          image.chunk_size,
          &session,
          image.chunks);
      if (!appended) {
        return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
            appended.error());
      }
      used_raw.at(partition.source_table_index) = true;
      continue;
    }
    if (partition.role ==
            imageformat::TsumugiManifestPartitionRole::microsoft_reserved &&
        partition.file_system ==
            imageformat::TsumugiManifestFileSystem::none) {
      const auto appended = append_piece(
          partition.payload_logical_offset,
          partition.payload_logical_length,
          true,
          image.chunk_size,
          &session,
          image.chunks);
      if (!appended) {
        return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
            appended.error());
      }
      partition.used_bytes = 0U;
      continue;
    }
    return failure<imageformat::TsumugiStagedImageV1>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"Tsumugi選択パーティションSource",
        L"選択パーティションにSnapshotまたは固定読取りSourceがありません");
  }
  if (std::any_of(used_volume.begin(), used_volume.end(),
                  [](const auto& item) { return !item.second; }) ||
      std::any_of(used_raw.begin(), used_raw.end(),
                  [](const auto& item) { return !item.second; })) {
    return failure<imageformat::TsumugiStagedImageV1>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"Tsumugi未使用Source計画",
        L"マニフェストで選択されていないSource計画があります");
  }
  const auto required_bytes = estimate_maximum_image_bytes(image);
  if (!required_bytes) {
    return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
        required_bytes.error());
  }
  const auto destination =
      request.validate_destination_capacity(required_bytes.value());
  if (!destination) {
    return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
        destination.error());
  }
  auto staged = imageformat::prepare_tsumugi_image_v1(image, callbacks);
  if (!staged) {
    return staged;
  }
  if (staged.value().report().stream.image_length > required_bytes.value()) {
    auto error = snapshot_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Tsumugi保存容量見積り照合",
        L"完成イメージが検証済み保存容量見積りを超えました");
    const auto aborted = staged.value().abort_incomplete();
    if (!aborted) {
      error.message += L"。検証済み.partialの破棄にも失敗しました: " +
          aborted.error().message;
    }
    return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
        std::move(error));
  }
  const auto final_layout = request.revalidate_locked_layout();
  if (!final_layout) {
    auto primary = final_layout.error();
    const auto aborted = staged.value().abort_incomplete();
    if (!aborted) {
      primary.message += L"。検証済み.partialの破棄にも失敗しました: " +
          aborted.error().message;
    }
    return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
        std::move(primary));
  }
  return staged;
}

clonecore::Result<imageformat::TsumugiStagedImageV1>
prepare_tsumugi_snapshot_image_v1_with_windows_apis(
    const TsumugiSnapshotImageRequest& request,
    const SnapshotCopyContext& snapshot_context,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (snapshot_context.mappings.size() != request.volumes.size()) {
    return failure<imageformat::TsumugiStagedImageV1>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi Windows Snapshot件数",
        L"VSSが返したSnapshot件数が計画と一致しません");
  }
  std::vector<clonecore::SnapshotVolumeBitmapBinding> bitmap_bindings;
  bitmap_bindings.reserve(request.volumes.size());
  for (std::size_t index = 0U; index < request.volumes.size(); ++index) {
    bitmap_bindings.push_back(clonecore::SnapshotVolumeBitmapBinding{
        .partition_entry_index =
            request.volumes[index].partition_entry_index,
        .snapshot_device_path =
            snapshot_context.mappings[index].snapshot_device_path,
    });
  }
  clonecore::WindowsSnapshotVolumeBitmapProvider bitmap_provider(
      std::move(bitmap_bindings));
  return prepare_tsumugi_snapshot_image_v1(
      request,
      snapshot_context,
      [](const SnapshotVolumeOpenRequest& open_request) {
        return open_snapshot_volume_reader_with_windows_apis(open_request);
      },
      bitmap_provider,
      callbacks);
}

}  // namespace ytec::vssrequester

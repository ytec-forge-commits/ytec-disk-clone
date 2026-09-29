#pragma once

#include "ytec/clonecore/offline_gpt_clone.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::clonecore {

struct VolumeBitmapBinding final {
  std::uint32_t partition_entry_index{};
  std::wstring volume_device_path;
};

struct SnapshotVolumeBitmapBinding final {
  std::uint32_t partition_entry_index{};
  std::wstring snapshot_device_path;
};

struct DecodedVolumeBitmapChunk final {
  std::uint64_t next_lcn{};
  std::vector<ByteRange> used_ranges;
};

[[nodiscard]] Result<DecodedVolumeBitmapChunk> decode_volume_bitmap_chunk(
    std::uint64_t returned_starting_lcn,
    std::uint64_t returned_bitmap_size,
    std::span<const std::byte> bitmap_bytes,
    std::uint64_t requested_lcn,
    std::uint64_t volume_cluster_count,
    std::uint64_t cluster_size);

class WindowsVolumeBitmapProvider final : public INtfsUsedRangeProvider {
 public:
  explicit WindowsVolumeBitmapProvider(std::vector<VolumeBitmapBinding> bindings);

  [[nodiscard]] Result<std::vector<ByteRange>> query_used_ranges(
      std::uint32_t partition_index,
      const NtfsGeometry& geometry) override;

 private:
  std::vector<VolumeBitmapBinding> bindings_;
};

// VSSが返したSnapshotデバイスだけを受け付けるオンライン用Providerです。
// Volume GUIDパスは明示的に拒否します。
class WindowsSnapshotVolumeBitmapProvider final
    : public INtfsUsedRangeProvider {
 public:
  explicit WindowsSnapshotVolumeBitmapProvider(
      std::vector<SnapshotVolumeBitmapBinding> bindings);

  [[nodiscard]] Result<std::vector<ByteRange>> query_used_ranges(
      std::uint32_t partition_index,
      const NtfsGeometry& geometry) override;

 private:
  std::vector<SnapshotVolumeBitmapBinding> bindings_;
};

}  // namespace ytec::clonecore

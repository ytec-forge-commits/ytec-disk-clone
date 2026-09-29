#include "ytec/imageformat/partition_snapshot.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace ytec::imageformat {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'D'}, std::byte{'C'}, std::byte{'P'}, std::byte{'A'},
    std::byte{'R'}, std::byte{'T'}, std::byte{0x0D}, std::byte{0x0A}};
constexpr std::uint32_t kMaximumRegionCount = 8;
constexpr std::uint64_t kMaximumSnapshotBytes = 16U * 1024U * 1024U;

clonecore::Error data_error(std::wstring operation, std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::invalid_data,
      .native_code = ERROR_INVALID_DATA,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

clonecore::Error argument_error(std::wstring operation, std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::invalid_argument,
      .native_code = ERROR_INVALID_PARAMETER,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

clonecore::Error verification_error(
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::verification_failed,
      .native_code = ERROR_CRC,
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

bool all_zero(const std::span<const std::byte> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

template <typename T>
T read_little(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
void write_little(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) noexcept {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

clonecore::Status validate_semantics(const PartitionSnapshot& snapshot) {
  if ((snapshot.style != PartitionTableStyle::mbr &&
       snapshot.style != PartitionTableStyle::gpt) ||
      snapshot.source_disk_size == 0 ||
      (snapshot.logical_sector_size != 512 &&
       snapshot.logical_sector_size != 4096) ||
      snapshot.source_disk_size % snapshot.logical_sector_size != 0 ||
      snapshot.regions.empty() ||
      snapshot.regions.size() > kMaximumRegionCount) {
    return clonecore::Status::failure(argument_error(
        L"パーティション表スナップショット寸法",
        L"形式、ディスク、セクター、または領域数が対応条件外です"));
  }

  std::uint64_t previous_end = 0;
  for (std::size_t index = 0; index < snapshot.regions.size(); ++index) {
    const auto& region = snapshot.regions[index];
    std::uint64_t end{};
    if (region.data.empty() ||
        region.disk_offset % snapshot.logical_sector_size != 0 ||
        region.data.size() % snapshot.logical_sector_size != 0 ||
        !checked_add(region.disk_offset, region.data.size(), end) ||
        end > snapshot.source_disk_size ||
        (index != 0 && region.disk_offset < previous_end)) {
      return clonecore::Status::failure(argument_error(
          L"パーティション表スナップショット領域",
          L"領域が未整列、重複、またはディスク境界外です"));
    }
    previous_end = end;
  }

  const auto& first = snapshot.regions.front();
  const auto& last = snapshot.regions.back();
  if (first.disk_offset != 0) {
    return clonecore::Status::failure(argument_error(
        L"パーティション表スナップショット先頭",
        L"先頭領域はディスクオフセット0から始める必要があります"));
  }
  if (snapshot.style == PartitionTableStyle::mbr &&
      (snapshot.regions.size() != 1 ||
       first.data.size() != snapshot.logical_sector_size)) {
    return clonecore::Status::failure(argument_error(
        L"MBRスナップショット",
        L"MBRは先頭の論理セクター1個だけを保存します"));
  }
  std::uint64_t last_end{};
  if (snapshot.style == PartitionTableStyle::gpt &&
      (snapshot.regions.size() != 2 ||
       !checked_add(last.disk_offset, last.data.size(), last_end) ||
       last_end != snapshot.source_disk_size)) {
    return clonecore::Status::failure(argument_error(
        L"GPTスナップショット",
        L"GPTは先頭領域とディスク末尾で終わるバックアップ領域が必要です"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<std::vector<std::byte>> build_partition_snapshot_v1(
    const PartitionSnapshot& snapshot) {
  const auto valid = validate_semantics(snapshot);
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(valid.error());
  }

  std::uint64_t total = kPartitionSnapshotHeaderSize +
      snapshot.regions.size() * kPartitionSnapshotRecordSize;
  for (const auto& region : snapshot.regions) {
    if (!checked_add(total, region.data.size(), total) ||
        total > kMaximumSnapshotBytes) {
      return clonecore::Result<std::vector<std::byte>>::failure(argument_error(
          L"パーティション表スナップショット全長",
          L"スナップショットが安全な上限を超えています"));
    }
  }

  std::vector<std::byte> output(static_cast<std::size_t>(total), std::byte{0});
  const std::span<std::byte> bytes(output);
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  write_little<std::uint16_t>(bytes, 8, 1);
  write_little<std::uint16_t>(bytes, 10, 0);
  write_little(bytes, 12, kPartitionSnapshotHeaderSize);
  write_little(bytes, 16, static_cast<std::uint16_t>(snapshot.style));
  write_little(bytes, 20, snapshot.logical_sector_size);
  write_little(
      bytes, 24, static_cast<std::uint32_t>(snapshot.regions.size()));
  write_little(bytes, 32, snapshot.source_disk_size);
  write_little(bytes, 40, kPartitionSnapshotRecordSize);

  std::uint64_t data_offset = kPartitionSnapshotHeaderSize +
      snapshot.regions.size() * kPartitionSnapshotRecordSize;
  for (std::size_t index = 0; index < snapshot.regions.size(); ++index) {
    const auto& region = snapshot.regions[index];
    const auto digest = sha256(region.data);
    if (!digest) {
      return clonecore::Result<std::vector<std::byte>>::failure(digest.error());
    }
    const std::size_t record_offset =
        kPartitionSnapshotHeaderSize + index * kPartitionSnapshotRecordSize;
    const auto record = bytes.subspan(record_offset, kPartitionSnapshotRecordSize);
    write_little(record, 0, region.disk_offset);
    write_little(
        record, 8, static_cast<std::uint64_t>(region.data.size()));
    write_little(record, 16, data_offset);
    std::copy(digest.value().begin(), digest.value().end(), record.begin() + 24);
    std::copy(
        region.data.begin(),
        region.data.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(data_offset));
    data_offset += region.data.size();
  }
  return clonecore::Result<std::vector<std::byte>>::success(std::move(output));
}

clonecore::Result<std::vector<std::byte>> capture_partition_snapshot_v1(
    const clonecore::ISourceDiskReader& source,
    const PartitionTableStyle style) {
  const std::uint32_t sector_size = source.logical_sector_size();
  const std::uint64_t disk_size = source.size_bytes();
  if ((sector_size != 512 && sector_size != 4096) ||
      disk_size == 0 || disk_size % sector_size != 0) {
    return clonecore::Result<std::vector<std::byte>>::failure(argument_error(
        L"パーティション表採取元",
        L"読取り元の容量または論理セクターサイズが対応条件外です"));
  }

  PartitionSnapshot snapshot{
      .style = style,
      .source_disk_size = disk_size,
      .logical_sector_size = sector_size,
  };
  if (style == PartitionTableStyle::mbr) {
    const auto parsed = clonecore::parse_mbr(source);
    if (!parsed) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          parsed.error());
    }
    auto sector = source.read(0, sector_size);
    if (!sector) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          sector.error());
    }
    if (sector.value().size() != sector_size) {
      return clonecore::Result<std::vector<std::byte>>::failure(data_error(
          L"MBRメタデータ採取",
          L"先頭論理セクターを完全に読み取れませんでした"));
    }
    snapshot.regions.push_back(PartitionTableRegion{
        .disk_offset = 0,
        .data = sector.take_value(),
    });
    return build_partition_snapshot_v1(snapshot);
  }

  if (style != PartitionTableStyle::gpt) {
    return clonecore::Result<std::vector<std::byte>>::failure(argument_error(
        L"パーティション表採取形式",
        L"MBRまたはGPTだけを採取できます"));
  }

  const auto parsed = clonecore::parse_gpt(source);
  if (!parsed) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        parsed.error());
  }
  const auto& gpt = parsed.value();
  const std::uint64_t entry_bytes =
      static_cast<std::uint64_t>(gpt.partition_entry_count) *
      gpt.partition_entry_size;
  std::uint64_t rounded_entry_bytes{};
  if (!checked_add(entry_bytes, sector_size - 1, rounded_entry_bytes)) {
    return clonecore::Result<std::vector<std::byte>>::failure(data_error(
        L"GPTメタデータ寸法",
        L"パーティション項目配列の寸法がオーバーフローしました"));
  }
  const std::uint64_t padded_entry_bytes =
      (rounded_entry_bytes / sector_size) * sector_size;
  std::uint64_t leading_length{};
  if (!checked_add(
          static_cast<std::uint64_t>(sector_size) * 2,
          padded_entry_bytes,
          leading_length) ||
      leading_length > disk_size ||
      padded_entry_bytes > disk_size - sector_size) {
    return clonecore::Result<std::vector<std::byte>>::failure(data_error(
        L"GPTメタデータ境界",
        L"GPTメタデータ領域がディスク境界外です"));
  }
  const std::uint64_t trailing_length =
      padded_entry_bytes + sector_size;
  const std::uint64_t trailing_offset = disk_size - trailing_length;
  if (trailing_offset < leading_length ||
      leading_length > std::numeric_limits<std::size_t>::max() ||
      trailing_length > std::numeric_limits<std::size_t>::max()) {
    return clonecore::Result<std::vector<std::byte>>::failure(data_error(
        L"GPTメタデータ境界",
        L"GPTメタデータ領域が重複またはメモリ上限外です"));
  }

  auto leading =
      source.read(0, static_cast<std::size_t>(leading_length));
  if (!leading) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        leading.error());
  }
  auto trailing = source.read(
      trailing_offset, static_cast<std::size_t>(trailing_length));
  if (!trailing) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        trailing.error());
  }
  if (leading.value().size() != leading_length ||
      trailing.value().size() != trailing_length) {
    return clonecore::Result<std::vector<std::byte>>::failure(data_error(
        L"GPTメタデータ採取",
        L"GPT先頭または末尾メタデータを完全に読み取れませんでした"));
  }
  snapshot.regions.push_back(PartitionTableRegion{
      .disk_offset = 0,
      .data = leading.take_value(),
  });
  snapshot.regions.push_back(PartitionTableRegion{
      .disk_offset = trailing_offset,
      .data = trailing.take_value(),
  });
  return build_partition_snapshot_v1(snapshot);
}

clonecore::Result<PartitionSnapshot> inspect_partition_snapshot_v1(
    const std::span<const std::byte> bytes) {
  if (bytes.size() < kPartitionSnapshotHeaderSize ||
      bytes.size() > kMaximumSnapshotBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
      read_little<std::uint16_t>(bytes, 8) != 1 ||
      read_little<std::uint16_t>(bytes, 10) != 0 ||
      read_little<std::uint32_t>(bytes, 12) != kPartitionSnapshotHeaderSize ||
      read_little<std::uint32_t>(bytes, 40) != kPartitionSnapshotRecordSize ||
      !all_zero(bytes.subspan(18, 2)) ||
      !all_zero(bytes.subspan(28, 4)) ||
      !all_zero(bytes.subspan(44, 20))) {
    return clonecore::Result<PartitionSnapshot>::failure(data_error(
        L"パーティション表スナップショットヘッダー",
        L"マジック、版、固定長、または予約領域が不正です"));
  }

  PartitionSnapshot snapshot;
  snapshot.style = static_cast<PartitionTableStyle>(
      read_little<std::uint16_t>(bytes, 16));
  snapshot.logical_sector_size = read_little<std::uint32_t>(bytes, 20);
  const std::uint32_t region_count = read_little<std::uint32_t>(bytes, 24);
  snapshot.source_disk_size = read_little<std::uint64_t>(bytes, 32);
  if (region_count == 0 || region_count > kMaximumRegionCount ||
      region_count >
          (bytes.size() - kPartitionSnapshotHeaderSize) /
              kPartitionSnapshotRecordSize) {
    return clonecore::Result<PartitionSnapshot>::failure(data_error(
        L"パーティション表スナップショット領域数",
        L"領域数が空、過大、またはレコード境界外です"));
  }

  std::uint64_t expected_data_offset = kPartitionSnapshotHeaderSize +
      static_cast<std::uint64_t>(region_count) *
          kPartitionSnapshotRecordSize;
  for (std::uint32_t index = 0; index < region_count; ++index) {
    const std::size_t record_offset = kPartitionSnapshotHeaderSize +
        static_cast<std::size_t>(index) * kPartitionSnapshotRecordSize;
    const auto record = bytes.subspan(record_offset, kPartitionSnapshotRecordSize);
    const std::uint64_t disk_offset = read_little<std::uint64_t>(record, 0);
    const std::uint64_t length = read_little<std::uint64_t>(record, 8);
    const std::uint64_t data_offset = read_little<std::uint64_t>(record, 16);
    std::uint64_t data_end{};
    if (length == 0 || data_offset != expected_data_offset ||
        !checked_add(data_offset, length, data_end) || data_end > bytes.size() ||
        !all_zero(record.subspan(56, 8))) {
      return clonecore::Result<PartitionSnapshot>::failure(data_error(
          L"パーティション表スナップショットレコード",
          L"データ位置、長さ、または予約領域が不正です"));
    }
    Sha256Digest expected_hash{};
    std::copy_n(record.begin() + 24, expected_hash.size(), expected_hash.begin());
    const auto region_bytes = bytes.subspan(
        static_cast<std::size_t>(data_offset), static_cast<std::size_t>(length));
    const auto actual_hash = sha256(region_bytes);
    if (!actual_hash) {
      return clonecore::Result<PartitionSnapshot>::failure(actual_hash.error());
    }
    if (actual_hash.value() != expected_hash) {
      return clonecore::Result<PartitionSnapshot>::failure(verification_error(
          L"パーティション表スナップショットSHA-256",
          L"保存領域のハッシュが一致しません"));
    }
    PartitionTableRegion region;
    region.disk_offset = disk_offset;
    region.data.assign(region_bytes.begin(), region_bytes.end());
    region.sha256 = expected_hash;
    snapshot.regions.push_back(std::move(region));
    expected_data_offset = data_end;
  }
  if (expected_data_offset != bytes.size()) {
    return clonecore::Result<PartitionSnapshot>::failure(data_error(
        L"パーティション表スナップショット末尾",
        L"未参照データまたは末尾追加があります"));
  }
  const auto valid = validate_semantics(snapshot);
  if (!valid) {
    return clonecore::Result<PartitionSnapshot>::failure(data_error(
        valid.error().operation, valid.error().message));
  }
  return clonecore::Result<PartitionSnapshot>::success(std::move(snapshot));
}

}  // namespace ytec::imageformat

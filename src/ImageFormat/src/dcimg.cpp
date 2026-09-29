#include "ytec/imageformat/dcimg.h"

#include "ytec/imageformat/compression.h"
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>

namespace ytec::imageformat {
namespace {

constexpr std::array<std::byte, 8> kHeaderMagic{
    std::byte{'D'}, std::byte{'C'}, std::byte{'I'}, std::byte{'M'},
    std::byte{'G'}, std::byte{0x1A}, std::byte{0x0D}, std::byte{0x0A}};
constexpr std::array<std::byte, 8> kFooterMagic{
    std::byte{'D'}, std::byte{'C'}, std::byte{'E'}, std::byte{'N'},
    std::byte{'D'}, std::byte{0x1A}, std::byte{0x0D}, std::byte{0x0A}};
constexpr std::uint32_t kEndianMarker = 0x01020304U;
constexpr std::uint64_t kMaximumChunkCount = kDcimgMaximumChunkCount;
constexpr std::uint64_t kMaximumMetadataLength = 16U * 1024U * 1024U;

clonecore::Error data_error(
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::invalid_data,
      .native_code = ERROR_INVALID_DATA,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

clonecore::Error argument_error(
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::invalid_argument,
      .native_code = ERROR_INVALID_PARAMETER,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

clonecore::Error unsupported_error(
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::unsupported_layout,
      .native_code = ERROR_NOT_SUPPORTED,
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

clonecore::Error cancelled_error(std::wstring operation) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::cancelled,
      .native_code = ERROR_CANCELLED,
      .operation = std::move(operation),
      .message =
          L"利用者の要求により、安全な境界でイメージ作成を取り消しました",
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

bool all_zero(const std::span<const std::byte> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

bool supported_chunk_size(const std::uint32_t chunk_size) noexcept {
  return chunk_size == kDcimgChunkSize16MiB ||
         chunk_size == kDcimgChunkSize32MiB;
}

void write_section(
    const std::span<std::byte> header,
    const std::size_t offset,
    const DcimgSection& section) noexcept {
  write_little(header, offset, section.offset);
  write_little(header, offset + 8, section.length);
}

DcimgSection read_section(
    const std::span<const std::byte> header,
    const std::size_t offset) noexcept {
  return DcimgSection{
      .offset = read_little<std::uint64_t>(header, offset),
      .length = read_little<std::uint64_t>(header, offset + 8),
  };
}

clonecore::Result<std::uint64_t> section_end(
    const DcimgSection& section,
    const std::wstring_view name,
    const std::uint64_t image_size) {
  std::uint64_t end{};
  if (!checked_add(section.offset, section.length, end) || end > image_size) {
    return clonecore::Result<std::uint64_t>::failure(data_error(
        std::wstring(name), L"セクションがイメージ境界外です"));
  }
  return clonecore::Result<std::uint64_t>::success(end);
}

clonecore::Result<Sha256Digest> chunk_digest(
    const DcimgBuildChunk& chunk) {
  if (chunk.zero_filled) {
    return sha256_zeroes(chunk.logical_length);
  }
  return sha256(chunk.data);
}

clonecore::Status validate_build_request(const DcimgBuildRequest& request) {
  if (request.source_disk_size == 0 ||
      !is_supported_sector_size_pair(
          request.logical_sector_size, request.physical_sector_size) ||
      request.source_disk_size % request.logical_sector_size != 0 ||
      !supported_chunk_size(request.chunk_size) ||
      (request.compression != DcimgCompression::none &&
       request.compression != DcimgCompression::zstandard)) {
    return clonecore::Status::failure(argument_error(
        L"dcimg作成寸法",
        L"ディスク、セクター、チャンク寸法が対応条件外です"));
  }
  if (request.manifest.empty() ||
      request.manifest.size() > kMaximumMetadataLength ||
      request.partition_table_snapshot.empty() ||
      request.partition_table_snapshot.size() > kMaximumMetadataLength ||
      request.chunks.size() > kMaximumChunkCount) {
    return clonecore::Status::failure(argument_error(
        L"dcimg作成メタデータ",
        L"必須メタデータが空か、安全な上限を超えています"));
  }

  std::uint64_t previous_end = 0;
  bool first = true;
  for (const auto& chunk : request.chunks) {
    std::uint64_t logical_end{};
    if (chunk.logical_length == 0 ||
        chunk.logical_length > request.chunk_size ||
        chunk.logical_offset % request.logical_sector_size != 0 ||
        chunk.logical_length % request.logical_sector_size != 0 ||
        !checked_add(
            chunk.logical_offset, chunk.logical_length, logical_end) ||
        logical_end > request.source_disk_size ||
        (!first && chunk.logical_offset < previous_end)) {
      return clonecore::Status::failure(argument_error(
          L"dcimg作成チャンク境界",
          L"チャンクの論理範囲が不正、重複、または未整列です"));
    }
    if ((chunk.zero_filled && !chunk.data.empty()) ||
        (!chunk.zero_filled && chunk.data.size() != chunk.logical_length)) {
      return clonecore::Status::failure(argument_error(
          L"dcimg作成チャンクデータ",
          L"ゼロチャンクのデータ、または非圧縮長が不正です"));
    }
    previous_end = logical_end;
    first = false;
  }
  return clonecore::success_status();
}

clonecore::Result<DcimgHeader> parse_header(
    const std::span<const std::byte> image) {
  if (image.size() < kDcimgHeaderSize + kDcimgFooterSize) {
    return clonecore::Result<DcimgHeader>::failure(
        data_error(L"dcimg固定ヘッダー", L"イメージが固定領域より短いです"));
  }
  const std::span<const std::byte> bytes(image.data(), kDcimgHeaderSize);
  if (!std::equal(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin())) {
    return clonecore::Result<DcimgHeader>::failure(
        data_error(L"dcimgマジック", L"dcimg v1の形式マジックではありません"));
  }
  const std::uint16_t major = read_little<std::uint16_t>(bytes, 8);
  const std::uint16_t minor = read_little<std::uint16_t>(bytes, 10);
  if (major != 1 || minor != 0) {
    return clonecore::Result<DcimgHeader>::failure(unsupported_error(
        L"dcimgバージョン", L"未知の形式バージョンは開けません"));
  }
  if (read_little<std::uint32_t>(bytes, 12) != kDcimgHeaderSize ||
      read_little<std::uint32_t>(bytes, 16) != 0 ||
      read_little<std::uint32_t>(bytes, 20) != kEndianMarker ||
      !all_zero(bytes.subspan(184))) {
    return clonecore::Result<DcimgHeader>::failure(data_error(
        L"dcimg固定ヘッダー",
        L"ヘッダー長、フラグ、エンディアン、予約領域が不正です"));
  }

  DcimgHeader header;
  header.major_version = major;
  header.minor_version = minor;
  header.source_disk_size = read_little<std::uint64_t>(bytes, 24);
  header.logical_sector_size = read_little<std::uint32_t>(bytes, 32);
  header.physical_sector_size = read_little<std::uint32_t>(bytes, 36);
  header.chunk_size = read_little<std::uint32_t>(bytes, 40);
  header.compression = static_cast<DcimgCompression>(
      read_little<std::uint16_t>(bytes, 44));
  header.compression_version = read_little<std::uint16_t>(bytes, 46);
  header.chunk_count = read_little<std::uint64_t>(bytes, 48);
  header.manifest = read_section(bytes, 56);
  header.partition_table_snapshot = read_section(bytes, 72);
  header.chunk_index = read_section(bytes, 88);
  header.data = read_section(bytes, 104);
  header.hash_table = read_section(bytes, 120);
  header.footer = read_section(bytes, 136);
  std::copy_n(bytes.begin() + 152, header.manifest_hash.size(),
              header.manifest_hash.begin());

  if (header.source_disk_size == 0 ||
      !is_supported_sector_size_pair(
          header.logical_sector_size, header.physical_sector_size) ||
      header.source_disk_size % header.logical_sector_size != 0 ||
      !supported_chunk_size(header.chunk_size) ||
      header.chunk_count > kMaximumChunkCount) {
    return clonecore::Result<DcimgHeader>::failure(data_error(
        L"dcimgヘッダー寸法", L"ヘッダー内の寸法が対応範囲外です"));
  }
  const bool compression_valid =
      (header.compression == DcimgCompression::none &&
       header.compression_version == 0) ||
      (header.compression == DcimgCompression::zstandard &&
       header.compression_version == 1);
  if (!compression_valid) {
    return clonecore::Result<DcimgHeader>::failure(unsupported_error(
        L"dcimg圧縮方式", L"未知の圧縮方式または圧縮プロファイルです"));
  }
  return clonecore::Result<DcimgHeader>::success(header);
}

clonecore::Status validate_sections(
    const DcimgHeader& header,
    const std::uint64_t image_size) {
  std::uint64_t expected_index_length{};
  std::uint64_t expected_hash_length{};
  if (!checked_multiply(
          header.chunk_count, kDcimgChunkRecordSize, expected_index_length) ||
      !checked_multiply(
          header.chunk_count, Sha256Digest{}.size(), expected_hash_length)) {
    return clonecore::Status::failure(data_error(
        L"dcimgセクション寸法", L"チャンク数の乗算がオーバーフローしました"));
  }
  if (header.manifest.offset != kDcimgHeaderSize ||
      header.manifest.length == 0 ||
      header.manifest.length > kMaximumMetadataLength ||
      header.partition_table_snapshot.length == 0 ||
      header.partition_table_snapshot.length > kMaximumMetadataLength ||
      header.chunk_index.length != expected_index_length ||
      header.hash_table.length != expected_hash_length ||
      header.footer.length != kDcimgFooterSize) {
    return clonecore::Status::failure(data_error(
        L"dcimgセクション寸法", L"必須セクションの長さが不正です"));
  }

  const auto manifest_end = section_end(
      header.manifest, L"dcimgマニフェスト", image_size);
  const auto partition_end = section_end(
      header.partition_table_snapshot, L"dcimgパーティション表", image_size);
  const auto index_end = section_end(
      header.chunk_index, L"dcimgチャンク索引", image_size);
  const auto data_end = section_end(
      header.data, L"dcimgデータ", image_size);
  const auto hash_end = section_end(
      header.hash_table, L"dcimgハッシュ表", image_size);
  const auto footer_end = section_end(
      header.footer, L"dcimgフッター", image_size);
  if (!manifest_end || !partition_end || !index_end || !data_end ||
      !hash_end || !footer_end) {
    return clonecore::Status::failure(
        !manifest_end   ? manifest_end.error()
        : !partition_end ? partition_end.error()
        : !index_end     ? index_end.error()
        : !data_end      ? data_end.error()
        : !hash_end      ? hash_end.error()
                         : footer_end.error());
  }
  if (header.partition_table_snapshot.offset != manifest_end.value() ||
      header.chunk_index.offset != partition_end.value() ||
      header.data.offset != index_end.value() ||
      header.hash_table.offset != data_end.value() ||
      header.footer.offset != hash_end.value() ||
      footer_end.value() != image_size) {
    return clonecore::Status::failure(data_error(
        L"dcimgセクション配置",
        L"v1セクションが正規順序で連続配置されていません"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<DcimgChunkRecord>> parse_chunks(
    const DcimgHeader& header,
    const std::span<const std::byte> index_bytes,
    const std::size_t index_base_offset) {
  std::vector<DcimgChunkRecord> chunks;
  chunks.reserve(static_cast<std::size_t>(header.chunk_count));
  std::uint64_t previous_logical_end = 0;
  std::uint64_t expected_stored_offset = header.data.offset;
  bool first = true;
  for (std::uint64_t index = 0; index < header.chunk_count; ++index) {
    const std::uint64_t relative_offset =
        index * kDcimgChunkRecordSize;
    const std::size_t record_offset =
        index_base_offset + static_cast<std::size_t>(relative_offset);
    const auto record_bytes =
        index_bytes.subspan(record_offset, kDcimgChunkRecordSize);

    DcimgChunkRecord record;
    record.logical_offset = read_little<std::uint64_t>(record_bytes, 0);
    record.uncompressed_length = read_little<std::uint64_t>(record_bytes, 8);
    record.stored_offset = read_little<std::uint64_t>(record_bytes, 16);
    record.stored_length = read_little<std::uint64_t>(record_bytes, 24);
    record.flags = static_cast<DcimgChunkFlags>(
        read_little<std::uint32_t>(record_bytes, 32));
    record.compression = static_cast<DcimgCompression>(
        read_little<std::uint16_t>(record_bytes, 36));
    std::copy_n(record_bytes.begin() + 40, record.sha256.size(),
                record.sha256.begin());

    if (!all_zero(record_bytes.subspan(38, 2)) ||
        !all_zero(record_bytes.subspan(72, 8)) ||
        (record.flags != DcimgChunkFlags::none &&
         record.flags != DcimgChunkFlags::zero_filled)) {
      return clonecore::Result<std::vector<DcimgChunkRecord>>::failure(
          data_error(L"dcimgチャンク予約領域",
                     L"未知フラグまたは予約領域の非ゼロ値があります"));
    }
    std::uint64_t logical_end{};
    if (record.uncompressed_length == 0 ||
        record.uncompressed_length > header.chunk_size ||
        record.logical_offset % header.logical_sector_size != 0 ||
        record.uncompressed_length % header.logical_sector_size != 0 ||
        !checked_add(record.logical_offset, record.uncompressed_length,
                     logical_end) ||
        logical_end > header.source_disk_size ||
        (!first && record.logical_offset < previous_logical_end)) {
      return clonecore::Result<std::vector<DcimgChunkRecord>>::failure(
          data_error(L"dcimgチャンク論理境界",
                     L"チャンクの論理範囲が不正、重複、または未整列です"));
    }

    if (record.flags == DcimgChunkFlags::zero_filled) {
      if (record.stored_offset != 0 || record.stored_length != 0 ||
          record.compression != DcimgCompression::none) {
        return clonecore::Result<std::vector<DcimgChunkRecord>>::failure(
            data_error(L"dcimgゼロチャンク",
                       L"ゼロチャンクに保存データまたは圧縮指定があります"));
      }
    } else {
      std::uint64_t stored_end{};
      if (record.stored_offset != expected_stored_offset ||
          record.stored_length == 0 ||
          !checked_add(record.stored_offset, record.stored_length, stored_end) ||
          stored_end > header.data.offset + header.data.length ||
          (record.compression == DcimgCompression::none &&
           record.stored_length != record.uncompressed_length) ||
          (record.compression == DcimgCompression::zstandard &&
           record.stored_length >= record.uncompressed_length) ||
          (record.compression != DcimgCompression::none &&
           record.compression != DcimgCompression::zstandard)) {
        return clonecore::Result<std::vector<DcimgChunkRecord>>::failure(
            data_error(L"dcimg保存チャンク境界",
                       L"保存範囲、長さ、圧縮指定が不正です"));
      }
      if (header.compression == DcimgCompression::none &&
          record.compression != DcimgCompression::none) {
        return clonecore::Result<std::vector<DcimgChunkRecord>>::failure(
            data_error(L"dcimg圧縮整合",
                       L"ヘッダーとチャンクの圧縮方式が一致しません"));
      }
      expected_stored_offset = stored_end;
    }
    previous_logical_end = logical_end;
    first = false;
    chunks.push_back(record);
  }
  if (expected_stored_offset != header.data.offset + header.data.length) {
    return clonecore::Result<std::vector<DcimgChunkRecord>>::failure(
        data_error(L"dcimgデータ被覆",
                   L"チャンク索引がデータセクション全体を正規に参照していません"));
  }
  return clonecore::Result<std::vector<DcimgChunkRecord>>::success(
      std::move(chunks));
}

struct StreamLayout final {
  DcimgHeader header;
  std::uint64_t image_length{};
};

clonecore::Result<StreamLayout> prepare_stream_layout(
    const DcimgStreamBuildRequest& request) {
  if (request.source_disk_size == 0 ||
      !is_supported_sector_size_pair(
          request.logical_sector_size, request.physical_sector_size) ||
      request.source_disk_size % request.logical_sector_size != 0 ||
      !supported_chunk_size(request.chunk_size) ||
      (request.compression != DcimgCompression::none &&
       request.compression != DcimgCompression::zstandard) ||
      request.verification_block_bytes == 0 ||
      request.verification_block_bytes > kDcimgChunkSize32MiB) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成寸法",
        L"ディスク、セクター、チャンク、または検証ブロック寸法が対応条件外です"));
  }
  if (request.manifest.empty() ||
      request.manifest.size() > kMaximumMetadataLength ||
      request.partition_table_snapshot.empty() ||
      request.partition_table_snapshot.size() > kMaximumMetadataLength ||
      request.chunks.empty() ||
      request.chunks.size() > kMaximumChunkCount) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成メタデータ",
        L"必須メタデータまたはチャンクが空か、安全な上限を超えています"));
  }

  std::uint64_t data_length = 0;
  std::uint64_t previous_end = 0;
  bool first = true;
  for (const auto& chunk : request.chunks) {
    std::uint64_t logical_end{};
    std::uint64_t source_end{};
    if (chunk.logical_length == 0 ||
        chunk.logical_length > request.chunk_size ||
        chunk.logical_offset % request.logical_sector_size != 0 ||
        chunk.logical_length % request.logical_sector_size != 0 ||
        !checked_add(
            chunk.logical_offset, chunk.logical_length, logical_end) ||
        logical_end > request.source_disk_size ||
        (!first && chunk.logical_offset < previous_end)) {
      return clonecore::Result<StreamLayout>::failure(argument_error(
          L"dcimg段階作成チャンク境界",
          L"チャンクの論理範囲が不正、重複、または未整列です"));
    }
    if (chunk.zero_filled) {
      if (chunk.source != nullptr || chunk.source_offset != 0) {
        return clonecore::Result<StreamLayout>::failure(argument_error(
            L"dcimg段階作成ゼロチャンク",
            L"ゼロチャンクに読取り元を指定できません"));
      }
    } else {
      if (chunk.source == nullptr ||
          chunk.source->logical_sector_size() !=
              request.logical_sector_size ||
          chunk.source_offset % request.logical_sector_size != 0 ||
          !checked_add(
              chunk.source_offset, chunk.logical_length, source_end) ||
          source_end > chunk.source->size_bytes() ||
          !checked_add(data_length, chunk.logical_length, data_length)) {
        return clonecore::Result<StreamLayout>::failure(argument_error(
            L"dcimg段階作成読取り元",
            L"チャンク読取り元の寸法、セクター、または範囲が不正です"));
      }
    }
    previous_end = logical_end;
    first = false;
  }

  const auto manifest_hash = sha256(request.manifest);
  if (!manifest_hash) {
    return clonecore::Result<StreamLayout>::failure(manifest_hash.error());
  }

  DcimgHeader header;
  header.major_version = 1;
  header.minor_version = 0;
  header.source_disk_size = request.source_disk_size;
  header.logical_sector_size = request.logical_sector_size;
  header.physical_sector_size = request.physical_sector_size;
  header.chunk_size = request.chunk_size;
  header.compression = request.compression;
  header.compression_version =
      request.compression == DcimgCompression::zstandard ? 1U : 0U;
  header.chunk_count = request.chunks.size();
  header.manifest = {kDcimgHeaderSize, request.manifest.size()};
  header.manifest_hash = manifest_hash.value();

  std::uint64_t index_length{};
  std::uint64_t hash_length{};
  if (!checked_multiply(
          header.chunk_count, kDcimgChunkRecordSize, index_length) ||
      !checked_multiply(
          header.chunk_count, Sha256Digest{}.size(), hash_length)) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成セクション",
        L"チャンク数からセクション長を計算できません"));
  }

  std::uint64_t next{};
  if (!checked_add(header.manifest.offset, header.manifest.length, next)) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成マニフェスト",
        L"セクション位置がオーバーフローしました"));
  }
  header.partition_table_snapshot = {
      next, request.partition_table_snapshot.size()};
  if (!checked_add(
          next, header.partition_table_snapshot.length, next)) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成パーティション表",
        L"セクション位置がオーバーフローしました"));
  }
  header.chunk_index = {next, index_length};
  if (!checked_add(next, header.chunk_index.length, next)) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成チャンク索引",
        L"セクション位置がオーバーフローしました"));
  }
  header.data = {next, data_length};
  if (!checked_add(next, header.data.length, next)) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成データ",
        L"セクション位置がオーバーフローしました"));
  }
  header.hash_table = {next, hash_length};
  if (!checked_add(next, header.hash_table.length, next)) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成ハッシュ表",
        L"セクション位置がオーバーフローしました"));
  }
  header.footer = {next, kDcimgFooterSize};
  std::uint64_t image_length{};
  if (!checked_add(next, kDcimgFooterSize, image_length)) {
    return clonecore::Result<StreamLayout>::failure(argument_error(
        L"dcimg段階作成全体長",
        L"イメージ長がオーバーフローしました"));
  }
  return clonecore::Result<StreamLayout>::success(StreamLayout{
      .header = header,
      .image_length = image_length,
  });
}

std::array<std::byte, kDcimgHeaderSize> serialize_header(
    const DcimgHeader& header) {
  std::array<std::byte, kDcimgHeaderSize> bytes{};
  std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin());
  write_little(bytes, 8, header.major_version);
  write_little(bytes, 10, header.minor_version);
  write_little(bytes, 12, kDcimgHeaderSize);
  write_little(bytes, 16, 0U);
  write_little(bytes, 20, kEndianMarker);
  write_little(bytes, 24, header.source_disk_size);
  write_little(bytes, 32, header.logical_sector_size);
  write_little(bytes, 36, header.physical_sector_size);
  write_little(bytes, 40, header.chunk_size);
  write_little(
      bytes, 44, static_cast<std::uint16_t>(header.compression));
  write_little(bytes, 46, header.compression_version);
  write_little(bytes, 48, header.chunk_count);
  write_section(bytes, 56, header.manifest);
  write_section(bytes, 72, header.partition_table_snapshot);
  write_section(bytes, 88, header.chunk_index);
  write_section(bytes, 104, header.data);
  write_section(bytes, 120, header.hash_table);
  write_section(bytes, 136, header.footer);
  std::copy(
      header.manifest_hash.begin(),
      header.manifest_hash.end(),
      bytes.begin() + 152);
  return bytes;
}

std::array<std::byte, kDcimgChunkRecordSize> serialize_chunk_record(
    const DcimgChunkRecord& chunk) {
  std::array<std::byte, kDcimgChunkRecordSize> record{};
  write_little(record, 0, chunk.logical_offset);
  write_little(record, 8, chunk.uncompressed_length);
  write_little(record, 16, chunk.stored_offset);
  write_little(record, 24, chunk.stored_length);
  write_little(
      record, 32, static_cast<std::uint32_t>(chunk.flags));
  write_little(
      record, 36, static_cast<std::uint16_t>(chunk.compression));
  std::copy(
      chunk.sha256.begin(),
      chunk.sha256.end(),
      record.begin() + 40);
  return record;
}

std::array<std::byte, kDcimgFooterSize> serialize_footer(
    const Sha256Digest& global_hash,
    const std::uint64_t image_length) {
  std::array<std::byte, kDcimgFooterSize> footer{};
  std::copy(kFooterMagic.begin(), kFooterMagic.end(), footer.begin());
  std::copy(
      global_hash.begin(),
      global_hash.end(),
      footer.begin() + 8);
  write_little(footer, 40, image_length);
  return footer;
}

clonecore::Error cleanup_error(
    const clonecore::Error& primary,
    const clonecore::Error& cleanup) {
  return clonecore::Error{
      .code = cleanup.code,
      .native_code = cleanup.native_code,
      .operation = L"dcimg未完了出力破棄",
      .message =
          L"元の失敗: " + primary.operation + L" / " + primary.message +
          L"。さらに未完了出力の破棄に失敗しました: " +
          cleanup.operation + L" / " + cleanup.message,
  };
}

}  // namespace

clonecore::Result<std::vector<std::byte>> build_dcimg_v1(
    const DcimgBuildRequest& request) {
  const auto request_status = validate_build_request(request);
  if (!request_status) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        request_status.error());
  }

  const auto manifest_hash = sha256(request.manifest);
  if (!manifest_hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        manifest_hash.error());
  }
  std::vector<Sha256Digest> chunk_hashes;
  chunk_hashes.reserve(request.chunks.size());
  struct StoredBuildChunk final {
    DcimgCompression compression{DcimgCompression::none};
    std::vector<std::byte> bytes;
  };
  std::vector<StoredBuildChunk> stored_chunks;
  stored_chunks.reserve(request.chunks.size());
  std::uint64_t data_length = 0;
  for (const auto& chunk : request.chunks) {
    const auto digest = chunk_digest(chunk);
    if (!digest) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          digest.error());
    }
    chunk_hashes.push_back(digest.value());
    StoredBuildChunk stored;
    if (!chunk.zero_filled) {
      if (request.compression == DcimgCompression::zstandard) {
        auto compressed = compress_zstandard_dcimg_v1(chunk.data);
        if (!compressed) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              compressed.error());
        }
        if (compressed.value().size() < chunk.data.size()) {
          stored.compression = DcimgCompression::zstandard;
          stored.bytes = compressed.take_value();
        } else {
          stored.bytes = chunk.data;
        }
      } else {
        stored.bytes = chunk.data;
      }
    }
    if (!checked_add(data_length, stored.bytes.size(), data_length)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          argument_error(L"dcimgデータ長", L"データ長がオーバーフローしました"));
    }
    stored_chunks.push_back(std::move(stored));
  }

  DcimgHeader header;
  header.major_version = 1;
  header.minor_version = 0;
  header.source_disk_size = request.source_disk_size;
  header.logical_sector_size = request.logical_sector_size;
  header.physical_sector_size = request.physical_sector_size;
  header.chunk_size = request.chunk_size;
  header.compression = request.compression;
  header.compression_version =
      request.compression == DcimgCompression::zstandard ? 1U : 0U;
  header.chunk_count = request.chunks.size();
  header.manifest = {kDcimgHeaderSize, request.manifest.size()};
  header.manifest_hash = manifest_hash.value();

  std::uint64_t next{};
  if (!checked_add(header.manifest.offset, header.manifest.length, next)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        argument_error(L"dcimgマニフェスト長", L"セクション位置がオーバーフローしました"));
  }
  header.partition_table_snapshot = {
      next, request.partition_table_snapshot.size()};
  if (!checked_add(next, header.partition_table_snapshot.length, next)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        argument_error(L"dcimgパーティション表長", L"セクション位置がオーバーフローしました"));
  }
  header.chunk_index = {
      next, header.chunk_count * kDcimgChunkRecordSize};
  if (!checked_add(next, header.chunk_index.length, next)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        argument_error(L"dcimgチャンク索引長", L"セクション位置がオーバーフローしました"));
  }
  header.data = {next, data_length};
  if (!checked_add(next, header.data.length, next)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        argument_error(L"dcimgデータ長", L"セクション位置がオーバーフローしました"));
  }
  header.hash_table = {next, header.chunk_count * Sha256Digest{}.size()};
  if (!checked_add(next, header.hash_table.length, next)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        argument_error(L"dcimgハッシュ表長", L"セクション位置がオーバーフローしました"));
  }
  header.footer = {next, kDcimgFooterSize};
  std::uint64_t image_length{};
  if (!checked_add(next, kDcimgFooterSize, image_length) ||
      image_length > std::numeric_limits<std::size_t>::max()) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        argument_error(L"dcimg全体長", L"イメージ長が安全な範囲を超えています"));
  }

  std::vector<std::byte> image(
      static_cast<std::size_t>(image_length), std::byte{0});
  std::span<std::byte> bytes(image);
  std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin());
  write_little(bytes, 8, header.major_version);
  write_little(bytes, 10, header.minor_version);
  write_little(bytes, 12, kDcimgHeaderSize);
  write_little(bytes, 16, 0U);
  write_little(bytes, 20, kEndianMarker);
  write_little(bytes, 24, header.source_disk_size);
  write_little(bytes, 32, header.logical_sector_size);
  write_little(bytes, 36, header.physical_sector_size);
  write_little(bytes, 40, header.chunk_size);
  write_little(bytes, 44, static_cast<std::uint16_t>(header.compression));
  write_little(bytes, 46, header.compression_version);
  write_little(bytes, 48, header.chunk_count);
  write_section(bytes, 56, header.manifest);
  write_section(bytes, 72, header.partition_table_snapshot);
  write_section(bytes, 88, header.chunk_index);
  write_section(bytes, 104, header.data);
  write_section(bytes, 120, header.hash_table);
  write_section(bytes, 136, header.footer);
  std::copy(header.manifest_hash.begin(), header.manifest_hash.end(),
            bytes.begin() + 152);

  std::copy(request.manifest.begin(), request.manifest.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(header.manifest.offset));
  std::copy(
      request.partition_table_snapshot.begin(),
      request.partition_table_snapshot.end(),
      bytes.begin() +
          static_cast<std::ptrdiff_t>(header.partition_table_snapshot.offset));

  std::uint64_t stored_offset = header.data.offset;
  for (std::size_t index = 0; index < request.chunks.size(); ++index) {
    const auto& chunk = request.chunks[index];
    const auto& stored = stored_chunks[index];
    const std::size_t record_offset = static_cast<std::size_t>(
        header.chunk_index.offset + index * kDcimgChunkRecordSize);
    const auto record = bytes.subspan(record_offset, kDcimgChunkRecordSize);
    write_little(record, 0, chunk.logical_offset);
    write_little(record, 8, chunk.logical_length);
    write_little(record, 16, chunk.zero_filled ? 0U : stored_offset);
    write_little(
        record, 24,
        chunk.zero_filled ? 0U : static_cast<std::uint64_t>(stored.bytes.size()));
    write_little(
        record,
        32,
        static_cast<std::uint32_t>(
            chunk.zero_filled ? DcimgChunkFlags::zero_filled
                              : DcimgChunkFlags::none));
    write_little(
        record, 36, static_cast<std::uint16_t>(stored.compression));
    std::copy(chunk_hashes[index].begin(), chunk_hashes[index].end(),
              record.begin() + 40);

    if (!chunk.zero_filled) {
      std::copy(
          stored.bytes.begin(),
          stored.bytes.end(),
          bytes.begin() + static_cast<std::ptrdiff_t>(stored_offset));
      stored_offset += stored.bytes.size();
    }
    const std::size_t hash_offset = static_cast<std::size_t>(
        header.hash_table.offset + index * Sha256Digest{}.size());
    std::copy(chunk_hashes[index].begin(), chunk_hashes[index].end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(hash_offset));
  }

  const std::size_t footer_offset = static_cast<std::size_t>(header.footer.offset);
  std::copy(kFooterMagic.begin(), kFooterMagic.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(footer_offset));
  write_little(bytes, footer_offset + 40, image_length);
  const auto global_hash = sha256(bytes.first(footer_offset));
  if (!global_hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        global_hash.error());
  }
  std::copy(global_hash.value().begin(), global_hash.value().end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(footer_offset + 8));
  return clonecore::Result<std::vector<std::byte>>::success(std::move(image));
}

clonecore::Result<std::vector<std::byte>> build_uncompressed_dcimg_v1(
    const DcimgBuildRequest& request) {
  if (request.compression != DcimgCompression::none) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        argument_error(
            L"dcimg非圧縮作成",
            L"非圧縮互換APIへ圧縮プロファイルを指定できません"));
  }
  return build_dcimg_v1(request);
}

clonecore::Result<DcimgStreamBuildReport>
write_verified_uncompressed_dcimg_v1(
    const DcimgStreamBuildRequest& request,
    IDcimgStagingTarget& target,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (request.compression != DcimgCompression::none) {
    return clonecore::Result<DcimgStreamBuildReport>::failure(
        argument_error(
            L"dcimg非圧縮段階作成",
            L"非圧縮互換APIへ圧縮プロファイルを指定できません"));
  }
  const auto layout_result = prepare_stream_layout(request);
  if (!layout_result) {
    return clonecore::Result<DcimgStreamBuildReport>::failure(
        layout_result.error());
  }
  const StreamLayout layout = layout_result.value();

  std::uint64_t total_read_bytes = 0;
  std::uint64_t total_write_bytes = 0;
  std::uint64_t total_chunk_verify_bytes = 0;
  for (const auto& chunk : request.chunks) {
    if (!checked_add(
            total_chunk_verify_bytes,
            chunk.logical_length,
            total_chunk_verify_bytes) ||
        (!chunk.zero_filled &&
         (!checked_add(
              total_read_bytes,
              chunk.logical_length,
              total_read_bytes) ||
          !checked_add(
              total_write_bytes,
              chunk.logical_length,
              total_write_bytes)))) {
      return clonecore::Result<DcimgStreamBuildReport>::failure(
          argument_error(
              L"dcimg進捗寸法",
              L"読取り、書込み、または検証の合計が上限を超えています"));
    }
  }
  std::uint64_t total_verify_bytes = 0;
  if (!checked_add(
          total_chunk_verify_bytes,
          layout.header.footer.offset,
          total_verify_bytes)) {
    return clonecore::Result<DcimgStreamBuildReport>::failure(
        argument_error(
            L"dcimg進捗寸法",
            L"チャンク検証と全体ハッシュ検証の合計が上限を超えています"));
  }
  clonecore::DiskOperationProgress progress{
      .stage = clonecore::DiskOperationStage::planning,
      .total_read_bytes = total_read_bytes,
      .total_write_bytes = total_write_bytes,
      .total_verify_bytes = total_verify_bytes,
      .cancellation_allowed = true,
  };
  clonecore::report_disk_operation_progress(callbacks, progress);
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return clonecore::Result<DcimgStreamBuildReport>::failure(
        cancelled_error(L"dcimgイメージ作成開始前"));
  }

  bool begin_attempted = false;
  const auto fail = [&](const clonecore::Error& primary) {
    if (begin_attempted) {
      const auto aborted = target.abort_incomplete();
      if (!aborted) {
        return clonecore::Result<DcimgStreamBuildReport>::failure(
            cleanup_error(primary, aborted.error()));
      }
    }
    return clonecore::Result<DcimgStreamBuildReport>::failure(primary);
  };
  const auto short_read_error = [](const std::wstring& operation) {
    return verification_error(
        operation, L"段階出力の読戻し長が要求長と一致しません");
  };

  begin_attempted = true;
  auto status = target.begin(layout.image_length);
  if (!status) {
    return fail(status.error());
  }

  const auto header_bytes = serialize_header(layout.header);
  status = target.write_at(0, header_bytes);
  if (!status) {
    return fail(status.error());
  }
  status = target.write_at(
      layout.header.manifest.offset, request.manifest);
  if (!status) {
    return fail(status.error());
  }
  status = target.write_at(
      layout.header.partition_table_snapshot.offset,
      request.partition_table_snapshot);
  if (!status) {
    return fail(status.error());
  }

  std::vector<DcimgChunkRecord> records;
  records.reserve(request.chunks.size());
  std::uint64_t stored_offset = layout.header.data.offset;
  std::uint64_t stored_data_bytes = 0;
  std::uint64_t zero_filled_bytes = 0;
  progress.stage = clonecore::DiskOperationStage::copying_data;
  clonecore::report_disk_operation_progress(callbacks, progress);
  for (std::size_t index = 0; index < request.chunks.size(); ++index) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return fail(cancelled_error(L"dcimg Snapshotチャンク作成"));
    }
    const auto& requested = request.chunks[index];
    Sha256Digest digest{};
    std::vector<std::byte> source_data;
    if (requested.zero_filled) {
      const auto zero_digest = sha256_zeroes(requested.logical_length);
      if (!zero_digest) {
        return fail(zero_digest.error());
      }
      digest = zero_digest.value();
    } else {
      auto read = requested.source->read(
          requested.source_offset,
          static_cast<std::size_t>(requested.logical_length));
      if (!read) {
        return fail(read.error());
      }
      if (read.value().size() !=
          static_cast<std::size_t>(requested.logical_length)) {
        return fail(short_read_error(L"dcimg Snapshotチャンク読取り"));
      }
      source_data = read.take_value();
      progress.read_bytes += requested.logical_length;
      clonecore::report_disk_operation_progress(callbacks, progress);
      const auto data_digest = sha256(source_data);
      if (!data_digest) {
        return fail(data_digest.error());
      }
      digest = data_digest.value();
      status = target.write_at(stored_offset, source_data);
      if (!status) {
        return fail(status.error());
      }
      progress.written_bytes += requested.logical_length;
      clonecore::report_disk_operation_progress(callbacks, progress);
    }

    DcimgChunkRecord record;
    record.logical_offset = requested.logical_offset;
    record.uncompressed_length = requested.logical_length;
    record.stored_offset = requested.zero_filled ? 0 : stored_offset;
    record.stored_length =
        requested.zero_filled ? 0 : requested.logical_length;
    record.flags =
        requested.zero_filled
            ? DcimgChunkFlags::zero_filled
            : DcimgChunkFlags::none;
    record.compression = DcimgCompression::none;
    record.sha256 = digest;
    const auto record_bytes = serialize_chunk_record(record);

    const std::uint64_t record_offset =
        layout.header.chunk_index.offset +
        index * kDcimgChunkRecordSize;
    status = target.write_at(record_offset, record_bytes);
    if (!status) {
      return fail(status.error());
    }
    const std::uint64_t hash_offset =
        layout.header.hash_table.offset +
        index * Sha256Digest{}.size();
    status = target.write_at(hash_offset, record.sha256);
    if (!status) {
      return fail(status.error());
    }

    if (requested.zero_filled) {
      zero_filled_bytes += requested.logical_length;
    } else {
      stored_offset += requested.logical_length;
      stored_data_bytes += requested.logical_length;
    }
    records.push_back(record);
  }
  if (stored_offset !=
      layout.header.data.offset + layout.header.data.length) {
    return fail(argument_error(
        L"dcimg段階作成データ被覆",
        L"保存データ長が事前計算したセクション長と一致しません"));
  }

  status = target.resize_before_verification(layout.image_length);
  if (!status) {
    return fail(status.error());
  }
  status = target.flush();
  if (!status) {
    return fail(status.error());
  }

  progress.stage = clonecore::DiskOperationStage::verifying_source;
  clonecore::report_disk_operation_progress(callbacks, progress);
  for (std::size_t index = 0; index < records.size(); ++index) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return fail(cancelled_error(L"dcimgチャンク読戻し検証"));
    }
    const auto expected_record = serialize_chunk_record(records[index]);
    const std::uint64_t record_offset =
        layout.header.chunk_index.offset +
        index * kDcimgChunkRecordSize;
    auto actual_record =
        target.read_at(record_offset, expected_record.size());
    if (!actual_record) {
      return fail(actual_record.error());
    }
    if (actual_record.value().size() != expected_record.size() ||
        !std::equal(
            expected_record.begin(),
            expected_record.end(),
            actual_record.value().begin())) {
      return fail(short_read_error(L"dcimgチャンク索引読戻し"));
    }

    const std::uint64_t hash_offset =
        layout.header.hash_table.offset +
        index * Sha256Digest{}.size();
    auto actual_table_hash =
        target.read_at(hash_offset, Sha256Digest{}.size());
    if (!actual_table_hash) {
      return fail(actual_table_hash.error());
    }
    if (actual_table_hash.value().size() != Sha256Digest{}.size() ||
        !std::equal(
            records[index].sha256.begin(),
            records[index].sha256.end(),
            actual_table_hash.value().begin())) {
      return fail(verification_error(
          L"dcimgハッシュ表読戻し",
          L"独立ハッシュ表が作成時のチャンクSHA-256と一致しません"));
    }

    clonecore::Result<Sha256Digest> actual_digest =
        records[index].flags == DcimgChunkFlags::zero_filled
            ? sha256_zeroes(records[index].uncompressed_length)
            : sha256_from_reader(
                  records[index].stored_length,
                  request.verification_block_bytes,
                  [&](const std::uint64_t relative_offset,
                      const std::size_t length) {
                    return target.read_at(
                        records[index].stored_offset + relative_offset,
                        length);
                  });
    if (!actual_digest) {
      return fail(actual_digest.error());
    }
    if (actual_digest.value() != records[index].sha256) {
      return fail(verification_error(
          L"dcimgチャンクデータ読戻し",
          L"段階出力から読み戻したチャンクSHA-256が一致しません"));
    }
    progress.verified_bytes += records[index].uncompressed_length;
    clonecore::report_disk_operation_progress(callbacks, progress);
  }

  const auto global_hash = sha256_from_reader(
      layout.header.footer.offset,
      request.verification_block_bytes,
      [&](const std::uint64_t offset, const std::size_t length) {
        if (clonecore::disk_operation_cancellation_requested(callbacks)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              cancelled_error(L"dcimg全体ハッシュ読戻し検証"));
        }
        auto bytes = target.read_at(offset, length);
        if (bytes && bytes.value().size() == length) {
          progress.verified_bytes += length;
          clonecore::report_disk_operation_progress(callbacks, progress);
        }
        return bytes;
      });
  if (!global_hash) {
    return fail(global_hash.error());
  }
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return fail(cancelled_error(L"dcimg最終確定前"));
  }
  const auto footer =
      serialize_footer(global_hash.value(), layout.image_length);
  status = target.write_at(layout.header.footer.offset, footer);
  if (!status) {
    return fail(status.error());
  }
  status = target.flush();
  if (!status) {
    return fail(status.error());
  }

  auto footer_readback =
      target.read_at(layout.header.footer.offset, footer.size());
  if (!footer_readback) {
    return fail(footer_readback.error());
  }
  if (footer_readback.value().size() != footer.size() ||
      !std::equal(
          footer.begin(), footer.end(), footer_readback.value().begin())) {
    return fail(verification_error(
        L"dcimgフッター読戻し",
        L"全体ハッシュを含むフッターの読戻しが一致しません"));
  }

  progress.stage = clonecore::DiskOperationStage::flushing_data;
  progress.cancellation_allowed = false;
  clonecore::report_disk_operation_progress(callbacks, progress);
  status = target.commit_verified();
  if (!status) {
    return fail(status.error());
  }
  begin_attempted = false;
  progress.stage = clonecore::DiskOperationStage::completed;
  progress.verified_bytes = progress.total_verify_bytes;
  clonecore::report_disk_operation_progress(callbacks, progress);
  return clonecore::Result<DcimgStreamBuildReport>::success(
      DcimgStreamBuildReport{
          .image_length = layout.image_length,
          .stored_data_bytes = stored_data_bytes,
          .zero_filled_bytes = zero_filled_bytes,
          .chunk_count = records.size(),
          .all_chunks_read_back_verified = true,
          .global_hash_read_back_verified = true,
          .committed = true,
      });
}

clonecore::Result<DcimgStreamBuildReport> write_verified_dcimg_v1(
    const DcimgStreamBuildRequest& request,
    IDcimgStagingTarget& target,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (request.compression == DcimgCompression::none) {
    return write_verified_uncompressed_dcimg_v1(
        request, target, callbacks);
  }
  if (request.compression != DcimgCompression::zstandard) {
    return clonecore::Result<DcimgStreamBuildReport>::failure(
        argument_error(
            L"dcimg段階作成圧縮方式",
            L"未知の圧縮方式を指定できません"));
  }

  const auto maximum_layout_result = prepare_stream_layout(request);
  if (!maximum_layout_result) {
    return clonecore::Result<DcimgStreamBuildReport>::failure(
        maximum_layout_result.error());
  }
  const StreamLayout maximum_layout = maximum_layout_result.value();

  std::uint64_t total_logical_data_bytes = 0U;
  std::uint64_t zero_filled_bytes = 0U;
  for (const auto& chunk : request.chunks) {
    if (chunk.zero_filled) {
      if (!checked_add(
              zero_filled_bytes,
              chunk.logical_length,
              zero_filled_bytes)) {
        return clonecore::Result<DcimgStreamBuildReport>::failure(
            argument_error(
                L"dcimg圧縮進捗寸法",
                L"ゼロ領域合計が64bit範囲外です"));
      }
    } else if (!checked_add(
                   total_logical_data_bytes,
                   chunk.logical_length,
                   total_logical_data_bytes)) {
      return clonecore::Result<DcimgStreamBuildReport>::failure(
          argument_error(
              L"dcimg圧縮進捗寸法",
              L"データ領域合計が64bit範囲外です"));
    }
  }

  clonecore::DiskOperationProgress progress{
      .stage = clonecore::DiskOperationStage::planning,
      .total_read_bytes = total_logical_data_bytes,
      .total_write_bytes = total_logical_data_bytes,
      .cancellation_allowed = true,
  };
  clonecore::report_disk_operation_progress(callbacks, progress);
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return clonecore::Result<DcimgStreamBuildReport>::failure(
        cancelled_error(L"dcimg圧縮イメージ作成開始前"));
  }

  bool begin_attempted = true;
  const auto fail = [&](const clonecore::Error& primary) {
    if (begin_attempted) {
      const auto aborted = target.abort_incomplete();
      if (!aborted) {
        return clonecore::Result<DcimgStreamBuildReport>::failure(
            cleanup_error(primary, aborted.error()));
      }
    }
    return clonecore::Result<DcimgStreamBuildReport>::failure(primary);
  };
  auto status = target.begin(maximum_layout.image_length);
  if (!status) {
    return fail(status.error());
  }
  status = target.write_at(
      maximum_layout.header.manifest.offset, request.manifest);
  if (!status) {
    return fail(status.error());
  }
  status = target.write_at(
      maximum_layout.header.partition_table_snapshot.offset,
      request.partition_table_snapshot);
  if (!status) {
    return fail(status.error());
  }

  std::vector<DcimgChunkRecord> records;
  records.reserve(request.chunks.size());
  std::uint64_t stored_offset = maximum_layout.header.data.offset;
  std::uint64_t stored_data_bytes = 0U;
  progress.stage = clonecore::DiskOperationStage::copying_data;
  clonecore::report_disk_operation_progress(callbacks, progress);
  for (const auto& requested : request.chunks) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return fail(cancelled_error(L"dcimg Zstandardチャンク作成"));
    }

    DcimgChunkRecord record{
        .logical_offset = requested.logical_offset,
        .uncompressed_length = requested.logical_length,
        .flags = requested.zero_filled
            ? DcimgChunkFlags::zero_filled
            : DcimgChunkFlags::none,
    };
    if (requested.zero_filled) {
      const auto digest = sha256_zeroes(requested.logical_length);
      if (!digest) {
        return fail(digest.error());
      }
      record.sha256 = digest.value();
    } else {
      auto source_data = requested.source->read(
          requested.source_offset,
          static_cast<std::size_t>(requested.logical_length));
      if (!source_data) {
        return fail(source_data.error());
      }
      if (source_data.value().size() != requested.logical_length) {
        return fail(verification_error(
            L"dcimg Zstandardチャンク読取り",
            L"コピー元チャンクを完全に読み取れませんでした"));
      }
      progress.read_bytes += requested.logical_length;
      clonecore::report_disk_operation_progress(callbacks, progress);
      const auto digest = sha256(source_data.value());
      if (!digest) {
        return fail(digest.error());
      }
      record.sha256 = digest.value();
      auto compressed =
          compress_zstandard_dcimg_v1(source_data.value());
      if (!compressed) {
        return fail(compressed.error());
      }
      const bool use_compressed =
          compressed.value().size() < source_data.value().size();
      const std::span<const std::byte> stored = use_compressed
          ? std::span<const std::byte>(compressed.value())
          : std::span<const std::byte>(source_data.value());
      record.stored_offset = stored_offset;
      record.stored_length = stored.size();
      record.compression = use_compressed
          ? DcimgCompression::zstandard
          : DcimgCompression::none;
      status = target.write_at(stored_offset, stored);
      if (!status) {
        return fail(status.error());
      }
      if (!checked_add(stored_offset, stored.size(), stored_offset) ||
          !checked_add(
              stored_data_bytes, stored.size(), stored_data_bytes)) {
        return fail(argument_error(
            L"dcimg Zstandard保存長",
            L"圧縮後の保存位置が64bit範囲外です"));
      }
      progress.written_bytes += requested.logical_length;
      clonecore::report_disk_operation_progress(callbacks, progress);
    }
    records.push_back(record);
  }

  StreamLayout final_layout = maximum_layout;
  final_layout.header.data.length = stored_data_bytes;
  std::uint64_t next = 0U;
  if (!checked_add(
          final_layout.header.data.offset,
          final_layout.header.data.length,
          next)) {
    return fail(argument_error(
        L"dcimg Zstandardデータセクション",
        L"データセクション末尾が64bit範囲外です"));
  }
  final_layout.header.hash_table.offset = next;
  if (!checked_add(
          next, final_layout.header.hash_table.length, next)) {
    return fail(argument_error(
        L"dcimg Zstandardハッシュ表",
        L"ハッシュ表末尾が64bit範囲外です"));
  }
  final_layout.header.footer.offset = next;
  if (!checked_add(next, kDcimgFooterSize, final_layout.image_length) ||
      final_layout.image_length > maximum_layout.image_length) {
    return fail(argument_error(
        L"dcimg Zstandard最終長",
        L"圧縮後の最終長が予約上限を超えています"));
  }

  const auto header_bytes = serialize_header(final_layout.header);
  status = target.write_at(0U, header_bytes);
  if (!status) {
    return fail(status.error());
  }
  for (std::size_t index = 0U; index < records.size(); ++index) {
    const auto record_bytes = serialize_chunk_record(records[index]);
    status = target.write_at(
        final_layout.header.chunk_index.offset +
            index * kDcimgChunkRecordSize,
        record_bytes);
    if (!status) {
      return fail(status.error());
    }
    status = target.write_at(
        final_layout.header.hash_table.offset +
            index * Sha256Digest{}.size(),
        records[index].sha256);
    if (!status) {
      return fail(status.error());
    }
  }
  status = target.resize_before_verification(final_layout.image_length);
  if (!status) {
    return fail(status.error());
  }
  status = target.flush();
  if (!status) {
    return fail(status.error());
  }

  progress.stage = clonecore::DiskOperationStage::verifying_source;
  clonecore::report_disk_operation_progress(callbacks, progress);
  const auto global_hash = sha256_from_reader(
      final_layout.header.footer.offset,
      request.verification_block_bytes,
      [&](const std::uint64_t offset, const std::size_t length) {
        if (clonecore::disk_operation_cancellation_requested(callbacks)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              cancelled_error(L"dcimg Zstandard全体ハッシュ作成"));
        }
        return target.read_at(offset, length);
      });
  if (!global_hash) {
    return fail(global_hash.error());
  }
  const auto footer =
      serialize_footer(global_hash.value(), final_layout.image_length);
  status = target.write_at(final_layout.header.footer.offset, footer);
  if (!status) {
    return fail(status.error());
  }
  status = target.flush();
  if (!status) {
    return fail(status.error());
  }

  auto inspection = inspect_dcimg_v1_from_reader(
      final_layout.image_length,
      request.verification_block_bytes,
      [&](const std::uint64_t offset, const std::size_t length) {
        if (clonecore::disk_operation_cancellation_requested(callbacks)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              cancelled_error(L"dcimg Zstandard全件読戻し検証"));
        }
        return target.read_at(offset, length);
      },
      DcimgReadInspectionCallbacks{
          .progress =
              [&](const DcimgReadInspectionProgress& observed) {
                progress.total_verify_bytes = observed.total_verify_bytes;
                progress.verified_bytes = observed.verified_bytes;
                clonecore::report_disk_operation_progress(
                    callbacks, progress);
              },
      });
  if (!inspection) {
    return fail(inspection.error());
  }
  if (inspection.value().container.header.compression !=
          DcimgCompression::zstandard ||
      inspection.value().container.global_hash != global_hash.value()) {
    return fail(verification_error(
        L"dcimg Zstandard最終検証",
        L"読戻し結果が作成した圧縮プロファイルまたは全体ハッシュと一致しません"));
  }
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return fail(cancelled_error(L"dcimg Zstandard最終確定前"));
  }

  progress.stage = clonecore::DiskOperationStage::flushing_data;
  progress.cancellation_allowed = false;
  clonecore::report_disk_operation_progress(callbacks, progress);
  status = target.commit_verified();
  if (!status) {
    return fail(status.error());
  }
  begin_attempted = false;
  progress.stage = clonecore::DiskOperationStage::completed;
  progress.verified_bytes = progress.total_verify_bytes;
  clonecore::report_disk_operation_progress(callbacks, progress);
  return clonecore::Result<DcimgStreamBuildReport>::success(
      DcimgStreamBuildReport{
          .image_length = final_layout.image_length,
          .stored_data_bytes = stored_data_bytes,
          .zero_filled_bytes = zero_filled_bytes,
          .chunk_count = records.size(),
          .all_chunks_read_back_verified = true,
          .global_hash_read_back_verified = true,
          .committed = true,
      });
}

clonecore::Result<DcimgInspection> inspect_dcimg_v1(
    const std::span<const std::byte> image) {
  const auto header_result = parse_header(image);
  if (!header_result) {
    return clonecore::Result<DcimgInspection>::failure(header_result.error());
  }
  const DcimgHeader header = header_result.value();
  const auto sections = validate_sections(header, image.size());
  if (!sections) {
    return clonecore::Result<DcimgInspection>::failure(sections.error());
  }
  const auto chunks_result = parse_chunks(
      header,
      image,
      static_cast<std::size_t>(header.chunk_index.offset));
  if (!chunks_result) {
    return clonecore::Result<DcimgInspection>::failure(chunks_result.error());
  }
  const auto& chunks = chunks_result.value();

  const std::size_t footer_offset = static_cast<std::size_t>(header.footer.offset);
  const auto footer = image.subspan(footer_offset, kDcimgFooterSize);
  if (!std::equal(kFooterMagic.begin(), kFooterMagic.end(), footer.begin()) ||
      read_little<std::uint64_t>(footer, 40) != image.size() ||
      !all_zero(footer.subspan(48))) {
    return clonecore::Result<DcimgInspection>::failure(data_error(
        L"dcimgフッター", L"フッターのマジック、全体長、予約領域が不正です"));
  }
  Sha256Digest expected_global{};
  std::copy_n(footer.begin() + 8, expected_global.size(), expected_global.begin());
  const auto actual_global = sha256(image.first(footer_offset));
  if (!actual_global) {
    return clonecore::Result<DcimgInspection>::failure(actual_global.error());
  }
  if (actual_global.value() != expected_global) {
    return clonecore::Result<DcimgInspection>::failure(verification_error(
        L"dcimg全体ハッシュ", L"フッターの全体SHA-256が一致しません"));
  }

  const auto manifest_bytes = image.subspan(
      static_cast<std::size_t>(header.manifest.offset),
      static_cast<std::size_t>(header.manifest.length));
  const auto actual_manifest = sha256(manifest_bytes);
  if (!actual_manifest) {
    return clonecore::Result<DcimgInspection>::failure(actual_manifest.error());
  }
  if (actual_manifest.value() != header.manifest_hash) {
    return clonecore::Result<DcimgInspection>::failure(verification_error(
        L"dcimgマニフェストハッシュ",
        L"マニフェストSHA-256が一致しません"));
  }

  for (std::size_t index = 0; index < chunks.size(); ++index) {
    Sha256Digest table_hash{};
    const std::size_t table_offset = static_cast<std::size_t>(
        header.hash_table.offset + index * table_hash.size());
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(table_offset),
                table_hash.size(), table_hash.begin());
    if (table_hash != chunks[index].sha256) {
      return clonecore::Result<DcimgInspection>::failure(verification_error(
          L"dcimgハッシュ表", L"チャンク索引とハッシュ表が一致しません"));
    }
  }

  for (const auto& chunk : chunks) {
    clonecore::Result<Sha256Digest> actual =
        sha256_zeroes(chunk.uncompressed_length);
    if (chunk.flags != DcimgChunkFlags::zero_filled) {
      const auto stored = image.subspan(
          static_cast<std::size_t>(chunk.stored_offset),
          static_cast<std::size_t>(chunk.stored_length));
      if (chunk.compression == DcimgCompression::zstandard) {
        const auto uncompressed = decompress_zstandard_dcimg_v1(
            stored, static_cast<std::size_t>(chunk.uncompressed_length));
        if (!uncompressed) {
          return clonecore::Result<DcimgInspection>::failure(
              uncompressed.error());
        }
        actual = sha256(uncompressed.value());
      } else {
        actual = sha256(stored);
      }
    }
    if (!actual) {
      return clonecore::Result<DcimgInspection>::failure(actual.error());
    }
    if (actual.value() != chunk.sha256) {
      return clonecore::Result<DcimgInspection>::failure(verification_error(
          L"dcimgチャンクSHA-256", L"チャンクデータが破損しています"));
    }
  }

  DcimgInspection inspection;
  inspection.header = header;
  inspection.chunks = chunks;
  inspection.global_hash = actual_global.value();
  inspection.manifest_hash_verified = true;
  inspection.chunk_hashes_verified = true;
  inspection.global_hash_verified = true;
  return clonecore::Result<DcimgInspection>::success(std::move(inspection));
}

clonecore::Result<DcimgReadInspection> inspect_dcimg_v1_from_reader(
    const std::uint64_t image_length,
    const std::size_t maximum_block_bytes,
    const Sha256ReadCallback& reader,
    const DcimgReadInspectionCallbacks& callbacks) {
  const auto read_exact =
      [&reader](
          const std::uint64_t offset,
          const std::size_t length,
          const std::wstring_view operation)
      -> clonecore::Result<std::vector<std::byte>> {
    if (!reader) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          argument_error(
              std::wstring(operation),
              L"読み取りCallbackが設定されていません"));
    }
    auto bytes = reader(offset, length);
    if (!bytes) {
      return bytes;
    }
    if (bytes.value().size() != length) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          data_error(
              std::wstring(operation),
              L"要求した長さを完全に読み取れませんでした"));
    }
    return bytes;
  };

  if (image_length < kDcimgHeaderSize + kDcimgFooterSize ||
      maximum_block_bytes == 0 ||
      maximum_block_bytes > kDcimgChunkSize32MiB || !reader) {
    return clonecore::Result<DcimgReadInspection>::failure(
        argument_error(
            L"dcimg読取り専用検証設定",
            L"イメージ長、読取り上限、またはCallbackが不正です"));
  }

  auto header_bytes = read_exact(
      0, kDcimgHeaderSize, L"dcimg固定ヘッダー読取り");
  if (!header_bytes) {
    return clonecore::Result<DcimgReadInspection>::failure(
        header_bytes.error());
  }
  std::vector<std::byte> header_probe(
      kDcimgHeaderSize + kDcimgFooterSize, std::byte{0});
  std::copy(
      header_bytes.value().begin(),
      header_bytes.value().end(),
      header_probe.begin());
  const auto parsed_header = parse_header(header_probe);
  if (!parsed_header) {
    return clonecore::Result<DcimgReadInspection>::failure(
        parsed_header.error());
  }
  const DcimgHeader header = parsed_header.value();
  const auto sections = validate_sections(header, image_length);
  if (!sections) {
    return clonecore::Result<DcimgReadInspection>::failure(
        sections.error());
  }

  auto index_bytes = read_exact(
      header.chunk_index.offset,
      static_cast<std::size_t>(header.chunk_index.length),
      L"dcimgチャンク索引読取り");
  if (!index_bytes) {
    return clonecore::Result<DcimgReadInspection>::failure(
        index_bytes.error());
  }
  const auto parsed_chunks =
      parse_chunks(header, index_bytes.value(), 0U);
  if (!parsed_chunks) {
    return clonecore::Result<DcimgReadInspection>::failure(
        parsed_chunks.error());
  }
  const auto& chunks = parsed_chunks.value();

  std::uint64_t total_verify_bytes = 0;
  const auto add_verify_work =
      [&total_verify_bytes](const std::uint64_t amount) {
        std::uint64_t next = 0;
        if (!checked_add(total_verify_bytes, amount, next)) {
          return false;
        }
        total_verify_bytes = next;
        return true;
      };
  bool valid_work_total =
      add_verify_work(kDcimgHeaderSize) &&
      add_verify_work(header.chunk_index.length) &&
      add_verify_work(kDcimgFooterSize) &&
      add_verify_work(header.footer.offset) &&
      add_verify_work(header.manifest.length) &&
      add_verify_work(header.partition_table_snapshot.length) &&
      add_verify_work(header.hash_table.length);
  for (const auto& chunk : chunks) {
    valid_work_total =
        valid_work_total &&
        add_verify_work(
            chunk.flags == DcimgChunkFlags::zero_filled
                ? chunk.uncompressed_length
                : chunk.stored_length);
  }
  valid_work_total =
      valid_work_total &&
      add_verify_work(kDcimgHeaderSize) &&
      add_verify_work(kDcimgFooterSize);
  if (!valid_work_total || total_verify_bytes == 0) {
    return clonecore::Result<DcimgReadInspection>::failure(
        data_error(
            L"dcimg検証作業量",
            L"検証作業量の合計が64bit範囲外です"));
  }
  std::uint64_t verified_bytes =
      kDcimgHeaderSize + header.chunk_index.length;
  const auto publish_progress =
      [&callbacks, &verified_bytes, total_verify_bytes]() noexcept {
        if (!callbacks.progress) {
          return;
        }
        try {
          callbacks.progress(DcimgReadInspectionProgress{
              .verified_bytes =
                  (std::min)(verified_bytes, total_verify_bytes),
              .total_verify_bytes = total_verify_bytes,
          });
        } catch (...) {
          // Inspection must not unwind through an observer.
        }
      };
  const auto advance_progress =
      [&verified_bytes, &publish_progress](
          const std::uint64_t amount) noexcept {
        verified_bytes += amount;
        publish_progress();
      };
  publish_progress();

  auto footer = read_exact(
      header.footer.offset,
      kDcimgFooterSize,
      L"dcimgフッター読取り");
  if (!footer) {
    return clonecore::Result<DcimgReadInspection>::failure(
        footer.error());
  }
  advance_progress(kDcimgFooterSize);
  const auto footer_span = std::span<const std::byte>(footer.value());
  if (!std::equal(
          kFooterMagic.begin(),
          kFooterMagic.end(),
          footer_span.begin()) ||
      read_little<std::uint64_t>(footer_span, 40) != image_length ||
      !all_zero(footer_span.subspan(48))) {
    return clonecore::Result<DcimgReadInspection>::failure(
        data_error(
            L"dcimgフッター",
            L"フッターのマジック、全体長、予約領域が不正です"));
  }

  Sha256Digest expected_global{};
  std::copy_n(
      footer_span.begin() + 8,
      expected_global.size(),
      expected_global.begin());
  const auto actual_global = sha256_from_reader(
      header.footer.offset,
      maximum_block_bytes,
      [&reader, &advance_progress](
          const std::uint64_t offset,
          const std::size_t length) {
        auto bytes = reader(offset, length);
        if (bytes && bytes.value().size() == length) {
          advance_progress(length);
        }
        return bytes;
      });
  if (!actual_global) {
    return clonecore::Result<DcimgReadInspection>::failure(
        actual_global.error());
  }
  if (actual_global.value() != expected_global) {
    return clonecore::Result<DcimgReadInspection>::failure(
        verification_error(
            L"dcimg全体ハッシュ",
            L"フッターの全体SHA-256が一致しません"));
  }

  auto manifest = read_exact(
      header.manifest.offset,
      static_cast<std::size_t>(header.manifest.length),
      L"dcimgマニフェスト読取り");
  if (!manifest) {
    return clonecore::Result<DcimgReadInspection>::failure(
        manifest.error());
  }
  advance_progress(header.manifest.length);
  const auto actual_manifest = sha256(manifest.value());
  if (!actual_manifest) {
    return clonecore::Result<DcimgReadInspection>::failure(
        actual_manifest.error());
  }
  if (actual_manifest.value() != header.manifest_hash) {
    return clonecore::Result<DcimgReadInspection>::failure(
        verification_error(
            L"dcimgマニフェストハッシュ",
            L"マニフェストSHA-256が一致しません"));
  }

  auto partition_snapshot = read_exact(
      header.partition_table_snapshot.offset,
      static_cast<std::size_t>(
          header.partition_table_snapshot.length),
      L"dcimgパーティション表読取り");
  if (!partition_snapshot) {
    return clonecore::Result<DcimgReadInspection>::failure(
        partition_snapshot.error());
  }
  advance_progress(header.partition_table_snapshot.length);

  auto hash_table = read_exact(
      header.hash_table.offset,
      static_cast<std::size_t>(header.hash_table.length),
      L"dcimgハッシュ表読取り");
  if (!hash_table) {
    return clonecore::Result<DcimgReadInspection>::failure(
        hash_table.error());
  }
  advance_progress(header.hash_table.length);
  for (std::size_t index = 0; index < chunks.size(); ++index) {
    Sha256Digest table_hash{};
    std::copy_n(
        hash_table.value().begin() +
            static_cast<std::ptrdiff_t>(
                index * table_hash.size()),
        table_hash.size(),
        table_hash.begin());
    if (table_hash != chunks[index].sha256) {
      return clonecore::Result<DcimgReadInspection>::failure(
          verification_error(
              L"dcimgハッシュ表",
              L"チャンク索引とハッシュ表が一致しません"));
    }
  }

  for (const auto& chunk : chunks) {
    clonecore::Result<Sha256Digest> actual =
        sha256_zeroes(chunk.uncompressed_length);
    if (chunk.flags != DcimgChunkFlags::zero_filled &&
        chunk.compression == DcimgCompression::none) {
      actual = sha256_from_reader(
          chunk.stored_length,
          maximum_block_bytes,
          [&reader, &chunk, &advance_progress](
              const std::uint64_t relative_offset,
              const std::size_t length) {
            auto bytes = reader(
                chunk.stored_offset + relative_offset,
                length);
            if (bytes && bytes.value().size() == length) {
              advance_progress(length);
            }
            return bytes;
          });
    } else if (chunk.flags != DcimgChunkFlags::zero_filled) {
      std::vector<std::byte> stored;
      try {
        stored.resize(static_cast<std::size_t>(chunk.stored_length));
      } catch (const std::bad_alloc&) {
        return clonecore::Result<DcimgReadInspection>::failure(
            clonecore::Error{
                .code = clonecore::ErrorCode::io_failed,
                .native_code = ERROR_NOT_ENOUGH_MEMORY,
                .operation = L"dcimg Zstandard検証メモリ",
                .message = L"圧縮チャンク用メモリを確保できませんでした",
            });
      }
      std::size_t position = 0U;
      while (position < stored.size()) {
        const std::size_t length =
            (std::min)(maximum_block_bytes, stored.size() - position);
        auto bytes = reader(chunk.stored_offset + position, length);
        if (!bytes) {
          return clonecore::Result<DcimgReadInspection>::failure(
              bytes.error());
        }
        if (bytes.value().size() != length) {
          return clonecore::Result<DcimgReadInspection>::failure(
              data_error(
                  L"dcimg Zstandardチャンク読取り",
                  L"要求した長さを完全に読み取れませんでした"));
        }
        std::copy(
            bytes.value().begin(),
            bytes.value().end(),
            stored.begin() + static_cast<std::ptrdiff_t>(position));
        position += length;
        advance_progress(length);
      }
      const auto uncompressed = decompress_zstandard_dcimg_v1(
          stored, static_cast<std::size_t>(chunk.uncompressed_length));
      if (!uncompressed) {
        return clonecore::Result<DcimgReadInspection>::failure(
            uncompressed.error());
      }
      actual = sha256(uncompressed.value());
    }
    if (!actual) {
      return clonecore::Result<DcimgReadInspection>::failure(
          actual.error());
    }
    if (actual.value() != chunk.sha256) {
      return clonecore::Result<DcimgReadInspection>::failure(
          verification_error(
              L"dcimgチャンクSHA-256",
              L"チャンクデータが破損しています"));
    }
    if (chunk.flags == DcimgChunkFlags::zero_filled) {
      advance_progress(chunk.uncompressed_length);
    }
  }

  auto final_header = read_exact(
      0, kDcimgHeaderSize, L"dcimg固定ヘッダー再読取り");
  auto final_footer = read_exact(
      header.footer.offset,
      kDcimgFooterSize,
      L"dcimgフッター再読取り");
  if (!final_header || !final_footer) {
    return clonecore::Result<DcimgReadInspection>::failure(
        !final_header ? final_header.error() : final_footer.error());
  }
  advance_progress(kDcimgHeaderSize);
  advance_progress(kDcimgFooterSize);
  if (final_header.value() != header_bytes.value() ||
      final_footer.value() != footer.value()) {
    return clonecore::Result<DcimgReadInspection>::failure(
        verification_error(
            L"dcimg検証中の変更",
            L"検証中にヘッダーまたはフッターが変更されました"));
  }

  DcimgInspection inspection{
      .header = header,
      .chunks = chunks,
      .global_hash = actual_global.value(),
      .manifest_hash_verified = true,
      .chunk_hashes_verified = true,
      .global_hash_verified = true,
  };
  return clonecore::Result<DcimgReadInspection>::success(
      DcimgReadInspection{
          .container = std::move(inspection),
          .manifest = manifest.take_value(),
          .partition_table_snapshot =
              partition_snapshot.take_value(),
      });
}

}  // namespace ytec::imageformat

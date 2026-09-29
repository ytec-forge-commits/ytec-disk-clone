#include "ytec/imageformat/tsumugi.h"

#include "ytec/imageformat/compression.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ytec::imageformat {
namespace {

constexpr std::array<std::byte, 8> kHeaderMagic{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'S'}, std::byte{'U'},
    std::byte{'M'}, std::byte{'G'}, std::byte{0x1A}, std::byte{0x0A}};
constexpr std::array<std::byte, 8> kMetadataMagic{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'M'}, std::byte{'E'},
    std::byte{'T'}, std::byte{'A'}, std::byte{0x1A}, std::byte{0x0A}};
constexpr std::array<std::byte, 8> kFooterMagic{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'E'}, std::byte{'N'},
    std::byte{'D'}, std::byte{0x1A}, std::byte{0x0D}, std::byte{0x0A}};
constexpr std::array<std::byte, 8> kChunkAadMagic{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'C'}, std::byte{'H'},
    std::byte{'A'}, std::byte{'D'}, std::byte{0x1A}, std::byte{0x0A}};
constexpr std::uint32_t kEndianMarker = 0x01020304U;
constexpr std::uint32_t kFeatureEncrypted = static_cast<std::uint32_t>(
    TsumugiRequiredFeature::encrypted);
constexpr std::uint32_t kFeatureUnreadableMap = static_cast<std::uint32_t>(
    TsumugiRequiredFeature::unreadable_range_map);
constexpr std::uint32_t kFeatureRescueReadEvidence =
    static_cast<std::uint32_t>(
        TsumugiRequiredFeature::rescue_read_evidence);
constexpr std::uint32_t kKnownRequiredFeatures =
    kFeatureEncrypted | kFeatureUnreadableMap |
    kFeatureRescueReadEvidence;
constexpr std::uint32_t kArgon2idKdf = 1U;
constexpr std::size_t kHeaderMetadataTagOffset = 172U;
constexpr std::size_t kHeaderHashOffset = 188U;

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

bool valid_payload_kind(const TsumugiPayloadKind kind) noexcept {
  return kind == TsumugiPayloadKind::exact_disk ||
      kind == TsumugiPayloadKind::shrink_disk ||
      kind == TsumugiPayloadKind::rescue_disk;
}

bool valid_compression(const ImageCompression compression) noexcept {
  return compression == ImageCompression::none ||
      compression == ImageCompression::zstandard;
}

bool valid_chunk_size(const std::uint32_t chunk_size) noexcept {
  return chunk_size == kImageChunkSize16MiB ||
      chunk_size == kImageChunkSize32MiB;
}

std::uint32_t raw_flags(const TsumugiChunkFlags flags) noexcept {
  return static_cast<std::uint32_t>(flags);
}

bool chunk_is_zero(const TsumugiChunkFlags flags) noexcept {
  return (raw_flags(flags) &
          static_cast<std::uint32_t>(TsumugiChunkFlags::zero_filled)) != 0U;
}

bool chunk_is_unreadable(const TsumugiChunkFlags flags) noexcept {
  return (raw_flags(flags) & 2U) != 0U;
}

bool valid_chunk_flags(const TsumugiChunkFlags flags) noexcept {
  const std::uint32_t value = raw_flags(flags);
  return value == 0U || value == 1U || value == 3U;
}

bool valid_rescue_read_evidence(
    const TsumugiRescueReadEvidence& evidence) noexcept {
  return evidence.forward_attempts != 0U &&
      evidence.reverse_attempts != 0U &&
      evidence.sector_attempts != 0U &&
      evidence.zero_fill_read_back_verified;
}

void write_rescue_read_evidence(
    const std::span<std::byte> record_bytes,
    const TsumugiRescueReadEvidence& evidence) noexcept {
  write_little(record_bytes, 96U, evidence.forward_attempts);
  write_little(record_bytes, 97U, evidence.reverse_attempts);
  write_little(record_bytes, 98U, evidence.sector_attempts);
  write_little(record_bytes, 99U, std::uint8_t{1U});
  write_little(record_bytes, 100U, evidence.forward_native_error);
  write_little(record_bytes, 104U, evidence.reverse_native_error);
  write_little(record_bytes, 108U, evidence.sector_native_error);
}

void write_section(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const ImageSection& section) noexcept {
  write_little(bytes, offset, section.offset);
  write_little(bytes, offset + 8U, section.length);
}

ImageSection read_section(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
  return ImageSection{
      .offset = read_little<std::uint64_t>(bytes, offset),
      .length = read_little<std::uint64_t>(bytes, offset + 8U),
  };
}

clonecore::Result<std::array<std::byte, kTsumugiGcmNonceBytes>>
nonce_for_counter(
    const std::array<std::byte, kTsumugiGcmNonceBytes>& base,
    const std::uint64_t counter) {
  std::array<std::byte, kTsumugiGcmNonceBytes> nonce = base;
  const auto bytes = std::span<std::byte>(nonce);
  const std::uint64_t base_counter = read_little<std::uint64_t>(bytes, 4U);
  std::uint64_t value{};
  if (!checked_add(base_counter, counter, value)) {
    return clonecore::Result<
        std::array<std::byte, kTsumugiGcmNonceBytes>>::failure(
        argument_error(
            L"Tsumugi GCM Nonce導出",
            L"画像内のNonce counterが64bit上限を超えます"));
  }
  write_little(bytes, 4U, value);
  return clonecore::Result<
      std::array<std::byte, kTsumugiGcmNonceBytes>>::success(nonce);
}

std::array<std::byte, 64> build_chunk_aad(
    const std::array<std::byte, 16>& image_id,
    const std::uint64_t chunk_index,
    const TsumugiChunkRecord& record) {
  std::array<std::byte, 64> aad{};
  const auto bytes = std::span<std::byte>(aad);
  std::copy(kChunkAadMagic.begin(), kChunkAadMagic.end(), bytes.begin());
  std::copy(image_id.begin(), image_id.end(), bytes.begin() + 8U);
  write_little(bytes, 24U, chunk_index);
  write_little(bytes, 32U, record.logical_offset);
  write_little(bytes, 40U, record.logical_length);
  write_little(bytes, 48U, raw_flags(record.flags));
  write_little(
      bytes, 52U, static_cast<std::uint16_t>(record.compression));
  write_little(bytes, 56U, record.stored_length);
  return aad;
}

std::vector<std::byte> canonical_metadata_aad(
    const std::span<const std::byte> header) {
  std::vector<std::byte> aad(header.begin(), header.end());
  std::fill(
      aad.begin() + kHeaderMetadataTagOffset,
      aad.begin() + kHeaderMetadataTagOffset + kTsumugiGcmTagBytes,
      std::byte{0});
  std::fill(
      aad.begin() + kHeaderHashOffset,
      aad.begin() + kHeaderHashOffset + Sha256Digest{}.size(),
      std::byte{0});
  return aad;
}

clonecore::Result<Sha256Digest> calculate_header_hash(
    const std::span<const std::byte> header) {
  if (header.size() != kTsumugiHeaderSize) {
    return clonecore::Result<Sha256Digest>::failure(argument_error(
        L"TsumugiヘッダーHash範囲",
        L"固定ヘッダー長が一致しません"));
  }
  std::vector<std::byte> canonical(header.begin(), header.end());
  std::fill(
      canonical.begin() + kHeaderHashOffset,
      canonical.begin() + kHeaderHashOffset + Sha256Digest{}.size(),
      std::byte{0});
  return sha256(canonical);
}

clonecore::Status validate_build_request(const TsumugiBuildRequest& request) {
  if (!valid_payload_kind(request.payload_kind) ||
      request.source_disk_size == 0U ||
      !is_supported_sector_size_pair(
          request.logical_sector_size, request.physical_sector_size) ||
      request.source_disk_size % request.logical_sector_size != 0U ||
      !valid_chunk_size(request.chunk_size) ||
      !valid_compression(request.compression) ||
      request.manifest.empty() ||
      request.manifest.size() > kTsumugiMaximumManifestBytes ||
      request.chunks.size() > kTsumugiMaximumChunkCount ||
      all_zero(request.image_id)) {
    return clonecore::Status::failure(argument_error(
        L"Tsumugi v1作成要求",
        L"画像種別、ディスク寸法、識別子、メタデータ、または圧縮設定が不正です"));
  }

  if (request.encryption.has_value()) {
    const auto assessment =
        assess_tsumugi_password(request.encryption->password);
    const auto& parameters = request.encryption->argon2;
    if (!assessment.accepted ||
        parameters.memory_kib != kTsumugiArgon2MemoryKiB ||
        parameters.iterations != kTsumugiArgon2Iterations ||
        parameters.parallelism != kTsumugiArgon2Parallelism ||
        all_zero(parameters.salt) ||
        all_zero(request.encryption->base_nonce)) {
      return clonecore::Status::failure(argument_error(
          L"Tsumugi v1暗号設定",
          L"v1はASCII 8文字以上と固定Argon2idプロファイルだけを作成できます"));
    }
  }

  std::uint64_t previous_end = 0U;
  bool first = true;
  const bool byte_stream_payload =
      request.payload_kind == TsumugiPayloadKind::shrink_disk;
  for (const auto& chunk : request.chunks) {
    std::uint64_t logical_end{};
    const bool zero = chunk_is_zero(chunk.flags);
    if (!valid_chunk_flags(chunk.flags) || chunk.logical_length == 0U ||
        chunk.logical_length > request.chunk_size ||
        (!byte_stream_payload &&
         (chunk.logical_offset % request.logical_sector_size != 0U ||
          chunk.logical_length % request.logical_sector_size != 0U)) ||
        !checked_add(
            chunk.logical_offset, chunk.logical_length, logical_end) ||
        logical_end > request.source_disk_size ||
        (!first && chunk.logical_offset < previous_end) ||
        (zero && !chunk.data.empty()) ||
        (!zero && chunk.data.size() != chunk.logical_length)) {
      return clonecore::Status::failure(argument_error(
          L"Tsumugi v1チャンク要求",
          L"チャンクが重複、境界外、非整列、またはデータ長不一致です"));
    }
    if (chunk_is_unreadable(chunk.flags) &&
        request.payload_kind != TsumugiPayloadKind::rescue_disk) {
      return clonecore::Status::failure(argument_error(
          L"Tsumugi v1欠損マップ",
          L"読取り不能範囲は救出イメージだけに記録できます"));
    }
    if (chunk.rescue_read_evidence.has_value() !=
            chunk_is_unreadable(chunk.flags) ||
        (chunk.rescue_read_evidence.has_value() &&
         !valid_rescue_read_evidence(*chunk.rescue_read_evidence))) {
      return clonecore::Status::failure(argument_error(
          L"Tsumugi v1救出読取り証跡",
          L"読取り不能範囲には有限リトライとゼロ埋め読戻しの証跡が必要です"));
    }
    previous_end = logical_end;
    first = false;
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> build_metadata(
    const std::span<const std::byte> manifest,
    const std::span<const TsumugiChunkRecord> records) {
  std::uint64_t records_length{};
  std::uint64_t metadata_length{};
  if (!checked_multiply(
          records.size(), kTsumugiChunkRecordSize, records_length) ||
      !checked_add(
          kTsumugiMetadataHeaderSize, manifest.size(), metadata_length) ||
      !checked_add(metadata_length, records_length, metadata_length) ||
      metadata_length > kTsumugiMaximumMetadataBytes ||
      metadata_length > (std::numeric_limits<std::size_t>::max)()) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        argument_error(
            L"Tsumugi v1メタデータ寸法",
            L"メタデータまたはチャンク索引が固定上限を超えています"));
  }

  std::vector<std::byte> metadata(
      static_cast<std::size_t>(metadata_length), std::byte{0});
  auto bytes = std::span<std::byte>(metadata);
  std::copy(kMetadataMagic.begin(), kMetadataMagic.end(), bytes.begin());
  write_little(bytes, 8U, kTsumugiMajorVersion);
  write_little(bytes, 10U, kTsumugiMinorVersion);
  write_little(bytes, 12U, kTsumugiMetadataHeaderSize);
  write_little(bytes, 16U, static_cast<std::uint64_t>(manifest.size()));
  write_little(bytes, 24U, static_cast<std::uint64_t>(records.size()));
  std::copy(
      manifest.begin(),
      manifest.end(),
      bytes.begin() + kTsumugiMetadataHeaderSize);

  std::size_t offset = kTsumugiMetadataHeaderSize + manifest.size();
  for (const auto& record : records) {
    const auto record_bytes = bytes.subspan(offset, kTsumugiChunkRecordSize);
    write_little(record_bytes, 0U, record.logical_offset);
    write_little(record_bytes, 8U, record.logical_length);
    write_little(record_bytes, 16U, record.stored_offset);
    write_little(record_bytes, 24U, record.stored_length);
    write_little(record_bytes, 32U, raw_flags(record.flags));
    write_little(
        record_bytes,
        36U,
        static_cast<std::uint16_t>(record.compression));
    write_little(record_bytes, 40U, record.nonce_counter);
    std::copy(
        record.plaintext_hash.begin(),
        record.plaintext_hash.end(),
        record_bytes.begin() + 48U);
    std::copy(
        record.authentication_tag.begin(),
        record.authentication_tag.end(),
        record_bytes.begin() + 80U);
    if (record.rescue_read_evidence.has_value()) {
      write_rescue_read_evidence(
          record_bytes, *record.rescue_read_evidence);
    }
    offset += kTsumugiChunkRecordSize;
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(metadata));
}

std::vector<std::byte> serialize_header(const TsumugiHeader& header) {
  std::vector<std::byte> result(kTsumugiHeaderSize, std::byte{0});
  auto bytes = std::span<std::byte>(result);
  std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin());
  write_little(bytes, 8U, header.major_version);
  write_little(bytes, 10U, header.minor_version);
  write_little(bytes, 12U, kTsumugiHeaderSize);
  write_little(bytes, 16U, kEndianMarker);
  write_little(bytes, 20U, header.required_features);
  write_little(bytes, 24U, std::uint32_t{0});
  write_little(
      bytes, 28U, static_cast<std::uint16_t>(header.payload_kind));
  write_little(
      bytes, 30U, static_cast<std::uint16_t>(header.compression));
  write_little(bytes, 32U, header.source_disk_size);
  write_little(bytes, 40U, header.logical_sector_size);
  write_little(bytes, 44U, header.physical_sector_size);
  write_little(bytes, 48U, header.chunk_size);
  write_little(bytes, 56U, header.chunk_count);
  write_section(bytes, 64U, header.data);
  write_section(bytes, 80U, header.metadata);
  write_section(bytes, 96U, header.footer);
  const bool encrypted = (header.required_features & kFeatureEncrypted) != 0U;
  write_little(bytes, 112U, header.argon2.memory_kib);
  write_little(bytes, 116U, header.argon2.iterations);
  write_little(bytes, 120U, header.argon2.parallelism);
  write_little(bytes, 124U, encrypted ? kArgon2idKdf : 0U);
  std::copy(header.argon2.salt.begin(), header.argon2.salt.end(), bytes.begin() + 128U);
  std::copy(header.base_nonce.begin(), header.base_nonce.end(), bytes.begin() + 144U);
  std::copy(header.image_id.begin(), header.image_id.end(), bytes.begin() + 156U);
  std::copy(
      header.metadata_tag.begin(),
      header.metadata_tag.end(),
      bytes.begin() + kHeaderMetadataTagOffset);
  std::copy(
      header.header_hash.begin(),
      header.header_hash.end(),
      bytes.begin() + kHeaderHashOffset);
  return result;
}

clonecore::Result<TsumugiHeader> parse_header(
    const std::span<const std::byte> image) {
  if (image.size() < kTsumugiHeaderSize + kTsumugiFooterSize) {
    return clonecore::Result<TsumugiHeader>::failure(data_error(
        L"Tsumugi v1固定領域",
        L"画像が固定ヘッダーとフッターより短いです"));
  }
  const auto bytes = image.first(kTsumugiHeaderSize);
  if (!std::equal(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin())) {
    return clonecore::Result<TsumugiHeader>::failure(data_error(
        L"Tsumugi v1マジック",
        L"正式な.tsumugi v1形式ではありません"));
  }
  if (read_little<std::uint16_t>(bytes, 8U) != kTsumugiMajorVersion ||
      read_little<std::uint16_t>(bytes, 10U) != kTsumugiMinorVersion) {
    return clonecore::Result<TsumugiHeader>::failure(unsupported_error(
        L"Tsumugi形式バージョン",
        L"このアプリが対応していない.tsumugi形式です"));
  }
  if (read_little<std::uint32_t>(bytes, 12U) != kTsumugiHeaderSize ||
      read_little<std::uint32_t>(bytes, 16U) != kEndianMarker ||
      read_little<std::uint32_t>(bytes, 24U) != 0U ||
      !all_zero(bytes.subspan(220U))) {
    return clonecore::Result<TsumugiHeader>::failure(data_error(
        L"Tsumugi v1固定ヘッダー",
        L"ヘッダー長、エンディアン、任意機能、または予約領域が不正です"));
  }

  TsumugiHeader header{};
  header.major_version = read_little<std::uint16_t>(bytes, 8U);
  header.minor_version = read_little<std::uint16_t>(bytes, 10U);
  header.required_features = read_little<std::uint32_t>(bytes, 20U);
  header.payload_kind = static_cast<TsumugiPayloadKind>(
      read_little<std::uint16_t>(bytes, 28U));
  header.compression = static_cast<ImageCompression>(
      read_little<std::uint16_t>(bytes, 30U));
  header.source_disk_size = read_little<std::uint64_t>(bytes, 32U);
  header.logical_sector_size = read_little<std::uint32_t>(bytes, 40U);
  header.physical_sector_size = read_little<std::uint32_t>(bytes, 44U);
  header.chunk_size = read_little<std::uint32_t>(bytes, 48U);
  header.chunk_count = read_little<std::uint64_t>(bytes, 56U);
  header.data = read_section(bytes, 64U);
  header.metadata = read_section(bytes, 80U);
  header.footer = read_section(bytes, 96U);
  header.argon2.memory_kib = read_little<std::uint32_t>(bytes, 112U);
  header.argon2.iterations = read_little<std::uint32_t>(bytes, 116U);
  header.argon2.parallelism = read_little<std::uint32_t>(bytes, 120U);
  std::copy_n(bytes.begin() + 128U, header.argon2.salt.size(), header.argon2.salt.begin());
  std::copy_n(bytes.begin() + 144U, header.base_nonce.size(), header.base_nonce.begin());
  std::copy_n(bytes.begin() + 156U, header.image_id.size(), header.image_id.begin());
  std::copy_n(
      bytes.begin() + kHeaderMetadataTagOffset,
      header.metadata_tag.size(),
      header.metadata_tag.begin());
  std::copy_n(
      bytes.begin() + kHeaderHashOffset,
      header.header_hash.size(),
      header.header_hash.begin());

  const std::uint32_t unknown =
      header.required_features & ~kKnownRequiredFeatures;
  const bool encrypted =
      (header.required_features & kFeatureEncrypted) != 0U;
  const bool unreadable =
      (header.required_features & kFeatureUnreadableMap) != 0U;
  const bool rescue_read_evidence =
      (header.required_features & kFeatureRescueReadEvidence) != 0U;
  if (unknown != 0U || !valid_payload_kind(header.payload_kind) ||
      !valid_compression(header.compression) ||
      header.source_disk_size == 0U ||
      !is_supported_sector_size_pair(
          header.logical_sector_size, header.physical_sector_size) ||
      header.source_disk_size % header.logical_sector_size != 0U ||
      !valid_chunk_size(header.chunk_size) ||
      header.chunk_count > kTsumugiMaximumChunkCount ||
      all_zero(header.image_id) ||
      (unreadable &&
       header.payload_kind != TsumugiPayloadKind::rescue_disk) ||
      (rescue_read_evidence &&
       (!unreadable ||
        header.payload_kind != TsumugiPayloadKind::rescue_disk))) {
    return clonecore::Result<TsumugiHeader>::failure(unsupported_error(
        L"Tsumugi v1必須機能",
        L"必須機能、画像種別、またはディスク寸法が対応範囲外です"));
  }
  const std::uint32_t kdf = read_little<std::uint32_t>(bytes, 124U);
  if (encrypted) {
    if (kdf != kArgon2idKdf ||
        header.argon2.memory_kib != kTsumugiArgon2MemoryKiB ||
        header.argon2.iterations != kTsumugiArgon2Iterations ||
        header.argon2.parallelism != kTsumugiArgon2Parallelism ||
        all_zero(header.argon2.salt) || all_zero(header.base_nonce)) {
      return clonecore::Result<TsumugiHeader>::failure(unsupported_error(
          L"Tsumugi v1暗号プロファイル",
          L"記録されたArgon2idプロファイルに対応していません"));
    }
  } else if (
      kdf != 0U || header.argon2.memory_kib != 0U ||
      header.argon2.iterations != 0U ||
      header.argon2.parallelism != 0U ||
      !all_zero(header.argon2.salt) || !all_zero(header.base_nonce) ||
      !all_zero(header.metadata_tag)) {
    return clonecore::Result<TsumugiHeader>::failure(data_error(
        L"Tsumugi v1非暗号ヘッダー",
        L"非暗号画像に暗号パラメーターが記録されています"));
  }

  const auto calculated = calculate_header_hash(bytes);
  if (!calculated || calculated.value() != header.header_hash) {
    return clonecore::Result<TsumugiHeader>::failure(
        calculated ? verification_error(
                         L"Tsumugi v1ヘッダーHash",
                         L"固定ヘッダーが破損または変更されています")
                   : calculated.error());
  }
  return clonecore::Result<TsumugiHeader>::success(std::move(header));
}

clonecore::Status validate_sections(
    const TsumugiHeader& header,
    const std::size_t image_size) {
  std::uint64_t metadata_end{};
  std::uint64_t data_end{};
  std::uint64_t footer_end{};
  if (header.metadata.offset != kTsumugiHeaderSize ||
      header.metadata.length < kTsumugiMetadataHeaderSize ||
      header.metadata.length > kTsumugiMaximumMetadataBytes ||
      !checked_add(
          header.metadata.offset, header.metadata.length, metadata_end) ||
      metadata_end != header.data.offset ||
      !checked_add(header.data.offset, header.data.length, data_end) ||
      data_end != header.footer.offset ||
      header.footer.length != kTsumugiFooterSize ||
      !checked_add(header.footer.offset, header.footer.length, footer_end) ||
      footer_end != image_size) {
    return clonecore::Status::failure(data_error(
        L"Tsumugi v1セクション境界",
        L"データ、メタデータ、フッターが連続していないか画像境界外です"));
  }
  return clonecore::success_status();
}

struct ParsedMetadata final {
  std::vector<std::byte> manifest;
  std::vector<TsumugiChunkRecord> records;
};

clonecore::Result<ParsedMetadata> parse_metadata(
    const std::span<const std::byte> metadata,
    const TsumugiHeader& header,
    const bool encrypted) {
  if (metadata.size() < kTsumugiMetadataHeaderSize ||
      !std::equal(
          kMetadataMagic.begin(), kMetadataMagic.end(), metadata.begin()) ||
      read_little<std::uint16_t>(metadata, 8U) != kTsumugiMajorVersion ||
      read_little<std::uint16_t>(metadata, 10U) != kTsumugiMinorVersion ||
      read_little<std::uint32_t>(metadata, 12U) !=
          kTsumugiMetadataHeaderSize) {
    return clonecore::Result<ParsedMetadata>::failure(data_error(
        L"Tsumugi v1メタデータヘッダー",
        L"復号済みメタデータのマジック、版、または長さが不正です"));
  }
  const std::uint64_t manifest_length =
      read_little<std::uint64_t>(metadata, 16U);
  const std::uint64_t chunk_count =
      read_little<std::uint64_t>(metadata, 24U);
  std::uint64_t records_length{};
  std::uint64_t expected_length{};
  if (manifest_length == 0U ||
      manifest_length > kTsumugiMaximumManifestBytes ||
      chunk_count != header.chunk_count ||
      !checked_multiply(
          chunk_count, kTsumugiChunkRecordSize, records_length) ||
      !checked_add(
          kTsumugiMetadataHeaderSize, manifest_length, expected_length) ||
      !checked_add(expected_length, records_length, expected_length) ||
      expected_length != metadata.size()) {
    return clonecore::Result<ParsedMetadata>::failure(data_error(
        L"Tsumugi v1メタデータ寸法",
        L"マニフェスト、チャンク件数、または索引寸法が一致しません"));
  }

  ParsedMetadata parsed{};
  parsed.manifest.assign(
      metadata.begin() + kTsumugiMetadataHeaderSize,
      metadata.begin() + kTsumugiMetadataHeaderSize +
          static_cast<std::size_t>(manifest_length));
  parsed.records.reserve(static_cast<std::size_t>(chunk_count));
  std::size_t offset = kTsumugiMetadataHeaderSize +
      static_cast<std::size_t>(manifest_length);
  std::uint64_t previous_logical_end = 0U;
  std::uint64_t expected_stored_offset = header.data.offset;
  bool has_unreadable = false;
  bool has_rescue_read_evidence = false;
  const bool rescue_read_evidence_feature =
      (header.required_features & kFeatureRescueReadEvidence) != 0U;
  const bool byte_stream_payload =
      header.payload_kind == TsumugiPayloadKind::shrink_disk;
  for (std::uint64_t index = 0; index < chunk_count; ++index) {
    const auto record_bytes =
        metadata.subspan(offset, kTsumugiChunkRecordSize);
    TsumugiChunkRecord record{};
    record.logical_offset = read_little<std::uint64_t>(record_bytes, 0U);
    record.logical_length = read_little<std::uint64_t>(record_bytes, 8U);
    record.stored_offset = read_little<std::uint64_t>(record_bytes, 16U);
    record.stored_length = read_little<std::uint64_t>(record_bytes, 24U);
    record.flags = static_cast<TsumugiChunkFlags>(
        read_little<std::uint32_t>(record_bytes, 32U));
    record.compression = static_cast<ImageCompression>(
        read_little<std::uint16_t>(record_bytes, 36U));
    record.nonce_counter =
        read_little<std::uint64_t>(record_bytes, 40U);
    std::copy_n(
        record_bytes.begin() + 48U,
        record.plaintext_hash.size(),
        record.plaintext_hash.begin());
    std::copy_n(
        record_bytes.begin() + 80U,
        record.authentication_tag.size(),
        record.authentication_tag.begin());
    const bool unreadable = chunk_is_unreadable(record.flags);
    bool serialized_rescue_evidence_valid = true;
    if (rescue_read_evidence_feature && unreadable) {
      TsumugiRescueReadEvidence evidence{
          .forward_attempts =
              read_little<std::uint8_t>(record_bytes, 96U),
          .reverse_attempts =
              read_little<std::uint8_t>(record_bytes, 97U),
          .sector_attempts =
              read_little<std::uint8_t>(record_bytes, 98U),
          .zero_fill_read_back_verified =
              read_little<std::uint8_t>(record_bytes, 99U) == 1U,
          .forward_native_error =
              read_little<std::uint32_t>(record_bytes, 100U),
          .reverse_native_error =
              read_little<std::uint32_t>(record_bytes, 104U),
          .sector_native_error =
              read_little<std::uint32_t>(record_bytes, 108U),
      };
      serialized_rescue_evidence_valid =
          read_little<std::uint8_t>(record_bytes, 99U) == 1U &&
          valid_rescue_read_evidence(evidence);
      record.rescue_read_evidence = evidence;
    } else {
      serialized_rescue_evidence_valid =
          all_zero(record_bytes.subspan(96U, 16U));
    }
    std::uint64_t logical_end{};
    std::uint64_t stored_end{};
    const bool zero = chunk_is_zero(record.flags);
    if (!all_zero(record_bytes.subspan(38U, 2U)) ||
        !serialized_rescue_evidence_valid ||
        !valid_chunk_flags(record.flags) ||
        !valid_compression(record.compression) ||
        record.logical_length == 0U ||
        record.logical_length > header.chunk_size ||
        (!byte_stream_payload &&
         (record.logical_offset % header.logical_sector_size != 0U ||
          record.logical_length % header.logical_sector_size != 0U)) ||
        !checked_add(
            record.logical_offset, record.logical_length, logical_end) ||
        logical_end > header.source_disk_size ||
        (index != 0U && record.logical_offset < previous_logical_end) ||
        record.stored_offset != expected_stored_offset ||
        !checked_add(
            record.stored_offset, record.stored_length, stored_end) ||
        stored_end > header.footer.offset ||
        (zero &&
         (record.stored_length != 0U ||
          record.compression != ImageCompression::none ||
          record.nonce_counter != 0U ||
          !all_zero(record.authentication_tag))) ||
        (!zero && record.stored_length == 0U) ||
        (encrypted && !zero && record.nonce_counter != index + 1U) ||
        (!encrypted &&
         (record.nonce_counter != 0U ||
          !all_zero(record.authentication_tag)))) {
      return clonecore::Result<ParsedMetadata>::failure(data_error(
          L"Tsumugi v1チャンク索引",
          L"チャンク範囲、格納範囲、暗号情報、または予約領域が不正です"));
    }
    if (chunk_is_unreadable(record.flags) &&
        header.payload_kind != TsumugiPayloadKind::rescue_disk) {
      return clonecore::Result<ParsedMetadata>::failure(data_error(
          L"Tsumugi v1欠損索引",
          L"救出画像以外に読取り不能範囲が記録されています"));
    }
    has_unreadable = has_unreadable || unreadable;
    has_rescue_read_evidence = has_rescue_read_evidence ||
        record.rescue_read_evidence.has_value();
    previous_logical_end = logical_end;
    expected_stored_offset = stored_end;
    parsed.records.push_back(record);
    offset += kTsumugiChunkRecordSize;
  }
  if (expected_stored_offset != header.footer.offset ||
      (((header.required_features & kFeatureUnreadableMap) != 0U) !=
       has_unreadable) ||
      (rescue_read_evidence_feature != has_rescue_read_evidence)) {
    return clonecore::Result<ParsedMetadata>::failure(data_error(
        L"Tsumugi v1データ被覆",
        L"チャンク索引がデータ領域全体を正確に被覆していません"));
  }
  return clonecore::Result<ParsedMetadata>::success(std::move(parsed));
}

}  // namespace

clonecore::Result<std::vector<std::byte>> build_tsumugi_v1(
    const TsumugiBuildRequest& request) {
  const auto valid = validate_build_request(request);
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(valid.error());
  }

  const bool encrypted = request.encryption.has_value();
  std::optional<TsumugiKey> key;
  if (encrypted) {
    auto derived = derive_tsumugi_key_argon2id(
        request.encryption->password, request.encryption->argon2);
    if (!derived) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          derived.error());
    }
    key.emplace(derived.take_value());
  }

  std::vector<std::byte> stored_data;
  std::vector<TsumugiChunkRecord> records;
  records.reserve(request.chunks.size());
  std::uint64_t record_bytes{};
  std::uint64_t metadata_bytes{};
  if (!checked_multiply(
          request.chunks.size(), kTsumugiChunkRecordSize, record_bytes) ||
      !checked_add(
          kTsumugiMetadataHeaderSize,
          request.manifest.size(),
          metadata_bytes) ||
      !checked_add(metadata_bytes, record_bytes, metadata_bytes) ||
      metadata_bytes > kTsumugiMaximumMetadataBytes) {
    return clonecore::Result<std::vector<std::byte>>::failure(argument_error(
        L"Tsumugi v1メタデータ寸法",
        L"マニフェストとチャンク索引が固定上限を超えています"));
  }
  const std::uint64_t data_start =
      static_cast<std::uint64_t>(kTsumugiHeaderSize) + metadata_bytes;
  bool has_unreadable = false;
  for (std::size_t index = 0; index < request.chunks.size(); ++index) {
    const auto& chunk = request.chunks[index];
    TsumugiChunkRecord record{};
    record.logical_offset = chunk.logical_offset;
    record.logical_length = chunk.logical_length;
    record.stored_offset = data_start + stored_data.size();
    record.flags = chunk.flags;
    record.rescue_read_evidence = chunk.rescue_read_evidence;
    has_unreadable = has_unreadable || chunk_is_unreadable(chunk.flags);
    auto digest = chunk_is_zero(chunk.flags)
        ? sha256_zeroes(chunk.logical_length)
        : sha256(chunk.data);
    if (!digest) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          digest.error());
    }
    record.plaintext_hash = digest.value();
    if (chunk_is_zero(chunk.flags)) {
      records.push_back(record);
      continue;
    }

    std::vector<std::byte> stored = chunk.data;
    if (request.compression == ImageCompression::zstandard) {
      auto compressed = compress_zstandard_image_chunk_v1(chunk.data);
      if (!compressed) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            compressed.error());
      }
      if (compressed.value().size() < stored.size()) {
        stored = compressed.take_value();
        record.compression = ImageCompression::zstandard;
      }
    }
    record.stored_length = stored.size();
    if (encrypted) {
      record.nonce_counter = index + 1U;
      const auto nonce = nonce_for_counter(
          request.encryption->base_nonce, record.nonce_counter);
      if (!nonce) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            nonce.error());
      }
      const auto aad = build_chunk_aad(request.image_id, index, record);
      auto encrypted_chunk = encrypt_tsumugi_aes256_gcm(
          *key, nonce.value(), aad, stored);
      if (!encrypted_chunk) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            encrypted_chunk.error());
      }
      stored = std::move(encrypted_chunk.value().ciphertext);
      record.authentication_tag = encrypted_chunk.value().tag;
    }
    stored_data.insert(stored_data.end(), stored.begin(), stored.end());
    records.push_back(record);
  }

  auto metadata = build_metadata(request.manifest, records);
  if (!metadata) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        metadata.error());
  }

  TsumugiHeader header{};
  header.major_version = kTsumugiMajorVersion;
  header.minor_version = kTsumugiMinorVersion;
  header.required_features =
      (encrypted ? kFeatureEncrypted : 0U) |
      (has_unreadable ? kFeatureUnreadableMap : 0U) |
      (has_unreadable ? kFeatureRescueReadEvidence : 0U);
  header.payload_kind = request.payload_kind;
  header.compression = request.compression;
  header.source_disk_size = request.source_disk_size;
  header.logical_sector_size = request.logical_sector_size;
  header.physical_sector_size = request.physical_sector_size;
  header.chunk_size = request.chunk_size;
  header.chunk_count = request.chunks.size();
  header.metadata = ImageSection{
      .offset = kTsumugiHeaderSize,
      .length = metadata.value().size(),
  };
  header.data = ImageSection{
      .offset = header.metadata.offset + header.metadata.length,
      .length = stored_data.size(),
  };
  header.footer = ImageSection{
      .offset = header.data.offset + header.data.length,
      .length = kTsumugiFooterSize,
  };
  header.image_id = request.image_id;
  if (encrypted) {
    header.argon2 = request.encryption->argon2;
    header.base_nonce = request.encryption->base_nonce;
  } else {
    header.argon2 = TsumugiArgon2Parameters{
        .memory_kib = 0U,
        .iterations = 0U,
        .parallelism = 0U,
        .salt = {},
    };
  }

  auto header_bytes = serialize_header(header);
  if (encrypted) {
    const auto nonce = nonce_for_counter(header.base_nonce, 0U);
    if (!nonce) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          nonce.error());
    }
    const auto aad = canonical_metadata_aad(header_bytes);
    auto encrypted_metadata = encrypt_tsumugi_aes256_gcm(
        *key, nonce.value(), aad, metadata.value());
    if (!encrypted_metadata) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          encrypted_metadata.error());
    }
    metadata.value() =
        std::move(encrypted_metadata.value().ciphertext);
    header.metadata_tag = encrypted_metadata.value().tag;
    header_bytes = serialize_header(header);
  }
  auto header_hash = calculate_header_hash(header_bytes);
  if (!header_hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        header_hash.error());
  }
  header.header_hash = header_hash.value();
  header_bytes = serialize_header(header);

  std::uint64_t total_length{};
  if (!checked_add(header.footer.offset, kTsumugiFooterSize, total_length) ||
      total_length > (std::numeric_limits<std::size_t>::max)()) {
    return clonecore::Result<std::vector<std::byte>>::failure(argument_error(
        L"Tsumugi v1画像寸法",
        L"完成画像がプロセスのアドレス上限を超えます"));
  }
  std::vector<std::byte> image;
  image.reserve(static_cast<std::size_t>(total_length));
  image.insert(image.end(), header_bytes.begin(), header_bytes.end());
  image.insert(image.end(), metadata.value().begin(), metadata.value().end());
  image.insert(image.end(), stored_data.begin(), stored_data.end());
  auto global_hash = sha256(image);
  if (!global_hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        global_hash.error());
  }
  std::array<std::byte, kTsumugiFooterSize> footer{};
  auto footer_bytes = std::span<std::byte>(footer);
  std::copy(kFooterMagic.begin(), kFooterMagic.end(), footer_bytes.begin());
  write_little(footer_bytes, 8U, total_length);
  std::copy(
      global_hash.value().begin(),
      global_hash.value().end(),
      footer_bytes.begin() + 16U);
  std::copy(
      request.image_id.begin(),
      request.image_id.end(),
      footer_bytes.begin() + 48U);
  image.insert(image.end(), footer.begin(), footer.end());
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(image));
}

clonecore::Result<TsumugiInspection> inspect_tsumugi_v1(
    const std::span<const std::byte> image,
    const TsumugiInspectRequest& request) {
  auto header_result = parse_header(image);
  if (!header_result) {
    return clonecore::Result<TsumugiInspection>::failure(
        header_result.error());
  }
  TsumugiHeader header = header_result.take_value();
  const auto sections = validate_sections(header, image.size());
  if (!sections) {
    return clonecore::Result<TsumugiInspection>::failure(sections.error());
  }

  const auto footer = image.subspan(
      static_cast<std::size_t>(header.footer.offset),
      kTsumugiFooterSize);
  if (!std::equal(kFooterMagic.begin(), kFooterMagic.end(), footer.begin()) ||
      read_little<std::uint64_t>(footer, 8U) != image.size() ||
      !std::equal(
          header.image_id.begin(),
          header.image_id.end(),
          footer.begin() + 48U)) {
    return clonecore::Result<TsumugiInspection>::failure(data_error(
        L"Tsumugi v1フッター",
        L"フッターのマジック、全長、または画像識別子が一致しません"));
  }
  Sha256Digest stored_global_hash{};
  std::copy_n(
      footer.begin() + 16U,
      stored_global_hash.size(),
      stored_global_hash.begin());
  auto global_hash = sha256(image.first(
      static_cast<std::size_t>(header.footer.offset)));
  if (!global_hash || global_hash.value() != stored_global_hash) {
    return clonecore::Result<TsumugiInspection>::failure(
        global_hash ? verification_error(
                          L"Tsumugi v1全体Hash",
                          L"画像全体が破損または変更されています")
                    : global_hash.error());
  }

  const bool encrypted =
      (header.required_features & kFeatureEncrypted) != 0U;
  std::optional<TsumugiKey> key;
  std::vector<std::byte> metadata(
      image.begin() + static_cast<std::size_t>(header.metadata.offset),
      image.begin() + static_cast<std::size_t>(
          header.metadata.offset + header.metadata.length));
  if (encrypted) {
    if (!request.password.has_value()) {
      return clonecore::Result<TsumugiInspection>::failure(
          clonecore::Error{
              .code = clonecore::ErrorCode::access_denied,
              .native_code = ERROR_PASSWORD_RESTRICTION,
              .operation = L"Tsumugi暗号画像のパスワード",
              .message = L"暗号化された画像を開くにはパスワードが必要です",
          });
    }
    auto derived = derive_tsumugi_key_argon2id(
        request.password.value(), header.argon2);
    if (!derived) {
      return clonecore::Result<TsumugiInspection>::failure(
          derived.error());
    }
    key.emplace(derived.take_value());
    const auto nonce = nonce_for_counter(header.base_nonce, 0U);
    if (!nonce) {
      return clonecore::Result<TsumugiInspection>::failure(nonce.error());
    }
    const auto aad = canonical_metadata_aad(image.first(kTsumugiHeaderSize));
    auto plaintext = decrypt_tsumugi_aes256_gcm(
        *key,
        nonce.value(),
        aad,
        metadata,
        header.metadata_tag);
    if (!plaintext) {
      return clonecore::Result<TsumugiInspection>::failure(
          plaintext.error());
    }
    metadata = plaintext.take_value();
  }

  auto parsed = parse_metadata(metadata, header, encrypted);
  if (!parsed) {
    return clonecore::Result<TsumugiInspection>::failure(parsed.error());
  }
  TsumugiInspection inspection{};
  inspection.header = header;
  inspection.manifest = std::move(parsed.value().manifest);
  inspection.chunks.reserve(parsed.value().records.size());
  for (std::size_t index = 0;
       index < parsed.value().records.size();
       ++index) {
    const auto& record = parsed.value().records[index];
    if (chunk_is_zero(record.flags)) {
      auto digest = sha256_zeroes(record.logical_length);
      if (!digest || digest.value() != record.plaintext_hash) {
        return clonecore::Result<TsumugiInspection>::failure(
            digest ? verification_error(
                         L"Tsumugi v1ゼロチャンクHash",
                         L"ゼロ埋め範囲のHashが一致しません")
                   : digest.error());
      }
      inspection.chunks.push_back(TsumugiVerifiedChunk{
          .record = record,
          .plaintext = {},
      });
      continue;
    }

    std::vector<std::byte> stored(
        image.begin() + static_cast<std::size_t>(record.stored_offset),
        image.begin() + static_cast<std::size_t>(
            record.stored_offset + record.stored_length));
    if (encrypted) {
      const auto nonce = nonce_for_counter(
          header.base_nonce, record.nonce_counter);
      if (!nonce) {
        return clonecore::Result<TsumugiInspection>::failure(nonce.error());
      }
      const auto aad = build_chunk_aad(header.image_id, index, record);
      auto decrypted = decrypt_tsumugi_aes256_gcm(
          *key,
          nonce.value(),
          aad,
          stored,
          record.authentication_tag);
      if (!decrypted) {
        return clonecore::Result<TsumugiInspection>::failure(
            decrypted.error());
      }
      stored = decrypted.take_value();
    }

    std::vector<std::byte> plaintext;
    if (record.compression == ImageCompression::zstandard) {
      auto decompressed = decompress_zstandard_image_chunk_v1(
          stored, static_cast<std::size_t>(record.logical_length));
      if (!decompressed) {
        return clonecore::Result<TsumugiInspection>::failure(
            decompressed.error());
      }
      plaintext = decompressed.take_value();
    } else {
      if (stored.size() != record.logical_length) {
        return clonecore::Result<TsumugiInspection>::failure(data_error(
            L"Tsumugi v1非圧縮チャンク長",
            L"非圧縮データ長が論理チャンク長と一致しません"));
      }
      plaintext = std::move(stored);
    }
    auto digest = sha256(plaintext);
    if (!digest || digest.value() != record.plaintext_hash) {
      if (!plaintext.empty()) {
        SecureZeroMemory(plaintext.data(), plaintext.size());
      }
      return clonecore::Result<TsumugiInspection>::failure(
          digest ? verification_error(
                       L"Tsumugi v1チャンクHash",
                       L"復号・展開後のチャンクHashが一致しません")
                 : digest.error());
    }
    inspection.chunks.push_back(TsumugiVerifiedChunk{
        .record = record,
        .plaintext = std::move(plaintext),
    });
  }
  inspection.global_hash = global_hash.value();
  inspection.header_hash_verified = true;
  inspection.metadata_authenticated = encrypted;
  inspection.all_chunks_verified = true;
  inspection.global_hash_verified = true;
  return clonecore::Result<TsumugiInspection>::success(
      std::move(inspection));
}

}  // namespace ytec::imageformat

#include "ytec/imageformat/tsumugi_stream.h"
#include "ytec/imageformat/tsumugi_create_resume.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/compression.h"

#include <Windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
constexpr std::size_t kMaximumPathCharacters = 32U * 1024U;
constexpr std::size_t kMaximumIoBlock = 32U * 1024U * 1024U;

clonecore::Error stream_error(
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

clonecore::Error argument_error(
    std::wstring operation,
    std::wstring message) {
  return stream_error(
      clonecore::ErrorCode::invalid_argument,
      ERROR_INVALID_PARAMETER,
      std::move(operation),
      std::move(message));
}

clonecore::Error data_error(
    std::wstring operation,
    std::wstring message) {
  return stream_error(
      clonecore::ErrorCode::invalid_data,
      ERROR_INVALID_DATA,
      std::move(operation),
      std::move(message));
}

clonecore::Error verification_error(
    std::wstring operation,
    std::wstring message) {
  return stream_error(
      clonecore::ErrorCode::verification_failed,
      ERROR_CRC,
      std::move(operation),
      std::move(message));
}

clonecore::Error unsupported_error(
    std::wstring operation,
    std::wstring message) {
  return stream_error(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      std::move(operation),
      std::move(message));
}

clonecore::Error cancelled_error(const std::wstring_view operation) {
  return stream_error(
      clonecore::ErrorCode::cancelled,
      ERROR_CANCELLED,
      std::wstring(operation),
      L"処理は安全に取り消されました");
}

clonecore::Status stop_at_safe_boundary(
    const clonecore::DiskOperationCallbacks& callbacks,
    const clonecore::DiskOperationSafeBoundary& boundary,
    const std::wstring_view operation) noexcept {
  if (clonecore::disk_operation_control_at_safe_boundary(
          callbacks, boundary) ==
      clonecore::DiskOperationControlDecision::cancel_operation) {
    return clonecore::Status::failure(cancelled_error(operation));
  }
  return clonecore::success_status();
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
  return (raw_flags(flags) & 1U) != 0U;
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
  const std::uint64_t base_counter =
      read_little<std::uint64_t>(bytes, 4U);
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

std::vector<std::byte> serialize_header(const TsumugiHeader& header) {
  std::vector<std::byte> result(kTsumugiHeaderSize, std::byte{0});
  const auto bytes = std::span<std::byte>(result);
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
  const bool encrypted =
      (header.required_features & kFeatureEncrypted) != 0U;
  write_little(bytes, 112U, header.argon2.memory_kib);
  write_little(bytes, 116U, header.argon2.iterations);
  write_little(bytes, 120U, header.argon2.parallelism);
  write_little(bytes, 124U, encrypted ? kArgon2idKdf : 0U);
  std::copy(
      header.argon2.salt.begin(),
      header.argon2.salt.end(),
      bytes.begin() + 128U);
  std::copy(
      header.base_nonce.begin(),
      header.base_nonce.end(),
      bytes.begin() + 144U);
  std::copy(
      header.image_id.begin(),
      header.image_id.end(),
      bytes.begin() + 156U);
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
  const auto bytes = std::span<std::byte>(metadata);
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
    const auto record_bytes =
        bytes.subspan(offset, kTsumugiChunkRecordSize);
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

bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool is_drive_absolute_path(const std::wstring_view path) noexcept {
  return path.size() >= 3U &&
      std::iswalpha(static_cast<wint_t>(path[0])) != 0 &&
      path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

bool has_forbidden_path_character(const std::wstring_view path) noexcept {
  for (std::size_t index = 2U; index < path.size(); ++index) {
    const wchar_t value = path[index];
    if (value < 32 || value == L':' || value == L'*' || value == L'?' ||
        value == L'"' || value == L'<' || value == L'>' ||
        value == L'|') {
      return true;
    }
  }
  return false;
}

bool has_tsumugi_extension(const std::wstring_view path) noexcept {
  constexpr std::wstring_view kExtension = L".tsumugi";
  return path.size() > kExtension.size() &&
      equals_ordinal_ignore_case(
          path.substr(path.size() - kExtension.size()), kExtension);
}

std::wstring extended_path(const std::wstring_view path) {
  return L"\\\\?\\" + std::wstring(path);
}

clonecore::Result<std::wstring> canonicalize_image_path(
    const std::wstring& requested) {
  if (!is_drive_absolute_path(requested) || requested.empty() ||
      requested.size() >= kMaximumPathCharacters ||
      has_forbidden_path_character(requested) ||
      requested.ends_with(L"\\") || requested.ends_with(L"/") ||
      requested.ends_with(L" ") || requested.ends_with(L".")) {
    return clonecore::Result<std::wstring>::failure(argument_error(
        L"Tsumugiファイルパス検証",
        L"ローカルドライブ上の絶対.tsumugiパスを指定してください"));
  }
  std::vector<wchar_t> buffer(kMaximumPathCharacters, L'\0');
  const DWORD length = GetFullPathNameW(
      requested.c_str(),
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::invalid_argument,
            L"Tsumugiファイル絶対パス化",
            length == 0U ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  std::wstring canonical(buffer.data(), length);
  std::replace(canonical.begin(), canonical.end(), L'/', L'\\');
  if (!is_drive_absolute_path(canonical) ||
      !has_tsumugi_extension(canonical) ||
      has_forbidden_path_character(canonical)) {
    return clonecore::Result<std::wstring>::failure(argument_error(
        L"Tsumugiファイルパス検証",
        L"ローカルドライブ上の絶対.tsumugiパスを指定してください"));
  }
  return clonecore::Result<std::wstring>::success(std::move(canonical));
}

clonecore::Status reject_reparse_path(const std::wstring& canonical) {
  const std::size_t separator = canonical.find_last_of(L'\\');
  if (separator == std::wstring::npos || separator < 2U) {
    return clonecore::Status::failure(argument_error(
        L"Tsumugi親ディレクトリ検証",
        L"保存先の親ディレクトリを特定できません"));
  }
  const std::wstring parent = canonical.substr(0U, separator);
  std::size_t end = 3U;
  for (;;) {
    while (end < parent.size() && parent[end] != L'\\') {
      ++end;
    }
    std::wstring component = parent.substr(0U, end);
    if (component.size() == 2U) {
      component.push_back(L'\\');
    }
    const DWORD attributes =
        GetFileAttributesW(extended_path(component).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"Tsumugi親ディレクトリ属性取得",
          GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return clonecore::Status::failure(unsupported_error(
          L"Tsumugi保存先reparse検証",
          L"reparse pointを経由する保存先は使用できません"));
    }
    if (end >= parent.size()) {
      break;
    }
    ++end;
  }
  return clonecore::success_status();
}

clonecore::Status require_supported_file_system(
    const std::wstring& canonical) {
  std::array<wchar_t, MAX_PATH + 1U> root{};
  if (!GetVolumePathNameW(
          extended_path(canonical).c_str(),
          root.data(),
          static_cast<DWORD>(root.size()))) {
    // GetVolumePathNameW does not accept every extended path spelling. Retry
    // with the already-canonical DOS path while retaining the local-drive gate.
    if (!GetVolumePathNameW(
            canonical.c_str(),
            root.data(),
            static_cast<DWORD>(root.size()))) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"Tsumugi保存先ファイルシステム取得",
          GetLastError()));
    }
  }
  std::array<wchar_t, 32U> file_system{};
  if (!GetVolumeInformationW(
          root.data(),
          nullptr,
          0U,
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"Tsumugi保存先ファイルシステム取得",
        GetLastError()));
  }
  if (!equals_ordinal_ignore_case(file_system.data(), L"NTFS") &&
      !equals_ordinal_ignore_case(file_system.data(), L"exFAT")) {
    return clonecore::Status::failure(unsupported_error(
        L"Tsumugi保存先ファイルシステム",
        L"単一.tsumugiファイルはNTFSまたはexFATだけに保存できます"));
  }
  return clonecore::success_status();
}

struct FileIdentity final {
  std::uint64_t volume_serial{};
  std::array<std::byte, 16> file_id{};
  std::uint64_t size{};
  FILETIME last_write{};
};

bool same_file_id(
    const FileIdentity& left,
    const FileIdentity& right) noexcept {
  return left.volume_serial == right.volume_serial &&
      left.file_id == right.file_id;
}

bool same_observation(
    const FileIdentity& left,
    const FileIdentity& right) noexcept {
  return same_file_id(left, right) && left.size == right.size &&
      CompareFileTime(&left.last_write, &right.last_write) == 0;
}

TsumugiStreamInspection::OpenedFileObservationV1 public_observation(
    const FileIdentity& identity) noexcept {
  return {
      .volume_serial = identity.volume_serial,
      .file_id = identity.file_id,
      .size = identity.size,
      .last_write_time =
          (static_cast<std::uint64_t>(identity.last_write.dwHighDateTime)
           << 32U) |
          identity.last_write.dwLowDateTime,
      .identity_from_open_handle = true,
  };
}

clonecore::Result<FileIdentity> identity_from_handle(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ID_INFO id{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(
          handle, FileIdInfo, &id, sizeof(id)) ||
      !GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileInformationByHandleEx(
          handle, FileBasicInfo, &basic, sizeof(basic))) {
    return clonecore::Result<FileIdentity>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  if (standard.EndOfFile.QuadPart < 0) {
    return clonecore::Result<FileIdentity>::failure(data_error(
        std::wstring(operation), L"ファイル長が負数です"));
  }
  FileIdentity result{};
  result.volume_serial = id.VolumeSerialNumber;
  std::memcpy(
      result.file_id.data(), id.FileId.Identifier, result.file_id.size());
  result.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
  result.last_write.dwLowDateTime = basic.LastWriteTime.LowPart;
  result.last_write.dwHighDateTime =
      static_cast<DWORD>(basic.LastWriteTime.HighPart);
  return clonecore::Result<FileIdentity>::success(result);
}

struct PathObservation final {
  bool exists{};
  std::optional<FileIdentity> identity;
};

struct LockedPathObservation final {
  PathObservation observation;
  clonecore::UniqueHandle handle;
};

clonecore::Result<PathObservation> observe_path(
    const std::wstring& canonical,
    const std::wstring_view operation) {
  clonecore::UniqueHandle handle(CreateFileW(
      extended_path(canonical).c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<PathObservation>::success(PathObservation{});
    }
    return clonecore::Result<PathObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed, operation, error));
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(
          handle.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
    return clonecore::Result<PathObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed, operation, GetLastError()));
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Result<PathObservation>::failure(unsupported_error(
        std::wstring(operation),
        L"ディレクトリまたはreparse pointは画像ファイルに使用できません"));
  }
  auto identity = identity_from_handle(handle.get(), operation);
  if (!identity) {
    return clonecore::Result<PathObservation>::failure(identity.error());
  }
  return clonecore::Result<PathObservation>::success(PathObservation{
      .exists = true,
      .identity = identity.take_value(),
  });
}

clonecore::Result<LockedPathObservation> lock_final_path_for_replacement(
    const std::wstring& canonical,
    const std::wstring_view operation) {
  // Hold deletion access without write/delete sharing for the entire staging
  // interval. Readers must share delete while replacement is pending; this
  // deliberate exclusivity keeps the reviewed final object immutable and lets
  // the same handle perform the recoverable rename at commit.
  clonecore::UniqueHandle handle(CreateFileW(
      extended_path(canonical).c_str(),
      DELETE | FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return clonecore::Result<LockedPathObservation>::success(
          LockedPathObservation{});
    }
    const clonecore::ErrorCode code =
        error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
            error == ERROR_LOCK_VIOLATION
        ? clonecore::ErrorCode::access_denied
        : clonecore::ErrorCode::query_failed;
    return clonecore::Result<LockedPathObservation>::failure(
        clonecore::make_win32_error(code, operation, error));
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(
          handle.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
    return clonecore::Result<LockedPathObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed, operation, GetLastError()));
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Result<LockedPathObservation>::failure(
        unsupported_error(
            std::wstring(operation),
            L"ディレクトリまたはreparse pointは画像ファイルに使用できません"));
  }
  auto identity = identity_from_handle(handle.get(), operation);
  if (!identity) {
    return clonecore::Result<LockedPathObservation>::failure(
        identity.error());
  }
  return clonecore::Result<LockedPathObservation>::success(
      LockedPathObservation{
          .observation = PathObservation{
              .exists = true,
              .identity = identity.take_value(),
          },
          .handle = std::move(handle),
      });
}

clonecore::Status seek_file(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::wstring_view operation) {
  if (offset > static_cast<std::uint64_t>(
                   (std::numeric_limits<LONGLONG>::max)())) {
    return clonecore::Status::failure(argument_error(
        std::wstring(operation), L"ファイル位置がWindows上限を超えます"));
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed, operation, GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Status write_exact(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes,
    const std::wstring_view operation) {
  auto status = seek_file(handle, offset, operation);
  if (!status) {
    return status;
  }
  std::size_t completed = 0U;
  while (completed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - completed,
        (std::numeric_limits<DWORD>::max)()));
    DWORD written = 0U;
    if (!WriteFile(
            handle,
            bytes.data() + completed,
            amount,
            &written,
            nullptr) ||
        written != amount) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          operation,
          written == amount ? GetLastError() : ERROR_WRITE_FAULT));
    }
    completed += written;
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> read_exact(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::size_t length,
    const std::wstring_view operation) {
  auto status = seek_file(handle, offset, operation);
  if (!status) {
    return clonecore::Result<std::vector<std::byte>>::failure(status.error());
  }
  std::vector<std::byte> bytes(length);
  std::size_t completed = 0U;
  while (completed < bytes.size()) {
    const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - completed,
        (std::numeric_limits<DWORD>::max)()));
    DWORD read = 0U;
    if (!ReadFile(
            handle,
            bytes.data() + completed,
            amount,
            &read,
            nullptr)) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              operation,
              GetLastError()));
    }
    if (read != amount) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          verification_error(
              std::wstring(operation),
              L"ファイルが途中で終了したか要求長と一致しません"));
    }
    completed += read;
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

clonecore::Status set_file_length(
    const HANDLE handle,
    const std::uint64_t length) {
  auto status = seek_file(handle, length, L"Tsumugi未完了ファイル長設定");
  if (!status) {
    return status;
  }
  if (!SetEndOfFile(handle)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"Tsumugi未完了ファイル長設定",
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Status validate_verification_block(
    const std::size_t block_bytes) {
  if (block_bytes == 0U || block_bytes > kMaximumIoBlock) {
    return clonecore::Status::failure(argument_error(
        L"Tsumugi検証ブロック寸法",
        L"検証ブロックは1～32MiBで指定してください"));
  }
  return clonecore::success_status();
}

struct ValidatedBuild final {
  std::wstring canonical_final;
  std::wstring partial_path;
  std::wstring recovery_path;
  PathObservation initial_final;
  clonecore::UniqueHandle locked_final_handle;
  std::uint64_t metadata_length{};
  std::uint64_t maximum_image_length{};
  std::uint64_t logical_payload_bytes{};
  bool has_unreadable{};
};

class OwnedPartial final {
 public:
  OwnedPartial() = default;
  ~OwnedPartial();
  OwnedPartial(const OwnedPartial&) = delete;
  OwnedPartial& operator=(const OwnedPartial&) = delete;
  OwnedPartial(OwnedPartial&& other) noexcept;
  OwnedPartial& operator=(OwnedPartial&&) = delete;

  [[nodiscard]] clonecore::Status create(const std::wstring& path);
  [[nodiscard]] clonecore::Status open_existing(const std::wstring& path);
  [[nodiscard]] HANDLE get() const noexcept;
  [[nodiscard]] const FileIdentity& identity() const noexcept;
  [[nodiscard]] clonecore::Status flush() const;
  void release_after_rename() noexcept;
  [[nodiscard]] clonecore::Status cleanup() noexcept;

 private:
  std::wstring path_;
  clonecore::UniqueHandle handle_;
  FileIdentity identity_{};
  bool owns_{};
};

struct CommitResult final {
  bool replaced_existing{};
  std::wstring retained_recovery_path;
};

struct VerifiedHandle final {
  TsumugiStreamInspection inspection;
  std::optional<TsumugiKey> key;
  std::uint64_t image_length{};
};

clonecore::Result<std::vector<std::byte>> decode_chunk(
    HANDLE handle,
    const TsumugiHeader& header,
    const TsumugiChunkRecord& record,
    std::uint64_t index,
    const TsumugiKey* key);

clonecore::Result<ValidatedBuild> validate_build_request(
    const TsumugiStreamBuildRequest& request,
    bool allow_existing_owned_partial = false);

clonecore::Result<CommitResult> commit_partial(
    OwnedPartial& partial,
    ValidatedBuild& validated);

clonecore::Result<VerifiedHandle> verify_open_handle(
    HANDLE handle,
    std::uint64_t image_length,
    std::optional<std::string_view> password,
    std::size_t block_bytes,
    const clonecore::DiskOperationCallbacks& callbacks);

clonecore::Status verify_immediate_chunk_read_back(
    const std::span<const std::byte> stored_read_back,
    const TsumugiChunkRecord& record,
    const std::uint64_t chunk_index,
    const std::array<std::byte, 16U>& image_id,
    const TsumugiEncryptionSettings* const encryption,
    const TsumugiKey* const encryption_key) {
  const auto wipe = [](std::vector<std::byte>& bytes) noexcept {
    if (!bytes.empty()) {
      SecureZeroMemory(bytes.data(), bytes.size());
    }
  };

  std::vector<std::byte> decoded(
      stored_read_back.begin(), stored_read_back.end());
  if ((encryption == nullptr) != (encryption_key == nullptr)) {
    wipe(decoded);
    return clonecore::Status::failure(stream_error(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_STATE,
        L"Tsumugiチャンク即時認証状態",
        L"暗号化設定と保持鍵の状態が一致しません"));
  }

  if (encryption != nullptr) {
    if (chunk_index == (std::numeric_limits<std::uint64_t>::max)() ||
        record.nonce_counter != chunk_index + 1U) {
      wipe(decoded);
      return clonecore::Status::failure(verification_error(
          L"Tsumugiチャンク即時Nonce照合",
          L"チャンク番号と暗号Nonce counterが一致しません"));
    }
    auto nonce = nonce_for_counter(
        encryption->base_nonce, record.nonce_counter);
    if (!nonce) {
      wipe(decoded);
      return clonecore::Status::failure(nonce.error());
    }
    const auto aad = build_chunk_aad(image_id, chunk_index, record);
    auto authenticated = decrypt_tsumugi_aes256_gcm(
        *encryption_key,
        nonce.value(),
        aad,
        decoded,
        record.authentication_tag);
    wipe(decoded);
    if (!authenticated) {
      return clonecore::Status::failure(authenticated.error());
    }
    decoded = authenticated.take_value();
  } else {
    const bool nonzero_tag = std::any_of(
        record.authentication_tag.begin(),
        record.authentication_tag.end(),
        [](const std::byte value) { return value != std::byte{0}; });
    if (record.nonce_counter != 0U || nonzero_tag) {
      wipe(decoded);
      return clonecore::Status::failure(verification_error(
          L"Tsumugiチャンク即時暗号状態",
          L"非暗号チャンクにNonceまたは認証Tagが設定されています"));
    }
  }

  std::vector<std::byte> plaintext;
  switch (record.compression) {
    case ImageCompression::none:
      plaintext = std::move(decoded);
      break;
    case ImageCompression::zstandard: {
      auto expanded = decompress_zstandard_image_chunk_v1(
          decoded, static_cast<std::size_t>(record.logical_length));
      wipe(decoded);
      if (!expanded) {
        return clonecore::Status::failure(expanded.error());
      }
      plaintext = expanded.take_value();
      break;
    }
    default:
      wipe(decoded);
      return clonecore::Status::failure(verification_error(
          L"Tsumugiチャンク即時圧縮方式",
          L"作成時に未対応の圧縮方式が記録されました"));
  }

  if (plaintext.size() != record.logical_length) {
    wipe(plaintext);
    return clonecore::Status::failure(verification_error(
        L"Tsumugiチャンク即時展開長",
        L"読戻しチャンクの復号・展開後長が計画値と一致しません"));
  }
  auto digest = sha256(plaintext);
  wipe(plaintext);
  if (!digest) {
    return clonecore::Status::failure(digest.error());
  }
  if (digest.value() != record.plaintext_hash) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugiチャンク即時Hash照合",
        L"読戻しチャンクの復号・展開後SHA-256がコピー元と一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status verify_fast_final_metadata(
    HANDLE handle,
    std::uint64_t image_length,
    std::span<const std::byte> expected_header,
    std::span<const std::byte> expected_metadata,
    std::span<const std::byte> expected_footer,
    const Sha256Digest& expected_global_hash,
    const TsumugiKey* encryption_key,
    const TsumugiStreamBuildRequest& request);

clonecore::Result<TsumugiHeader> parse_header_bytes(
    std::span<const std::byte> bytes);

clonecore::Status validate_sections(
    const TsumugiHeader& header,
    std::uint64_t image_size);

clonecore::Result<std::pair<clonecore::UniqueHandle, FileIdentity>>
open_input_file(const std::wstring& canonical);

template <typename T>
clonecore::Result<T> fail_and_cleanup(
    clonecore::Error primary,
    OwnedPartial& partial) {
  const auto cleanup = partial.cleanup();
  if (!cleanup) {
    return clonecore::Result<T>::failure(stream_error(
        cleanup.error().code,
        cleanup.error().native_code,
        L"Tsumugi未完了出力破棄",
        L"処理失敗後の.partial破棄にも失敗しました: " +
            cleanup.error().message));
  }
  return clonecore::Result<T>::failure(std::move(primary));
}

}  // namespace
}  // namespace ytec::imageformat

namespace ytec::imageformat {
namespace {

constexpr std::array<std::byte, 8U> kResumeJournalMagic{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'R'}, std::byte{'J'},
    std::byte{'N'}, std::byte{'L'}, std::byte{'1'}, std::byte{0}};
constexpr std::array<std::byte, 8U> kResumeJournalFrameMagic{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'R'}, std::byte{'F'},
    std::byte{'R'}, std::byte{'M'}, std::byte{'1'}, std::byte{0}};
constexpr std::uint16_t kResumeJournalMajor = 1U;
constexpr std::uint16_t kResumeJournalMinor = 0U;
constexpr std::uint32_t kResumeJournalHeaderBytes = 512U;
constexpr std::uint32_t kResumeJournalFrameBytes = 240U;
constexpr std::uint32_t kResumeJournalEncrypted = 0x01U;
constexpr std::size_t kResumeJournalChainRootOffset = 400U;
constexpr std::size_t kResumeJournalHeaderHashOffset = 432U;
constexpr std::size_t kResumeJournalFrameChainOffset = 200U;
constexpr std::string_view kResumeJournalRootDomain =
    "YTEC-TSUMUGI-CREATE-RESUME-ROOT-V1";
constexpr std::string_view kResumeJournalHeaderDomain =
    "YTEC-TSUMUGI-CREATE-RESUME-HEADER-V1";
constexpr std::string_view kResumeJournalFrameDomain =
    "YTEC-TSUMUGI-CREATE-RESUME-FRAME-V1";
constexpr std::string_view kResumeJournalEmptyDomain =
    "YTEC-TSUMUGI-CREATE-RESUME-EMPTY-V1";

clonecore::Result<Sha256Digest> resume_domain_hash(
    const std::string_view domain,
    const std::span<const std::byte> bytes) {
  std::vector<std::byte> canonical;
  canonical.reserve(domain.size() + bytes.size());
  for (const char value : domain) {
    canonical.push_back(static_cast<std::byte>(value));
  }
  canonical.insert(canonical.end(), bytes.begin(), bytes.end());
  return sha256(canonical);
}

std::array<std::byte, kTsumugiChunkRecordSize> serialize_resume_record(
    const TsumugiChunkRecord& record) noexcept {
  std::array<std::byte, kTsumugiChunkRecordSize> result{};
  const auto bytes = std::span<std::byte>(result);
  write_little(bytes, 0U, record.logical_offset);
  write_little(bytes, 8U, record.logical_length);
  write_little(bytes, 16U, record.stored_offset);
  write_little(bytes, 24U, record.stored_length);
  write_little(bytes, 32U, raw_flags(record.flags));
  write_little(
      bytes, 36U, static_cast<std::uint16_t>(record.compression));
  write_little(bytes, 40U, record.nonce_counter);
  std::copy(
      record.plaintext_hash.begin(),
      record.plaintext_hash.end(),
      bytes.begin() + 48U);
  std::copy(
      record.authentication_tag.begin(),
      record.authentication_tag.end(),
      bytes.begin() + 80U);
  return result;
}

TsumugiChunkRecord parse_resume_record(
    const std::span<const std::byte, kTsumugiChunkRecordSize> bytes) {
  TsumugiChunkRecord record{};
  record.logical_offset = read_little<std::uint64_t>(bytes, 0U);
  record.logical_length = read_little<std::uint64_t>(bytes, 8U);
  record.stored_offset = read_little<std::uint64_t>(bytes, 16U);
  record.stored_length = read_little<std::uint64_t>(bytes, 24U);
  record.flags = static_cast<TsumugiChunkFlags>(
      read_little<std::uint32_t>(bytes, 32U));
  record.compression = static_cast<ImageCompression>(
      read_little<std::uint16_t>(bytes, 36U));
  record.nonce_counter = read_little<std::uint64_t>(bytes, 40U);
  std::copy_n(bytes.begin() + 48U, 32U, record.plaintext_hash.begin());
  std::copy_n(
      bytes.begin() + 80U, 16U, record.authentication_tag.begin());
  return record;
}

clonecore::Result<std::vector<std::byte>> serialize_resume_header(
    TsumugiCreateResumeJournalHeaderV1& header) {
  std::vector<std::byte> bytes(
      kResumeJournalHeaderBytes, std::byte{0});
  auto output = std::span<std::byte>(bytes);
  std::copy(kResumeJournalMagic.begin(), kResumeJournalMagic.end(),
            output.begin());
  write_little(output, 8U, kResumeJournalMajor);
  write_little(output, 10U, kResumeJournalMinor);
  write_little(output, 12U, kResumeJournalHeaderBytes);
  write_little(output, 16U, kEndianMarker);
  write_little(
      output, 20U, header.encrypted ? kResumeJournalEncrypted : 0U);
  write_little(
      output, 24U, static_cast<std::uint16_t>(header.payload_kind));
  write_little(
      output, 26U, static_cast<std::uint16_t>(header.compression));
  write_little(
      output,
      28U,
      static_cast<std::uint8_t>(header.verification_mode));
  write_little(output, 32U, header.source_disk_size);
  write_little(output, 40U, header.expected_logical_bytes);
  write_little(output, 48U, header.chunk_count);
  write_little(output, 56U, std::uint64_t{0U});
  // manifest length is derivable from metadata length and chunk count. The
  // field remains reserved zero in v1 to prevent two competing declarations.
  write_little(output, 64U, header.metadata_length);
  write_little(output, 72U, header.payload_offset);
  write_little(output, 80U, header.logical_sector_size);
  write_little(output, 84U, header.physical_sector_size);
  write_little(output, 88U, header.chunk_size);
  write_little(output, 92U, header.argon2.memory_kib);
  write_little(output, 96U, header.argon2.iterations);
  write_little(output, 100U, header.argon2.parallelism);
  std::copy(header.binding.operation_id.begin(),
            header.binding.operation_id.end(), output.begin() + 104U);
  std::copy(header.binding.plan_hash.begin(), header.binding.plan_hash.end(),
            output.begin() + 120U);
  std::copy(header.binding.source_identity_hash.begin(),
            header.binding.source_identity_hash.end(), output.begin() + 152U);
  std::copy(header.binding.source_state_hash.begin(),
            header.binding.source_state_hash.end(), output.begin() + 184U);
  std::copy(header.binding.destination_storage_identity_hash.begin(),
            header.binding.destination_storage_identity_hash.end(),
            output.begin() + 216U);
  std::copy(header.binding.output_identity_hash.begin(),
            header.binding.output_identity_hash.end(), output.begin() + 248U);
  std::copy(header.final_path_hash.begin(), header.final_path_hash.end(),
            output.begin() + 280U);
  std::copy(header.manifest_hash.begin(), header.manifest_hash.end(),
            output.begin() + 312U);
  std::copy(header.image_id.begin(), header.image_id.end(),
            output.begin() + 344U);
  std::copy(header.argon2.salt.begin(), header.argon2.salt.end(),
            output.begin() + 360U);
  std::copy(header.base_nonce.begin(), header.base_nonce.end(),
            output.begin() + 376U);

  auto root = resume_domain_hash(kResumeJournalRootDomain, bytes);
  if (!root) {
    return clonecore::Result<std::vector<std::byte>>::failure(root.error());
  }
  header.chain_root = root.value();
  std::copy(header.chain_root.begin(), header.chain_root.end(),
            output.begin() + kResumeJournalChainRootOffset);
  auto header_hash = resume_domain_hash(kResumeJournalHeaderDomain, bytes);
  if (!header_hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        header_hash.error());
  }
  std::copy(header_hash.value().begin(), header_hash.value().end(),
            output.begin() + kResumeJournalHeaderHashOffset);
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

clonecore::Result<std::vector<std::byte>> serialize_resume_frame(
    TsumugiCreateResumeJournalChunkV1& chunk) {
  std::vector<std::byte> bytes(kResumeJournalFrameBytes, std::byte{0});
  auto output = std::span<std::byte>(bytes);
  std::copy(kResumeJournalFrameMagic.begin(), kResumeJournalFrameMagic.end(),
            output.begin());
  write_little(output, 8U, kResumeJournalMajor);
  write_little(output, 10U, kResumeJournalMinor);
  write_little(output, 12U, kResumeJournalFrameBytes);
  write_little(output, 16U, chunk.chunk_index);
  const auto record = serialize_resume_record(chunk.record);
  std::copy(record.begin(), record.end(), output.begin() + 24U);
  std::copy(chunk.stored_bytes_hash.begin(), chunk.stored_bytes_hash.end(),
            output.begin() + 136U);
  std::copy(chunk.previous_chain_hash.begin(),
            chunk.previous_chain_hash.end(), output.begin() + 168U);
  auto chain = resume_domain_hash(kResumeJournalFrameDomain, bytes);
  if (!chain) {
    return clonecore::Result<std::vector<std::byte>>::failure(chain.error());
  }
  chunk.chain_hash = chain.value();
  std::copy(chunk.chain_hash.begin(), chunk.chain_hash.end(),
            output.begin() + kResumeJournalFrameChainOffset);
  return clonecore::Result<std::vector<std::byte>>::success(std::move(bytes));
}

bool resume_binding_valid(
    const TsumugiCreateResumeBindingV1& binding) noexcept {
  return !all_zero(binding.operation_id) && !all_zero(binding.plan_hash) &&
      !all_zero(binding.source_identity_hash) &&
      !all_zero(binding.source_state_hash) &&
      !all_zero(binding.destination_storage_identity_hash) &&
      !all_zero(binding.output_identity_hash);
}

bool resume_phase_known(const TsumugiCreateResumePhaseV1 phase) noexcept {
  return phase == TsumugiCreateResumePhaseV1::preparing ||
      phase == TsumugiCreateResumePhaseV1::prepared ||
      phase == TsumugiCreateResumePhaseV1::commit_ready;
}

}  // namespace

clonecore::Result<TsumugiCreateResumeJournalInspectionV1>
inspect_tsumugi_create_resume_journal_v1(
    const std::span<const std::byte> journal_bytes) {
  try {
  if (journal_bytes.size() < kResumeJournalHeaderBytes ||
      journal_bytes.size() > kTsumugiCreateResumeJournalMaximumBytes) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
        L"Tsumugi resume journal寸法",
        L"journal寸法が固定上限またはheader下限の範囲外です"));
  }
  const auto header_bytes = journal_bytes.first(kResumeJournalHeaderBytes);
  if (!std::equal(kResumeJournalMagic.begin(), kResumeJournalMagic.end(),
                  header_bytes.begin()) ||
      read_little<std::uint16_t>(header_bytes, 8U) != kResumeJournalMajor ||
      read_little<std::uint16_t>(header_bytes, 10U) != kResumeJournalMinor ||
      read_little<std::uint32_t>(header_bytes, 12U) !=
          kResumeJournalHeaderBytes ||
      read_little<std::uint32_t>(header_bytes, 16U) != kEndianMarker) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
        L"Tsumugi resume journal版",
        L"magic、major/minor、header寸法またはendianが未対応です"));
  }
  const std::uint32_t flags =
      read_little<std::uint32_t>(header_bytes, 20U);
  if ((flags & ~kResumeJournalEncrypted) != 0U ||
      std::any_of(header_bytes.begin() + 29U, header_bytes.begin() + 32U,
                  [](const std::byte value) { return value != std::byte{0}; }) ||
      read_little<std::uint64_t>(header_bytes, 56U) != 0U ||
      std::any_of(header_bytes.begin() + 388U, header_bytes.begin() + 400U,
                  [](const std::byte value) { return value != std::byte{0}; }) ||
      std::any_of(header_bytes.begin() + 464U, header_bytes.end(),
                  [](const std::byte value) { return value != std::byte{0}; })) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
        L"Tsumugi resume journal flags",
        L"unknown flagsまたはreserved領域が非zeroです"));
  }

  std::vector<std::byte> canonical_root(header_bytes.begin(),
                                         header_bytes.end());
  std::fill(canonical_root.begin() + kResumeJournalChainRootOffset,
            canonical_root.begin() + kResumeJournalChainRootOffset + 32U,
            std::byte{0});
  std::fill(canonical_root.begin() + kResumeJournalHeaderHashOffset,
            canonical_root.begin() + kResumeJournalHeaderHashOffset + 32U,
            std::byte{0});
  auto calculated_root =
      resume_domain_hash(kResumeJournalRootDomain, canonical_root);
  Sha256Digest stored_root{};
  std::copy_n(header_bytes.begin() + kResumeJournalChainRootOffset, 32U,
              stored_root.begin());
  std::vector<std::byte> canonical_header(header_bytes.begin(),
                                           header_bytes.end());
  std::fill(canonical_header.begin() + kResumeJournalHeaderHashOffset,
            canonical_header.begin() + kResumeJournalHeaderHashOffset + 32U,
            std::byte{0});
  auto calculated_header =
      resume_domain_hash(kResumeJournalHeaderDomain, canonical_header);
  Sha256Digest stored_header{};
  std::copy_n(header_bytes.begin() + kResumeJournalHeaderHashOffset, 32U,
              stored_header.begin());
  if (!calculated_root || !calculated_header ||
      calculated_root.value() != stored_root ||
      calculated_header.value() != stored_header) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(verification_error(
        L"Tsumugi resume journal header Hash",
        L"chain rootまたはheader Hashが一致しません"));
  }

  TsumugiCreateResumeJournalInspectionV1 inspection{};
  auto& header = inspection.header;
  const auto copy_digest = [&](const std::size_t offset,
                               Sha256Digest& output) {
    std::copy_n(header_bytes.begin() +
                    static_cast<std::ptrdiff_t>(offset),
                output.size(), output.begin());
  };
  std::copy_n(header_bytes.begin() + 104U, 16U,
              header.binding.operation_id.begin());
  copy_digest(120U, header.binding.plan_hash);
  copy_digest(152U, header.binding.source_identity_hash);
  copy_digest(184U, header.binding.source_state_hash);
  copy_digest(216U, header.binding.destination_storage_identity_hash);
  copy_digest(248U, header.binding.output_identity_hash);
  copy_digest(280U, header.final_path_hash);
  copy_digest(312U, header.manifest_hash);
  std::copy_n(header_bytes.begin() + 344U, 16U, header.image_id.begin());
  std::copy_n(header_bytes.begin() + 360U, 16U,
              header.argon2.salt.begin());
  std::copy_n(header_bytes.begin() + 376U, kTsumugiGcmNonceBytes,
              header.base_nonce.begin());
  header.chain_root = stored_root;
  header.payload_kind = static_cast<TsumugiPayloadKind>(
      read_little<std::uint16_t>(header_bytes, 24U));
  header.compression = static_cast<ImageCompression>(
      read_little<std::uint16_t>(header_bytes, 26U));
  header.verification_mode = static_cast<TsumugiCreateVerificationMode>(
      read_little<std::uint8_t>(header_bytes, 28U));
  header.source_disk_size = read_little<std::uint64_t>(header_bytes, 32U);
  header.expected_logical_bytes =
      read_little<std::uint64_t>(header_bytes, 40U);
  header.chunk_count = read_little<std::uint64_t>(header_bytes, 48U);
  header.metadata_length = read_little<std::uint64_t>(header_bytes, 64U);
  header.payload_offset = read_little<std::uint64_t>(header_bytes, 72U);
  header.logical_sector_size = read_little<std::uint32_t>(header_bytes, 80U);
  header.physical_sector_size = read_little<std::uint32_t>(header_bytes, 84U);
  header.chunk_size = read_little<std::uint32_t>(header_bytes, 88U);
  header.argon2.memory_kib = read_little<std::uint32_t>(header_bytes, 92U);
  header.argon2.iterations = read_little<std::uint32_t>(header_bytes, 96U);
  header.argon2.parallelism = read_little<std::uint32_t>(header_bytes, 100U);
  header.encrypted = (flags & kResumeJournalEncrypted) != 0U;

  std::uint64_t records_bytes{};
  std::uint64_t expected_payload{};
  if (!resume_binding_valid(header.binding) ||
      all_zero(header.final_path_hash) || all_zero(header.manifest_hash) ||
      all_zero(header.chain_root) || all_zero(header.image_id) ||
      header.payload_kind != TsumugiPayloadKind::exact_disk ||
      !valid_compression(header.compression) ||
      !is_supported_tsumugi_create_verification_mode(
          header.verification_mode) ||
      header.source_disk_size == 0U ||
      header.expected_logical_bytes == 0U ||
      header.expected_logical_bytes > header.source_disk_size ||
      header.chunk_count == 0U ||
      header.chunk_count > kTsumugiCreateResumeJournalMaximumRecords ||
      !is_supported_sector_size_pair(
          header.logical_sector_size, header.physical_sector_size) ||
      !valid_chunk_size(header.chunk_size) ||
      !checked_multiply(
          header.chunk_count, kTsumugiChunkRecordSize, records_bytes) ||
      header.metadata_length < kTsumugiMetadataHeaderSize + records_bytes ||
      header.metadata_length > kTsumugiMaximumMetadataBytes ||
      !checked_add(
          kTsumugiHeaderSize, header.metadata_length, expected_payload) ||
      expected_payload != header.payload_offset ||
      (header.encrypted &&
       (header.argon2.memory_kib != kTsumugiArgon2MemoryKiB ||
        header.argon2.iterations != kTsumugiArgon2Iterations ||
        header.argon2.parallelism != kTsumugiArgon2Parallelism ||
        all_zero(header.argon2.salt) || all_zero(header.base_nonce))) ||
      (!header.encrypted &&
       (header.argon2.memory_kib != 0U ||
        header.argon2.iterations != 0U ||
        header.argon2.parallelism != 0U ||
        !all_zero(header.argon2.salt) || !all_zero(header.base_nonce)))) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
        L"Tsumugi resume journal header構造",
        L"束縛、形式上限、セクション寸法、または非秘密暗号parameterが不正です"));
  }
  const std::size_t frames_bytes =
      journal_bytes.size() - kResumeJournalHeaderBytes;
  if (frames_bytes % kResumeJournalFrameBytes != 0U) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
        L"Tsumugi resume journal frame寸法",
        L"truncateされたか余分なframe byteがあります"));
  }
  const std::uint64_t frame_count =
      frames_bytes / kResumeJournalFrameBytes;
  if (frame_count > header.chunk_count ||
      frame_count > kTsumugiCreateResumeJournalMaximumRecords ||
      frame_count > (std::numeric_limits<std::size_t>::max)()) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
        L"Tsumugi resume journal frame件数",
        L"frame件数が宣言数またはallocation上限を超えます"));
  }
  inspection.chunks.reserve(static_cast<std::size_t>(frame_count));
  auto empty_stored_hash = sha256(std::span<const std::byte>{});
  if (!empty_stored_hash) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(
        empty_stored_hash.error());
  }
  Sha256Digest expected_previous = header.chain_root;
  std::uint64_t expected_stored_offset = header.payload_offset;
  std::uint64_t previous_logical_end{};
  std::uint64_t settled_logical{};
  for (std::uint64_t index = 0U; index < frame_count; ++index) {
    const std::size_t offset = kResumeJournalHeaderBytes +
        static_cast<std::size_t>(index) * kResumeJournalFrameBytes;
    const auto frame = journal_bytes.subspan(offset, kResumeJournalFrameBytes);
    if (!std::equal(kResumeJournalFrameMagic.begin(),
                    kResumeJournalFrameMagic.end(), frame.begin()) ||
        read_little<std::uint16_t>(frame, 8U) != kResumeJournalMajor ||
        read_little<std::uint16_t>(frame, 10U) != kResumeJournalMinor ||
        read_little<std::uint32_t>(frame, 12U) !=
            kResumeJournalFrameBytes ||
        read_little<std::uint64_t>(frame, 16U) != index ||
        !all_zero(frame.subspan(20U, 4U)) ||
        !all_zero(frame.subspan(24U + 38U, 2U)) ||
        !all_zero(frame.subspan(24U + 96U, 16U)) ||
        std::any_of(frame.begin() + 232U, frame.end(),
                    [](const std::byte value) {
                      return value != std::byte{0};
                    })) {
      return clonecore::Result<
          TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
          L"Tsumugi resume journal frame構造",
          L"frame版、寸法、index、reservedまたは一意性が不正です"));
    }
    TsumugiCreateResumeJournalChunkV1 chunk{};
    chunk.chunk_index = index;
    std::array<std::byte, kTsumugiChunkRecordSize> record_bytes{};
    std::copy_n(frame.begin() + 24U, record_bytes.size(),
                record_bytes.begin());
    chunk.record = parse_resume_record(record_bytes);
    std::copy_n(frame.begin() + 136U, 32U,
                chunk.stored_bytes_hash.begin());
    std::copy_n(frame.begin() + 168U, 32U,
                chunk.previous_chain_hash.begin());
    std::copy_n(frame.begin() + 200U, 32U, chunk.chain_hash.begin());
    std::vector<std::byte> canonical_frame(frame.begin(), frame.end());
    std::fill(canonical_frame.begin() + kResumeJournalFrameChainOffset,
              canonical_frame.begin() +
                  kResumeJournalFrameChainOffset + 32U,
              std::byte{0});
    auto calculated_chain =
        resume_domain_hash(kResumeJournalFrameDomain, canonical_frame);
    const bool zero = chunk_is_zero(chunk.record.flags);
    const bool nonzero_tag = std::any_of(
        chunk.record.authentication_tag.begin(),
        chunk.record.authentication_tag.end(),
        [](const std::byte value) { return value != std::byte{0}; });
    std::uint64_t logical_end{};
    std::uint64_t stored_end{};
    if (!calculated_chain ||
        chunk.previous_chain_hash != expected_previous ||
        chunk.chain_hash != calculated_chain.value() ||
        all_zero(chunk.stored_bytes_hash) ||
        all_zero(chunk.record.plaintext_hash) ||
        !valid_chunk_flags(chunk.record.flags) ||
        chunk_is_unreadable(chunk.record.flags) ||
        chunk.record.logical_length == 0U ||
        chunk.record.logical_length > header.chunk_size ||
        chunk.record.logical_offset % header.logical_sector_size != 0U ||
        chunk.record.logical_length % header.logical_sector_size != 0U ||
        !checked_add(chunk.record.logical_offset,
                     chunk.record.logical_length, logical_end) ||
        logical_end > header.source_disk_size ||
        (index != 0U && chunk.record.logical_offset < previous_logical_end) ||
        chunk.record.stored_offset != expected_stored_offset ||
        chunk.record.stored_length > chunk.record.logical_length ||
        chunk.record.stored_length > kTsumugiMaximumCryptBytes ||
        !checked_add(chunk.record.stored_offset,
                     chunk.record.stored_length, stored_end) ||
        (zero && (chunk.record.stored_length != 0U ||
                  chunk.record.compression != ImageCompression::none ||
                  chunk.record.nonce_counter != 0U || nonzero_tag)) ||
        (!zero && (chunk.record.stored_length == 0U ||
                   !valid_compression(chunk.record.compression))) ||
        (header.encrypted && !zero &&
         (chunk.record.nonce_counter != index + 1U || !nonzero_tag)) ||
        (!header.encrypted &&
         (chunk.record.nonce_counter != 0U || nonzero_tag)) ||
        !checked_add(settled_logical,
                     chunk.record.logical_length, settled_logical)) {
      return clonecore::Result<
          TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
          L"Tsumugi resume journal chunk",
          L"chain、record境界、offset、暗号状態、または長さが不正です"));
    }
    if (zero && empty_stored_hash.value() != chunk.stored_bytes_hash) {
      return clonecore::Result<
          TsumugiCreateResumeJournalInspectionV1>::failure(
          verification_error(
              L"Tsumugi resume journal zero格納Hash",
              L"zero chunkの格納Hashが空byte列SHA-256と一致しません"));
    }
    expected_previous = chunk.chain_hash;
    expected_stored_offset = stored_end;
    previous_logical_end = logical_end;
    inspection.chunks.push_back(chunk);
  }
  if (settled_logical > header.expected_logical_bytes ||
      (frame_count == header.chunk_count &&
       settled_logical != header.expected_logical_bytes)) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(data_error(
        L"Tsumugi resume journal進捗",
        L"検証済み論理byteが宣言上限を超えます"));
  }
  inspection.journal_length = journal_bytes.size();
  inspection.header_hash = stored_header;
  inspection.verified_prefix_hash = expected_previous;
  return clonecore::Result<
      TsumugiCreateResumeJournalInspectionV1>::success(
      std::move(inspection));
  } catch (const std::bad_alloc&) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(stream_error(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"Tsumugi resume journal有界parse",
        L"固定上限内のparseメモリを確保できませんでした"));
  } catch (...) {
    return clonecore::Result<
        TsumugiCreateResumeJournalInspectionV1>::failure(stream_error(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"Tsumugi resume journal例外境界",
        L"untrusted journalのparseをfail-closedで停止しました"));
  }
}

}  // namespace ytec::imageformat

namespace ytec::imageformat {

namespace {

constexpr std::string_view kResumeFinalPathDomain =
    "YTEC-TSUMUGI-CREATE-RESUME-FINAL-PATH-V1";

void wipe_resume_bytes(std::vector<std::byte>& bytes) noexcept {
  if (!bytes.empty()) {
    SecureZeroMemory(bytes.data(), bytes.size());
  }
}

template <typename Callback, typename... Arguments>
clonecore::Status invoke_resume_checkpoint_hook(
    const Callback& callback,
    const std::wstring_view operation,
    const Arguments&... arguments) noexcept {
  if (!callback) {
    return clonecore::Status::failure(argument_error(
        std::wstring(operation), L"必須のcheckpoint callbackがありません"));
  }
  try {
    return callback(arguments...);
  } catch (const std::bad_alloc&) {
    return clonecore::Status::failure(stream_error(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        std::wstring(operation),
        L"checkpoint callbackで有界メモリを確保できませんでした"));
  } catch (...) {
    return clonecore::Status::failure(stream_error(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        std::wstring(operation),
        L"checkpoint callbackが例外を返したため安全に停止します"));
  }
}

clonecore::Result<Sha256Digest> hash_resume_final_path(
    const std::wstring& canonical_path) {
  const auto characters = std::span<const wchar_t>(
      canonical_path.data(), canonical_path.size());
  return resume_domain_hash(kResumeFinalPathDomain, std::as_bytes(characters));
}

struct PreparedResumeJournalHeader final {
  TsumugiCreateResumeJournalHeaderV1 header;
  std::vector<std::byte> bytes;
};

clonecore::Result<PreparedResumeJournalHeader>
build_expected_resume_journal_header(
    const TsumugiCreateResumeRequestV1& request,
    const ValidatedBuild& validated) {
  std::uint64_t expected_logical_bytes{};
  for (const auto& chunk : request.stream.chunks) {
    if (!checked_add(
            expected_logical_bytes,
            chunk.logical_length,
            expected_logical_bytes)) {
      return clonecore::Result<PreparedResumeJournalHeader>::failure(
          argument_error(
              L"Tsumugi resume論理長",
              L"チャンク論理長の合計が64bit上限を超えます"));
    }
  }
  std::uint64_t frames_bytes{};
  std::uint64_t journal_bytes{};
  if (request.stream.chunks.empty() ||
      request.stream.chunks.size() >
          kTsumugiCreateResumeJournalMaximumRecords ||
      !checked_multiply(
          request.stream.chunks.size(),
          kResumeJournalFrameBytes,
          frames_bytes) ||
      !checked_add(kResumeJournalHeaderBytes, frames_bytes, journal_bytes) ||
      journal_bytes > kTsumugiCreateResumeJournalMaximumBytes) {
    return clonecore::Result<PreparedResumeJournalHeader>::failure(
        argument_error(
            L"Tsumugi resume journal上限",
            L"通常画像のチャンク数またはjournal寸法が固定上限を超えます"));
  }
  auto final_path_hash = hash_resume_final_path(validated.canonical_final);
  auto manifest_hash = sha256(request.stream.manifest);
  if (!final_path_hash || !manifest_hash) {
    return clonecore::Result<PreparedResumeJournalHeader>::failure(
        final_path_hash ? manifest_hash.error() : final_path_hash.error());
  }
  std::uint64_t payload_offset{};
  if (!checked_add(
          kTsumugiHeaderSize, validated.metadata_length, payload_offset)) {
    return clonecore::Result<PreparedResumeJournalHeader>::failure(
        argument_error(
            L"Tsumugi resume payload offset",
            L"固定領域寸法が64bit上限を超えます"));
  }

  TsumugiCreateResumeJournalHeaderV1 header{};
  header.binding = request.binding;
  header.final_path_hash = final_path_hash.value();
  header.manifest_hash = manifest_hash.value();
  header.payload_kind = request.stream.payload_kind;
  header.compression = request.stream.compression;
  header.verification_mode = request.stream.verification_mode;
  header.source_disk_size = request.stream.source_disk_size;
  header.expected_logical_bytes = expected_logical_bytes;
  header.chunk_count = request.stream.chunks.size();
  header.metadata_length = validated.metadata_length;
  header.payload_offset = payload_offset;
  header.logical_sector_size = request.stream.logical_sector_size;
  header.physical_sector_size = request.stream.physical_sector_size;
  header.chunk_size = request.stream.chunk_size;
  header.image_id = request.stream.image_id;
  header.encrypted = request.stream.encryption.has_value();
  if (request.stream.encryption.has_value()) {
    header.argon2 = request.stream.encryption->argon2;
    header.base_nonce = request.stream.encryption->base_nonce;
  } else {
    header.argon2 = TsumugiArgon2Parameters{
        .memory_kib = 0U,
        .iterations = 0U,
        .parallelism = 0U,
        .salt = {},
    };
    header.base_nonce = {};
  }
  auto bytes = serialize_resume_header(header);
  if (!bytes) {
    return clonecore::Result<PreparedResumeJournalHeader>::failure(
        bytes.error());
  }
  if (all_zero(header.chain_root) || all_zero(header.manifest_hash) ||
      all_zero(header.final_path_hash)) {
    return clonecore::Result<PreparedResumeJournalHeader>::failure(
        verification_error(
            L"Tsumugi resume journal header Hash",
            L"永続束縛Hashがzero値となったため安全に停止します"));
  }
  return clonecore::Result<PreparedResumeJournalHeader>::success(
      PreparedResumeJournalHeader{
          .header = std::move(header),
          .bytes = bytes.take_value(),
      });
}

bool equal_resume_argon2(
    const TsumugiArgon2Parameters& left,
    const TsumugiArgon2Parameters& right) noexcept {
  return left.memory_kib == right.memory_kib &&
      left.iterations == right.iterations &&
      left.parallelism == right.parallelism && left.salt == right.salt;
}

bool equal_resume_journal_headers(
    const TsumugiCreateResumeJournalHeaderV1& left,
    const TsumugiCreateResumeJournalHeaderV1& right) noexcept {
  return left.binding == right.binding &&
      left.final_path_hash == right.final_path_hash &&
      left.manifest_hash == right.manifest_hash &&
      left.chain_root == right.chain_root &&
      left.payload_kind == right.payload_kind &&
      left.compression == right.compression &&
      left.verification_mode == right.verification_mode &&
      left.source_disk_size == right.source_disk_size &&
      left.expected_logical_bytes == right.expected_logical_bytes &&
      left.chunk_count == right.chunk_count &&
      left.metadata_length == right.metadata_length &&
      left.payload_offset == right.payload_offset &&
      left.logical_sector_size == right.logical_sector_size &&
      left.physical_sector_size == right.physical_sector_size &&
      left.chunk_size == right.chunk_size && left.image_id == right.image_id &&
      left.encrypted == right.encrypted &&
      equal_resume_argon2(left.argon2, right.argon2) &&
      left.base_nonce == right.base_nonce;
}

clonecore::Result<std::uint64_t> resume_file_length(
    const HANDLE handle,
    const std::wstring_view operation) {
  auto identity = identity_from_handle(handle, operation);
  if (!identity) {
    return clonecore::Result<std::uint64_t>::failure(identity.error());
  }
  return clonecore::Result<std::uint64_t>::success(identity.value().size);
}

clonecore::Status verify_resume_read_back(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::span<const std::byte> expected,
    const std::wstring_view operation) {
  auto actual = read_exact(handle, offset, expected.size(), operation);
  if (!actual) {
    return clonecore::Status::failure(actual.error());
  }
  if (!std::equal(expected.begin(), expected.end(), actual.value().begin())) {
    return clonecore::Status::failure(verification_error(
        std::wstring(operation), L"flush後の読戻し内容が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status initialize_resume_owned_files(
    OwnedPartial& partial,
    OwnedPartial& journal,
    const PreparedResumeJournalHeader& prepared_header) {
  auto status = set_file_length(partial.get(), 0U);
  if (!status) {
    return status;
  }
  status = set_file_length(journal.get(), 0U);
  if (!status) {
    return status;
  }

  constexpr std::size_t kReservationBlock = 1024U * 1024U;
  const std::vector<std::byte> zeroes(kReservationBlock, std::byte{0});
  std::uint64_t offset{};
  while (offset < prepared_header.header.payload_offset) {
    const std::size_t length = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            prepared_header.header.payload_offset - offset, zeroes.size()));
    status = write_exact(
        partial.get(),
        offset,
        std::span<const std::byte>(zeroes.data(), length),
        L"Tsumugi resume固定領域予約");
    if (!status) {
      return status;
    }
    offset += length;
  }
  status = partial.flush();
  if (!status) {
    return status;
  }
  offset = 0U;
  while (offset < prepared_header.header.payload_offset) {
    const std::size_t length = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            prepared_header.header.payload_offset - offset, zeroes.size()));
    auto actual = read_exact(
        partial.get(), offset, length, L"Tsumugi resume固定領域読戻し");
    if (!actual) {
      return clonecore::Status::failure(actual.error());
    }
    if (!all_zero(actual.value())) {
      return clonecore::Status::failure(verification_error(
          L"Tsumugi resume固定領域読戻し",
          L"予約領域がzeroで読戻せませんでした"));
    }
    offset += length;
  }

  status = write_exact(
      journal.get(),
      0U,
      prepared_header.bytes,
      L"Tsumugi resume journal header書込み");
  if (!status) {
    return status;
  }
  status = journal.flush();
  if (!status) {
    return status;
  }
  status = verify_resume_read_back(
      journal.get(),
      0U,
      prepared_header.bytes,
      L"Tsumugi resume journal header読戻し");
  if (!status) {
    return status;
  }
  auto inspected = inspect_tsumugi_create_resume_journal_v1(
      prepared_header.bytes);
  if (!inspected ||
      !equal_resume_journal_headers(
          inspected.value().header, prepared_header.header)) {
    return inspected
        ? clonecore::Status::failure(verification_error(
              L"Tsumugi resume journal header照合",
              L"読戻しheaderが作成要求と一致しません"))
        : clonecore::Status::failure(inspected.error());
  }
  return clonecore::success_status();
}

template <typename T>
clonecore::Result<T> fail_before_resume_slot_is_durable(
    clonecore::Error primary,
    OwnedPartial& partial,
    OwnedPartial& journal) {
  const auto journal_cleanup = journal.cleanup();
  const auto partial_cleanup = partial.cleanup();
  if (!journal_cleanup || !partial_cleanup) {
    const auto& cleanup =
        journal_cleanup ? partial_cleanup.error() : journal_cleanup.error();
    return clonecore::Result<T>::failure(stream_error(
        cleanup.code,
        cleanup.native_code,
        L"Tsumugi resume初回slot前cleanup",
        L"active checkpoint未確定時のowned object cleanupに失敗しました: " +
            cleanup.message));
  }
  return clonecore::Result<T>::failure(std::move(primary));
}

class ResumeOwnedFilesRetention final {
 public:
  ResumeOwnedFilesRetention(
      OwnedPartial& partial,
      OwnedPartial& journal) noexcept
      : partial_(partial), journal_(journal) {}
  ~ResumeOwnedFilesRetention() {
    if (retain_) {
      journal_.release_after_rename();
      partial_.release_after_rename();
    }
  }

  ResumeOwnedFilesRetention(const ResumeOwnedFilesRetention&) = delete;
  ResumeOwnedFilesRetention& operator=(const ResumeOwnedFilesRetention&) =
      delete;

  void retain() noexcept { retain_ = true; }

 private:
  OwnedPartial& partial_;
  OwnedPartial& journal_;
  bool retain_{};
};

bool resume_record_matches_planned_chunk(
    const TsumugiChunkRecord& record,
    const TsumugiStreamBuildChunk& planned) noexcept {
  return record.logical_offset == planned.logical_offset &&
      record.logical_length == planned.logical_length &&
      record.flags == planned.flags &&
      !record.rescue_read_evidence.has_value() &&
      !planned.rescue_read_evidence.has_value();
}

struct VerifiedResumePrefix final {
  TsumugiCreateResumeJournalInspectionV1 inspection;
  std::uint64_t verified_logical_bytes{};
  std::uint64_t primary_output_length{};
};

clonecore::Result<VerifiedResumePrefix> verify_resume_prefix(
    const TsumugiCreateResumeRequestV1& request,
    const PreparedResumeJournalHeader& prepared_header,
    const std::uint64_t journal_length,
    const HANDLE partial_handle,
    const HANDLE journal_handle,
    const TsumugiKey* const key) {
  if (journal_length < kResumeJournalHeaderBytes ||
      journal_length > kTsumugiCreateResumeJournalMaximumBytes ||
      journal_length > (std::numeric_limits<std::size_t>::max)()) {
    return clonecore::Result<VerifiedResumePrefix>::failure(data_error(
        L"Tsumugi resume journal checkpoint長",
        L"checkpointのjournal長が固定上限外です"));
  }
  auto journal_bytes = read_exact(
      journal_handle,
      0U,
      static_cast<std::size_t>(journal_length),
      L"Tsumugi resume journal有界読取り");
  if (!journal_bytes) {
    return clonecore::Result<VerifiedResumePrefix>::failure(
        journal_bytes.error());
  }
  auto inspected = inspect_tsumugi_create_resume_journal_v1(
      journal_bytes.value());
  if (!inspected) {
    return clonecore::Result<VerifiedResumePrefix>::failure(
        inspected.error());
  }
  if (!equal_resume_journal_headers(
          inspected.value().header, prepared_header.header)) {
    return clonecore::Result<VerifiedResumePrefix>::failure(
        verification_error(
            L"Tsumugi resume journal作成要求照合",
            L"source、plan、出力、形式、暗号parameterまたはmanifestが一致しません"));
  }

  std::uint64_t logical_bytes{};
  std::uint64_t primary_length = prepared_header.header.payload_offset;
  for (std::size_t index = 0U;
       index < inspected.value().chunks.size(); ++index) {
    const auto& chunk = inspected.value().chunks[index];
    if (index >= request.stream.chunks.size() ||
        !resume_record_matches_planned_chunk(
            chunk.record, request.stream.chunks[index])) {
      return clonecore::Result<VerifiedResumePrefix>::failure(
          verification_error(
              L"Tsumugi resume chunk plan照合",
              L"journalの検証済みchunkが現在のplanと一致しません"));
    }
    if (!checked_add(
            logical_bytes,
            chunk.record.logical_length,
            logical_bytes) ||
        !checked_add(
            chunk.record.stored_offset,
            chunk.record.stored_length,
            primary_length)) {
      return clonecore::Result<VerifiedResumePrefix>::failure(data_error(
          L"Tsumugi resume chunk長",
          L"検証済みprefixの長さが64bit上限を超えます"));
    }
    if (chunk_is_zero(chunk.record.flags)) {
      auto stored_hash = sha256(std::span<const std::byte>{});
      auto plaintext_hash = sha256_zeroes(chunk.record.logical_length);
      if (!stored_hash || !plaintext_hash ||
          stored_hash.value() != chunk.stored_bytes_hash ||
          plaintext_hash.value() != chunk.record.plaintext_hash) {
        return clonecore::Result<VerifiedResumePrefix>::failure(
            verification_error(
                L"Tsumugi resume zero chunk Hash",
                L"zero chunkの格納Hashまたは論理Hashが一致しません"));
      }
      continue;
    }
    auto stored = read_exact(
        partial_handle,
        chunk.record.stored_offset,
        static_cast<std::size_t>(chunk.record.stored_length),
        L"Tsumugi resume検証済みpayload読戻し");
    if (!stored) {
      return clonecore::Result<VerifiedResumePrefix>::failure(stored.error());
    }
    auto stored_hash = sha256(stored.value());
    if (!stored_hash || stored_hash.value() != chunk.stored_bytes_hash) {
      wipe_resume_bytes(stored.value());
      return clonecore::Result<VerifiedResumePrefix>::failure(
          stored_hash
              ? verification_error(
                    L"Tsumugi resume格納chunk Hash",
                    L"partial上の格納byteがjournal Hashと一致しません")
              : stored_hash.error());
    }
    const auto verified = verify_immediate_chunk_read_back(
        stored.value(),
        chunk.record,
        index,
        request.stream.image_id,
        request.stream.encryption ? &*request.stream.encryption : nullptr,
        key);
    wipe_resume_bytes(stored.value());
    if (!verified) {
      return clonecore::Result<VerifiedResumePrefix>::failure(
          verified.error());
    }
  }
  return clonecore::Result<VerifiedResumePrefix>::success(
      VerifiedResumePrefix{
          .inspection = inspected.take_value(),
          .verified_logical_bytes = logical_bytes,
          .primary_output_length = primary_length,
      });
}

clonecore::Status validate_preparing_resume_progress(
    const TsumugiCreateResumeProgressV1& progress,
    const PreparedResumeJournalHeader& prepared_header) {
  if (progress.phase != TsumugiCreateResumePhaseV1::preparing ||
      progress.verified_logical_bytes != 0U ||
      progress.verified_chunk_count != 0U ||
      progress.primary_output_length != 0U ||
      progress.journal_length != 0U ||
      progress.verified_prefix_hash != prepared_header.header.chain_root) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi resume preparing checkpoint",
        L"preparing checkpointが現在のexact planと一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_verified_resume_progress(
    const TsumugiCreateResumeProgressV1& progress,
    const VerifiedResumePrefix& verified,
    const TsumugiCreateResumePhaseV1 expected_phase,
    const bool compare_chain_hash) {
  if (progress.phase != expected_phase ||
      progress.verified_chunk_count != verified.inspection.chunks.size() ||
      progress.verified_logical_bytes != verified.verified_logical_bytes ||
      progress.journal_length != verified.inspection.journal_length ||
      (expected_phase == TsumugiCreateResumePhaseV1::prepared &&
       progress.primary_output_length != verified.primary_output_length) ||
      (compare_chain_hash &&
       progress.verified_prefix_hash !=
           verified.inspection.verified_prefix_hash)) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi resume checkpoint prefix照合",
        L"checkpointの件数、長さ、Hashまたはphaseが検証済みprefixと一致しません"));
  }
  return clonecore::success_status();
}

struct ResumeFinalizationResult final {
  TsumugiStreamBuildReport report;
  Sha256Digest global_hash{};
  std::uint64_t final_length{};
};

clonecore::Result<ResumeFinalizationResult> finalize_resume_partial(
    const TsumugiCreateResumeRequestV1& request,
    const ValidatedBuild& validated,
    const TsumugiCreateResumeJournalInspectionV1& journal,
    OwnedPartial& partial,
    const TsumugiKey* const key,
    const clonecore::DiskOperationCallbacks& callbacks) {
  std::vector<TsumugiChunkRecord> records;
  records.reserve(journal.chunks.size());
  std::uint64_t zero_filled_bytes{};
  for (const auto& journal_chunk : journal.chunks) {
    records.push_back(journal_chunk.record);
    if (chunk_is_zero(journal_chunk.record.flags) &&
        !checked_add(
            zero_filled_bytes,
            journal_chunk.record.logical_length,
            zero_filled_bytes)) {
      return clonecore::Result<ResumeFinalizationResult>::failure(
          data_error(
              L"Tsumugi resume zero論理長",
              L"zero論理長が64bit上限を超えます"));
    }
  }
  auto metadata = build_metadata(request.stream.manifest, records);
  if (!metadata) {
    return clonecore::Result<ResumeFinalizationResult>::failure(
        metadata.error());
  }
  if (metadata.value().size() != validated.metadata_length ||
      journal.chunks.size() != request.stream.chunks.size()) {
    return clonecore::Result<ResumeFinalizationResult>::failure(
        verification_error(
            L"Tsumugi resume最終metadata",
            L"journalと最終metadataの件数または長さが一致しません"));
  }
  std::uint64_t payload_end = journal.header.payload_offset;
  if (!journal.chunks.empty() &&
      !checked_add(
          journal.chunks.back().record.stored_offset,
          journal.chunks.back().record.stored_length,
          payload_end)) {
    return clonecore::Result<ResumeFinalizationResult>::failure(data_error(
        L"Tsumugi resume payload完成長",
        L"payload完成長が64bit上限を超えます"));
  }
  const std::uint64_t stored_data_bytes =
      payload_end - journal.header.payload_offset;

  const bool encrypted = request.stream.encryption.has_value();
  if (encrypted != (key != nullptr)) {
    return clonecore::Result<ResumeFinalizationResult>::failure(stream_error(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_STATE,
        L"Tsumugi resume暗号鍵状態",
        L"暗号設定と再入力から導出した鍵の状態が一致しません"));
  }
  TsumugiHeader header{};
  header.major_version = kTsumugiMajorVersion;
  header.minor_version = kTsumugiMinorVersion;
  header.required_features = encrypted ? kFeatureEncrypted : 0U;
  header.payload_kind = request.stream.payload_kind;
  header.compression = request.stream.compression;
  header.source_disk_size = request.stream.source_disk_size;
  header.logical_sector_size = request.stream.logical_sector_size;
  header.physical_sector_size = request.stream.physical_sector_size;
  header.chunk_size = request.stream.chunk_size;
  header.chunk_count = records.size();
  header.metadata = ImageSection{
      .offset = kTsumugiHeaderSize,
      .length = validated.metadata_length,
  };
  header.data = ImageSection{
      .offset = journal.header.payload_offset,
      .length = stored_data_bytes,
  };
  header.footer = ImageSection{
      .offset = payload_end,
      .length = kTsumugiFooterSize,
  };
  header.image_id = request.stream.image_id;
  if (encrypted) {
    header.argon2 = request.stream.encryption->argon2;
    header.base_nonce = request.stream.encryption->base_nonce;
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
    auto nonce = nonce_for_counter(header.base_nonce, 0U);
    if (!nonce) {
      wipe_resume_bytes(metadata.value());
      return clonecore::Result<ResumeFinalizationResult>::failure(
          nonce.error());
    }
    const auto aad = canonical_metadata_aad(header_bytes);
    auto encrypted_metadata = encrypt_tsumugi_aes256_gcm(
        *key, nonce.value(), aad, metadata.value());
    wipe_resume_bytes(metadata.value());
    if (!encrypted_metadata) {
      return clonecore::Result<ResumeFinalizationResult>::failure(
          encrypted_metadata.error());
    }
    metadata.value() = std::move(encrypted_metadata.value().ciphertext);
    header.metadata_tag = encrypted_metadata.value().tag;
    header_bytes = serialize_header(header);
  }
  auto header_hash = calculate_header_hash(header_bytes);
  if (!header_hash) {
    wipe_resume_bytes(metadata.value());
    return clonecore::Result<ResumeFinalizationResult>::failure(
        header_hash.error());
  }
  header.header_hash = header_hash.value();
  header_bytes = serialize_header(header);

  auto status = write_exact(
      partial.get(), 0U, header_bytes, L"Tsumugi resume最終header書戻し");
  if (status) {
    status = write_exact(
        partial.get(),
        header.metadata.offset,
        metadata.value(),
        L"Tsumugi resume最終metadata書戻し");
  }
  wipe_resume_bytes(metadata.value());
  if (!status) {
    return clonecore::Result<ResumeFinalizationResult>::failure(
        status.error());
  }
  status = set_file_length(partial.get(), header.footer.offset);
  if (status) {
    status = partial.flush();
  }
  if (!status) {
    return clonecore::Result<ResumeFinalizationResult>::failure(
        status.error());
  }

  std::uint64_t global_verified{};
  std::uint64_t verified_blocks{};
  auto global_hash = sha256_from_reader(
      header.footer.offset,
      request.stream.verification_block_bytes,
      [&](const std::uint64_t offset, const std::size_t length) {
        if (clonecore::disk_operation_cancellation_requested(callbacks)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              cancelled_error(L"Tsumugi resume完成前全体Hash"));
        }
        auto bytes = read_exact(
            partial.get(),
            offset,
            length,
            L"Tsumugi resume完成前全体Hash読戻し");
        if (!bytes) {
          return bytes;
        }
        global_verified += length;
        ++verified_blocks;
        const auto boundary = stop_at_safe_boundary(
            callbacks,
            clonecore::DiskOperationSafeBoundary{
                .kind = clonecore::DiskOperationSafeBoundaryKind::
                    read_only_block,
                .stage = clonecore::DiskOperationStage::verifying_final,
                .completed_bytes = global_verified,
                .completed_units = verified_blocks,
            },
            L"Tsumugi resume全体Hash安全境界");
        if (!boundary) {
          wipe_resume_bytes(bytes.value());
          return clonecore::Result<std::vector<std::byte>>::failure(
              boundary.error());
        }
        return bytes;
      });
  if (!global_hash) {
    return clonecore::Result<ResumeFinalizationResult>::failure(
        global_hash.error());
  }

  std::uint64_t final_length{};
  if (!checked_add(
          header.footer.offset, kTsumugiFooterSize, final_length)) {
    return clonecore::Result<ResumeFinalizationResult>::failure(data_error(
        L"Tsumugi resume完成長",
        L"完成長が64bit上限を超えます"));
  }
  std::array<std::byte, kTsumugiFooterSize> footer{};
  auto footer_bytes = std::span<std::byte>(footer);
  std::copy(kFooterMagic.begin(), kFooterMagic.end(), footer_bytes.begin());
  write_little(footer_bytes, 8U, final_length);
  std::copy(global_hash.value().begin(), global_hash.value().end(),
            footer_bytes.begin() + 16U);
  std::copy(request.stream.image_id.begin(), request.stream.image_id.end(),
            footer_bytes.begin() + 48U);
  status = write_exact(
      partial.get(),
      header.footer.offset,
      footer,
      L"Tsumugi resume footer書込み");
  if (status) {
    status = set_file_length(partial.get(), final_length);
  }
  if (status) {
    status = partial.flush();
  }
  if (!status) {
    return clonecore::Result<ResumeFinalizationResult>::failure(
        status.error());
  }

  std::optional<std::string_view> password;
  if (encrypted) {
    password = request.stream.encryption->password;
  }
  auto fully_verified = verify_open_handle(
      partial.get(),
      final_length,
      password,
      request.stream.verification_block_bytes,
      callbacks);
  if (!fully_verified) {
    return clonecore::Result<ResumeFinalizationResult>::failure(
        fully_verified.error());
  }
  if (fully_verified.value().inspection.header.image_id !=
          request.stream.image_id ||
      fully_verified.value().inspection.manifest != request.stream.manifest ||
      fully_verified.value().inspection.records.size() != records.size() ||
      fully_verified.value().inspection.global_hash != global_hash.value() ||
      !fully_verified.value().inspection.header_hash_verified ||
      !fully_verified.value().inspection.all_chunks_verified ||
      !fully_verified.value().inspection.global_hash_verified) {
    return clonecore::Result<ResumeFinalizationResult>::failure(
        verification_error(
            L"Tsumugi resume完成partial完全検証",
            L"同一handleの完全検証結果が作成要求と一致しません"));
  }

  return clonecore::Result<ResumeFinalizationResult>::success(
      ResumeFinalizationResult{
          .report = TsumugiStreamBuildReport{
              .final_path = validated.canonical_final,
              .image_length = final_length,
              .stored_data_bytes = stored_data_bytes,
              .zero_filled_bytes = zero_filled_bytes,
              .chunk_count = records.size(),
              .all_chunks_read_back_verified = true,
              .all_chunks_authenticated_and_hashed = true,
              .global_hash_read_back_verified = true,
              .final_metadata_read_back_verified = true,
              .final_complete_scan_performed = true,
              .verification_mode = request.stream.verification_mode,
              .committed = false,
          },
          .global_hash = global_hash.value(),
          .final_length = final_length,
      });
}

clonecore::Result<TsumugiCreateResumeJournalChunkV1>
append_verified_resume_chunk(
    const TsumugiCreateResumeRequestV1& request,
    const std::uint64_t index,
    const TsumugiCreateResumeProgressV1& progress,
    OwnedPartial& partial,
    OwnedPartial& journal,
    const TsumugiKey* const key) {
  if (index >= request.stream.chunks.size()) {
    return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
        argument_error(
            L"Tsumugi resume append index",
            L"append対象chunkがplan範囲外です"));
  }
  auto partial_length = resume_file_length(
      partial.get(), L"Tsumugi resume append前partial長");
  auto journal_length = resume_file_length(
      journal.get(), L"Tsumugi resume append前journal長");
  if (!partial_length || !journal_length) {
    return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
        partial_length ? journal_length.error() : partial_length.error());
  }
  if (partial_length.value() != progress.primary_output_length ||
      journal_length.value() != progress.journal_length) {
    return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
        verification_error(
            L"Tsumugi resume append前長さ",
            L"owned object長がcheckpoint確定prefixと一致しません"));
  }

  const auto& planned =
      request.stream.chunks[static_cast<std::size_t>(index)];
  TsumugiChunkRecord record{};
  record.logical_offset = planned.logical_offset;
  record.logical_length = planned.logical_length;
  record.stored_offset = progress.primary_output_length;
  record.flags = planned.flags;

  Sha256Digest stored_hash{};
  if (chunk_is_zero(planned.flags)) {
    auto plaintext_hash = sha256_zeroes(planned.logical_length);
    auto empty_hash = sha256(std::span<const std::byte>{});
    if (!plaintext_hash || !empty_hash) {
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          plaintext_hash ? empty_hash.error() : plaintext_hash.error());
    }
    record.plaintext_hash = plaintext_hash.value();
    stored_hash = empty_hash.value();
  } else {
    auto source_bytes = planned.source->read(
        planned.source_offset,
        static_cast<std::size_t>(planned.logical_length));
    if (!source_bytes) {
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          source_bytes.error());
    }
    if (source_bytes.value().size() != planned.logical_length) {
      wipe_resume_bytes(source_bytes.value());
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          verification_error(
              L"Tsumugi resume source読取り長",
              L"Sourceが計画chunk長を正確に返しませんでした"));
    }
    auto plaintext_hash = sha256(source_bytes.value());
    if (!plaintext_hash) {
      wipe_resume_bytes(source_bytes.value());
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          plaintext_hash.error());
    }
    record.plaintext_hash = plaintext_hash.value();
    std::vector<std::byte> stored = source_bytes.value();
    if (request.stream.compression == ImageCompression::zstandard) {
      auto compressed = compress_zstandard_image_chunk_v1(
          source_bytes.value());
      if (!compressed) {
        wipe_resume_bytes(source_bytes.value());
        wipe_resume_bytes(stored);
        return clonecore::Result<
            TsumugiCreateResumeJournalChunkV1>::failure(compressed.error());
      }
      if (compressed.value().size() < stored.size()) {
        wipe_resume_bytes(stored);
        stored = compressed.take_value();
        record.compression = ImageCompression::zstandard;
      } else {
        wipe_resume_bytes(compressed.value());
      }
    }
    record.stored_length = stored.size();
    if (request.stream.encryption.has_value()) {
      if (key == nullptr) {
        wipe_resume_bytes(source_bytes.value());
        wipe_resume_bytes(stored);
        return clonecore::Result<
            TsumugiCreateResumeJournalChunkV1>::failure(stream_error(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_STATE,
            L"Tsumugi resume chunk暗号鍵",
            L"再入力passwordから導出した鍵がありません"));
      }
      record.nonce_counter = index + 1U;
      auto nonce = nonce_for_counter(
          request.stream.encryption->base_nonce, record.nonce_counter);
      if (!nonce) {
        wipe_resume_bytes(source_bytes.value());
        wipe_resume_bytes(stored);
        return clonecore::Result<
            TsumugiCreateResumeJournalChunkV1>::failure(nonce.error());
      }
      const auto aad = build_chunk_aad(
          request.stream.image_id, index, record);
      auto encrypted = encrypt_tsumugi_aes256_gcm(
          *key, nonce.value(), aad, stored);
      wipe_resume_bytes(stored);
      if (!encrypted) {
        wipe_resume_bytes(source_bytes.value());
        return clonecore::Result<
            TsumugiCreateResumeJournalChunkV1>::failure(encrypted.error());
      }
      stored = std::move(encrypted.value().ciphertext);
      record.authentication_tag = encrypted.value().tag;
      record.stored_length = stored.size();
    }
    auto status = write_exact(
        partial.get(),
        record.stored_offset,
        stored,
        L"Tsumugi resume payload suffix書込み");
    if (status) {
      status = partial.flush();
    }
    if (!status) {
      wipe_resume_bytes(source_bytes.value());
      wipe_resume_bytes(stored);
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          status.error());
    }
    auto read_back = read_exact(
        partial.get(),
        record.stored_offset,
        stored.size(),
        L"Tsumugi resume payload suffix読戻し");
    if (!read_back) {
      wipe_resume_bytes(source_bytes.value());
      wipe_resume_bytes(stored);
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          read_back.error());
    }
    if (!std::equal(
            stored.begin(), stored.end(), read_back.value().begin())) {
      wipe_resume_bytes(source_bytes.value());
      wipe_resume_bytes(stored);
      wipe_resume_bytes(read_back.value());
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          verification_error(
              L"Tsumugi resume payload suffix読戻し",
              L"flush後の格納byteが書込み内容と一致しません"));
    }
    auto hashed_stored = sha256(read_back.value());
    if (!hashed_stored) {
      wipe_resume_bytes(source_bytes.value());
      wipe_resume_bytes(stored);
      wipe_resume_bytes(read_back.value());
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          hashed_stored.error());
    }
    stored_hash = hashed_stored.value();
    status = verify_immediate_chunk_read_back(
        read_back.value(),
        record,
        index,
        request.stream.image_id,
        request.stream.encryption ? &*request.stream.encryption : nullptr,
        key);
    wipe_resume_bytes(source_bytes.value());
    wipe_resume_bytes(stored);
    wipe_resume_bytes(read_back.value());
    if (!status) {
      return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
          status.error());
    }
  }

  TsumugiCreateResumeJournalChunkV1 journal_chunk{
      .chunk_index = index,
      .record = record,
      .stored_bytes_hash = stored_hash,
      .previous_chain_hash = progress.verified_prefix_hash,
  };
  auto frame = serialize_resume_frame(journal_chunk);
  if (!frame) {
    return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
        frame.error());
  }
  std::uint64_t next_journal_length{};
  if (!checked_add(
          progress.journal_length, frame.value().size(), next_journal_length) ||
      next_journal_length > kTsumugiCreateResumeJournalMaximumBytes) {
    return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
        data_error(
            L"Tsumugi resume journal append長",
            L"journal appendが固定上限を超えます"));
  }
  auto status = write_exact(
      journal.get(),
      progress.journal_length,
      frame.value(),
      L"Tsumugi resume journal frame書込み");
  if (status) {
    status = journal.flush();
  }
  if (status) {
    status = verify_resume_read_back(
        journal.get(),
        progress.journal_length,
        frame.value(),
        L"Tsumugi resume journal frame読戻し");
  }
  if (!status) {
    return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::failure(
        status.error());
  }
  return clonecore::Result<TsumugiCreateResumeJournalChunkV1>::success(
      std::move(journal_chunk));
}

}  // namespace

class TsumugiStagedFileV1::Impl final {
 public:
  enum class State : std::uint8_t {
    pending,
    committed,
    aborted,
  };

  Impl(
      OwnedPartial&& owned_partial,
      ValidatedBuild&& build,
      TsumugiStreamBuildReport build_report,
      clonecore::DiskOperationCallbacks operation_callbacks,
      const std::uint64_t logical_payload,
      const std::uint64_t maximum_image,
      const std::uint64_t source_size) noexcept
      : partial(std::move(owned_partial)),
        validated(std::move(build)),
        report(std::move(build_report)),
        callbacks(std::move(operation_callbacks)),
        logical_payload_bytes(logical_payload),
        maximum_image_length(maximum_image),
        source_disk_size(source_size) {}

  OwnedPartial partial;
  ValidatedBuild validated;
  TsumugiStreamBuildReport report;
  clonecore::DiskOperationCallbacks callbacks;
  std::uint64_t logical_payload_bytes{};
  std::uint64_t maximum_image_length{};
  std::uint64_t source_disk_size{};
  State state{State::pending};
};

bool is_supported_tsumugi_create_verification_mode(
    const TsumugiCreateVerificationMode mode) noexcept {
  return mode == TsumugiCreateVerificationMode::complete ||
      mode == TsumugiCreateVerificationMode::fast;
}

clonecore::Result<TsumugiStagedFileV1>
prepare_verified_tsumugi_file_v1(
    const TsumugiStreamBuildRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto validated_result = validate_build_request(request);
  if (!validated_result) {
    return clonecore::Result<TsumugiStagedFileV1>::failure(
        validated_result.error());
  }
  ValidatedBuild validated = validated_result.take_value();
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return clonecore::Result<TsumugiStagedFileV1>::failure(
        cancelled_error(L"Tsumugi作成開始前"));
  }

  const bool encrypted = request.encryption.has_value();
  std::optional<TsumugiKey> key;
  if (encrypted) {
    auto derived = derive_tsumugi_key_argon2id(
        request.encryption->password, request.encryption->argon2);
    if (!derived) {
      return clonecore::Result<TsumugiStagedFileV1>::failure(
          derived.error());
    }
    key.emplace(derived.take_value());
  }

  OwnedPartial partial;
  auto status = partial.create(validated.partial_path);
  if (!status) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        status.error(), partial);
  }

  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::planning,
          .total_read_bytes = validated.logical_payload_bytes,
          .total_write_bytes = validated.logical_payload_bytes,
          .total_verify_bytes =
              validated.maximum_image_length + request.source_disk_size,
          .cancellation_allowed = true,
      });

  std::uint64_t payload_offset{};
  if (!checked_add(
          kTsumugiHeaderSize,
          validated.metadata_length,
          payload_offset)) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        argument_error(
            L"Tsumugiペイロード開始位置",
            L"ペイロード開始位置が64bit上限を超えます"),
        partial);
  }
  constexpr std::size_t kZeroReservationBlock = 1024U * 1024U;
  const std::vector<std::byte> zeroes(
      kZeroReservationBlock, std::byte{0});
  std::uint64_t reserved = 0U;
  while (reserved < payload_offset) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          cancelled_error(L"Tsumugi固定領域予約"), partial);
    }
    const std::size_t amount = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            payload_offset - reserved, zeroes.size()));
    status = write_exact(
        partial.get(),
        reserved,
        std::span<const std::byte>(zeroes.data(), amount),
        L"Tsumugi固定領域予約");
    if (!status) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          status.error(), partial);
    }
    reserved += amount;
  }

  std::vector<TsumugiChunkRecord> records;
  records.reserve(request.chunks.size());
  std::uint64_t current_payload_offset = payload_offset;
  std::uint64_t source_read_bytes = 0U;
  std::uint64_t payload_written_bytes = 0U;
  std::uint64_t zero_filled_bytes = 0U;
  std::uint64_t settled_logical_bytes = 0U;
  std::uint64_t settled_chunk_count = 0U;
  const auto reach_verified_chunk = [&]() -> clonecore::Status {
    clonecore::report_disk_operation_progress(
        callbacks,
        clonecore::DiskOperationProgress{
            .stage = clonecore::DiskOperationStage::copying_data,
            .total_read_bytes = validated.logical_payload_bytes,
            .total_write_bytes = validated.logical_payload_bytes,
            .total_verify_bytes = validated.logical_payload_bytes,
            .read_bytes = source_read_bytes,
            .written_bytes = payload_written_bytes,
            .verified_bytes = settled_logical_bytes,
            .cancellation_allowed = true,
            .pause_allowed = true,
        });
    return stop_at_safe_boundary(
        callbacks,
        clonecore::DiskOperationSafeBoundary{
            .kind =
                clonecore::DiskOperationSafeBoundaryKind::verified_chunk,
            .stage = clonecore::DiskOperationStage::copying_data,
            .completed_bytes = settled_logical_bytes,
            .completed_units = settled_chunk_count,
        },
        L"Tsumugiペイロード安全境界");
  };
  for (std::uint64_t index = 0U; index < request.chunks.size(); ++index) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          cancelled_error(L"Tsumugiペイロード作成"), partial);
    }
    const auto& chunk = request.chunks[static_cast<std::size_t>(index)];
    TsumugiChunkRecord record{};
    record.logical_offset = chunk.logical_offset;
    record.logical_length = chunk.logical_length;
    record.stored_offset = current_payload_offset;
    record.flags = chunk.flags;
    record.rescue_read_evidence = chunk.rescue_read_evidence;
    auto digest = chunk_is_zero(chunk.flags)
        ? sha256_zeroes(chunk.logical_length)
        : clonecore::Result<Sha256Digest>::failure(argument_error(
              L"TsumugiストリームチャンクHash",
              L"Sourceチャンクは読取り後にHashします"));
    if (chunk_is_zero(chunk.flags)) {
      if (!digest) {
        return fail_and_cleanup<TsumugiStagedFileV1>(
            digest.error(), partial);
      }
      record.plaintext_hash = digest.value();
      zero_filled_bytes += chunk.logical_length;
      records.push_back(record);
      if (!checked_add(
              settled_logical_bytes,
              chunk.logical_length,
              settled_logical_bytes)) {
        return fail_and_cleanup<TsumugiStagedFileV1>(
            argument_error(
                L"Tsumugi検証済み論理長",
                L"検証済み論理長が64bit上限を超えます"),
            partial);
      }
      ++settled_chunk_count;
      status = reach_verified_chunk();
      if (!status) {
        return fail_and_cleanup<TsumugiStagedFileV1>(
            status.error(), partial);
      }
      continue;
    }

    auto source_bytes = chunk.source->read(
        chunk.source_offset,
        static_cast<std::size_t>(chunk.logical_length));
    if (!source_bytes) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          source_bytes.error(), partial);
    }
    if (source_bytes.value().size() != chunk.logical_length) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          verification_error(
              L"Tsumugi Source読取り長",
              L"Sourceが要求したチャンク長を正確に返しませんでした"),
          partial);
    }
    digest = sha256(source_bytes.value());
    if (!digest) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          digest.error(), partial);
    }
    record.plaintext_hash = digest.value();
    std::vector<std::byte> stored = source_bytes.value();
    if (request.compression == ImageCompression::zstandard) {
      auto compressed =
          compress_zstandard_image_chunk_v1(source_bytes.value());
      if (!compressed) {
        return fail_and_cleanup<TsumugiStagedFileV1>(
            compressed.error(), partial);
      }
      if (compressed.value().size() < stored.size()) {
        stored = compressed.take_value();
        record.compression = ImageCompression::zstandard;
      }
    }
    record.stored_length = stored.size();
    if (encrypted) {
      record.nonce_counter = index + 1U;
      auto nonce = nonce_for_counter(
          request.encryption->base_nonce, record.nonce_counter);
      if (!nonce) {
        return fail_and_cleanup<TsumugiStagedFileV1>(
            nonce.error(), partial);
      }
      const auto aad = build_chunk_aad(request.image_id, index, record);
      auto encrypted_chunk = encrypt_tsumugi_aes256_gcm(
          *key, nonce.value(), aad, stored);
      if (!encrypted_chunk) {
        return fail_and_cleanup<TsumugiStagedFileV1>(
            encrypted_chunk.error(), partial);
      }
      if (!stored.empty()) {
        SecureZeroMemory(stored.data(), stored.size());
      }
      stored = std::move(encrypted_chunk.value().ciphertext);
      record.authentication_tag = encrypted_chunk.value().tag;
    }
    status = write_exact(
        partial.get(),
        current_payload_offset,
        stored,
        L"Tsumugiペイロード書込み");
    if (!status) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          status.error(), partial);
    }
    auto stored_read_back = read_exact(
        partial.get(),
        current_payload_offset,
        stored.size(),
        L"Tsumugiペイロード即時読戻し");
    if (!stored_read_back) {
      if (!stored.empty()) {
        SecureZeroMemory(stored.data(), stored.size());
      }
      return fail_and_cleanup<TsumugiStagedFileV1>(
          stored_read_back.error(), partial);
    }
    const bool stored_matches =
        stored_read_back.value().size() == stored.size() &&
        std::equal(
            stored.begin(), stored.end(), stored_read_back.value().begin());
    if (!stored_matches) {
      if (!stored_read_back.value().empty()) {
        SecureZeroMemory(
            stored_read_back.value().data(), stored_read_back.value().size());
      }
      if (!stored.empty()) {
        SecureZeroMemory(stored.data(), stored.size());
      }
      return fail_and_cleanup<TsumugiStagedFileV1>(
          verification_error(
              L"Tsumugiペイロード即時読戻し",
              L"チャンク書込み直後の読戻し内容が一致しません"),
          partial);
    }
    status = verify_immediate_chunk_read_back(
        stored_read_back.value(),
        record,
        index,
        request.image_id,
        request.encryption ? &*request.encryption : nullptr,
        key ? &*key : nullptr);
    if (!stored_read_back.value().empty()) {
      SecureZeroMemory(
          stored_read_back.value().data(), stored_read_back.value().size());
    }
    if (!status) {
      if (!stored.empty()) {
        SecureZeroMemory(stored.data(), stored.size());
      }
      return fail_and_cleanup<TsumugiStagedFileV1>(
          status.error(), partial);
    }
    if (!checked_add(
            current_payload_offset,
            record.stored_length,
            current_payload_offset)) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          argument_error(
              L"Tsumugiペイロード寸法",
              L"格納ペイロード長が64bit上限を超えます"),
          partial);
    }
    source_read_bytes += chunk.logical_length;
    payload_written_bytes += record.stored_length;
    records.push_back(record);
    if (!checked_add(
            settled_logical_bytes,
            chunk.logical_length,
            settled_logical_bytes)) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          argument_error(
              L"Tsumugi検証済み論理長",
              L"検証済み論理長が64bit上限を超えます"),
          partial);
    }
    ++settled_chunk_count;
    if (!source_bytes.value().empty()) {
      SecureZeroMemory(
          source_bytes.value().data(), source_bytes.value().size());
    }
    if (!stored.empty()) {
      SecureZeroMemory(stored.data(), stored.size());
    }
    status = reach_verified_chunk();
    if (!status) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          status.error(), partial);
    }
  }

  // Metadata/index/footer publication is transactional but not a safe pause
  // interval. Clear a queued request before entering it.
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::flushing_data,
          .total_read_bytes = validated.logical_payload_bytes,
          .total_write_bytes = validated.logical_payload_bytes,
          .total_verify_bytes = validated.maximum_image_length +
              request.source_disk_size,
          .read_bytes = source_read_bytes,
          .written_bytes = payload_written_bytes,
          .verified_bytes = settled_logical_bytes,
          .cancellation_allowed = true,
          .pause_allowed = false,
      });

  auto metadata = build_metadata(request.manifest, records);
  if (!metadata) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        metadata.error(), partial);
  }
  if (metadata.value().size() != validated.metadata_length) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        stream_error(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_STATE,
            L"Tsumugiメタデータ予約照合",
            L"予約済みメタデータ長と完成索引長が一致しません"),
        partial);
  }

  TsumugiHeader header{};
  header.major_version = kTsumugiMajorVersion;
  header.minor_version = kTsumugiMinorVersion;
  header.required_features =
      (encrypted ? kFeatureEncrypted : 0U) |
      (validated.has_unreadable ? kFeatureUnreadableMap : 0U) |
      (validated.has_unreadable ? kFeatureRescueReadEvidence : 0U);
  header.payload_kind = request.payload_kind;
  header.compression = request.compression;
  header.source_disk_size = request.source_disk_size;
  header.logical_sector_size = request.logical_sector_size;
  header.physical_sector_size = request.physical_sector_size;
  header.chunk_size = request.chunk_size;
  header.chunk_count = request.chunks.size();
  header.metadata = ImageSection{
      .offset = kTsumugiHeaderSize,
      .length = validated.metadata_length,
  };
  header.data = ImageSection{
      .offset = payload_offset,
      .length = payload_written_bytes,
  };
  header.footer = ImageSection{
      .offset = current_payload_offset,
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
    auto nonce = nonce_for_counter(header.base_nonce, 0U);
    if (!nonce) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          nonce.error(), partial);
    }
    const auto aad = canonical_metadata_aad(header_bytes);
    auto encrypted_metadata = encrypt_tsumugi_aes256_gcm(
        *key, nonce.value(), aad, metadata.value());
    if (!encrypted_metadata) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          encrypted_metadata.error(), partial);
    }
    if (!metadata.value().empty()) {
      SecureZeroMemory(metadata.value().data(), metadata.value().size());
    }
    metadata.value() =
        std::move(encrypted_metadata.value().ciphertext);
    header.metadata_tag = encrypted_metadata.value().tag;
    header_bytes = serialize_header(header);
  }
  auto header_hash = calculate_header_hash(header_bytes);
  if (!header_hash) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        header_hash.error(), partial);
  }
  header.header_hash = header_hash.value();
  header_bytes = serialize_header(header);

  status = write_exact(
      partial.get(), 0U, header_bytes, L"Tsumugi最終ヘッダー書戻し");
  if (!status) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        status.error(), partial);
  }
  status = write_exact(
      partial.get(),
      header.metadata.offset,
      metadata.value(),
      L"Tsumugi最終メタデータ書戻し");
  if (!status) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        status.error(), partial);
  }
  status = set_file_length(partial.get(), header.footer.offset);
  if (!status) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        status.error(), partial);
  }
  status = partial.flush();
  if (!status) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        status.error(), partial);
  }

  std::uint64_t global_verified = 0U;
  std::uint64_t global_verified_blocks = 0U;
  auto global_hash = sha256_from_reader(
      header.footer.offset,
      request.verification_block_bytes,
      [&](const std::uint64_t offset, const std::size_t length) {
        if (clonecore::disk_operation_cancellation_requested(callbacks)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              cancelled_error(L"Tsumugi完成前全体Hash読戻し"));
        }
        auto bytes = read_exact(
            partial.get(),
            offset,
            length,
            L"Tsumugi完成前全体Hash読戻し");
        if (bytes) {
          global_verified += length;
          ++global_verified_blocks;
          clonecore::report_disk_operation_progress(
              callbacks,
              clonecore::DiskOperationProgress{
                  .stage =
                      clonecore::DiskOperationStage::verifying_final,
                  .total_verify_bytes =
                      validated.maximum_image_length +
                      request.source_disk_size,
                  .verified_bytes = global_verified,
                  .cancellation_allowed = true,
                  .pause_allowed = true,
              });
          const auto pause_status = stop_at_safe_boundary(
              callbacks,
              clonecore::DiskOperationSafeBoundary{
                  .kind = clonecore::DiskOperationSafeBoundaryKind::
                      read_only_block,
                  .stage = clonecore::DiskOperationStage::verifying_final,
                  .completed_bytes = global_verified,
                  .completed_units = global_verified_blocks,
              },
              L"Tsumugi完成前全体Hash安全境界");
          if (!pause_status) {
            if (!bytes.value().empty()) {
              SecureZeroMemory(bytes.value().data(), bytes.value().size());
            }
            return clonecore::Result<std::vector<std::byte>>::failure(
                pause_status.error());
          }
        }
        return bytes;
      });
  if (!global_hash) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        global_hash.error(), partial);
  }

  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::flushing_data,
          .total_verify_bytes = validated.maximum_image_length +
              request.source_disk_size,
          .verified_bytes = global_verified,
          .cancellation_allowed = true,
          .pause_allowed = false,
      });

  std::uint64_t final_length{};
  if (!checked_add(
          header.footer.offset, kTsumugiFooterSize, final_length)) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        argument_error(
            L"Tsumugi完成長",
            L"完成ファイル長が64bit上限を超えます"),
        partial);
  }
  std::array<std::byte, kTsumugiFooterSize> footer{};
  const auto footer_bytes = std::span<std::byte>(footer);
  std::copy(kFooterMagic.begin(), kFooterMagic.end(), footer_bytes.begin());
  write_little(footer_bytes, 8U, final_length);
  std::copy(
      global_hash.value().begin(),
      global_hash.value().end(),
      footer_bytes.begin() + 16U);
  std::copy(
      request.image_id.begin(),
      request.image_id.end(),
      footer_bytes.begin() + 48U);
  status = write_exact(
      partial.get(), header.footer.offset, footer, L"Tsumugiフッター書込み");
  if (!status) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        status.error(), partial);
  }
  status = set_file_length(partial.get(), final_length);
  if (!status) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        status.error(), partial);
  }
  status = partial.flush();
  if (!status) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        status.error(), partial);
  }

  bool final_complete_scan_performed = false;
  if (request.verification_mode ==
      TsumugiCreateVerificationMode::complete) {
    std::optional<std::string_view> verification_password;
    if (encrypted) {
      verification_password = request.encryption->password;
    }
    auto verified = verify_open_handle(
        partial.get(),
        final_length,
        verification_password,
        request.verification_block_bytes,
        callbacks);
    if (!verified) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          verified.error(), partial);
    }
    if (verified.value().inspection.header.image_id != request.image_id ||
        verified.value().inspection.header.footer.offset !=
            header.footer.offset ||
        !verified.value().inspection.all_chunks_verified ||
        !verified.value().inspection.global_hash_verified) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          verification_error(
              L"Tsumugi作成後完全検証",
              L"読戻した画像の識別子、寸法、または検証結果が一致しません"),
          partial);
    }
    final_complete_scan_performed = true;
  } else {
    status = verify_fast_final_metadata(
        partial.get(),
        final_length,
        header_bytes,
        metadata.value(),
        footer,
        global_hash.value(),
        key ? &*key : nullptr,
        request);
    if (!status) {
      return fail_and_cleanup<TsumugiStagedFileV1>(
          status.error(), partial);
    }
  }
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::flushing_data,
          .total_read_bytes = validated.logical_payload_bytes,
          .total_write_bytes = validated.logical_payload_bytes,
          .total_verify_bytes = validated.maximum_image_length +
              request.source_disk_size,
          .read_bytes = source_read_bytes,
          .written_bytes = payload_written_bytes,
          .verified_bytes = validated.maximum_image_length +
              request.source_disk_size,
          .cancellation_allowed = true,
          .pause_allowed = false,
      });
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return fail_and_cleanup<TsumugiStagedFileV1>(
        cancelled_error(L"Tsumugi完成名確定前"), partial);
  }

  const std::uint64_t logical_payload_bytes =
      validated.logical_payload_bytes;
  const std::uint64_t maximum_image_length =
      validated.maximum_image_length;
  TsumugiStreamBuildReport report{
      .final_path = validated.canonical_final,
      .image_length = final_length,
      .stored_data_bytes = payload_written_bytes,
      .zero_filled_bytes = zero_filled_bytes,
      .chunk_count = request.chunks.size(),
      .all_chunks_read_back_verified = true,
      .all_chunks_authenticated_and_hashed = true,
      .global_hash_read_back_verified = true,
      .final_metadata_read_back_verified = true,
      .final_complete_scan_performed = final_complete_scan_performed,
      .verification_mode = request.verification_mode,
      .committed = false,
  };
  auto impl = std::make_unique<TsumugiStagedFileV1::Impl>(
      std::move(partial),
      std::move(validated),
      std::move(report),
      callbacks,
      logical_payload_bytes,
      maximum_image_length,
      request.source_disk_size);
  return clonecore::Result<TsumugiStagedFileV1>::success(
      TsumugiStagedFileV1(std::move(impl)));
}

clonecore::Result<TsumugiCreateResumePreparedV1>
prepare_resumable_tsumugi_file_v1(
    const TsumugiCreateResumeRequestV1& request,
    const clonecore::DiskOperationCallbacks& callbacks) {
  try {
    const bool resumed = request.expected_progress.has_value();
    auto validated_result = validate_build_request(request.stream, resumed);
    if (!validated_result) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          validated_result.error());
    }
    ValidatedBuild validated = validated_result.take_value();
    if (request.stream.payload_kind != TsumugiPayloadKind::exact_disk ||
        validated.has_unreadable || !resume_binding_valid(request.binding) ||
        !request.checkpoint.create_before_first_mutation ||
        !request.checkpoint.prove_existing_before_resume ||
        !request.checkpoint.replace_after_verified_prefix) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          argument_error(
              L"Tsumugi resume通常画像契約",
              L"exact通常画像、完全な非zero束縛、3つのcheckpoint callbackが必要です"));
    }
    auto prepared_header_result = build_expected_resume_journal_header(
        request, validated);
    if (!prepared_header_result) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          prepared_header_result.error());
    }
    PreparedResumeJournalHeader prepared_header =
        prepared_header_result.take_value();
    const TsumugiCreateResumeOwnedPathsV1 paths{
        .image_partial_path = validated.partial_path,
        .journal_path =
            validated.canonical_final + L".resume-journal.partial",
    };

    if (request.expected_progress.has_value() &&
        (!resume_phase_known(request.expected_progress->phase) ||
         all_zero(request.expected_progress->verified_prefix_hash) ||
         request.expected_progress->verified_chunk_count >
             request.stream.chunks.size() ||
         request.expected_progress->verified_logical_bytes >
             prepared_header.header.expected_logical_bytes ||
         request.expected_progress->journal_length >
             kTsumugiCreateResumeJournalMaximumBytes ||
         request.expected_progress->primary_output_length >
             validated.maximum_image_length)) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          data_error(
              L"Tsumugi resume checkpoint上限",
              L"phase、件数、長さ、またはprefix Hashが固定範囲外です"));
    }

    std::optional<TsumugiKey> key;
    if (request.stream.encryption.has_value()) {
      auto derived = derive_tsumugi_key_argon2id(
          request.stream.encryption->password,
          request.stream.encryption->argon2);
      if (!derived) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            derived.error());
      }
      key.emplace(derived.take_value());
    }
    if (clonecore::disk_operation_cancellation_requested(callbacks) &&
        !resumed) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          cancelled_error(L"Tsumugi resume新規開始前"));
    }

    OwnedPartial partial;
    OwnedPartial journal;
    ResumeOwnedFilesRetention retention(partial, journal);
    TsumugiCreateResumeProgressV1 progress{};
    std::uint64_t reused_chunks{};

    if (!resumed) {
      auto status = partial.create(paths.image_partial_path);
      if (status) {
        status = journal.create(paths.journal_path);
      }
      if (!status) {
        return fail_before_resume_slot_is_durable<
            TsumugiCreateResumePreparedV1>(
            status.error(), partial, journal);
      }
      progress = TsumugiCreateResumeProgressV1{
          .phase = TsumugiCreateResumePhaseV1::preparing,
          .verified_prefix_hash = prepared_header.header.chain_root,
      };
      // A durable create can report an ambiguous failure after its write (for
      // example, if the adapter's post-write readback fails). From the first
      // callback invocation onward, never delete objects that a slot may bind.
      retention.retain();
      status = invoke_resume_checkpoint_hook(
          request.checkpoint.create_before_first_mutation,
          L"Tsumugi resume checkpoint初回永続化",
          paths,
          request.binding,
          progress);
      if (!status) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            status.error());
      }
      status = initialize_resume_owned_files(
          partial, journal, prepared_header);
      if (!status) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            status.error());
      }
      const TsumugiCreateResumeProgressV1 prepared{
          .phase = TsumugiCreateResumePhaseV1::prepared,
          .primary_output_length = prepared_header.header.payload_offset,
          .journal_length = kResumeJournalHeaderBytes,
          .verified_prefix_hash = prepared_header.header.chain_root,
      };
      status = invoke_resume_checkpoint_hook(
          request.checkpoint.replace_after_verified_prefix,
          L"Tsumugi resume固定領域checkpoint更新",
          progress,
          prepared);
      if (!status) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            status.error());
      }
      progress = prepared;
    } else {
      retention.retain();
      auto status = partial.open_existing(paths.image_partial_path);
      if (status) {
        status = journal.open_existing(paths.journal_path);
      }
      if (!status) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            status.error());
      }
      progress = *request.expected_progress;
      status = invoke_resume_checkpoint_hook(
          request.checkpoint.prove_existing_before_resume,
          L"Tsumugi resume既存束縛再証明",
          paths,
          request.binding,
          progress);
      if (!status) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            status.error());
      }
      auto actual_partial_length = resume_file_length(
          partial.get(), L"Tsumugi resume既存partial長");
      auto actual_journal_length = resume_file_length(
          journal.get(), L"Tsumugi resume既存journal長");
      if (!actual_partial_length || !actual_journal_length) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            actual_partial_length
                ? actual_journal_length.error()
                : actual_partial_length.error());
      }
      if (actual_partial_length.value() > validated.maximum_image_length ||
          actual_journal_length.value() >
              kTsumugiCreateResumeJournalMaximumBytes ||
          actual_partial_length.value() < progress.primary_output_length ||
          actual_journal_length.value() < progress.journal_length) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            verification_error(
                L"Tsumugi resume owned object長",
                L"owned objectがcheckpointより短いか固定上限を超えます"));
      }

      if (progress.phase == TsumugiCreateResumePhaseV1::preparing) {
        status = validate_preparing_resume_progress(
            progress, prepared_header);
        if (!status) {
          return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
              status.error());
        }
        status = initialize_resume_owned_files(
            partial, journal, prepared_header);
        if (!status) {
          return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
              status.error());
        }
        const TsumugiCreateResumeProgressV1 prepared{
            .phase = TsumugiCreateResumePhaseV1::prepared,
            .primary_output_length = prepared_header.header.payload_offset,
            .journal_length = kResumeJournalHeaderBytes,
            .verified_prefix_hash = prepared_header.header.chain_root,
        };
        status = invoke_resume_checkpoint_hook(
            request.checkpoint.replace_after_verified_prefix,
            L"Tsumugi resume preparing回復checkpoint更新",
            progress,
            prepared);
        if (!status) {
          return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
              status.error());
        }
        progress = prepared;
      } else {
        auto verified = verify_resume_prefix(
            request,
            prepared_header,
            progress.journal_length,
            partial.get(),
            journal.get(),
            key ? &*key : nullptr);
        if (!verified) {
          return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
              verified.error());
        }
        reused_chunks = verified.value().inspection.chunks.size();
        if (progress.phase == TsumugiCreateResumePhaseV1::commit_ready) {
          status = validate_verified_resume_progress(
              progress,
              verified.value(),
              TsumugiCreateResumePhaseV1::commit_ready,
              false);
          if (!status || reused_chunks != request.stream.chunks.size() ||
              verified.value().verified_logical_bytes !=
                  prepared_header.header.expected_logical_bytes ||
              actual_partial_length.value() !=
                  progress.primary_output_length ||
              actual_journal_length.value() != progress.journal_length) {
            return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
                status
                    ? verification_error(
                          L"Tsumugi resume commit-ready長",
                          L"commit-ready stateが全chunkまたはexact object長を証明できません")
                    : status.error());
          }
          std::optional<std::string_view> password;
          if (request.stream.encryption.has_value()) {
            password = request.stream.encryption->password;
          }
          auto complete = verify_open_handle(
              partial.get(),
              progress.primary_output_length,
              password,
              request.stream.verification_block_bytes,
              callbacks);
          if (!complete) {
            return clonecore::Result<
                TsumugiCreateResumePreparedV1>::failure(complete.error());
          }
          std::uint64_t zero_filled{};
          for (const auto& record : complete.value().inspection.records) {
            if (chunk_is_zero(record.flags) &&
                !checked_add(
                    zero_filled,
                    record.logical_length,
                    zero_filled)) {
              return clonecore::Result<
                  TsumugiCreateResumePreparedV1>::failure(data_error(
                  L"Tsumugi resume commit-ready zero長",
                  L"zero論理長が64bit上限を超えます"));
            }
          }
          if (complete.value().inspection.global_hash !=
                  progress.verified_prefix_hash ||
              complete.value().inspection.header.image_id !=
                  request.stream.image_id ||
              complete.value().inspection.manifest !=
                  request.stream.manifest ||
              complete.value().inspection.records.size() !=
                  request.stream.chunks.size() ||
              !complete.value().inspection.header_hash_verified ||
              !complete.value().inspection.all_chunks_verified ||
              !complete.value().inspection.global_hash_verified) {
            return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
                verification_error(
                    L"Tsumugi resume commit-ready完全検証",
                    L"完成partialがcheckpointのglobal Hashまたはplanと一致しません"));
          }
          const auto& header = complete.value().inspection.header;
          return clonecore::Result<TsumugiCreateResumePreparedV1>::success(
              TsumugiCreateResumePreparedV1{
                  .stream = TsumugiStreamBuildReport{
                      .final_path = validated.canonical_final,
                      .image_length = progress.primary_output_length,
                      .stored_data_bytes = header.data.length,
                      .zero_filled_bytes = zero_filled,
                      .chunk_count = request.stream.chunks.size(),
                      .all_chunks_read_back_verified = true,
                      .all_chunks_authenticated_and_hashed = true,
                      .global_hash_read_back_verified = true,
                      .final_metadata_read_back_verified = true,
                      .final_complete_scan_performed = true,
                      .verification_mode = request.stream.verification_mode,
                      .committed = false,
                  },
                  .owned_paths = paths,
                  .progress = progress,
                  .reused_verified_chunk_count = reused_chunks,
                  .appended_chunk_count = 0U,
                  .resumed = true,
                  .complete_partial_verified = true,
                  .commit_ready = true,
              });
        }
        status = validate_verified_resume_progress(
            progress,
            verified.value(),
            TsumugiCreateResumePhaseV1::prepared,
            true);
        if (!status) {
          return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
              status.error());
        }
        if (actual_partial_length.value() > progress.primary_output_length) {
          status = set_file_length(
              partial.get(), progress.primary_output_length);
          if (status) {
            status = partial.flush();
          }
        }
        if (status &&
            actual_journal_length.value() > progress.journal_length) {
          status = set_file_length(journal.get(), progress.journal_length);
          if (status) {
            status = journal.flush();
          }
        }
        if (!status) {
          return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
              status.error());
        }
      }
    }

    const std::uint64_t first_append = progress.verified_chunk_count;
    for (std::uint64_t index = first_append;
         index < request.stream.chunks.size(); ++index) {
      if (clonecore::disk_operation_cancellation_requested(callbacks)) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            cancelled_error(L"Tsumugi resume payload suffix開始前"));
      }
      auto appended = append_verified_resume_chunk(
          request,
          index,
          progress,
          partial,
          journal,
          key ? &*key : nullptr);
      if (!appended) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            appended.error());
      }
      TsumugiCreateResumeProgressV1 next = progress;
      if (!checked_add(
              next.verified_logical_bytes,
              appended.value().record.logical_length,
              next.verified_logical_bytes) ||
          !checked_add(
              next.primary_output_length,
              appended.value().record.stored_length,
              next.primary_output_length) ||
          !checked_add(
              next.journal_length,
              kResumeJournalFrameBytes,
              next.journal_length)) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            data_error(
                L"Tsumugi resume progress加算",
                L"検証済みprefix長が64bit上限を超えます"));
      }
      ++next.verified_chunk_count;
      next.verified_prefix_hash = appended.value().chain_hash;
      auto status = invoke_resume_checkpoint_hook(
          request.checkpoint.replace_after_verified_prefix,
          L"Tsumugi resume chunk checkpoint更新",
          progress,
          next);
      if (!status) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            status.error());
      }
      progress = next;
      clonecore::report_disk_operation_progress(
          callbacks,
          clonecore::DiskOperationProgress{
              .stage = clonecore::DiskOperationStage::copying_data,
              .total_read_bytes =
                  prepared_header.header.expected_logical_bytes,
              .total_write_bytes =
                  prepared_header.header.expected_logical_bytes,
              .total_verify_bytes =
                  prepared_header.header.expected_logical_bytes,
              .read_bytes = progress.verified_logical_bytes,
              .written_bytes = progress.primary_output_length -
                  prepared_header.header.payload_offset,
              .verified_bytes = progress.verified_logical_bytes,
              .cancellation_allowed = true,
              .pause_allowed = true,
          });
      status = stop_at_safe_boundary(
          callbacks,
          clonecore::DiskOperationSafeBoundary{
              .kind = clonecore::DiskOperationSafeBoundaryKind::
                  verified_chunk,
              .stage = clonecore::DiskOperationStage::copying_data,
              .completed_bytes = progress.verified_logical_bytes,
              .completed_units = progress.verified_chunk_count,
          },
          L"Tsumugi resume durable chunk境界");
      if (!status) {
        return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
            status.error());
      }
    }

    auto complete_prefix = verify_resume_prefix(
        request,
        prepared_header,
        progress.journal_length,
        partial.get(),
        journal.get(),
        key ? &*key : nullptr);
    if (!complete_prefix) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          complete_prefix.error());
    }
    auto status = validate_verified_resume_progress(
        progress,
        complete_prefix.value(),
        TsumugiCreateResumePhaseV1::prepared,
        true);
    if (!status || progress.verified_chunk_count !=
            request.stream.chunks.size() ||
        progress.verified_logical_bytes !=
            prepared_header.header.expected_logical_bytes) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          status
              ? verification_error(
                    L"Tsumugi resume payload完成prefix",
                    L"全chunkがdurable checkpointへ確定していません")
              : status.error());
    }

    auto finalized = finalize_resume_partial(
        request,
        validated,
        complete_prefix.value().inspection,
        partial,
        key ? &*key : nullptr,
        callbacks);
    if (!finalized) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          finalized.error());
    }
    TsumugiCreateResumeProgressV1 commit_ready = progress;
    commit_ready.phase = TsumugiCreateResumePhaseV1::commit_ready;
    commit_ready.primary_output_length = finalized.value().final_length;
    commit_ready.verified_prefix_hash = finalized.value().global_hash;
    status = invoke_resume_checkpoint_hook(
        request.checkpoint.replace_after_verified_prefix,
        L"Tsumugi resume commit-ready checkpoint更新",
        progress,
        commit_ready);
    if (!status) {
      return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
          status.error());
    }
    progress = commit_ready;
    return clonecore::Result<TsumugiCreateResumePreparedV1>::success(
        TsumugiCreateResumePreparedV1{
            .stream = std::move(finalized.value().report),
            .owned_paths = paths,
            .progress = progress,
            .reused_verified_chunk_count = reused_chunks,
            .appended_chunk_count =
                progress.verified_chunk_count - first_append,
            .resumed = resumed,
            .complete_partial_verified = true,
            .commit_ready = true,
        });
  } catch (const std::bad_alloc&) {
    return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
        stream_error(
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY,
            L"Tsumugi resume有界処理",
            L"固定上限内のメモリを確保できませんでした"));
  } catch (...) {
    return clonecore::Result<TsumugiCreateResumePreparedV1>::failure(
        stream_error(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"Tsumugi resume例外境界",
            L"予期しない例外をfail-closedで停止しました"));
  }
}

TsumugiStagedFileV1::TsumugiStagedFileV1(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

TsumugiStagedFileV1::~TsumugiStagedFileV1() {
  if (pending()) {
    static_cast<void>(abort_incomplete());
  }
}

TsumugiStagedFileV1::TsumugiStagedFileV1(
    TsumugiStagedFileV1&&) noexcept = default;

TsumugiStagedFileV1& TsumugiStagedFileV1::operator=(
    TsumugiStagedFileV1&&) noexcept = default;

const TsumugiStreamBuildReport&
TsumugiStagedFileV1::report() const noexcept {
  static const TsumugiStreamBuildReport empty{};
  return impl_ ? impl_->report : empty;
}

bool TsumugiStagedFileV1::pending() const noexcept {
  return impl_ && impl_->state == Impl::State::pending;
}

clonecore::Result<TsumugiStreamBuildReport>
TsumugiStagedFileV1::commit_verified() {
  if (!pending()) {
    return clonecore::Result<TsumugiStreamBuildReport>::failure(
        argument_error(
            L"Tsumugi検証済み出力確定",
            L"確定または破棄済みの出力は再実行できません"));
  }
  if (clonecore::disk_operation_cancellation_requested(impl_->callbacks)) {
    return clonecore::Result<TsumugiStreamBuildReport>::failure(
        cancelled_error(L"Tsumugi完成名確定前"));
  }
  auto committed = commit_partial(impl_->partial, impl_->validated);
  if (!committed) {
    return clonecore::Result<TsumugiStreamBuildReport>::failure(
        committed.error());
  }
  impl_->report.retained_recovery_path =
      std::move(committed.value().retained_recovery_path);
  impl_->report.replaced_existing =
      committed.value().replaced_existing;
  impl_->report.committed = true;
  impl_->state = Impl::State::committed;
  clonecore::report_disk_operation_progress(
      impl_->callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::completed,
          .total_read_bytes = impl_->logical_payload_bytes,
          .total_write_bytes = impl_->logical_payload_bytes,
          .total_verify_bytes =
              impl_->maximum_image_length + impl_->source_disk_size,
          .read_bytes = impl_->logical_payload_bytes,
          .written_bytes = impl_->report.stored_data_bytes,
          .verified_bytes =
              impl_->maximum_image_length + impl_->source_disk_size,
          .cancellation_allowed = false,
      });
  return clonecore::Result<TsumugiStreamBuildReport>::success(
      impl_->report);
}

clonecore::Status TsumugiStagedFileV1::abort_incomplete() noexcept {
  if (!impl_ || impl_->state == Impl::State::aborted) {
    return clonecore::success_status();
  }
  if (impl_->state == Impl::State::committed) {
    return clonecore::Status::failure(argument_error(
        L"Tsumugi確定済み出力の破棄拒否",
        L"完成名へ確定済みの画像は未完了出力として破棄できません"));
  }
  const auto cleaned = impl_->partial.cleanup();
  if (cleaned) {
    impl_->validated.locked_final_handle.reset();
    impl_->state = Impl::State::aborted;
  }
  return cleaned;
}

clonecore::Result<TsumugiStreamBuildReport>
write_verified_tsumugi_file_v1(
    const TsumugiStreamBuildRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto staged = prepare_verified_tsumugi_file_v1(request, callbacks);
  if (!staged) {
    return clonecore::Result<TsumugiStreamBuildReport>::failure(
        staged.error());
  }
  return staged.value().commit_verified();
}

clonecore::Result<TsumugiFileHeaderProbe>
probe_tsumugi_file_header_v1(const std::wstring& image_path) {
  auto canonical = canonicalize_image_path(image_path);
  if (!canonical) {
    return clonecore::Result<TsumugiFileHeaderProbe>::failure(
        canonical.error());
  }
  auto reparse = reject_reparse_path(canonical.value());
  if (!reparse) {
    return clonecore::Result<TsumugiFileHeaderProbe>::failure(
        reparse.error());
  }
  auto opened = open_input_file(canonical.value());
  if (!opened) {
    return clonecore::Result<TsumugiFileHeaderProbe>::failure(opened.error());
  }
  const std::uint64_t image_length = opened.value().second.size;
  if (image_length < kTsumugiHeaderSize + kTsumugiFooterSize) {
    return clonecore::Result<TsumugiFileHeaderProbe>::failure(data_error(
        L"Tsumugi固定領域の事前確認",
        L"画像が固定ヘッダーとフッターより短いです"));
  }
  auto header_bytes = read_exact(
      opened.value().first.get(),
      0U,
      kTsumugiHeaderSize,
      L"Tsumugi固定ヘッダーの限定読取り");
  if (!header_bytes) {
    return clonecore::Result<TsumugiFileHeaderProbe>::failure(
        header_bytes.error());
  }
  auto parsed = parse_header_bytes(header_bytes.value());
  if (!parsed) {
    return clonecore::Result<TsumugiFileHeaderProbe>::failure(parsed.error());
  }
  auto sections = validate_sections(parsed.value(), image_length);
  if (!sections) {
    return clonecore::Result<TsumugiFileHeaderProbe>::failure(
        sections.error());
  }
  auto final_identity = identity_from_handle(
      opened.value().first.get(), L"Tsumugi固定ヘッダー確認後の入力再識別");
  if (!final_identity ||
      !same_observation(opened.value().second, final_identity.value())) {
    return clonecore::Result<TsumugiFileHeaderProbe>::failure(
        final_identity
            ? stream_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Tsumugi固定ヘッダー確認後の入力再識別",
                  L"限定読取り中に入力ファイルの識別または寸法が変化しました")
            : final_identity.error());
  }
  TsumugiHeader header = parsed.take_value();
  const bool encrypted =
      (header.required_features & kFeatureEncrypted) != 0U;
  return clonecore::Result<TsumugiFileHeaderProbe>::success({
      .header = std::move(header),
      .image_length = image_length,
      .encrypted = encrypted,
  });
}

clonecore::Result<TsumugiStreamInspection> verify_tsumugi_file_v1(
    const TsumugiStreamVerifyRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto block = validate_verification_block(request.verification_block_bytes);
  if (!block) {
    return clonecore::Result<TsumugiStreamInspection>::failure(block.error());
  }
  auto canonical = canonicalize_image_path(request.image_path);
  if (!canonical) {
    return clonecore::Result<TsumugiStreamInspection>::failure(
        canonical.error());
  }
  auto reparse = reject_reparse_path(canonical.value());
  if (!reparse) {
    return clonecore::Result<TsumugiStreamInspection>::failure(
        reparse.error());
  }
  auto opened = open_input_file(canonical.value());
  if (!opened) {
    return clonecore::Result<TsumugiStreamInspection>::failure(opened.error());
  }
  auto verified = verify_open_handle(
      opened.value().first.get(),
      opened.value().second.size,
      request.password,
      request.verification_block_bytes,
      callbacks);
  if (!verified) {
    return clonecore::Result<TsumugiStreamInspection>::failure(
        verified.error());
  }
  auto final_identity = identity_from_handle(
      opened.value().first.get(), L"Tsumugi検証後入力再識別");
  if (!final_identity ||
      !same_observation(opened.value().second, final_identity.value())) {
    return clonecore::Result<TsumugiStreamInspection>::failure(
        final_identity
            ? stream_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Tsumugi検証後入力再識別",
                  L"検証中に入力ファイルの安定識別または寸法が変化しました")
            : final_identity.error());
  }
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::verifying_source,
          .total_verify_bytes = opened.value().second.size +
              verified.value().inspection.header.source_disk_size,
          .verified_bytes = opened.value().second.size +
              verified.value().inspection.header.source_disk_size,
          .cancellation_allowed = false,
          .pause_allowed = false,
      });
  verified.value().inspection.opened_file =
      public_observation(final_identity.value());
  return clonecore::Result<TsumugiStreamInspection>::success(
      std::move(verified.value().inspection));
}

clonecore::Result<TsumugiStreamRestoreReport>
read_verified_tsumugi_file_v1(
    const TsumugiStreamVerifyRequest& request,
    const TsumugiVerifiedChunkCallback& verified_chunk,
    const clonecore::DiskOperationCallbacks& callbacks,
    const TsumugiVerifiedInspectionGate& inspection_gate) {
  if (!verified_chunk) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        argument_error(
            L"Tsumugi復元Callback",
            L"検証済みチャンクのCallbackが指定されていません"));
  }
  auto block = validate_verification_block(request.verification_block_bytes);
  if (!block) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        block.error());
  }
  auto canonical = canonicalize_image_path(request.image_path);
  if (!canonical) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        canonical.error());
  }
  auto reparse = reject_reparse_path(canonical.value());
  if (!reparse) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        reparse.error());
  }
  auto opened = open_input_file(canonical.value());
  if (!opened) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        opened.error());
  }
  auto verified = verify_open_handle(
      opened.value().first.get(),
      opened.value().second.size,
      request.password,
      request.verification_block_bytes,
      callbacks);
  if (!verified) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        verified.error());
  }
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::verifying_source,
          .total_verify_bytes = opened.value().second.size +
              verified.value().inspection.header.source_disk_size,
          .verified_bytes = opened.value().second.size +
              verified.value().inspection.header.source_disk_size,
          .cancellation_allowed = true,
          .pause_allowed = false,
      });
  auto pre_callback_identity = identity_from_handle(
      opened.value().first.get(), L"Tsumugi復元Callback前入力再識別");
  if (!pre_callback_identity ||
      !same_observation(
          opened.value().second, pre_callback_identity.value())) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        pre_callback_identity
            ? stream_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Tsumugi復元Callback前入力再識別",
                  L"完全検証中に入力ファイルが変化したため復元書込みを開始しません")
            : pre_callback_identity.error());
  }
  verified.value().inspection.opened_file =
      public_observation(pre_callback_identity.value());
  if (inspection_gate) {
    clonecore::Status accepted = clonecore::success_status();
    try {
      accepted = inspection_gate(verified.value().inspection);
    } catch (...) {
      accepted = clonecore::Status::failure(stream_error(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"Tsumugi復元計画同一ハンドル確認",
          L"復元計画確認Callbackが例外を送出しました"));
    }
    if (!accepted) {
      return clonecore::Result<TsumugiStreamRestoreReport>::failure(
          accepted.error());
    }
  }
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        cancelled_error(L"Tsumugi復元書込み開始前"));
  }

  std::uint64_t delivered_bytes = 0U;
  std::uint64_t delivered_chunks = 0U;
  for (std::uint64_t index = 0U;
       index < verified.value().inspection.records.size();
       ++index) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return clonecore::Result<TsumugiStreamRestoreReport>::failure(
          cancelled_error(L"Tsumugi検証済みチャンク受渡し"));
    }
    const auto& record = verified.value().inspection.records[
        static_cast<std::size_t>(index)];
    std::vector<std::byte> plaintext;
    if (!chunk_is_zero(record.flags)) {
      auto decoded = decode_chunk(
          opened.value().first.get(),
          verified.value().inspection.header,
          record,
          index,
          verified.value().key ? &*verified.value().key : nullptr);
      if (!decoded) {
        return clonecore::Result<TsumugiStreamRestoreReport>::failure(
            decoded.error());
      }
      plaintext = decoded.take_value();
    }
    clonecore::Status delivered = clonecore::success_status();
    try {
      delivered = verified_chunk(record, plaintext);
    } catch (...) {
      delivered = clonecore::Status::failure(stream_error(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"Tsumugi検証済みチャンクCallback",
          L"復元Callbackが例外を送出しました"));
    }
    if (!plaintext.empty()) {
      SecureZeroMemory(plaintext.data(), plaintext.size());
    }
    if (!delivered) {
      return clonecore::Result<TsumugiStreamRestoreReport>::failure(
          delivered.error());
    }
    delivered_bytes += record.logical_length;
    ++delivered_chunks;
    clonecore::report_disk_operation_progress(
        callbacks,
        clonecore::DiskOperationProgress{
            .stage = clonecore::DiskOperationStage::copying_data,
            .total_read_bytes =
                verified.value().inspection.header.source_disk_size,
            .total_write_bytes =
                verified.value().inspection.header.source_disk_size,
            .read_bytes = delivered_bytes,
            .written_bytes = delivered_bytes,
            .verified_bytes = delivered_bytes,
            .cancellation_allowed = true,
            .pause_allowed = true,
        });
    const auto pause_status = stop_at_safe_boundary(
        callbacks,
        clonecore::DiskOperationSafeBoundary{
            .kind =
                clonecore::DiskOperationSafeBoundaryKind::verified_chunk,
            .stage = clonecore::DiskOperationStage::copying_data,
            .completed_bytes = delivered_bytes,
            .completed_units = delivered_chunks,
        },
        L"Tsumugi復元チャンク安全境界");
    if (!pause_status) {
      return clonecore::Result<TsumugiStreamRestoreReport>::failure(
          pause_status.error());
    }
  }
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::flushing_data,
          .total_read_bytes =
              verified.value().inspection.header.source_disk_size,
          .total_write_bytes =
              verified.value().inspection.header.source_disk_size,
          .read_bytes = delivered_bytes,
          .written_bytes = delivered_bytes,
          .verified_bytes = delivered_bytes,
          .cancellation_allowed = false,
          .pause_allowed = false,
      });
  auto final_identity = identity_from_handle(
      opened.value().first.get(), L"Tsumugi復元pass後入力再識別");
  if (!final_identity ||
      !same_observation(opened.value().second, final_identity.value())) {
    return clonecore::Result<TsumugiStreamRestoreReport>::failure(
        final_identity
            ? stream_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Tsumugi復元pass後入力再識別",
                  L"復元pass中に入力ファイルの識別、寸法、または更新時刻が変化しました")
            : final_identity.error());
  }
  TsumugiStreamRestoreReport report{};
  report.inspection = std::move(verified.value().inspection);
  report.delivered_logical_bytes = delivered_bytes;
  report.delivered_chunk_count = delivered_chunks;
  report.callbacks_started_after_complete_verification = true;
  return clonecore::Result<TsumugiStreamRestoreReport>::success(
      std::move(report));
}

}  // namespace ytec::imageformat

namespace ytec::imageformat {
namespace {

clonecore::Result<ValidatedBuild> validate_build_request(
    const TsumugiStreamBuildRequest& request,
    const bool allow_existing_owned_partial) {
  if (!is_supported_tsumugi_create_verification_mode(
          request.verification_mode)) {
    return clonecore::Result<ValidatedBuild>::failure(argument_error(
        L"Tsumugi作成時検証モード",
        L"作成時検証モードが完全または高速ではありません"));
  }
  auto canonical = canonicalize_image_path(request.final_path);
  if (!canonical) {
    return clonecore::Result<ValidatedBuild>::failure(canonical.error());
  }
  auto status = reject_reparse_path(canonical.value());
  if (!status) {
    return clonecore::Result<ValidatedBuild>::failure(status.error());
  }
  status = require_supported_file_system(canonical.value());
  if (!status) {
    return clonecore::Result<ValidatedBuild>::failure(status.error());
  }
  status = validate_verification_block(request.verification_block_bytes);
  if (!status) {
    return clonecore::Result<ValidatedBuild>::failure(status.error());
  }
  if (!valid_payload_kind(request.payload_kind) ||
      request.source_disk_size == 0U ||
      request.source_disk_size > static_cast<std::uint64_t>(
                                     (std::numeric_limits<LONGLONG>::max)()) ||
      !is_supported_sector_size_pair(
          request.logical_sector_size, request.physical_sector_size) ||
      request.source_disk_size % request.logical_sector_size != 0U ||
      !valid_chunk_size(request.chunk_size) ||
      !valid_compression(request.compression) || request.manifest.empty() ||
      request.manifest.size() > kTsumugiMaximumManifestBytes ||
      request.chunks.size() > kTsumugiMaximumChunkCount ||
      all_zero(request.image_id)) {
    return clonecore::Result<ValidatedBuild>::failure(argument_error(
        L"Tsumugiストリーム作成要求",
        L"画像種別、ディスク寸法、識別子、マニフェスト、または圧縮設定が不正です"));
  }
  if (request.encryption.has_value()) {
    const auto password =
        assess_tsumugi_password(request.encryption->password);
    const auto& parameters = request.encryption->argon2;
    if (!password.accepted ||
        parameters.memory_kib != kTsumugiArgon2MemoryKiB ||
        parameters.iterations != kTsumugiArgon2Iterations ||
        parameters.parallelism != kTsumugiArgon2Parallelism ||
        all_zero(parameters.salt) ||
        all_zero(request.encryption->base_nonce)) {
      return clonecore::Result<ValidatedBuild>::failure(argument_error(
          L"Tsumugiストリーム暗号設定",
          L"ASCII 8文字以上、固定Argon2id設定、画像固有SaltとNonceが必要です"));
    }
  }

  std::uint64_t previous_end = 0U;
  std::uint64_t payload_upper_bound = 0U;
  bool first = true;
  bool has_unreadable = false;
  const bool byte_stream_payload =
      request.payload_kind == TsumugiPayloadKind::shrink_disk;
  for (const auto& chunk : request.chunks) {
    std::uint64_t logical_end{};
    std::uint64_t source_end{};
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
        (zero && chunk.source != nullptr) ||
        (!zero && chunk.source == nullptr)) {
      return clonecore::Result<ValidatedBuild>::failure(argument_error(
          L"Tsumugiストリームチャンク要求",
          L"チャンクが重複、境界外、非整列、またはSource指定不正です"));
    }
    if (!zero &&
        (chunk.source->logical_sector_size() !=
             request.logical_sector_size ||
         (!byte_stream_payload &&
          chunk.source_offset % request.logical_sector_size != 0U) ||
         !checked_add(
             chunk.source_offset, chunk.logical_length, source_end) ||
         source_end > chunk.source->size_bytes() ||
         !checked_add(
             payload_upper_bound,
             chunk.logical_length,
             payload_upper_bound))) {
      return clonecore::Result<ValidatedBuild>::failure(argument_error(
          L"TsumugiストリームSource範囲",
          L"Sourceのセクター寸法または読取り範囲が一致しません"));
    }
    if (chunk_is_unreadable(chunk.flags) &&
        request.payload_kind != TsumugiPayloadKind::rescue_disk) {
      return clonecore::Result<ValidatedBuild>::failure(argument_error(
          L"Tsumugiストリーム欠損マップ",
          L"読取り不能範囲は救出イメージだけに記録できます"));
    }
    if (chunk.rescue_read_evidence.has_value() !=
            chunk_is_unreadable(chunk.flags) ||
        (chunk.rescue_read_evidence.has_value() &&
         !valid_rescue_read_evidence(*chunk.rescue_read_evidence))) {
      return clonecore::Result<ValidatedBuild>::failure(argument_error(
          L"Tsumugiストリーム救出読取り証跡",
          L"読取り不能範囲には有限リトライとゼロ埋め読戻しの証跡が必要です"));
    }
    has_unreadable = has_unreadable || chunk_is_unreadable(chunk.flags);
    previous_end = logical_end;
    first = false;
  }
  std::uint64_t records_length{};
  std::uint64_t metadata_length{};
  std::uint64_t maximum_length{};
  if (!checked_multiply(
          request.chunks.size(),
          kTsumugiChunkRecordSize,
          records_length) ||
      !checked_add(
          kTsumugiMetadataHeaderSize,
          request.manifest.size(),
          metadata_length) ||
      !checked_add(metadata_length, records_length, metadata_length) ||
      metadata_length > kTsumugiMaximumMetadataBytes ||
      !checked_add(kTsumugiHeaderSize, metadata_length, maximum_length) ||
      !checked_add(maximum_length, payload_upper_bound, maximum_length) ||
      !checked_add(maximum_length, kTsumugiFooterSize, maximum_length) ||
      maximum_length > static_cast<std::uint64_t>(
                           (std::numeric_limits<LONGLONG>::max)())) {
    return clonecore::Result<ValidatedBuild>::failure(argument_error(
        L"Tsumugiストリーム最大寸法",
        L"作成予定画像が形式またはWindowsの上限を超えます"));
  }

  const std::wstring partial = canonical.value() + L".partial";
  const std::wstring recovery = canonical.value() + L".replace-backup";
  auto locked_final = lock_final_path_for_replacement(
      canonical.value(), L"Tsumugi既存完成ファイル固定");
  if (!locked_final) {
    return clonecore::Result<ValidatedBuild>::failure(
        locked_final.error());
  }
  LockedPathObservation locked = locked_final.take_value();
  if (locked.observation.exists && !request.replace_existing) {
    return clonecore::Result<ValidatedBuild>::failure(stream_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_FILE_EXISTS,
        L"Tsumugi既存完成ファイル確認",
        L"完成ファイルが既に存在し、置換が許可されていません"));
  }
  auto partial_observation = observe_path(
      partial, L"Tsumugi既存未完了ファイル確認");
  if (!partial_observation) {
    return clonecore::Result<ValidatedBuild>::failure(
        partial_observation.error());
  }
  if (partial_observation.value().exists &&
      !allow_existing_owned_partial) {
    return clonecore::Result<ValidatedBuild>::failure(stream_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_FILE_EXISTS,
        L"Tsumugi既存未完了ファイル確認",
        L"既存の.partialは所有権を確認できないため上書きも削除もしません"));
  }
  auto recovery_observation = observe_path(
      recovery, L"Tsumugi既存回復ファイル確認");
  if (!recovery_observation) {
    return clonecore::Result<ValidatedBuild>::failure(
        recovery_observation.error());
  }
  if (recovery_observation.value().exists) {
    return clonecore::Result<ValidatedBuild>::failure(stream_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_FILE_EXISTS,
        L"Tsumugi既存回復ファイル確認",
        L"前回の回復用ファイルが残っているため新規作成も自動置換もしません"));
  }

  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW(
          canonical.value().c_str(), &available, nullptr, nullptr)) {
    const std::size_t separator = canonical.value().find_last_of(L'\\');
    const std::wstring parent =
        canonical.value().substr(0U, separator + 1U);
    if (!GetDiskFreeSpaceExW(
            parent.c_str(), &available, nullptr, nullptr)) {
      return clonecore::Result<ValidatedBuild>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"Tsumugi保存先空き容量取得",
              GetLastError()));
    }
  }
  if (available.QuadPart < maximum_length) {
    return clonecore::Result<ValidatedBuild>::failure(stream_error(
        clonecore::ErrorCode::io_failed,
        ERROR_DISK_FULL,
        L"Tsumugi保存先空き容量確認",
        L"最悪時の画像寸法を安全に保持できる空き容量がありません"));
  }

  return clonecore::Result<ValidatedBuild>::success(ValidatedBuild{
      .canonical_final = canonical.take_value(),
      .partial_path = partial,
      .recovery_path = recovery,
      .initial_final = std::move(locked.observation),
      .locked_final_handle = std::move(locked.handle),
      .metadata_length = metadata_length,
      .maximum_image_length = maximum_length,
      .logical_payload_bytes = payload_upper_bound,
      .has_unreadable = has_unreadable,
  });
}

class LocalSecurityDescriptor final {
 public:
  LocalSecurityDescriptor() = default;
  ~LocalSecurityDescriptor() {
    if (value_ != nullptr) {
      LocalFree(value_);
    }
  }
  LocalSecurityDescriptor(const LocalSecurityDescriptor&) = delete;
  LocalSecurityDescriptor& operator=(const LocalSecurityDescriptor&) = delete;

  [[nodiscard]] PSECURITY_DESCRIPTOR* put() noexcept { return &value_; }
  [[nodiscard]] PSECURITY_DESCRIPTOR get() const noexcept { return value_; }

 private:
  PSECURITY_DESCRIPTOR value_{};
};

OwnedPartial::OwnedPartial(OwnedPartial&& other) noexcept
    : path_(std::move(other.path_)),
      handle_(std::move(other.handle_)),
      identity_(other.identity_),
      owns_(std::exchange(other.owns_, false)) {}

OwnedPartial::~OwnedPartial() {
  if (owns_) {
    static_cast<void>(cleanup());
  }
}

clonecore::Status OwnedPartial::create(const std::wstring& path) {
  if (owns_ || handle_) {
    return clonecore::Status::failure(argument_error(
        L"Tsumugi未完了ファイル作成",
        L"未完了ファイルは一度だけ作成できます"));
  }
  LocalSecurityDescriptor descriptor;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)",
          SDDL_REVISION_1,
          descriptor.put(),
          nullptr)) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::access_denied,
        L"Tsumugi未完了ファイルACL作成",
        GetLastError()));
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor.get();
  attributes.bInheritHandle = FALSE;
  handle_.reset(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ | GENERIC_WRITE | DELETE,
      FILE_SHARE_READ,
      &attributes,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!handle_) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"Tsumugi未完了ファイルCREATE_NEW",
        GetLastError()));
  }
  auto identity = identity_from_handle(
      handle_.get(), L"Tsumugi未完了ファイル識別");
  if (!identity) {
    FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
    static_cast<void>(SetFileInformationByHandle(
        handle_.get(),
        FileDispositionInfo,
        &disposition,
        sizeof(disposition)));
    handle_.reset();
    return clonecore::Status::failure(identity.error());
  }
  path_ = path;
  identity_ = identity.take_value();
  owns_ = true;
  return clonecore::success_status();
}

clonecore::Status OwnedPartial::open_existing(const std::wstring& path) {
  if (owns_ || handle_) {
    return clonecore::Status::failure(argument_error(
        L"Tsumugi再開未完了ファイル",
        L"同じowned handleを再利用できません"));
  }
  handle_.reset(CreateFileW(
      extended_path(path).c_str(),
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle_) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::identity_mismatch,
        L"Tsumugi再開未完了ファイルopen",
        GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(
          handle_.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
      !GetFileInformationByHandleEx(
          handle_.get(), FileStandardInfo, &standard, sizeof(standard))) {
    const DWORD native_code = GetLastError();
    handle_.reset();
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"Tsumugi再開未完了ファイル属性",
        native_code));
  }
  if ((tag.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
      standard.NumberOfLinks != 1U) {
    handle_.reset();
    return clonecore::Status::failure(unsupported_error(
        L"Tsumugi再開未完了ファイル形状",
        L"通常ファイル、reparseなし、hard link数1を証明できません"));
  }
  auto identity = identity_from_handle(
      handle_.get(), L"Tsumugi再開未完了ファイル識別");
  if (!identity) {
    handle_.reset();
    return clonecore::Status::failure(identity.error());
  }
  // GetFinalPathNameByHandleW may expand an 8.3 spelling even when both names
  // identify the same local file. The open handle denies delete sharing, so
  // the path cannot be relinked while a second path observation is compared by
  // volume/File ID instead of by an alias-sensitive string.
  auto path_observation = observe_path(
      path, L"Tsumugi再開未完了ファイルpath再識別");
  if (!path_observation || !path_observation.value().exists ||
      !path_observation.value().identity.has_value() ||
      !same_file_id(
          identity.value(), path_observation.value().identity.value())) {
    handle_.reset();
    return path_observation
        ? clonecore::Status::failure(stream_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Tsumugi再開未完了ファイルpath再識別",
              L"open handleと固定pathのFile IDが一致しません"))
        : clonecore::Status::failure(path_observation.error());
  }
  path_ = path;
  identity_ = identity.take_value();
  owns_ = true;
  return clonecore::success_status();
}

HANDLE OwnedPartial::get() const noexcept { return handle_.get(); }

const FileIdentity& OwnedPartial::identity() const noexcept {
  return identity_;
}

clonecore::Status OwnedPartial::flush() const {
  if (!handle_ || !FlushFileBuffers(handle_.get())) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"Tsumugi未完了ファイルflush",
        handle_ ? GetLastError() : ERROR_INVALID_HANDLE));
  }
  return clonecore::success_status();
}

void OwnedPartial::release_after_rename() noexcept {
  owns_ = false;
  handle_.reset();
}

clonecore::Status OwnedPartial::cleanup() noexcept {
  if (!owns_) {
    return clonecore::success_status();
  }
  handle_.reset();
  auto observation = observe_path(
      path_, L"Tsumugi未完了ファイル破棄前識別");
  if (!observation) {
    return clonecore::Status::failure(observation.error());
  }
  if (!observation.value().exists) {
    owns_ = false;
    return clonecore::success_status();
  }
  if (!observation.value().identity.has_value() ||
      !same_file_id(identity_, observation.value().identity.value())) {
    return clonecore::Status::failure(stream_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi未完了ファイル破棄",
        L".partialの安定識別が変化したため削除しません"));
  }
  if (!DeleteFileW(extended_path(path_).c_str())) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"Tsumugi未完了ファイル破棄",
        GetLastError()));
  }
  owns_ = false;
  return clonecore::success_status();
}

clonecore::Status rename_handle_no_replace(
    const HANDLE handle,
    const std::wstring& destination,
    const std::wstring_view operation) {
  const std::size_t name_bytes = destination.size() * sizeof(wchar_t);
  const std::size_t buffer_bytes =
      offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t);
  if (name_bytes > (std::numeric_limits<DWORD>::max)() ||
      buffer_bytes > (std::numeric_limits<DWORD>::max)()) {
    return clonecore::Status::failure(argument_error(
        std::wstring(operation), L"ファイル名がWindows上限を超えます"));
  }
  std::vector<std::byte> buffer(buffer_bytes, std::byte{0});
  auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  rename->ReplaceIfExists = FALSE;
  rename->RootDirectory = nullptr;
  rename->FileNameLength = static_cast<DWORD>(name_bytes);
  std::memcpy(rename->FileName, destination.data(), name_bytes);
  if (!SetFileInformationByHandle(
          handle,
          FileRenameInfo,
          buffer.data(),
          static_cast<DWORD>(buffer.size()))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed, operation, GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Result<CommitResult> commit_partial(
    OwnedPartial& partial,
    ValidatedBuild& validated) {
  auto partial_observation = observe_path(
      validated.partial_path, L"Tsumugi確定前partial再識別");
  if (!partial_observation || !partial_observation.value().exists ||
      !partial_observation.value().identity.has_value() ||
      !same_file_id(
          partial.identity(),
          partial_observation.value().identity.value())) {
    return clonecore::Result<CommitResult>::failure(
        partial_observation
            ? stream_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Tsumugi確定前partial再識別",
                  L"検証済み.partialの安定識別が一致しません")
            : partial_observation.error());
  }
  auto final_observation = observe_path(
      validated.canonical_final, L"Tsumugi確定前完成名再識別");
  if (!final_observation) {
    return clonecore::Result<CommitResult>::failure(final_observation.error());
  }
  if (validated.initial_final.exists != final_observation.value().exists ||
      validated.initial_final.exists !=
          validated.locked_final_handle.valid() ||
      (validated.initial_final.exists &&
       (!validated.initial_final.identity.has_value() ||
        !final_observation.value().identity.has_value() ||
        !same_observation(
            validated.initial_final.identity.value(),
            final_observation.value().identity.value())))) {
    return clonecore::Result<CommitResult>::failure(stream_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi確定前完成名再識別",
        L"作成中に既存完成ファイルが追加、削除、または変更されました"));
  }

  if (!validated.initial_final.exists) {
    auto renamed = rename_handle_no_replace(
        partial.get(),
        validated.canonical_final,
        L"Tsumugi検証済みファイル確定");
    if (!renamed) {
      return clonecore::Result<CommitResult>::failure(renamed.error());
    }
    partial.release_after_rename();
    return clonecore::Result<CommitResult>::success(CommitResult{});
  }

  auto old_identity = identity_from_handle(
      validated.locked_final_handle.get(),
      L"Tsumugi旧完成ファイル最終識別");
  if (!old_identity || !validated.initial_final.identity.has_value() ||
      !same_observation(
          old_identity.value(), validated.initial_final.identity.value())) {
    return clonecore::Result<CommitResult>::failure(
        old_identity
            ? stream_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_FILE_INVALID,
                  L"Tsumugi旧完成ファイル最終識別",
                  L"置換直前に旧完成ファイルが変更されました")
            : old_identity.error());
  }
  auto recovery_observation = observe_path(
      validated.recovery_path, L"Tsumugi回復名最終確認");
  if (!recovery_observation || recovery_observation.value().exists) {
    return clonecore::Result<CommitResult>::failure(
        recovery_observation
            ? stream_error(
                  clonecore::ErrorCode::confirmation_required,
                  ERROR_FILE_EXISTS,
                  L"Tsumugi回復名最終確認",
                  L"回復用ファイル名が使用中のため置換しません")
            : recovery_observation.error());
  }

  auto renamed_old = rename_handle_no_replace(
      validated.locked_final_handle.get(),
      validated.recovery_path,
      L"Tsumugi旧完成ファイルの回復退避");
  if (!renamed_old) {
    return clonecore::Result<CommitResult>::failure(renamed_old.error());
  }
  auto renamed_new = rename_handle_no_replace(
      partial.get(),
      validated.canonical_final,
      L"Tsumugi検証済みファイル確定");
  if (!renamed_new) {
    const auto rollback = rename_handle_no_replace(
        validated.locked_final_handle.get(),
        validated.canonical_final,
        L"Tsumugi旧完成ファイルrollback");
    if (!rollback) {
      return clonecore::Result<CommitResult>::failure(stream_error(
          clonecore::ErrorCode::io_failed,
          rollback.error().native_code,
          L"Tsumugi置換rollback",
          L"新ファイル確定と旧ファイル復帰に失敗しました。回復用ファイルを保持しています"));
    }
    return clonecore::Result<CommitResult>::failure(renamed_new.error());
  }
  partial.release_after_rename();
  validated.locked_final_handle.reset();

  auto retained = validated.recovery_path;
  auto recovery_after = observe_path(
      validated.recovery_path, L"Tsumugi回復ファイル削除前識別");
  if (recovery_after && recovery_after.value().exists &&
      recovery_after.value().identity.has_value() &&
      same_file_id(
          old_identity.value(), recovery_after.value().identity.value()) &&
      DeleteFileW(extended_path(validated.recovery_path).c_str())) {
    retained.clear();
  }
  return clonecore::Result<CommitResult>::success(CommitResult{
      .replaced_existing = true,
      .retained_recovery_path = std::move(retained),
  });
}

}  // namespace
}  // namespace ytec::imageformat

namespace ytec::imageformat {
namespace {

clonecore::Result<TsumugiHeader> parse_header_bytes(
    const std::span<const std::byte> bytes) {
  if (bytes.size() != kTsumugiHeaderSize ||
      !std::equal(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin())) {
    return clonecore::Result<TsumugiHeader>::failure(data_error(
        L"Tsumugi v1固定ヘッダー",
        L"正式な.tsumugi v1固定ヘッダーではありません"));
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
  std::copy_n(
      bytes.begin() + 128U,
      header.argon2.salt.size(),
      header.argon2.salt.begin());
  std::copy_n(
      bytes.begin() + 144U,
      header.base_nonce.size(),
      header.base_nonce.begin());
  std::copy_n(
      bytes.begin() + 156U,
      header.image_id.size(),
      header.image_id.begin());
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
      header.source_disk_size > static_cast<std::uint64_t>(
                                    (std::numeric_limits<LONGLONG>::max)()) ||
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
        header.argon2.parallelism != kTsumugiArgon2Parallelism) {
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
  auto calculated = calculate_header_hash(bytes);
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
    const std::uint64_t image_size) {
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
        L"ヘッダー、メタデータ、ペイロード、フッターが仕様順で連続していません"));
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
  for (std::uint64_t index = 0U; index < chunk_count; ++index) {
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
        record.stored_length > record.logical_length ||
        record.stored_length > kTsumugiMaximumCryptBytes ||
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
    has_unreadable = has_unreadable || chunk_is_unreadable(record.flags);
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
        L"チャンク索引がペイロード全体または欠損マップを正確に表現していません"));
  }
  return clonecore::Result<ParsedMetadata>::success(std::move(parsed));
}

clonecore::Status verify_fast_final_metadata(
    const HANDLE handle,
    const std::uint64_t image_length,
    const std::span<const std::byte> expected_header,
    const std::span<const std::byte> expected_metadata,
    const std::span<const std::byte> expected_footer,
    const Sha256Digest& expected_global_hash,
    const TsumugiKey* const encryption_key,
    const TsumugiStreamBuildRequest& request) {
  if (expected_header.size() != kTsumugiHeaderSize ||
      expected_footer.size() != kTsumugiFooterSize) {
    return clonecore::Status::failure(stream_error(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"Tsumugi高速検証の期待値",
        L"固定ヘッダーまたはフッターの期待長が一致しません"));
  }

  auto before = identity_from_handle(
      handle, L"Tsumugi高速検証前の未完了ファイル再識別");
  if (!before) {
    return clonecore::Status::failure(before.error());
  }
  if (before.value().size != image_length) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi高速検証の完成長",
        L"flush後の完成ファイル長が計画値と一致しません"));
  }

  auto header_bytes = read_exact(
      handle, 0U, kTsumugiHeaderSize, L"Tsumugi高速検証ヘッダー読戻し");
  if (!header_bytes) {
    return clonecore::Status::failure(header_bytes.error());
  }
  if (header_bytes.value().size() != expected_header.size() ||
      !std::equal(
          expected_header.begin(),
          expected_header.end(),
          header_bytes.value().begin())) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi高速検証ヘッダー読戻し",
        L"最終ヘッダーの読戻し内容が書込み期待値と一致しません"));
  }
  auto parsed_header = parse_header_bytes(header_bytes.value());
  if (!parsed_header) {
    return clonecore::Status::failure(parsed_header.error());
  }
  auto sections = validate_sections(parsed_header.value(), image_length);
  if (!sections) {
    return sections;
  }
  if (parsed_header.value().metadata.length != expected_metadata.size() ||
      parsed_header.value().image_id != request.image_id) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi高速検証ヘッダー照合",
        L"最終メタデータ長または画像識別子が作成要求と一致しません"));
  }

  auto metadata = read_exact(
      handle,
      parsed_header.value().metadata.offset,
      static_cast<std::size_t>(parsed_header.value().metadata.length),
      L"Tsumugi高速検証メタデータ読戻し");
  if (!metadata) {
    return clonecore::Status::failure(metadata.error());
  }
  if (metadata.value().size() != expected_metadata.size() ||
      !std::equal(
          expected_metadata.begin(),
          expected_metadata.end(),
          metadata.value().begin())) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi高速検証メタデータ読戻し",
        L"最終メタデータの読戻し内容が書込み期待値と一致しません"));
  }

  const bool encrypted =
      (parsed_header.value().required_features & kFeatureEncrypted) != 0U;
  if (encrypted) {
    if (encryption_key == nullptr || !request.encryption.has_value()) {
      return clonecore::Status::failure(stream_error(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"Tsumugi高速検証メタデータ認証",
          L"暗号化メタデータの検証鍵が保持されていません"));
    }
    auto nonce = nonce_for_counter(parsed_header.value().base_nonce, 0U);
    if (!nonce) {
      return clonecore::Status::failure(nonce.error());
    }
    const auto aad = canonical_metadata_aad(header_bytes.value());
    auto plaintext = decrypt_tsumugi_aes256_gcm(
        *encryption_key,
        nonce.value(),
        aad,
        metadata.value(),
        parsed_header.value().metadata_tag);
    if (!metadata.value().empty()) {
      SecureZeroMemory(metadata.value().data(), metadata.value().size());
    }
    if (!plaintext) {
      return clonecore::Status::failure(plaintext.error());
    }
    metadata = std::move(plaintext);
  } else if (encryption_key != nullptr || request.encryption.has_value()) {
    return clonecore::Status::failure(stream_error(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_STATE,
        L"Tsumugi高速検証暗号状態",
        L"ヘッダーと作成要求の暗号状態が一致しません"));
  }

  auto parsed_metadata = parse_metadata(
      metadata.value(), parsed_header.value(), encrypted);
  if (!parsed_metadata) {
    return clonecore::Status::failure(parsed_metadata.error());
  }
  if (parsed_metadata.value().manifest != request.manifest ||
      parsed_metadata.value().records.size() != request.chunks.size()) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi高速検証メタデータ照合",
        L"認証済みマニフェストまたはチャンク件数が作成要求と一致しません"));
  }

  auto footer = read_exact(
      handle,
      parsed_header.value().footer.offset,
      kTsumugiFooterSize,
      L"Tsumugi高速検証フッター読戻し");
  if (!footer) {
    return clonecore::Status::failure(footer.error());
  }
  if (footer.value().size() != expected_footer.size() ||
      !std::equal(
          expected_footer.begin(),
          expected_footer.end(),
          footer.value().begin())) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi高速検証フッター読戻し",
        L"最終フッターの読戻し内容が書込み期待値と一致しません"));
  }
  Sha256Digest stored_global_hash{};
  std::copy_n(
      footer.value().begin() + 16U,
      stored_global_hash.size(),
      stored_global_hash.begin());
  if (stored_global_hash != expected_global_hash ||
      !std::equal(
          kFooterMagic.begin(), kFooterMagic.end(), footer.value().begin()) ||
      read_little<std::uint64_t>(footer.value(), 8U) != image_length ||
      !std::equal(
          request.image_id.begin(),
          request.image_id.end(),
          footer.value().begin() + 48U)) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi高速検証フッター照合",
        L"全体Hash、完成長、または画像識別子が作成結果と一致しません"));
  }

  auto after = identity_from_handle(
      handle, L"Tsumugi高速検証後の未完了ファイル再識別");
  if (!after) {
    return clonecore::Status::failure(after.error());
  }
  if (!same_observation(before.value(), after.value())) {
    return clonecore::Status::failure(verification_error(
        L"Tsumugi高速検証中の未完了ファイル再識別",
        L"高速検証中にファイル識別子、長さ、または更新時刻が変化しました"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> decode_chunk(
    const HANDLE handle,
    const TsumugiHeader& header,
    const TsumugiChunkRecord& record,
    const std::uint64_t index,
    const TsumugiKey* key) {
  if (chunk_is_zero(record.flags)) {
    return clonecore::Result<std::vector<std::byte>>::success({});
  }
  auto stored = read_exact(
      handle,
      record.stored_offset,
      static_cast<std::size_t>(record.stored_length),
      L"Tsumugiペイロード読取り");
  if (!stored) {
    return clonecore::Result<std::vector<std::byte>>::failure(stored.error());
  }
  const bool encrypted =
      (header.required_features & kFeatureEncrypted) != 0U;
  if (encrypted) {
    if (key == nullptr) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          stream_error(
              clonecore::ErrorCode::access_denied,
              ERROR_PASSWORD_RESTRICTION,
              L"Tsumugi暗号チャンク鍵",
              L"暗号チャンクの復号鍵がありません"));
    }
    auto nonce = nonce_for_counter(header.base_nonce, record.nonce_counter);
    if (!nonce) {
      return clonecore::Result<std::vector<std::byte>>::failure(nonce.error());
    }
    const auto aad = build_chunk_aad(header.image_id, index, record);
    auto plaintext = decrypt_tsumugi_aes256_gcm(
        *key,
        nonce.value(),
        aad,
        stored.value(),
        record.authentication_tag);
    if (!stored.value().empty()) {
      SecureZeroMemory(stored.value().data(), stored.value().size());
    }
    if (!plaintext) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          plaintext.error());
    }
    stored = std::move(plaintext);
  }

  std::vector<std::byte> plaintext;
  if (record.compression == ImageCompression::zstandard) {
    auto decompressed = decompress_zstandard_image_chunk_v1(
        stored.value(), static_cast<std::size_t>(record.logical_length));
    if (!decompressed) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          decompressed.error());
    }
    plaintext = decompressed.take_value();
  } else {
    if (stored.value().size() != record.logical_length) {
      return clonecore::Result<std::vector<std::byte>>::failure(data_error(
          L"Tsumugi非圧縮チャンク長",
          L"非圧縮データ長が論理チャンク長と一致しません"));
    }
    plaintext = stored.take_value();
  }
  auto digest = sha256(plaintext);
  if (!digest || digest.value() != record.plaintext_hash) {
    if (!plaintext.empty()) {
      SecureZeroMemory(plaintext.data(), plaintext.size());
    }
    return clonecore::Result<std::vector<std::byte>>::failure(
        digest ? verification_error(
                     L"TsumugiチャンクHash",
                     L"復号・展開後のチャンクHashが一致しません")
               : digest.error());
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(plaintext));
}

clonecore::Result<VerifiedHandle> verify_open_handle(
    const HANDLE handle,
    const std::uint64_t image_length,
    const std::optional<std::string_view> password,
    const std::size_t block_bytes,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (image_length < kTsumugiHeaderSize + kTsumugiFooterSize) {
    return clonecore::Result<VerifiedHandle>::failure(data_error(
        L"Tsumugi固定領域",
        L"画像が固定ヘッダーとフッターより短いです"));
  }
  auto header_bytes = read_exact(
      handle, 0U, kTsumugiHeaderSize, L"Tsumugiヘッダー読取り");
  if (!header_bytes) {
    return clonecore::Result<VerifiedHandle>::failure(header_bytes.error());
  }
  auto parsed_header = parse_header_bytes(header_bytes.value());
  if (!parsed_header) {
    return clonecore::Result<VerifiedHandle>::failure(parsed_header.error());
  }
  TsumugiHeader header = parsed_header.take_value();
  auto sections = validate_sections(header, image_length);
  if (!sections) {
    return clonecore::Result<VerifiedHandle>::failure(sections.error());
  }

  auto footer = read_exact(
      handle,
      header.footer.offset,
      kTsumugiFooterSize,
      L"Tsumugiフッター読取り");
  if (!footer) {
    return clonecore::Result<VerifiedHandle>::failure(footer.error());
  }
  if (!std::equal(
          kFooterMagic.begin(), kFooterMagic.end(), footer.value().begin()) ||
      read_little<std::uint64_t>(footer.value(), 8U) != image_length ||
      !std::equal(
          header.image_id.begin(),
          header.image_id.end(),
          footer.value().begin() + 48U)) {
    return clonecore::Result<VerifiedHandle>::failure(data_error(
        L"Tsumugi v1フッター",
        L"フッターのマジック、全長、または画像識別子が一致しません"));
  }
  Sha256Digest stored_global_hash{};
  std::copy_n(
      footer.value().begin() + 16U,
      stored_global_hash.size(),
      stored_global_hash.begin());

  auto metadata = read_exact(
      handle,
      header.metadata.offset,
      static_cast<std::size_t>(header.metadata.length),
      L"Tsumugiメタデータ読取り");
  if (!metadata) {
    return clonecore::Result<VerifiedHandle>::failure(metadata.error());
  }
  const bool encrypted =
      (header.required_features & kFeatureEncrypted) != 0U;
  std::optional<TsumugiKey> key;
  if (encrypted) {
    if (!password.has_value()) {
      return clonecore::Result<VerifiedHandle>::failure(stream_error(
          clonecore::ErrorCode::access_denied,
          ERROR_PASSWORD_RESTRICTION,
          L"Tsumugi暗号画像のパスワード",
          L"暗号化された画像を開くにはパスワードが必要です"));
    }
    auto derived = derive_tsumugi_key_argon2id(password.value(), header.argon2);
    if (!derived) {
      return clonecore::Result<VerifiedHandle>::failure(derived.error());
    }
    key.emplace(derived.take_value());
    auto nonce = nonce_for_counter(header.base_nonce, 0U);
    if (!nonce) {
      return clonecore::Result<VerifiedHandle>::failure(nonce.error());
    }
    const auto aad = canonical_metadata_aad(header_bytes.value());
    auto plaintext = decrypt_tsumugi_aes256_gcm(
        *key,
        nonce.value(),
        aad,
        metadata.value(),
        header.metadata_tag);
    if (!metadata.value().empty()) {
      SecureZeroMemory(metadata.value().data(), metadata.value().size());
    }
    if (!plaintext) {
      return clonecore::Result<VerifiedHandle>::failure(plaintext.error());
    }
    metadata = std::move(plaintext);
  }

  auto parsed_metadata = parse_metadata(metadata.value(), header, encrypted);
  if (!parsed_metadata) {
    return clonecore::Result<VerifiedHandle>::failure(
        parsed_metadata.error());
  }

  // Authenticate the bounded metadata first so a missing or wrong password is
  // rejected without scanning a TB-scale payload. Restoration is still gated
  // on both the following whole-file hash and every plaintext chunk hash.
  std::uint64_t globally_verified = 0U;
  std::uint64_t globally_verified_blocks = 0U;
  auto global_hash = sha256_from_reader(
      header.footer.offset,
      block_bytes,
      [&](const std::uint64_t offset, const std::size_t length) {
        if (clonecore::disk_operation_cancellation_requested(callbacks)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              cancelled_error(L"Tsumugi全体Hash検証"));
        }
        auto bytes = read_exact(
            handle, offset, length, L"Tsumugi全体Hash読戻し");
        if (bytes) {
          globally_verified += length;
          ++globally_verified_blocks;
          clonecore::report_disk_operation_progress(
              callbacks,
              clonecore::DiskOperationProgress{
                  .stage =
                      clonecore::DiskOperationStage::verifying_final,
                  .total_verify_bytes =
                      header.footer.offset + header.source_disk_size,
                  .verified_bytes = globally_verified,
                  .cancellation_allowed = true,
                  .pause_allowed = true,
              });
          const auto pause_status = stop_at_safe_boundary(
              callbacks,
              clonecore::DiskOperationSafeBoundary{
                  .kind = clonecore::DiskOperationSafeBoundaryKind::
                      read_only_block,
                  .stage = clonecore::DiskOperationStage::verifying_final,
                  .completed_bytes = globally_verified,
                  .completed_units = globally_verified_blocks,
              },
              L"Tsumugi全体Hash安全境界");
          if (!pause_status) {
            if (!bytes.value().empty()) {
              SecureZeroMemory(bytes.value().data(), bytes.value().size());
            }
            return clonecore::Result<std::vector<std::byte>>::failure(
                pause_status.error());
          }
        }
        return bytes;
      });
  if (!global_hash || global_hash.value() != stored_global_hash) {
    return clonecore::Result<VerifiedHandle>::failure(
        global_hash ? verification_error(
                          L"Tsumugi全体Hash",
                          L"ファイル全体のHashが一致しません")
                    : global_hash.error());
  }
  std::uint64_t chunks_verified = 0U;
  for (std::uint64_t index = 0U;
       index < parsed_metadata.value().records.size();
       ++index) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return clonecore::Result<VerifiedHandle>::failure(
          cancelled_error(L"Tsumugiチャンク完全検証"));
    }
    const auto& record = parsed_metadata.value().records[
        static_cast<std::size_t>(index)];
    if (chunk_is_zero(record.flags)) {
      auto digest = sha256_zeroes(record.logical_length);
      if (!digest || digest.value() != record.plaintext_hash) {
        return clonecore::Result<VerifiedHandle>::failure(
            digest ? verification_error(
                         L"TsumugiゼロチャンクHash",
                         L"ゼロ埋め範囲のHashが一致しません")
                   : digest.error());
      }
    } else {
      auto plaintext = decode_chunk(
          handle, header, record, index, key ? &*key : nullptr);
      if (!plaintext) {
        return clonecore::Result<VerifiedHandle>::failure(plaintext.error());
      }
      if (!plaintext.value().empty()) {
        SecureZeroMemory(
            plaintext.value().data(), plaintext.value().size());
      }
    }
    chunks_verified += record.logical_length;
    clonecore::report_disk_operation_progress(
        callbacks,
        clonecore::DiskOperationProgress{
            .stage = clonecore::DiskOperationStage::verifying_final,
            .total_verify_bytes =
                header.footer.offset + header.source_disk_size,
            .verified_bytes = globally_verified + chunks_verified,
            .cancellation_allowed = true,
            .pause_allowed = true,
        });
    const auto pause_status = stop_at_safe_boundary(
        callbacks,
        clonecore::DiskOperationSafeBoundary{
            .kind =
                clonecore::DiskOperationSafeBoundaryKind::verified_chunk,
            .stage = clonecore::DiskOperationStage::verifying_final,
            .completed_bytes = globally_verified + chunks_verified,
            .completed_units = index + 1U,
        },
        L"Tsumugiチャンク完全検証安全境界");
    if (!pause_status) {
      return clonecore::Result<VerifiedHandle>::failure(
          pause_status.error());
    }
  }

  TsumugiStreamInspection inspection{};
  inspection.header = header;
  inspection.manifest = std::move(parsed_metadata.value().manifest);
  inspection.records = std::move(parsed_metadata.value().records);
  inspection.global_hash = global_hash.value();
  inspection.header_hash_verified = true;
  inspection.metadata_authenticated = encrypted;
  inspection.all_chunks_verified = true;
  inspection.global_hash_verified = true;
  return clonecore::Result<VerifiedHandle>::success(VerifiedHandle{
      .inspection = std::move(inspection),
      .key = std::move(key),
      .image_length = image_length,
  });
}

clonecore::Result<std::pair<clonecore::UniqueHandle, FileIdentity>>
open_input_file(const std::wstring& canonical) {
  clonecore::UniqueHandle handle(CreateFileW(
      extended_path(canonical).c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
          FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!handle) {
    return clonecore::Result<
        std::pair<clonecore::UniqueHandle, FileIdentity>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"Tsumugi画像を読取り専用で開く",
            GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(
          handle.get(), FileAttributeTagInfo, &tag, sizeof(tag))) {
    return clonecore::Result<
        std::pair<clonecore::UniqueHandle, FileIdentity>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"Tsumugi入力ファイル属性取得",
            GetLastError()));
  }
  if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return clonecore::Result<
        std::pair<clonecore::UniqueHandle, FileIdentity>>::failure(
        unsupported_error(
            L"Tsumugi入力ファイルreparse検証",
            L"reparse pointまたはディレクトリは画像入力に使用できません"));
  }
  auto identity = identity_from_handle(
      handle.get(), L"Tsumugi入力ファイル識別");
  if (!identity) {
    return clonecore::Result<
        std::pair<clonecore::UniqueHandle, FileIdentity>>::failure(
        identity.error());
  }
  return clonecore::Result<
      std::pair<clonecore::UniqueHandle, FileIdentity>>::success(
      std::pair<clonecore::UniqueHandle, FileIdentity>{
          std::move(handle), identity.take_value()});
}

}  // namespace
}  // namespace ytec::imageformat

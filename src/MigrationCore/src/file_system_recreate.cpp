#include "ytec/migrationcore/file_system_recreate.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::migrationcore {
namespace {

constexpr std::uint64_t kKiB = 1024ULL;
constexpr std::uint64_t kMiB = 1024ULL * kKiB;
constexpr std::uint64_t kFatEpochFileTime = 119'600'064'000'000'000ULL;
constexpr std::uint64_t kFatEndExclusiveFileTime =
    159'992'928'000'000'000ULL;
constexpr std::uint64_t kFat32MinimumDataClusters = 65'525ULL;
constexpr std::uint64_t kFat32MaximumDataClusters = 0x0FFF'FFF5ULL;
constexpr std::uint64_t kExfatMaximumDataClusters = 0xFFFF'FFF5ULL;
constexpr std::uint64_t kFat32MaximumClusterSize = 64ULL * kKiB;
constexpr std::uint64_t kExfatMaximumClusterSize = 2ULL * kMiB;
constexpr std::uint64_t kMinimumFreeReserveBytes = 16ULL * kMiB;
constexpr std::uint64_t kExfatUpcaseAndSystemReserveBytes = 256ULL * kKiB;

clonecore::Error recreate_error(
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
  return clonecore::Result<T>::failure(recreate_error(
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(recreate_error(
      code, native_code, std::move(operation), std::move(message)));
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

clonecore::Result<std::uint64_t> align_up(
    const std::uint64_t value,
    const std::uint64_t alignment,
    const std::wstring_view operation) {
  if (alignment == 0U ||
      value > (std::numeric_limits<std::uint64_t>::max)() -
          (alignment - 1U)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"容量の境界整列がオーバーフローしました");
  }
  return clonecore::Result<std::uint64_t>::success(
      ((value + alignment - 1U) / alignment) * alignment);
}

bool is_power_of_two(const std::uint64_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

bool is_supported_file_system(
    const MigrationFileSystem file_system) noexcept {
  return file_system == MigrationFileSystem::fat32 ||
      file_system == MigrationFileSystem::exfat;
}

bool is_zero_digest(const FileSystemRecreateSha256& digest) noexcept {
  return std::all_of(
      digest.begin(), digest.end(), [](const std::byte value) {
        return value == std::byte{0};
      });
}

constexpr FileSystemRecreateSha256 kEmptySha256{
    std::byte{0xE3}, std::byte{0xB0}, std::byte{0xC4}, std::byte{0x42},
    std::byte{0x98}, std::byte{0xFC}, std::byte{0x1C}, std::byte{0x14},
    std::byte{0x9A}, std::byte{0xFB}, std::byte{0xF4}, std::byte{0xC8},
    std::byte{0x99}, std::byte{0x6F}, std::byte{0xB9}, std::byte{0x24},
    std::byte{0x27}, std::byte{0xAE}, std::byte{0x41}, std::byte{0xE4},
    std::byte{0x64}, std::byte{0x9B}, std::byte{0x93}, std::byte{0x4C},
    std::byte{0xA4}, std::byte{0x95}, std::byte{0x99}, std::byte{0x1B},
    std::byte{0x78}, std::byte{0x52}, std::byte{0xB8}, std::byte{0x55},
};

bool valid_timestamp(const std::uint64_t value) noexcept {
  return value >= kFatEpochFileTime &&
      value < kFatEndExclusiveFileTime &&
      (value - kFatEpochFileTime) %
              kFileSystemRecreateTimestampQuantum100ns ==
          0U;
}

bool is_high_surrogate(const wchar_t value) noexcept {
  const auto code = static_cast<std::uint16_t>(value);
  return code >= 0xD800U && code <= 0xDBFFU;
}

bool is_low_surrogate(const wchar_t value) noexcept {
  const auto code = static_cast<std::uint16_t>(value);
  return code >= 0xDC00U && code <= 0xDFFFU;
}

bool is_forbidden_name_character(const wchar_t value) noexcept {
  const auto code = static_cast<std::uint16_t>(value);
  return code < 0x20U || value == L'"' || value == L'*' ||
      value == L'/' || value == L':' || value == L'<' || value == L'>' ||
      value == L'?' || value == L'\\' || value == L'|';
}

wchar_t upper_ascii(const wchar_t value) noexcept {
  return value >= L'a' && value <= L'z'
      ? static_cast<wchar_t>(value - (L'a' - L'A'))
      : value;
}

bool is_reserved_dos_component(const std::wstring_view component) noexcept {
  const std::size_t dot = component.find(L'.');
  const std::wstring_view base = component.substr(0U, dot);
  std::array<wchar_t, 4U> uppercase{};
  if (base.size() > uppercase.size()) {
    return false;
  }
  for (std::size_t index = 0; index < base.size(); ++index) {
    uppercase[index] = upper_ascii(base[index]);
  }
  const std::wstring_view normalized(uppercase.data(), base.size());
  if (normalized == L"CON" || normalized == L"PRN" ||
      normalized == L"AUX" || normalized == L"NUL") {
    return true;
  }
  if (normalized.size() == 4U &&
      ((normalized.substr(0U, 3U) == L"COM") ||
       (normalized.substr(0U, 3U) == L"LPT"))) {
    const wchar_t suffix = normalized[3U];
    return (suffix >= L'1' && suffix <= L'9') || suffix == L'\u00B9' ||
        suffix == L'\u00B2' || suffix == L'\u00B3';
  }
  return false;
}

clonecore::Status validate_path_component(
    const std::wstring_view component,
    const FileSystemRecreateFormatGeometry& geometry) {
  if (component.empty() ||
      component.size() > geometry.maximum_component_utf16_units ||
      component == L"." || component == L".." ||
      component.back() == L'.' || component.back() == L' ' ||
      is_reserved_dos_component(component)) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_INVALID_NAME,
        L"再作成ツリーの名前",
        L"空要素、予約名、末尾の点／空白、または長すぎる名前は扱えません");
  }

  for (std::size_t index = 0; index < component.size(); ++index) {
    const wchar_t value = component[index];
    if (is_forbidden_name_character(value)) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_NAME,
          L"再作成ツリーの名前",
          L"代替ストリームまたはWindowsで扱えない名前文字を含みます");
    }
    if (is_high_surrogate(value)) {
      if (index + 1U >= component.size() ||
          !is_low_surrogate(component[index + 1U])) {
        return status_failure(
            clonecore::ErrorCode::invalid_data,
            ERROR_NO_UNICODE_TRANSLATION,
            L"再作成ツリーのUTF-16",
            L"UTF-16サロゲートペアが不正です");
      }
      ++index;
    } else if (is_low_surrogate(value)) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_NO_UNICODE_TRANSLATION,
          L"再作成ツリーのUTF-16",
          L"UTF-16サロゲートペアが不正です");
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_relative_path(
    const std::wstring& path,
    const FileSystemRecreateFormatGeometry& geometry) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  if (path.empty() || path.size() > geometry.maximum_path_utf16_units ||
      path.front() == L'\\' || path.back() == L'\\') {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_INVALID_NAME,
        L"再作成ツリーの相対パス",
        L"ルート相対の正規UTF-16パスではありません");
  }

  std::size_t begin = 0U;
  while (begin < path.size()) {
    const std::size_t separator = path.find(L'\\', begin);
    const std::size_t end = separator == std::wstring::npos
        ? path.size()
        : separator;
    const auto valid = validate_path_component(
        std::wstring_view(path).substr(begin, end - begin), geometry);
    if (!valid) {
      return valid;
    }
    if (separator == std::wstring::npos) {
      break;
    }
    begin = separator + 1U;
  }
  return clonecore::success_status();
}

clonecore::Result<int> compare_paths_case_insensitive(
    const std::wstring& left,
    const std::wstring& right) {
  if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
      right.size() >
          static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return failure<int>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成ツリーのパス順序",
        L"比較できるパス長を超えています");
  }
  const int comparison = CompareStringOrdinal(
      left.data(),
      static_cast<int>(left.size()),
      right.data(),
      static_cast<int>(right.size()),
      TRUE);
  if (comparison == 0) {
    return failure<int>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"再作成ツリーのパス順序",
        L"大文字小文字を区別しないパス比較に失敗しました");
  }
  return clonecore::Result<int>::success(comparison);
}

clonecore::Status validate_geometry(
    const FileSystemRecreateFormatGeometry& geometry) {
  const std::uint64_t maximum_cluster_size =
      geometry.file_system == MigrationFileSystem::fat32
      ? kFat32MaximumClusterSize
      : kExfatMaximumClusterSize;
  if (!is_supported_file_system(geometry.file_system) ||
      geometry.target_volume_bytes == 0U ||
      geometry.target_volume_bytes > kExfatMaximumRecreatedFileBytes ||
      (geometry.logical_sector_size != 512U &&
       geometry.logical_sector_size != 4096U) ||
      !is_power_of_two(geometry.cluster_size) ||
      geometry.cluster_size < geometry.logical_sector_size ||
      geometry.cluster_size > maximum_cluster_size ||
      geometry.cluster_size % geometry.logical_sector_size != 0U ||
      geometry.target_volume_bytes % geometry.logical_sector_size != 0U ||
      geometry.maximum_component_utf16_units == 0U ||
      geometry.maximum_component_utf16_units >
          kMaximumFileSystemRecreateComponentUtf16Units ||
      geometry.maximum_path_utf16_units <
          geometry.maximum_component_utf16_units ||
      geometry.maximum_path_utf16_units >
          kMaximumFileSystemRecreatePathUtf16Units) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成ファイルシステムのgeometry",
        L"FAT32／exFATの容量、セクター、クラスタ、または名前制約が対応範囲外です");
  }

  const std::uint64_t maximum_clusters =
      geometry.target_volume_bytes / geometry.cluster_size;
  const std::uint64_t filesystem_limit =
      geometry.file_system == MigrationFileSystem::fat32
      ? kFat32MaximumDataClusters
      : kExfatMaximumDataClusters;
  if (maximum_clusters > filesystem_limit) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"再作成ファイルシステムのクラスタ数",
        L"コピー先容量とクラスタサイズの組合せが形式上限を超えます");
  }
  return clonecore::success_status();
}

clonecore::Status validate_tree(
    const CanonicalFileSystemTree& tree,
    const FileSystemRecreateFormatGeometry& geometry) {
  const auto geometry_valid = validate_geometry(geometry);
  if (!geometry_valid) {
    return geometry_valid;
  }
  if (tree.file_system != geometry.file_system ||
      tree.source_table_index == 0U ||
      tree.entries.size() > kMaximumFileSystemRecreateEntries ||
      is_zero_digest(tree.enumeration_epoch_sha256) ||
      !tree.namespace_fully_enumerated || !tree.opened_handles_only ||
      !tree.every_regular_file_hashed_to_stable_eof ||
      !tree.short_name_aliases_collision_free) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"再作成ツリーの列挙証跡",
        L"形式一致、正の元区画番号、完全列挙、opened-handle、安定EOF、短名衝突なしの証跡が不足しています");
  }

  const std::uint64_t maximum_file_bytes =
      tree.file_system == MigrationFileSystem::fat32
      ? kFat32MaximumRecreatedFileBytes
      : kExfatMaximumRecreatedFileBytes;
  std::set<std::wstring> known_paths;
  std::set<std::wstring> directory_paths;
  const CanonicalFileSystemTreeEntry* previous = nullptr;
  for (const auto& entry : tree.entries) {
    const auto path_valid = validate_relative_path(entry.relative_path, geometry);
    if (!path_valid) {
      return path_valid;
    }
    if (previous != nullptr) {
      const auto comparison = compare_paths_case_insensitive(
          previous->relative_path, entry.relative_path);
      if (!comparison) {
        return clonecore::Status::failure(comparison.error());
      }
      if (comparison.value() == CSTR_EQUAL) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"再作成ツリーの名前衝突",
            L"同一名または大文字小文字だけが異なる名前が重複しています");
      }
      if (comparison.value() != CSTR_LESS_THAN) {
        return status_failure(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"再作成ツリーの正規順序",
            L"ツリー項目が大文字小文字を区別しない正規昇順ではありません");
      }
    }

    const std::size_t separator = entry.relative_path.rfind(L'\\');
    if (separator != std::wstring::npos) {
      const std::wstring parent = entry.relative_path.substr(0U, separator);
      if (!known_paths.contains(parent) ||
          !directory_paths.contains(parent)) {
        return status_failure(
            clonecore::ErrorCode::invalid_data,
            ERROR_PATH_NOT_FOUND,
            L"再作成ツリーの親ディレクトリ",
            L"親ディレクトリが正規順序で先に定義されていません");
      }
    }

    const bool kind_valid =
        entry.kind == FileSystemRecreateEntryKind::directory ||
        entry.kind == FileSystemRecreateEntryKind::regular_file;
    const bool common_evidence_valid =
        entry.hard_link_count == 1U &&
        entry.alternate_data_stream_count == 0U &&
        entry.reparse_tag == 0U && entry.opened_handle_identity_stable &&
        entry.namespace_supported &&
        (entry.portable_attributes &
         ~kFileSystemRecreatePortableAttributeMask) == 0U &&
        valid_timestamp(entry.creation_time_utc_100ns) &&
        valid_timestamp(entry.last_write_time_utc_100ns);
    const bool directory_valid =
        entry.kind != FileSystemRecreateEntryKind::directory ||
        (entry.size_bytes == 0U && is_zero_digest(entry.content_sha256) &&
         !entry.unnamed_stream_hashed_to_stable_eof);
    const bool regular_file_valid =
        entry.kind != FileSystemRecreateEntryKind::regular_file ||
        (entry.size_bytes <= maximum_file_bytes &&
         entry.unnamed_stream_hashed_to_stable_eof &&
         (entry.size_bytes == 0U
              ? entry.content_sha256 == kEmptySha256
              : !is_zero_digest(entry.content_sha256)));
    if (!kind_valid || !common_evidence_valid || !directory_valid ||
        !regular_file_valid) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"再作成ツリーの項目証跡",
          L"通常ファイル／ディレクトリ、表現可能な属性・時刻、単一link、ADS・reparseなし、安定EOFの条件を満たしません");
    }

    known_paths.insert(entry.relative_path);
    if (entry.kind == FileSystemRecreateEntryKind::directory) {
      directory_paths.insert(entry.relative_path);
    }
    previous = &entry;
  }
  return clonecore::success_status();
}

class AlgorithmHandle final {
 public:
  AlgorithmHandle() = default;
  ~AlgorithmHandle() {
    if (value_ != nullptr) {
      BCryptCloseAlgorithmProvider(value_, 0U);
    }
  }
  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
  AlgorithmHandle(AlgorithmHandle&&) = delete;
  AlgorithmHandle& operator=(AlgorithmHandle&&) = delete;

  [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }

 private:
  BCRYPT_ALG_HANDLE value_{};
};

class HashHandle final {
 public:
  HashHandle() = default;
  ~HashHandle() {
    if (value_ != nullptr) {
      BCryptDestroyHash(value_);
    }
  }
  HashHandle(const HashHandle&) = delete;
  HashHandle& operator=(const HashHandle&) = delete;
  HashHandle(HashHandle&&) = delete;
  HashHandle& operator=(HashHandle&&) = delete;

  [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &value_; }
  [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return value_; }

 private:
  BCRYPT_HASH_HANDLE value_{};
};

class CanonicalSha256 final {
 public:
  [[nodiscard]] clonecore::Status initialize() {
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        algorithm_.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
    if (!BCRYPT_SUCCESS(status) || algorithm_.get() == nullptr) {
      if (BCRYPT_SUCCESS(status)) {
        return invalid_handle_failure();
      }
      return crypto_failure(status);
    }
    ULONG returned{};
    ULONG object_length{};
    status = BCryptGetProperty(
        algorithm_.get(),
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&object_length),
        sizeof(object_length),
        &returned,
        0U);
    if (!BCRYPT_SUCCESS(status) || returned != sizeof(object_length) ||
        object_length == 0U) {
      return crypto_failure(status);
    }
    object_.resize(object_length);
    status = BCryptCreateHash(
        algorithm_.get(),
        hash_.put(),
        object_.data(),
        object_length,
        nullptr,
        0U,
        0U);
    if (!BCRYPT_SUCCESS(status)) {
      return crypto_failure(status);
    }
    return hash_.get() != nullptr ? clonecore::success_status()
                                  : invalid_handle_failure();
  }

  [[nodiscard]] clonecore::Status update(
      const std::span<const std::byte> bytes) {
    if (hash_.get() == nullptr) {
      return invalid_handle_failure();
    }
    std::size_t consumed = 0U;
    while (consumed < bytes.size()) {
      const ULONG amount = static_cast<ULONG>((std::min)(
          bytes.size() - consumed,
          static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
      const NTSTATUS status = BCryptHashData(
          hash_.get(),
          reinterpret_cast<PUCHAR>(
              const_cast<std::byte*>(bytes.data() + consumed)),
          amount,
          0U);
      if (!BCRYPT_SUCCESS(status)) {
        return crypto_failure(status);
      }
      consumed += amount;
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateSha256> finish() {
    if (hash_.get() == nullptr) {
      return clonecore::Result<FileSystemRecreateSha256>::failure(
          invalid_handle_error());
    }
    FileSystemRecreateSha256 digest{};
    const NTSTATUS status = BCryptFinishHash(
        hash_.get(),
        reinterpret_cast<PUCHAR>(digest.data()),
        static_cast<ULONG>(digest.size()),
        0U);
    if (!BCRYPT_SUCCESS(status)) {
      return clonecore::Result<FileSystemRecreateSha256>::failure(
          crypto_error(status));
    }
    return clonecore::Result<FileSystemRecreateSha256>::success(digest);
  }

 private:
  [[nodiscard]] static clonecore::Error crypto_error(
      const NTSTATUS status) {
    return recreate_error(
        clonecore::ErrorCode::verification_failed,
        static_cast<DWORD>(status),
        L"再作成manifest SHA-256",
        L"Windows CNG SHA-256処理に失敗しました");
  }

  [[nodiscard]] static clonecore::Status crypto_failure(
      const NTSTATUS status) {
    return clonecore::Status::failure(crypto_error(status));
  }

  [[nodiscard]] static clonecore::Error invalid_handle_error() {
    return recreate_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_HANDLE,
        L"再作成manifest SHA-256 handle",
        L"Windows CNGが有効なSHA-256 handleを返しませんでした");
  }

  [[nodiscard]] static clonecore::Status invalid_handle_failure() {
    return clonecore::Status::failure(invalid_handle_error());
  }

  AlgorithmHandle algorithm_;
  HashHandle hash_;
  std::vector<UCHAR> object_;
};

class CanonicalHashEncoder final {
 public:
  [[nodiscard]] clonecore::Status initialize(const std::string_view domain) {
    const auto initialized = hash_.initialize();
    if (!initialized) {
      return initialized;
    }
    const auto domain_bytes = std::as_bytes(std::span(domain));
    const auto updated = hash_.update(domain_bytes);
    if (!updated) {
      return updated;
    }
    return u8(0U);
  }

  [[nodiscard]] clonecore::Status u8(const std::uint8_t value) {
    const std::array bytes{std::byte{value}};
    return hash_.update(bytes);
  }

  [[nodiscard]] clonecore::Status u32(const std::uint32_t value) {
    const std::array bytes{
        std::byte{static_cast<std::uint8_t>(value)},
        std::byte{static_cast<std::uint8_t>(value >> 8U)},
        std::byte{static_cast<std::uint8_t>(value >> 16U)},
        std::byte{static_cast<std::uint8_t>(value >> 24U)},
    };
    return hash_.update(bytes);
  }

  [[nodiscard]] clonecore::Status u64(const std::uint64_t value) {
    std::array<std::byte, 8U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      bytes[index] = std::byte{
          static_cast<std::uint8_t>(value >> (index * 8U))};
    }
    return hash_.update(bytes);
  }

  [[nodiscard]] clonecore::Status digest(
      const FileSystemRecreateSha256& value) {
    return hash_.update(value);
  }

  [[nodiscard]] clonecore::Status utf16(const std::wstring& value) {
    if (value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"再作成manifest UTF-16",
          L"パス長を正規形式へ格納できません");
    }
    const auto length = u32(static_cast<std::uint32_t>(value.size()));
    if (!length) {
      return length;
    }
    for (const wchar_t character : value) {
      const std::uint16_t unit = static_cast<std::uint16_t>(character);
      const std::array bytes{
          std::byte{static_cast<std::uint8_t>(unit)},
          std::byte{static_cast<std::uint8_t>(unit >> 8U)},
      };
      const auto updated = hash_.update(bytes);
      if (!updated) {
        return updated;
      }
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Result<FileSystemRecreateSha256> finish() {
    return hash_.finish();
  }

 private:
  CanonicalSha256 hash_;
};

clonecore::Result<FileSystemRecreateSha256> hash_validated_tree(
    const CanonicalFileSystemTree& tree) {
  CanonicalHashEncoder encoder;
  auto status = encoder.initialize("YTEC-FS-RECREATE-TREE-V1");
  if (status) {
    status = encoder.u32(1U);
  }
  if (status) {
    status = encoder.u8(static_cast<std::uint8_t>(tree.file_system));
  }
  if (status) {
    status = encoder.u32(tree.source_table_index);
  }
  if (status) {
    status = encoder.u64(static_cast<std::uint64_t>(tree.entries.size()));
  }
  for (const auto& entry : tree.entries) {
    if (!status) {
      break;
    }
    status = encoder.utf16(entry.relative_path);
    if (status) {
      status = encoder.u8(static_cast<std::uint8_t>(entry.kind));
    }
    if (status) {
      status = encoder.u32(entry.portable_attributes);
    }
    if (status) {
      status = encoder.u64(entry.creation_time_utc_100ns);
    }
    if (status) {
      status = encoder.u64(entry.last_write_time_utc_100ns);
    }
    if (status) {
      status = encoder.u64(entry.size_bytes);
    }
    if (status) {
      status = encoder.digest(entry.content_sha256);
    }
  }
  if (!status) {
    return clonecore::Result<FileSystemRecreateSha256>::failure(status.error());
  }
  return encoder.finish();
}

clonecore::Result<std::uint64_t> format_overhead_bytes(
    const FileSystemRecreateFormatGeometry& geometry) {
  const std::uint64_t maximum_clusters =
      geometry.target_volume_bytes / geometry.cluster_size;
  std::uint64_t fat_raw{};
  if (!checked_multiply(maximum_clusters, 4U, fat_raw)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成形式のFAT容量",
        L"FAT容量計算がオーバーフローしました");
  }
  const auto fat = align_up(
      fat_raw, geometry.logical_sector_size, L"再作成形式のFAT整列");
  if (!fat) {
    return fat;
  }

  std::uint64_t boot_bytes{};
  const std::uint64_t boot_sectors =
      geometry.file_system == MigrationFileSystem::fat32 ? 32U : 24U;
  if (!checked_multiply(
          boot_sectors, geometry.logical_sector_size, boot_bytes)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成形式のboot領域",
        L"boot領域容量計算がオーバーフローしました");
  }

  std::uint64_t overhead = boot_bytes;
  if (geometry.file_system == MigrationFileSystem::fat32) {
    std::uint64_t two_fats{};
    if (!checked_multiply(fat.value(), 2U, two_fats) ||
        !checked_add(overhead, two_fats, overhead)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"FAT32再作成形式overhead",
          L"二重FATを含む形式容量がオーバーフローしました");
    }
  } else {
    const std::uint64_t bitmap_raw = maximum_clusters / 8U +
        (maximum_clusters % 8U == 0U ? 0U : 1U);
    const auto bitmap = align_up(
        bitmap_raw,
        geometry.cluster_size,
        L"exFAT allocation bitmap整列");
    const auto system = align_up(
        kExfatUpcaseAndSystemReserveBytes,
        geometry.cluster_size,
        L"exFAT system file整列");
    if (!bitmap || !system || !checked_add(overhead, fat.value(), overhead) ||
        !checked_add(overhead, bitmap.value(), overhead) ||
        !checked_add(overhead, system.value(), overhead)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"exFAT再作成形式overhead",
          L"FAT、bitmap、またはsystem file容量がオーバーフローしました");
    }
  }
  return align_up(
      overhead,
      geometry.cluster_size,
      L"再作成形式overhead整列");
}

std::wstring parent_path(const std::wstring& path) {
  const std::size_t separator = path.rfind(L'\\');
  return separator == std::wstring::npos
      ? std::wstring{}
      : path.substr(0U, separator);
}

std::wstring_view leaf_name(const std::wstring& path) noexcept {
  const std::size_t separator = path.rfind(L'\\');
  return separator == std::wstring::npos
      ? std::wstring_view(path)
      : std::wstring_view(path).substr(separator + 1U);
}

clonecore::Result<std::uint64_t> directory_record_bytes(
    const MigrationFileSystem file_system,
    const std::size_t name_utf16_units) {
  const std::uint64_t units = static_cast<std::uint64_t>(name_utf16_units);
  const std::uint64_t secondary_units =
      file_system == MigrationFileSystem::fat32 ? 13U : 15U;
  const std::uint64_t fixed_records =
      file_system == MigrationFileSystem::fat32 ? 1U : 2U;
  const std::uint64_t variable_records =
      units / secondary_units + (units % secondary_units == 0U ? 0U : 1U);
  std::uint64_t records{};
  std::uint64_t bytes{};
  if (!checked_add(fixed_records, variable_records, records) ||
      !checked_multiply(records, 32U, bytes)) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成directory record",
        L"名前のdirectory record容量がオーバーフローしました");
  }
  return clonecore::Result<std::uint64_t>::success(bytes);
}

clonecore::Result<FileSystemRecreateCapacity> plan_capacity(
    const CanonicalFileSystemTree& tree,
    const FileSystemRecreateFormatGeometry& geometry) {
  const auto overhead = format_overhead_bytes(geometry);
  if (!overhead) {
    return clonecore::Result<FileSystemRecreateCapacity>::failure(
        overhead.error());
  }

  // Every directory also needs an end-of-directory record.  FAT32 child
  // directories additionally need the conservative '.' and '..' records.
  constexpr std::uint64_t kEndOfDirectoryRecordBytes = 32U;
  std::map<std::wstring, std::uint64_t> records_by_directory;
  records_by_directory.emplace(L"", kEndOfDirectoryRecordBytes);
  FileSystemRecreateCapacity capacity{
      .namespace_record_bytes = kEndOfDirectoryRecordBytes,
      .conservative_format_overhead_bytes = overhead.value(),
  };

  for (const auto& entry : tree.entries) {
    const auto record_bytes = directory_record_bytes(
        tree.file_system, leaf_name(entry.relative_path).size());
    if (!record_bytes) {
      return clonecore::Result<FileSystemRecreateCapacity>::failure(
          record_bytes.error());
    }
    const std::wstring parent = parent_path(entry.relative_path);
    auto parent_records = records_by_directory.find(parent);
    if (parent_records == records_by_directory.end() ||
        !checked_add(
            parent_records->second,
            record_bytes.value(),
            parent_records->second) ||
        !checked_add(
            capacity.namespace_record_bytes,
            record_bytes.value(),
            capacity.namespace_record_bytes)) {
      return failure<FileSystemRecreateCapacity>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"再作成namespace容量",
          L"directory recordの合計がオーバーフローしました");
    }

    if (entry.kind == FileSystemRecreateEntryKind::directory) {
      const std::uint64_t initial_records =
          kEndOfDirectoryRecordBytes +
          (tree.file_system == MigrationFileSystem::fat32 ? 64U : 0U);
      records_by_directory.emplace(entry.relative_path, initial_records);
      if (!checked_add(
              capacity.namespace_record_bytes,
              initial_records,
              capacity.namespace_record_bytes)) {
        return failure<FileSystemRecreateCapacity>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"再作成directory metadata容量",
            L"directory metadata容量がオーバーフローしました");
      }
    } else {
      const auto allocated = align_up(
          entry.size_bytes,
          geometry.cluster_size,
          L"再作成通常ファイルcluster整列");
      if (!allocated ||
          !checked_add(
              capacity.total_content_bytes,
              entry.size_bytes,
              capacity.total_content_bytes) ||
          !checked_add(
              capacity.regular_file_allocation_bytes,
              allocated.value(),
              capacity.regular_file_allocation_bytes)) {
        return failure<FileSystemRecreateCapacity>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"再作成通常ファイル容量",
            L"通常ファイルの内容またはcluster割当がオーバーフローしました");
      }
    }
  }

  for (const auto& [path, record_bytes] : records_by_directory) {
    static_cast<void>(path);
    const auto allocated = align_up(
        (std::max)(record_bytes, 1ULL),
        geometry.cluster_size,
        L"再作成directory cluster整列");
    if (!allocated ||
        !checked_add(
            capacity.directory_allocation_bytes,
            allocated.value(),
            capacity.directory_allocation_bytes)) {
      return failure<FileSystemRecreateCapacity>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"再作成directory割当容量",
          L"directory cluster割当がオーバーフローしました");
    }
  }

  std::uint64_t payload_allocation{};
  if (!checked_add(
          capacity.regular_file_allocation_bytes,
          capacity.directory_allocation_bytes,
          payload_allocation)) {
    return failure<FileSystemRecreateCapacity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成payload容量",
        L"ファイルとdirectoryの割当合計がオーバーフローしました");
  }
  const std::uint64_t proportional_reserve = payload_allocation / 32U +
      (payload_allocation % 32U == 0U ? 0U : 1U);
  const auto reserve = align_up(
      (std::max)(kMinimumFreeReserveBytes, proportional_reserve),
      geometry.cluster_size,
      L"再作成空き容量reserve整列");
  if (!reserve) {
    return clonecore::Result<FileSystemRecreateCapacity>::failure(
        reserve.error());
  }
  capacity.minimum_free_reserve_bytes = reserve.value();

  std::uint64_t required{};
  if (!checked_add(
          capacity.conservative_format_overhead_bytes,
          payload_allocation,
          required) ||
      !checked_add(required, capacity.minimum_free_reserve_bytes, required)) {
    return failure<FileSystemRecreateCapacity>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"再作成最小容量",
        L"形式、payload、空き容量reserveの合計がオーバーフローしました");
  }
  capacity.minimum_required_volume_bytes = required;

  if (required > geometry.target_volume_bytes) {
    return failure<FileSystemRecreateCapacity>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"再作成コピー先容量",
        L"形式overhead、全ファイル、directory record、cluster丸め、安全な空き容量を確保できません");
  }

  const std::uint64_t usable_clusters =
      (geometry.target_volume_bytes -
       capacity.conservative_format_overhead_bytes) /
      geometry.cluster_size;
  if (geometry.file_system == MigrationFileSystem::fat32 &&
      (usable_clusters < kFat32MinimumDataClusters ||
       usable_clusters > kFat32MaximumDataClusters)) {
    return failure<FileSystemRecreateCapacity>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"FAT32再作成cluster範囲",
        L"保守的形式overhead控除後のcluster数がFAT32範囲外です");
  }
  return clonecore::Result<FileSystemRecreateCapacity>::success(capacity);
}

clonecore::Result<FileSystemRecreateSha256> hash_plan(
    const FileSystemRecreatePlan& plan) {
  CanonicalHashEncoder encoder;
  auto status = encoder.initialize("YTEC-FS-RECREATE-PLAN-V1");
  if (status) {
    status = encoder.u32(1U);
  }
  if (status) {
    status = encoder.u32(plan.source_table_index());
  }
  if (status) {
    status = encoder.u32(plan.target_partition_number());
  }
  if (status) {
    status = encoder.u64(plan.target_partition_offset_bytes());
  }
  const auto& geometry = plan.target_geometry();
  if (status) {
    status = encoder.u8(static_cast<std::uint8_t>(geometry.file_system));
  }
  if (status) {
    status = encoder.u64(geometry.target_volume_bytes);
  }
  if (status) {
    status = encoder.u32(geometry.logical_sector_size);
  }
  if (status) {
    status = encoder.u64(geometry.cluster_size);
  }
  if (status) {
    status = encoder.u32(geometry.maximum_path_utf16_units);
  }
  if (status) {
    status = encoder.u32(geometry.maximum_component_utf16_units);
  }
  if (status) {
    status = encoder.digest(plan.source_epoch_sha256());
  }
  if (status) {
    status = encoder.digest(plan.canonical_manifest_sha256());
  }
  const auto& capacity = plan.capacity();
  const std::array capacity_values{
      capacity.total_content_bytes,
      capacity.regular_file_allocation_bytes,
      capacity.directory_allocation_bytes,
      capacity.namespace_record_bytes,
      capacity.conservative_format_overhead_bytes,
      capacity.minimum_free_reserve_bytes,
      capacity.minimum_required_volume_bytes,
  };
  for (const std::uint64_t value : capacity_values) {
    if (status) {
      status = encoder.u64(value);
    }
  }
  if (!status) {
    return clonecore::Result<FileSystemRecreateSha256>::failure(status.error());
  }
  return encoder.finish();
}

bool geometry_equal(
    const FileSystemRecreateFormatGeometry& left,
    const FileSystemRecreateFormatGeometry& right) noexcept {
  return left.file_system == right.file_system &&
      left.target_volume_bytes == right.target_volume_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.cluster_size == right.cluster_size &&
      left.maximum_path_utf16_units == right.maximum_path_utf16_units &&
      left.maximum_component_utf16_units ==
          right.maximum_component_utf16_units;
}

bool entry_equal(
    const CanonicalFileSystemTreeEntry& left,
    const CanonicalFileSystemTreeEntry& right) noexcept {
  return left.relative_path == right.relative_path &&
      left.kind == right.kind && left.size_bytes == right.size_bytes &&
      left.portable_attributes == right.portable_attributes &&
      left.creation_time_utc_100ns == right.creation_time_utc_100ns &&
      left.last_write_time_utc_100ns == right.last_write_time_utc_100ns &&
      left.content_sha256 == right.content_sha256 &&
      left.hard_link_count == right.hard_link_count &&
      left.alternate_data_stream_count ==
          right.alternate_data_stream_count &&
      left.reparse_tag == right.reparse_tag &&
      left.opened_handle_identity_stable ==
          right.opened_handle_identity_stable &&
      left.unnamed_stream_hashed_to_stable_eof ==
          right.unnamed_stream_hashed_to_stable_eof &&
      left.namespace_supported == right.namespace_supported;
}

}  // namespace

clonecore::Result<FileSystemRecreateSha256>
hash_canonical_file_system_tree(
    const CanonicalFileSystemTree& tree,
    const FileSystemRecreateFormatGeometry& geometry) {
  const auto valid = validate_tree(tree, geometry);
  if (!valid) {
    return clonecore::Result<FileSystemRecreateSha256>::failure(valid.error());
  }
  return hash_validated_tree(tree);
}

clonecore::Result<FileSystemRecreatePlan> plan_file_system_recreation(
    const FileSystemRecreatePlanningRequest& request) {
  if (request.target_partition_number == 0U ||
      request.target_partition_offset_bytes == 0U ||
      request.target_geometry.logical_sector_size == 0U ||
      request.target_partition_offset_bytes %
              request.target_geometry.logical_sector_size !=
          0U) {
    return failure<FileSystemRecreatePlan>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"再作成コピー先binding",
        L"コピー先パーティション番号、offset、またはsector整列が不正です");
  }

  const auto manifest_hash = hash_canonical_file_system_tree(
      request.source_tree, request.target_geometry);
  if (!manifest_hash) {
    return clonecore::Result<FileSystemRecreatePlan>::failure(
        manifest_hash.error());
  }
  const auto capacity = plan_capacity(
      request.source_tree, request.target_geometry);
  if (!capacity) {
    return clonecore::Result<FileSystemRecreatePlan>::failure(
        capacity.error());
  }

  FileSystemRecreatePlan plan;
  plan.source_table_index_ = request.source_tree.source_table_index;
  plan.target_partition_number_ = request.target_partition_number;
  plan.target_partition_offset_bytes_ =
      request.target_partition_offset_bytes;
  plan.target_geometry_ = request.target_geometry;
  plan.source_epoch_sha256_ =
      request.source_tree.enumeration_epoch_sha256;
  plan.canonical_manifest_sha256_ = manifest_hash.value();
  plan.capacity_ = capacity.value();
  plan.entries_ = request.source_tree.entries;
  const auto plan_digest = hash_plan(plan);
  if (!plan_digest) {
    return clonecore::Result<FileSystemRecreatePlan>::failure(
        plan_digest.error());
  }
  plan.plan_sha256_ = plan_digest.value();
  return clonecore::Result<FileSystemRecreatePlan>::success(std::move(plan));
}

clonecore::Result<FileSystemRecreateVerification>
verify_recreated_file_system_tree(
    const FileSystemRecreatePlan& plan,
    const FileSystemRecreateTargetReadback& readback) {
  if (readback.target_partition_number != plan.target_partition_number() ||
      readback.target_partition_offset_bytes !=
          plan.target_partition_offset_bytes() ||
      !geometry_equal(readback.actual_geometry, plan.target_geometry()) ||
      readback.target_tree.source_table_index != plan.source_table_index()) {
    return failure<FileSystemRecreateVerification>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"再作成コピー先の読戻しbinding",
        L"読戻したパーティション、offset、形式geometry、または元区画対応が計画と一致しません");
  }

  const auto observed_hash = hash_canonical_file_system_tree(
      readback.target_tree, readback.actual_geometry);
  if (!observed_hash) {
    return clonecore::Result<FileSystemRecreateVerification>::failure(
        observed_hash.error());
  }

  const auto observed_entries = std::span(readback.target_tree.entries);
  if (observed_entries.size() != plan.entries().size()) {
    return failure<FileSystemRecreateVerification>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"再作成ツリーの項目数照合",
        L"コピー先の完全列挙項目数が計画と一致しません");
  }
  for (std::size_t index = 0U; index < observed_entries.size(); ++index) {
    if (!entry_equal(plan.entries()[index], observed_entries[index])) {
      return failure<FileSystemRecreateVerification>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"再作成ツリーの完全照合",
          L"パス、種類、容量、属性、時刻、link状態、または全内容SHA-256が一致しません");
    }
  }
  if (observed_hash.value() != plan.canonical_manifest_sha256()) {
    return failure<FileSystemRecreateVerification>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"再作成manifest SHA-256照合",
        L"項目照合後の正規manifest SHA-256が計画と一致しません");
  }

  FileSystemRecreateVerification verification{
      .expected_manifest_sha256 = plan.canonical_manifest_sha256(),
      .observed_manifest_sha256 = observed_hash.value(),
      .target_epoch_sha256 =
          readback.target_tree.enumeration_epoch_sha256,
      .exact_tree_and_content_equivalence = true,
      .namespace_fully_enumerated =
          readback.target_tree.namespace_fully_enumerated,
      .every_regular_file_hashed_to_stable_eof =
          readback.target_tree.every_regular_file_hashed_to_stable_eof,
  };
  for (const auto& entry : observed_entries) {
    if (entry.kind == FileSystemRecreateEntryKind::directory) {
      ++verification.directory_count;
    } else {
      ++verification.regular_file_count;
      if (!checked_add(
              verification.regular_file_bytes_read,
              entry.size_bytes,
              verification.regular_file_bytes_read)) {
        return failure<FileSystemRecreateVerification>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"再作成読戻し容量",
            L"読戻した通常ファイル容量の合計がオーバーフローしました");
      }
    }
  }
  return clonecore::Result<FileSystemRecreateVerification>::success(
      verification);
}

}  // namespace ytec::migrationcore

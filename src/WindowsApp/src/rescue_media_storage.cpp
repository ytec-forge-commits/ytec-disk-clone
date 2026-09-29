#include "ytec/windowsapp/rescue_media_storage.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ytec::windowsapp {
namespace {

using clonecore::ErrorCode;

constexpr std::size_t kMaximumBootFileCount =
    kRescueUsbMaximumBootTreeEntryCount;
constexpr std::uint64_t kMaximumBootPathCharacters =
    kRescueUsbMaximumBootPathCharacters;
constexpr std::uint64_t kMaximumBootLogicalBytes =
    kRescueUsbMaximumBootLogicalBytes;
constexpr std::size_t kMaximumDataFileCount =
    kRescueUsbMaximumDataTreeEntryCount;
constexpr std::uint64_t kMaximumDataPathCharacters =
    kRescueUsbMaximumDataPathCharacters;
constexpr std::uint64_t kMaximumDataLogicalBytes =
    kRescueUsbMaximumDataLogicalBytes;

clonecore::Error storage_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"レスキューUSBレイアウト／所有権確認",
      .message = std::move(message),
  };
}

template <typename T>
clonecore::Result<T> failure(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Result<T>::failure(
      storage_error(code, native_code, std::move(message)));
}

std::wstring upper_ascii(std::wstring value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return character >= L'a' && character <= L'z'
                   ? static_cast<wchar_t>(character - L'a' + L'A')
                   : character;
      });
  return value;
}

bool equals_ascii_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
         std::equal(
             left.begin(), left.end(), right.begin(),
             [](const wchar_t lhs, const wchar_t rhs) {
               return std::towlower(lhs) == std::towlower(rhs);
             });
}

bool all_zero(const imageformat::Sha256Digest& digest) noexcept {
  return std::all_of(digest.begin(), digest.end(), [](const std::byte value) {
    return value == std::byte{};
  });
}

bool valid_media_id(const std::wstring_view value) noexcept {
  if (value.size() != 36U) {
    return false;
  }
  bool nonzero = false;
  for (std::size_t index = 0U; index < value.size(); ++index) {
    const bool separator =
        index == 8U || index == 13U || index == 18U || index == 23U;
    if (separator) {
      if (value[index] != L'-') {
        return false;
      }
      continue;
    }
    const wchar_t character = value[index];
    if (!((character >= L'0' && character <= L'9') ||
          (character >= L'a' && character <= L'f') ||
          (character >= L'A' && character <= L'F'))) {
      return false;
    }
    nonzero = nonzero || character != L'0';
  }
  return nonzero;
}

bool valid_relative_path(const std::wstring_view path) noexcept {
  if (path.empty() || path.size() >= 32U * 1024U ||
      path.front() == L'\\' || path.front() == L'/' ||
      path.back() == L'\\' || path.back() == L'/' ||
      path.find(L'/') != std::wstring_view::npos ||
      path.find(L':') != std::wstring_view::npos) {
    return false;
  }
  std::size_t start = 0U;
  while (start < path.size()) {
    const std::size_t separator = path.find(L'\\', start);
    const std::size_t end = separator == std::wstring_view::npos
                                ? path.size()
                                : separator;
    const std::wstring_view component = path.substr(start, end - start);
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ' ||
        std::any_of(
            component.begin(), component.end(), [](const wchar_t character) {
              return character < L' ' || character == L'<' ||
                     character == L'>' || character == L'"' ||
                     character == L'|' || character == L'?' ||
                     character == L'*';
            })) {
      return false;
    }
    const std::size_t dot = component.find(L'.');
    const std::wstring basename = upper_ascii(std::wstring(
        component.substr(0U, dot == std::wstring_view::npos
                                 ? component.size()
                                 : dot)));
    const bool numbered_device =
        basename.size() == 4U &&
        (basename.starts_with(L"COM") || basename.starts_with(L"LPT")) &&
        basename[3] >= L'1' && basename[3] <= L'9';
    if (basename == L"CON" || basename == L"PRN" ||
        basename == L"AUX" || basename == L"NUL" ||
        basename == L"CLOCK$" || basename == L"CONIN$" ||
        basename == L"CONOUT$" || numbered_device) {
      return false;
    }
    if (separator == std::wstring_view::npos) {
      break;
    }
    start = separator + 1U;
  }
  return true;
}

clonecore::Status canonicalize_file_tree(
    std::vector<RescueMediaFileFingerprint>& files,
    const std::wstring_view role,
    const std::size_t maximum_file_count,
    const std::uint64_t maximum_path_characters,
    const std::uint64_t maximum_logical_bytes) {
  if (files.size() > maximum_file_count) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::invalid_data,
        ERROR_TOO_MANY_OPEN_FILES,
        std::wstring(role) + L"のファイル件数が安全上限を超えています"));
  }
  std::uint64_t total_path_characters = 0U;
  std::uint64_t total_logical_bytes = 0U;
  for (auto& file : files) {
    if (file.relative_path.size() >
            maximum_path_characters - total_path_characters ||
        file.length > maximum_logical_bytes - total_logical_bytes) {
      return clonecore::Status::failure(storage_error(
          ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          std::wstring(role) +
              L"の総パス長または総論理バイト数が安全上限を超えています"));
    }
    total_path_characters +=
        static_cast<std::uint64_t>(file.relative_path.size());
    total_logical_bytes += file.length;
    if (!valid_relative_path(file.relative_path) || file.reparse_point ||
        file.hard_link_count != 1U || all_zero(file.sha256)) {
      return clonecore::Status::failure(storage_error(
          ErrorCode::unsupported_layout,
          ERROR_REPARSE_TAG_INVALID,
          std::wstring(role) +
              L"に不正パス、reparse point、hardlink、または無効なSHA-256があります"));
    }
    file.relative_path = upper_ascii(std::move(file.relative_path));
  }
  std::sort(
      files.begin(), files.end(),
      [](const auto& left, const auto& right) {
        return left.relative_path < right.relative_path;
      });
  if (std::adjacent_find(
          files.begin(), files.end(), [](const auto& left, const auto& right) {
            return left.relative_path == right.relative_path;
          }) != files.end()) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::identity_mismatch,
        ERROR_DUP_NAME,
        std::wstring(role) + L"に大小文字違いを含む重複パスがあります"));
  }
  return clonecore::success_status();
}

clonecore::Status canonicalize_directories(
    std::vector<std::wstring>& directories,
    const std::wstring_view role,
    const std::size_t maximum_entry_count,
    const std::uint64_t maximum_path_characters) {
  if (directories.size() > maximum_entry_count) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::invalid_data,
        ERROR_TOO_MANY_OPEN_FILES,
        std::wstring(role) + L"のディレクトリ件数が安全上限を超えています"));
  }
  std::uint64_t total_path_characters = 0U;
  for (auto& directory : directories) {
    if (directory.size() >
        maximum_path_characters - total_path_characters) {
      return clonecore::Status::failure(storage_error(
          ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          std::wstring(role) + L"の総パス長が安全上限を超えています"));
    }
    total_path_characters +=
        static_cast<std::uint64_t>(directory.size());
    if (!valid_relative_path(directory)) {
      return clonecore::Status::failure(storage_error(
          ErrorCode::unsupported_layout,
          ERROR_INVALID_NAME,
          std::wstring(role) + L"に不正なディレクトリパスがあります"));
    }
    directory = upper_ascii(std::move(directory));
  }
  std::sort(directories.begin(), directories.end());
  if (std::adjacent_find(directories.begin(), directories.end()) !=
      directories.end()) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::identity_mismatch,
        ERROR_DUP_NAME,
        std::wstring(role) + L"に大小文字違いを含む重複パスがあります"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::string> utf8_from_wstring(
    const std::wstring_view value) {
  if (value.empty()) {
    return clonecore::Result<std::string>::success({});
  }
  if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return failure<std::string>(
        ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"媒体ツリーの相対パスがUTF-8変換上限を超えています");
  }
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) {
    return clonecore::Result<std::string>::failure(
        clonecore::make_win32_error(
            ErrorCode::invalid_data,
            L"媒体ツリー相対パスのUTF-8変換",
            GetLastError()));
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          required,
          nullptr,
          nullptr) != required) {
    return clonecore::Result<std::string>::failure(
        clonecore::make_win32_error(
            ErrorCode::invalid_data,
            L"媒体ツリー相対パスのUTF-8変換",
            GetLastError()));
  }
  return clonecore::Result<std::string>::success(std::move(result));
}

std::string digest_to_hex(const imageformat::Sha256Digest& digest) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string result;
  result.reserve(digest.size() * 2U);
  for (const std::byte value : digest) {
    const auto byte = std::to_integer<unsigned int>(value);
    result.push_back(kHex[(byte >> 4U) & 0x0FU]);
    result.push_back(kHex[byte & 0x0FU]);
  }
  return result;
}

clonecore::Result<RescueMediaTreeIdentity> make_tree_identity_impl(
    std::vector<RescueMediaFileFingerprint> files,
    std::vector<std::wstring> directories,
    const std::wstring_view role,
    const std::size_t maximum_entry_count,
    const std::uint64_t maximum_path_characters,
    const std::uint64_t maximum_logical_bytes) {
  auto status = canonicalize_file_tree(
      files,
      role,
      maximum_entry_count,
      maximum_path_characters,
      maximum_logical_bytes);
  if (!status) {
    return clonecore::Result<RescueMediaTreeIdentity>::failure(
        status.error());
  }
  status = canonicalize_directories(
      directories,
      role,
      maximum_entry_count,
      maximum_path_characters);
  if (!status) {
    return clonecore::Result<RescueMediaTreeIdentity>::failure(
        status.error());
  }
  if (files.size() > maximum_entry_count - directories.size()) {
    return failure<RescueMediaTreeIdentity>(
        ErrorCode::invalid_data,
        ERROR_TOO_MANY_OPEN_FILES,
        std::wstring(role) + L"の総エントリ件数が安全上限を超えています");
  }

  std::uint64_t total_path_characters = 0U;
  std::uint64_t total_logical_bytes = 0U;
  for (const auto& directory : directories) {
    if (directory.size() >
        maximum_path_characters - total_path_characters) {
      return failure<RescueMediaTreeIdentity>(
          ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          std::wstring(role) + L"の総パス長が安全上限を超えています");
    }
    total_path_characters +=
        static_cast<std::uint64_t>(directory.size());
  }
  for (const auto& file : files) {
    if (file.relative_path.size() >
            maximum_path_characters - total_path_characters ||
        file.length > maximum_logical_bytes - total_logical_bytes) {
      return failure<RescueMediaTreeIdentity>(
          ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          std::wstring(role) +
              L"の総パス長または総論理バイト数が安全上限を超えています");
    }
    total_path_characters +=
        static_cast<std::uint64_t>(file.relative_path.size());
    total_logical_bytes += file.length;
    if (std::binary_search(
            directories.begin(),
            directories.end(),
            file.relative_path)) {
      return failure<RescueMediaTreeIdentity>(
          ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          std::wstring(role) + L"にファイル／ディレクトリ重複があります");
    }
  }

  std::string canonical{"YTEC-RESCUE-MEDIA-TREE-V1\nC:"};
  canonical += std::to_string(files.size() + directories.size());
  canonical += ':';
  canonical += std::to_string(total_path_characters);
  canonical += ':';
  canonical += std::to_string(total_logical_bytes);
  for (const auto& directory : directories) {
    auto utf8 = utf8_from_wstring(directory);
    if (!utf8) {
      return clonecore::Result<RescueMediaTreeIdentity>::failure(
          utf8.error());
    }
    canonical += "\nD:";
    canonical += std::to_string(directory.size());
    canonical += ':';
    canonical += utf8.value();
  }
  for (const auto& file : files) {
    auto utf8 = utf8_from_wstring(file.relative_path);
    if (!utf8) {
      return clonecore::Result<RescueMediaTreeIdentity>::failure(
          utf8.error());
    }
    canonical += "\nF:";
    canonical += std::to_string(file.relative_path.size());
    canonical += ':';
    canonical += utf8.value();
    canonical += ':';
    canonical += std::to_string(file.length);
    canonical += ':';
    canonical += digest_to_hex(file.sha256);
  }
  const auto digest = imageformat::sha256(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(canonical.data()),
      canonical.size()));
  if (!digest) {
    return clonecore::Result<RescueMediaTreeIdentity>::failure(
        digest.error());
  }
  return clonecore::Result<RescueMediaTreeIdentity>::success({
      .entry_count = static_cast<std::uint64_t>(
          files.size() + directories.size()),
      .file_count = static_cast<std::uint64_t>(files.size()),
      .total_path_characters = total_path_characters,
      .total_logical_bytes = total_logical_bytes,
      .root_digest = digest.value(),
  });
}

clonecore::Status validate_tree_identity_shape(
    const RescueMediaTreeIdentity& identity,
    const std::wstring_view role,
    const std::size_t maximum_entry_count,
    const std::uint64_t maximum_path_characters,
    const std::uint64_t maximum_logical_bytes) {
  if (identity.entry_count > maximum_entry_count ||
      identity.file_count > identity.entry_count ||
      identity.total_path_characters > maximum_path_characters ||
      identity.total_logical_bytes > maximum_logical_bytes ||
      all_zero(identity.root_digest)) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(role) + L"の集約値またはroot digestが不正です"));
  }
  return clonecore::success_status();
}

const RescueMediaFileFingerprint* find_file(
    const std::vector<RescueMediaFileFingerprint>& files,
    const std::wstring_view path) noexcept {
  const std::wstring key = upper_ascii(std::wstring(path));
  const auto found = std::lower_bound(
      files.begin(), files.end(), key,
      [](const RescueMediaFileFingerprint& file, const std::wstring& value) {
        return file.relative_path < value;
      });
  return found != files.end() && found->relative_path == key
             ? &*found
             : nullptr;
}

clonecore::Result<RescueUsbOwnedMediaInspection>
canonicalize_owned_media(RescueUsbOwnedMediaInspection inspection) {
  inspection.boot_file_system =
      upper_ascii(std::move(inspection.boot_file_system));
  std::transform(
      inspection.media_id.begin(),
      inspection.media_id.end(),
      inspection.media_id.begin(),
      [](const wchar_t character) {
        return character >= L'A' && character <= L'F'
                   ? static_cast<wchar_t>(character - L'A' + L'a')
                   : character;
      });
  auto status = canonicalize_file_tree(
      inspection.manifest_owned_boot_files,
      L"所有manifestの起動ファイル",
      kMaximumBootFileCount,
      kMaximumBootPathCharacters,
      kMaximumBootLogicalBytes);
  if (!status) {
    return clonecore::Result<RescueUsbOwnedMediaInspection>::failure(
        status.error());
  }
  status = canonicalize_directories(
      inspection.manifest_owned_boot_directories,
      L"所有manifestの起動ディレクトリ",
      kMaximumBootFileCount,
      kMaximumBootPathCharacters);
  if (!status) {
    return clonecore::Result<RescueUsbOwnedMediaInspection>::failure(
        status.error());
  }
  status = canonicalize_file_tree(
      inspection.observed_boot_files,
      L"観測した起動ファイル",
      kMaximumBootFileCount,
      kMaximumBootPathCharacters,
      kMaximumBootLogicalBytes);
  if (!status) {
    return clonecore::Result<RescueUsbOwnedMediaInspection>::failure(
        status.error());
  }
  status = canonicalize_directories(
      inspection.observed_boot_directories,
      L"観測した起動ディレクトリ",
      kMaximumBootFileCount,
      kMaximumBootPathCharacters);
  if (!status) {
    return clonecore::Result<RescueUsbOwnedMediaInspection>::failure(
        status.error());
  }
  auto manifest_identity = make_tree_identity_impl(
      inspection.manifest_owned_boot_files,
      inspection.manifest_owned_boot_directories,
      L"所有manifestの起動ツリー",
      kMaximumBootFileCount,
      kMaximumBootPathCharacters,
      kMaximumBootLogicalBytes);
  if (!manifest_identity) {
    return clonecore::Result<RescueUsbOwnedMediaInspection>::failure(
        manifest_identity.error());
  }
  auto observed_identity = make_tree_identity_impl(
      inspection.observed_boot_files,
      inspection.observed_boot_directories,
      L"観測した起動ツリー",
      kMaximumBootFileCount,
      kMaximumBootPathCharacters,
      kMaximumBootLogicalBytes);
  if (!observed_identity) {
    return clonecore::Result<RescueUsbOwnedMediaInspection>::failure(
        observed_identity.error());
  }
  if (manifest_identity.value() !=
          inspection.manifest_owned_boot_tree_identity ||
      observed_identity.value() != inspection.observed_boot_tree_identity) {
    return failure<RescueUsbOwnedMediaInspection>(
        ErrorCode::verification_failed,
        ERROR_DATA_CHECKSUM_ERROR,
        L"起動ツリーの集約値またはroot digestが列挙内容と一致しません");
  }
  status = validate_tree_identity_shape(
      inspection.observed_data_tree_identity,
      L"保持するデータツリー",
      kMaximumDataFileCount,
      kMaximumDataPathCharacters,
      kMaximumDataLogicalBytes);
  if (!status) {
    return clonecore::Result<RescueUsbOwnedMediaInspection>::failure(
        status.error());
  }
  return clonecore::Result<RescueUsbOwnedMediaInspection>::success(
      std::move(inspection));
}

clonecore::Status validate_owned_media(
    const RescueUsbOwnedMediaInspection& inspection,
    const RescueUsbCanonicalLayout& layout,
    const RescueUsbDataFileSystem requested_file_system) {
  if (inspection.schema_version != kRescueUsbOwnershipSchemaVersion ||
      inspection.purpose != kRescueUsbOwnershipPurpose ||
      !valid_media_id(inspection.media_id) ||
      !equals_ascii_case_insensitive(inspection.boot_file_system, L"FAT32") ||
      inspection.data_file_system != requested_file_system ||
      inspection.boot_partition_bytes != kRescueUsbBootPartitionBytes ||
      inspection.manifest_layout != layout) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"Y-TEC所有manifest、媒体ID、ファイルシステム、または完全レイアウトが一致しません"));
  }

  auto canonical = canonicalize_owned_media(inspection);
  if (!canonical) {
    return clonecore::Status::failure(canonical.error());
  }
  const auto& owned = canonical.value().manifest_owned_boot_files;
  const auto& observed = canonical.value().observed_boot_files;
  if (owned != observed ||
      canonical.value().manifest_owned_boot_directories !=
          canonical.value().observed_boot_directories ||
      canonical.value().manifest_owned_boot_tree_identity !=
          canonical.value().observed_boot_tree_identity) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::verification_failed,
        ERROR_DATA_CHECKSUM_ERROR,
        L"起動領域に所有manifest外の内容または改変された内容があります"));
  }

  constexpr std::array<std::wstring_view, 4U> required_files{
      L"sources\\boot.wim",
      L"bootmgr",
      L"EFI\\BOOT\\bootx64.efi",
      kRescueUsbMarkerRelativePath,
  };
  for (const std::wstring_view required : required_files) {
    if (find_file(owned, required) == nullptr) {
      return clonecore::Status::failure(storage_error(
          ErrorCode::verification_failed,
          ERROR_FILE_NOT_FOUND,
          L"Y-TEC所有manifestに必須起動ファイルがありません"));
    }
  }

  const std::wstring transaction = upper_ascii(
      std::wstring(kRescueUsbTransactionRelativePath));
  if (!std::binary_search(
          canonical.value().manifest_owned_boot_directories.begin(),
          canonical.value().manifest_owned_boot_directories.end(),
          transaction) ||
      std::any_of(
          canonical.value().manifest_owned_boot_files.begin(),
          canonical.value().manifest_owned_boot_files.end(),
          [&](const RescueMediaFileFingerprint& file) {
            return file.relative_path.starts_with(transaction + L"\\");
          }) ||
      std::any_of(
          canonical.value().manifest_owned_boot_directories.begin(),
          canonical.value().manifest_owned_boot_directories.end(),
          [&](const std::wstring& directory) {
            return directory != transaction &&
                directory.starts_with(transaction + L"\\");
          })) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::verification_failed,
        ERROR_DIR_NOT_EMPTY,
        L"所有manifestの予約transaction領域がないか空ではありません"));
  }

  const auto* marker = find_file(owned, kRescueUsbMarkerRelativePath);
  std::vector<std::byte> marker_bytes;
  marker_bytes.reserve(inspection.media_id.size());
  for (const wchar_t character : inspection.media_id) {
    if (character > 0x7FU) {
      return clonecore::Status::failure(storage_error(
          ErrorCode::invalid_data,
          ERROR_NO_UNICODE_TRANSLATION,
          L"レスキュー媒体IDがASCIIではありません"));
    }
    marker_bytes.push_back(
        static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  const auto expected_marker_hash = imageformat::sha256(marker_bytes);
  if (!expected_marker_hash || marker == nullptr || marker->length != 36U ||
      marker->sha256 != expected_marker_hash.value()) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::verification_failed,
        ERROR_DATA_CHECKSUM_ERROR,
        L"媒体ルートのrescue-media-id.txtが所有manifestと一致しません"));
  }
  return clonecore::success_status();
}

template <typename Unsigned>
void append_unsigned(std::vector<std::byte>& bytes, const Unsigned value) {
  static_assert(std::is_unsigned_v<Unsigned>);
  for (std::size_t index = 0U; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::byte>(
        (value >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
  }
}

void append_bool(std::vector<std::byte>& bytes, const bool value) {
  append_unsigned(bytes, static_cast<std::uint8_t>(value ? 1U : 0U));
}

void append_string(std::vector<std::byte>& bytes, const std::string_view value) {
  append_unsigned(bytes, static_cast<std::uint64_t>(value.size()));
  for (const unsigned char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
}

void append_wstring(
    std::vector<std::byte>& bytes,
    const std::wstring_view value) {
  append_unsigned(bytes, static_cast<std::uint64_t>(value.size()));
  for (const wchar_t character : value) {
    append_unsigned(bytes, static_cast<std::uint32_t>(character));
  }
}

void append_layout(
    std::vector<std::byte>& bytes,
    const RescueUsbCanonicalLayout& layout) {
  append_unsigned(bytes, static_cast<std::uint8_t>(layout.disk_style));
  append_unsigned(bytes, static_cast<std::uint64_t>(layout.partitions.size()));
  for (const auto& partition : layout.partitions) {
    append_unsigned(bytes, partition.number);
    append_unsigned(bytes, static_cast<std::uint8_t>(partition.style));
    append_wstring(bytes, partition.type);
    append_unsigned(bytes, partition.offset_bytes);
    append_unsigned(bytes, partition.size_bytes);
    append_bool(bytes, partition.bootable);
  }
}

void append_files(
    std::vector<std::byte>& bytes,
    std::vector<RescueMediaFileFingerprint> files) {
  const auto status = canonicalize_file_tree(
      files,
      L"review binding",
      kMaximumDataFileCount,
      kMaximumDataPathCharacters,
      kMaximumDataLogicalBytes);
  if (!status) {
    // Invalid trees are rejected before this helper is reached. Keep their
    // binding deterministic so a tampered plan cannot accidentally match.
    append_unsigned(bytes, std::numeric_limits<std::uint64_t>::max());
    return;
  }
  append_unsigned(bytes, static_cast<std::uint64_t>(files.size()));
  for (const auto& file : files) {
    append_wstring(bytes, file.relative_path);
    append_unsigned(bytes, file.length);
    bytes.insert(bytes.end(), file.sha256.begin(), file.sha256.end());
    append_bool(bytes, file.reparse_point);
    append_unsigned(bytes, file.hard_link_count);
  }
}

void append_directories(
    std::vector<std::byte>& bytes,
    std::vector<std::wstring> directories) {
  const auto status = canonicalize_directories(
      directories,
      L"review binding",
      kMaximumBootFileCount,
      kMaximumBootPathCharacters);
  if (!status) {
    append_unsigned(bytes, std::numeric_limits<std::uint64_t>::max());
    return;
  }
  append_unsigned(bytes, static_cast<std::uint64_t>(directories.size()));
  for (const auto& directory : directories) {
    append_wstring(bytes, directory);
  }
}

void append_tree_identity(
    std::vector<std::byte>& bytes,
    const RescueMediaTreeIdentity& identity) {
  append_unsigned(bytes, identity.entry_count);
  append_unsigned(bytes, identity.file_count);
  append_unsigned(bytes, identity.total_path_characters);
  append_unsigned(bytes, identity.total_logical_bytes);
  bytes.insert(
      bytes.end(), identity.root_digest.begin(), identity.root_digest.end());
}

clonecore::Result<imageformat::Sha256Digest> calculate_binding(
    const RescueUsbStoragePlan& plan) {
  std::vector<std::byte> bytes;
  bytes.reserve(1024U);
  append_string(bytes, "ytec-rescue-usb-storage-plan-v1");
  append_unsigned(bytes, static_cast<std::uint8_t>(plan.mode));
  append_unsigned(bytes, static_cast<std::uint8_t>(plan.data_file_system));
  append_unsigned(bytes, plan.expected_target.disk_number);
  append_wstring(bytes, plan.expected_target.model);
  append_unsigned(bytes, plan.expected_target.size_bytes);
  append_unsigned(bytes, plan.expected_target.logical_sector_size);
  append_string(bytes, plan.expected_target.serial_suffix);
  append_wstring(bytes, plan.expected_target.device_instance_id);
  append_bool(bytes, plan.expected_target.is_system_disk);
  append_layout(bytes, plan.reviewed_layout);
  append_unsigned(bytes, plan.planned_boot_partition_bytes);
  append_bool(bytes, plan.planned_data_partition_uses_remaining_space);
  append_bool(bytes, plan.reviewed_owned_media.has_value());
  if (plan.reviewed_owned_media.has_value()) {
    const auto& owned = plan.reviewed_owned_media.value();
    append_unsigned(bytes, owned.schema_version);
    append_wstring(bytes, owned.purpose);
    append_wstring(bytes, owned.media_id);
    append_wstring(bytes, upper_ascii(owned.boot_file_system));
    append_unsigned(bytes, static_cast<std::uint8_t>(owned.data_file_system));
    append_unsigned(bytes, owned.boot_partition_bytes);
    append_layout(bytes, owned.manifest_layout);
    append_tree_identity(bytes, owned.manifest_owned_boot_tree_identity);
    append_files(bytes, owned.manifest_owned_boot_files);
    append_directories(bytes, owned.manifest_owned_boot_directories);
    append_tree_identity(bytes, owned.observed_boot_tree_identity);
    append_files(bytes, owned.observed_boot_files);
    append_directories(bytes, owned.observed_boot_directories);
    append_tree_identity(bytes, owned.observed_data_tree_identity);
  }
  append_bool(bytes, plan.physical_write_started);
  return imageformat::sha256(bytes);
}

clonecore::Status validate_usb_target_state(
    const diskmodel::DiskInfo& target) {
  if (target.size_bytes < kRescueUsbMinimumBytes) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"レスキューUSBは8GiB以上が必要です"));
  }
  if (target.is_system_disk || !target.removable.has_value() ||
      !target.read_only.has_value() || !target.offline.has_value() ||
      !target.removable.value() || target.read_only.value() ||
      target.offline.value() ||
      !equals_ascii_case_insensitive(target.bus_type, L"USB")) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"オンライン・書込み可能・非システムの取り外し可能USBだけを使用できます"));
  }
  if (target.logical_sector_size == 0U ||
      target.size_bytes / target.logical_sector_size > 0xFFFFFFFFULL) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"BIOS互換MBRで安全に表現できないUSB容量またはセクター構成です"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_initializable_layout(
    const RescueUsbCanonicalLayout& layout,
    const std::uint64_t disk_size) {
  if (layout.disk_style == diskmodel::PartitionStyle::raw) {
    return layout.partitions.empty()
        ? clonecore::success_status()
        : clonecore::Status::failure(storage_error(
              ErrorCode::unsupported_layout,
              ERROR_INVALID_DATA,
              L"RAW USBに既存パーティションがあり完全レイアウトを信頼できません"));
  }
  if ((layout.disk_style != diskmodel::PartitionStyle::mbr &&
       layout.disk_style != diskmodel::PartitionStyle::gpt) ||
      layout.partitions.size() > 128U) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"全初期化対象の基本パーティション形式または件数を安全に扱えません"));
  }
  auto partitions = layout.partitions;
  std::sort(
      partitions.begin(), partitions.end(),
      [](const auto& left, const auto& right) {
        return std::tie(left.offset_bytes, left.size_bytes, left.number) <
            std::tie(right.offset_bytes, right.size_bytes, right.number);
      });
  std::vector<std::uint32_t> numbers;
  numbers.reserve(partitions.size());
  std::uint64_t previous_end{};
  for (const auto& partition : partitions) {
    if (partition.number == 0U ||
        partition.style != layout.disk_style ||
        partition.offset_bytes == 0U || partition.size_bytes == 0U ||
        partition.offset_bytes > disk_size ||
        partition.size_bytes > disk_size - partition.offset_bytes ||
        partition.offset_bytes < previous_end) {
      return clonecore::Status::failure(storage_error(
          ErrorCode::unsupported_layout,
          ERROR_INVALID_DATA,
          L"全初期化対象の完全パーティションレイアウトが不正です"));
    }
    previous_end = partition.offset_bytes + partition.size_bytes;
    numbers.push_back(partition.number);
  }
  std::sort(numbers.begin(), numbers.end());
  if (std::adjacent_find(numbers.begin(), numbers.end()) != numbers.end()) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::unsupported_layout,
        ERROR_DUP_NAME,
        L"全初期化対象のパーティション番号が重複しています"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_refresh_layout(
    const RescueUsbCanonicalLayout& layout,
    const std::uint64_t disk_size) {
  if (layout.disk_style != diskmodel::PartitionStyle::mbr ||
      layout.partitions.size() != 2U) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"保持更新は検証済みMBR・2領域のY-TEC媒体だけに限定します"));
  }
  const auto& boot = layout.partitions[0];
  const auto& data = layout.partitions[1];
  if (boot.number != 1U || data.number != 2U ||
      boot.style != diskmodel::PartitionStyle::mbr ||
      data.style != diskmodel::PartitionStyle::mbr ||
      boot.size_bytes != kRescueUsbBootPartitionBytes ||
      !boot.bootable || data.bootable || boot.offset_bytes == 0U ||
      data.offset_bytes != boot.offset_bytes + boot.size_bytes ||
      data.size_bytes == 0U || data.offset_bytes > disk_size ||
      data.size_bytes != disk_size - data.offset_bytes) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::unsupported_layout,
        ERROR_INVALID_DATA,
        L"4GiB FAT32起動領域＋残容量データ領域の完全レイアウトではありません"));
  }
  return clonecore::success_status();
}

}  // namespace

RescueUsbCanonicalLayout make_rescue_usb_canonical_layout(
    const diskmodel::DiskInfo& disk) {
  RescueUsbCanonicalLayout layout{
      .disk_style = disk.partition_style,
  };
  layout.partitions.reserve(disk.partitions.size());
  for (const auto& partition : disk.partitions) {
    layout.partitions.push_back({
        .number = partition.number,
        .style = partition.style,
        .type = upper_ascii(partition.type),
        .offset_bytes = partition.offset_bytes,
        .size_bytes = partition.size_bytes,
        .bootable = partition.bootable,
    });
  }
  std::sort(
      layout.partitions.begin(), layout.partitions.end(),
      [](const auto& left, const auto& right) {
        return std::tie(
                   left.number,
                   left.style,
                   left.type,
                   left.offset_bytes,
                   left.size_bytes,
                   left.bootable) <
               std::tie(
                   right.number,
                   right.style,
                   right.type,
                   right.offset_bytes,
                   right.size_bytes,
                   right.bootable);
      });
  return layout;
}

clonecore::Result<imageformat::Sha256Digest>
make_rescue_usb_canonical_layout_digest(
    const RescueUsbCanonicalLayout& layout) {
  std::vector<std::byte> bytes;
  bytes.reserve(256U + layout.partitions.size() * 64U);
  append_string(bytes, "ytec-rescue-usb-canonical-layout-v1");
  append_layout(bytes, layout);
  return imageformat::sha256(bytes);
}

clonecore::Result<RescueUsbStoragePlan> plan_rescue_usb_storage(
    const RescueUsbStoragePlanInput& input) {
  if (input.target == nullptr) {
    return failure<RescueUsbStoragePlan>(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"USBの読取り専用インベントリがありません");
  }
  const auto state = validate_usb_target_state(*input.target);
  if (!state) {
    return clonecore::Result<RescueUsbStoragePlan>::failure(state.error());
  }
  const auto identity =
      diskmodel::make_stable_disk_identity(*input.target, false);
  if (!identity) {
    return clonecore::Result<RescueUsbStoragePlan>::failure(identity.error());
  }
  const RescueUsbCanonicalLayout layout =
      make_rescue_usb_canonical_layout(*input.target);

  RescueUsbStoragePlan plan{
      .mode = input.mode,
      .data_file_system = input.data_file_system,
      .expected_target = identity.value(),
      .reviewed_layout = layout,
      .planned_boot_partition_bytes = kRescueUsbBootPartitionBytes,
      .planned_data_partition_uses_remaining_space = true,
      .physical_write_started = false,
  };
  if (input.mode == RescueUsbProvisioningMode::initialize_all) {
    if (input.owned_media != nullptr) {
      return failure<RescueUsbStoragePlan>(
          ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"全初期化計画へ保持更新用manifestを混在できません");
    }
    const auto initializable = validate_initializable_layout(
        layout, input.target->size_bytes);
    if (!initializable) {
      return clonecore::Result<RescueUsbStoragePlan>::failure(
          initializable.error());
    }
  } else {
    const auto refresh_layout =
        validate_refresh_layout(layout, input.target->size_bytes);
    if (!refresh_layout) {
      return clonecore::Result<RescueUsbStoragePlan>::failure(
          refresh_layout.error());
    }
    if (input.owned_media == nullptr) {
      return failure<RescueUsbStoragePlan>(
          ErrorCode::verification_failed,
          ERROR_FILE_NOT_FOUND,
          L"保持更新には媒体上のY-TEC所有manifestと媒体IDの完全検証が必要です");
    }
    const auto ownership = validate_owned_media(
        *input.owned_media, layout, input.data_file_system);
    if (!ownership) {
      return clonecore::Result<RescueUsbStoragePlan>::failure(
          ownership.error());
    }
    auto canonical_owned = canonicalize_owned_media(*input.owned_media);
    if (!canonical_owned) {
      return clonecore::Result<RescueUsbStoragePlan>::failure(
          canonical_owned.error());
    }
    plan.reviewed_owned_media = canonical_owned.take_value();
  }

  const auto binding = calculate_binding(plan);
  if (!binding) {
    return clonecore::Result<RescueUsbStoragePlan>::failure(binding.error());
  }
  plan.review_binding_digest = binding.value();
  return clonecore::Result<RescueUsbStoragePlan>::success(std::move(plan));
}

clonecore::Status validate_rescue_usb_storage_plan(
    const RescueUsbStoragePlan& plan,
    const diskmodel::DiskInfo& observed_target,
    const RescueUsbOwnedMediaInspection* observed_owned_media) {
  const auto binding_status =
      validate_rescue_usb_storage_plan_binding(plan);
  if (!binding_status) {
    return binding_status;
  }
  const auto observed_identity =
      diskmodel::make_stable_disk_identity(observed_target, false);
  if (!observed_identity) {
    return clonecore::Status::failure(observed_identity.error());
  }
  const auto identity = clonecore::validate_stable_identity(
      plan.expected_target,
      observed_identity.value(),
      L"レスキューUSB書込み境界");
  if (!identity) {
    return identity;
  }
  if (observed_target.disk_number != plan.expected_target.disk_number ||
      make_rescue_usb_canonical_layout(observed_target) !=
          plan.reviewed_layout) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"USBのディスク番号または完全パーティションレイアウトがレビュー時から変化しました"));
  }
  const auto state = validate_usb_target_state(observed_target);
  if (!state) {
    return state;
  }

  if (plan.mode == RescueUsbProvisioningMode::initialize_all) {
    if (observed_owned_media != nullptr ||
        plan.reviewed_owned_media.has_value()) {
      return clonecore::Status::failure(storage_error(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"全初期化計画へ保持更新用manifestを混在できません"));
    }
    return clonecore::success_status();
  }
  if (!plan.reviewed_owned_media.has_value() ||
      observed_owned_media == nullptr) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::verification_failed,
        ERROR_FILE_NOT_FOUND,
        L"保持更新の書込み境界で所有manifestを再取得できません"));
  }
  const auto layout = validate_refresh_layout(
      plan.reviewed_layout, observed_target.size_bytes);
  if (!layout) {
    return layout;
  }
  auto canonical_observed = canonicalize_owned_media(*observed_owned_media);
  if (!canonical_observed) {
    return clonecore::Status::failure(canonical_observed.error());
  }
  const auto ownership = validate_owned_media(
      canonical_observed.value(),
      plan.reviewed_layout,
      plan.data_file_system);
  if (!ownership) {
    return ownership;
  }
  if (canonical_observed.value() != plan.reviewed_owned_media.value()) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::identity_mismatch,
        ERROR_DATA_CHECKSUM_ERROR,
        L"所有manifest、起動領域、または保持データがレビュー時から変化しました"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_rescue_usb_storage_plan_binding(
    const RescueUsbStoragePlan& plan) {
  if (plan.physical_write_started || all_zero(plan.review_binding_digest) ||
      plan.planned_boot_partition_bytes !=
          kRescueUsbBootPartitionBytes ||
      !plan.planned_data_partition_uses_remaining_space) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"レビュー計画が書込み開始済みまたは未署名です"));
  }
  const auto recalculated = calculate_binding(plan);
  if (!recalculated || recalculated.value() != plan.review_binding_digest) {
    return clonecore::Status::failure(
        recalculated
            ? storage_error(
                  ErrorCode::identity_mismatch,
                  ERROR_DATA_CHECKSUM_ERROR,
                  L"レビュー済みUSB計画が作成後に変更されています")
            : recalculated.error());
  }
  return clonecore::success_status();
}

clonecore::Status validate_rescue_usb_completed_layout(
    const RescueUsbCanonicalLayout& layout,
    const std::uint64_t disk_size) {
  return validate_refresh_layout(layout, disk_size);
}

clonecore::Status validate_rescue_usb_data_unchanged(
    const std::vector<RescueMediaFileFingerprint>& before,
    const std::vector<RescueMediaFileFingerprint>& after) {
  auto canonical_before = before;
  auto canonical_after = after;
  auto status = canonicalize_file_tree(
      canonical_before,
      L"更新前の保持データ",
      kMaximumDataFileCount,
      kMaximumDataPathCharacters,
      kMaximumDataLogicalBytes);
  if (!status) {
    return status;
  }
  status = canonicalize_file_tree(
      canonical_after,
      L"更新後の保持データ",
      kMaximumDataFileCount,
      kMaximumDataPathCharacters,
      kMaximumDataLogicalBytes);
  if (!status) {
    return status;
  }
  if (canonical_before != canonical_after) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::verification_failed,
        ERROR_DATA_CHECKSUM_ERROR,
        L"保持更新の前後でデータ領域のパス、長さ、またはSHA-256が変化しました"));
  }
  return clonecore::success_status();
}

clonecore::Result<RescueMediaTreeIdentity>
make_rescue_usb_boot_tree_identity(
    const std::vector<RescueMediaFileFingerprint>& files,
    const std::vector<std::wstring>& directories) {
  return make_tree_identity_impl(
      files,
      directories,
      L"レスキューUSB起動ツリー",
      kMaximumBootFileCount,
      kMaximumBootPathCharacters,
      kMaximumBootLogicalBytes);
}

clonecore::Result<RescueMediaTreeIdentity>
make_rescue_usb_private_data_tree_identity(
    const std::vector<RescueMediaFileFingerprint>& files,
    const std::vector<std::wstring>& directories) {
  return make_tree_identity_impl(
      files,
      directories,
      L"レスキューUSB保持データツリー",
      kMaximumDataFileCount,
      kMaximumDataPathCharacters,
      kMaximumDataLogicalBytes);
}

clonecore::Status validate_rescue_usb_data_unchanged(
    const RescueMediaTreeIdentity& before,
    const RescueMediaTreeIdentity& after) {
  auto status = validate_tree_identity_shape(
      before,
      L"更新前の保持データツリー",
      kMaximumDataFileCount,
      kMaximumDataPathCharacters,
      kMaximumDataLogicalBytes);
  if (!status) {
    return status;
  }
  status = validate_tree_identity_shape(
      after,
      L"更新後の保持データツリー",
      kMaximumDataFileCount,
      kMaximumDataPathCharacters,
      kMaximumDataLogicalBytes);
  if (!status) {
    return status;
  }
  if (before != after) {
    return clonecore::Status::failure(storage_error(
        ErrorCode::verification_failed,
        ERROR_DATA_CHECKSUM_ERROR,
        L"保持更新の前後でデータ領域の件数、総容量、または非公開root digestが変化しました"));
  }
  return clonecore::success_status();
}

std::wstring_view rescue_usb_provisioning_mode_name(
    const RescueUsbProvisioningMode mode) noexcept {
  switch (mode) {
    case RescueUsbProvisioningMode::initialize_all:
      return L"Initialize";
    case RescueUsbProvisioningMode::preserve_data_refresh:
      return L"Refresh";
  }
  return L"Unknown";
}

std::wstring_view rescue_usb_data_file_system_name(
    const RescueUsbDataFileSystem file_system) noexcept {
  switch (file_system) {
    case RescueUsbDataFileSystem::ntfs:
      return L"NTFS";
    case RescueUsbDataFileSystem::exfat:
      return L"exFAT";
  }
  return L"Unknown";
}

}  // namespace ytec::windowsapp

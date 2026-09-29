#include "ytec/windowsapp/rescue_media_inspection.h"

#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/sha256.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

namespace ytec::windowsapp {
namespace {

using clonecore::ErrorCode;

constexpr std::size_t kMaximumJsonKeyCharacters = 64U;
constexpr std::size_t kMaximumJsonStringCharacters = 32U * 1024U - 1U;
constexpr std::size_t kMaximumManifestPartitionCount = 128U;
constexpr std::size_t kHashReadBlockBytes = 1024U * 1024U;

bool inspection_cancelled(
    const std::atomic_bool* const cancellation_requested) noexcept {
  return cancellation_requested != nullptr &&
      cancellation_requested->load();
}

clonecore::Error inspection_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"レスキューUSB所有情報の読取り専用検査",
      .message = std::move(message),
  };
}

template <typename T>
clonecore::Result<T> failure(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Result<T>::failure(
      inspection_error(code, native_code, std::move(message)));
}

bool is_json_whitespace(const char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

class JsonCursor final {
 public:
  explicit JsonCursor(const std::string_view input) noexcept : input_(input) {}

  void skip_whitespace() noexcept {
    while (position_ < input_.size() &&
           is_json_whitespace(input_[position_])) {
      ++position_;
    }
  }

  [[nodiscard]] bool consume(const char expected) noexcept {
    skip_whitespace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  [[nodiscard]] bool finished() noexcept {
    skip_whitespace();
    return position_ == input_.size();
  }

  [[nodiscard]] clonecore::Result<std::wstring> parse_string(
      const std::size_t maximum_characters) {
    skip_whitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
      return failure<std::wstring>(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"所有manifestのJSON文字列を読み取れません");
    }
    ++position_;
    std::wstring output;
    output.reserve((std::min)(maximum_characters, input_.size() - position_));
    while (position_ < input_.size()) {
      const unsigned char current =
          static_cast<unsigned char>(input_[position_++]);
      if (current == static_cast<unsigned char>('"')) {
        return clonecore::Result<std::wstring>::success(std::move(output));
      }
      std::uint32_t code_point{};
      if (current == static_cast<unsigned char>('\\')) {
        if (position_ >= input_.size()) {
          break;
        }
        const char escaped = input_[position_++];
        switch (escaped) {
          case '"':
          case '\\':
          case '/':
            code_point = static_cast<unsigned char>(escaped);
            break;
          case 'b':
            code_point = 0x08U;
            break;
          case 'f':
            code_point = 0x0CU;
            break;
          case 'n':
            code_point = 0x0AU;
            break;
          case 'r':
            code_point = 0x0DU;
            break;
          case 't':
            code_point = 0x09U;
            break;
          case 'u': {
            auto first = parse_hex_quad();
            if (!first) {
              return clonecore::Result<std::wstring>::failure(first.error());
            }
            code_point = first.value();
            if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
              if (position_ + 2U > input_.size() ||
                  input_[position_] != '\\' ||
                  input_[position_ + 1U] != 'u') {
                return invalid_unicode();
              }
              position_ += 2U;
              auto second = parse_hex_quad();
              if (!second || second.value() < 0xDC00U ||
                  second.value() > 0xDFFFU) {
                return invalid_unicode();
              }
              code_point = 0x10000U +
                  ((code_point - 0xD800U) << 10U) +
                  (second.value() - 0xDC00U);
            } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
              return invalid_unicode();
            }
            break;
          }
          default:
            return failure<std::wstring>(
                ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"所有manifestに未対応のJSON escapeがあります");
        }
      } else if (current < 0x80U) {
        if (current < 0x20U) {
          return invalid_unicode();
        }
        code_point = current;
      } else {
        auto decoded = decode_utf8(current);
        if (!decoded) {
          return clonecore::Result<std::wstring>::failure(decoded.error());
        }
        code_point = decoded.value();
      }
      if (!append_code_point(output, code_point, maximum_characters)) {
        return failure<std::wstring>(
            ErrorCode::invalid_data,
            ERROR_BUFFER_OVERFLOW,
            L"所有manifestのJSON文字列が上限を超えています");
      }
    }
    return failure<std::wstring>(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"所有manifestのJSON文字列が閉じられていません");
  }

  [[nodiscard]] clonecore::Result<std::uint64_t> parse_uint64() {
    skip_whitespace();
    const std::size_t start = position_;
    while (position_ < input_.size() && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      ++position_;
    }
    if (position_ == start ||
        (position_ - start > 1U && input_[start] == '0')) {
      return failure<std::uint64_t>(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"所有manifestのJSON整数を読み取れません");
    }
    std::uint64_t value{};
    const auto converted = std::from_chars(
        input_.data() + start, input_.data() + position_, value);
    if (converted.ec != std::errc{} ||
        converted.ptr != input_.data() + position_) {
      return failure<std::uint64_t>(
          ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"所有manifestのJSON整数が範囲外です");
    }
    return clonecore::Result<std::uint64_t>::success(value);
  }

  [[nodiscard]] clonecore::Result<bool> parse_bool() {
    skip_whitespace();
    if (input_.substr(position_, 4U) == "true") {
      position_ += 4U;
      return clonecore::Result<bool>::success(true);
    }
    if (input_.substr(position_, 5U) == "false") {
      position_ += 5U;
      return clonecore::Result<bool>::success(false);
    }
    return failure<bool>(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"所有manifestのJSON真偽値を読み取れません");
  }

 private:
  [[nodiscard]] clonecore::Result<std::uint32_t> parse_hex_quad() {
    if (position_ + 4U > input_.size()) {
      return failure<std::uint32_t>(
          ErrorCode::invalid_data,
          ERROR_NO_UNICODE_TRANSLATION,
          L"所有manifestのUnicode escapeが不完全です");
    }
    std::uint32_t value{};
    for (std::size_t index = 0U; index < 4U; ++index) {
      const char character = input_[position_++];
      std::uint32_t digit{};
      if (character >= '0' && character <= '9') {
        digit = static_cast<std::uint32_t>(character - '0');
      } else if (character >= 'a' && character <= 'f') {
        digit = static_cast<std::uint32_t>(character - 'a' + 10);
      } else if (character >= 'A' && character <= 'F') {
        digit = static_cast<std::uint32_t>(character - 'A' + 10);
      } else {
        return failure<std::uint32_t>(
            ErrorCode::invalid_data,
            ERROR_NO_UNICODE_TRANSLATION,
            L"所有manifestのUnicode escapeが不正です");
      }
      value = (value << 4U) | digit;
    }
    return clonecore::Result<std::uint32_t>::success(value);
  }

  [[nodiscard]] clonecore::Result<std::uint32_t> decode_utf8(
      const unsigned char first) {
    std::size_t continuation_count{};
    std::uint32_t value{};
    std::uint32_t minimum{};
    if (first >= 0xC2U && first <= 0xDFU) {
      continuation_count = 1U;
      value = first & 0x1FU;
      minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      continuation_count = 2U;
      value = first & 0x0FU;
      minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      continuation_count = 3U;
      value = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return failure<std::uint32_t>(
          ErrorCode::invalid_data,
          ERROR_NO_UNICODE_TRANSLATION,
          L"所有manifestが正規UTF-8ではありません");
    }
    if (continuation_count > input_.size() - position_) {
      return failure<std::uint32_t>(
          ErrorCode::invalid_data,
          ERROR_NO_UNICODE_TRANSLATION,
          L"所有manifestのUTF-8文字が不完全です");
    }
    for (std::size_t index = 0U; index < continuation_count; ++index) {
      const unsigned char next =
          static_cast<unsigned char>(input_[position_++]);
      if ((next & 0xC0U) != 0x80U) {
        return failure<std::uint32_t>(
            ErrorCode::invalid_data,
            ERROR_NO_UNICODE_TRANSLATION,
            L"所有manifestが正規UTF-8ではありません");
      }
      value = (value << 6U) | (next & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
      return failure<std::uint32_t>(
          ErrorCode::invalid_data,
          ERROR_NO_UNICODE_TRANSLATION,
          L"所有manifestに不正なUnicode値があります");
    }
    return clonecore::Result<std::uint32_t>::success(value);
  }

  static bool append_code_point(
      std::wstring& output,
      const std::uint32_t code_point,
      const std::size_t maximum_characters) {
    if (code_point <= 0xFFFFU) {
      if (output.size() >= maximum_characters) {
        return false;
      }
      output.push_back(static_cast<wchar_t>(code_point));
      return true;
    }
    if (maximum_characters < 2U ||
        output.size() > maximum_characters - 2U) {
      return false;
    }
    const std::uint32_t adjusted = code_point - 0x10000U;
    output.push_back(static_cast<wchar_t>(0xD800U + (adjusted >> 10U)));
    output.push_back(static_cast<wchar_t>(0xDC00U + (adjusted & 0x3FFU)));
    return true;
  }

  [[nodiscard]] clonecore::Result<std::wstring> invalid_unicode() const {
    return failure<std::wstring>(
        ErrorCode::invalid_data,
        ERROR_NO_UNICODE_TRANSLATION,
        L"所有manifestに不正なUnicode文字があります");
  }

  std::string_view input_;
  std::size_t position_{};
};

clonecore::Result<imageformat::Sha256Digest> parse_digest(
    const std::wstring_view text) {
  if (text.size() != 64U) {
    return failure<imageformat::Sha256Digest>(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"所有manifestのSHA-256長が不正です");
  }
  imageformat::Sha256Digest digest{};
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    const auto hex = [](const wchar_t character) -> int {
      if (character >= L'0' && character <= L'9') {
        return character - L'0';
      }
      if (character >= L'A' && character <= L'F') {
        return character - L'A' + 10;
      }
      return -1;
    };
    const int high = hex(text[index * 2U]);
    const int low = hex(text[index * 2U + 1U]);
    if (high < 0 || low < 0) {
      return failure<imageformat::Sha256Digest>(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"所有manifestのSHA-256が16進数ではありません");
    }
    digest[index] = static_cast<std::byte>((high << 4) | low);
  }
  return clonecore::Result<imageformat::Sha256Digest>::success(digest);
}

clonecore::Result<diskmodel::PartitionStyle> parse_partition_style(
    const std::wstring_view text) {
  if (text == L"RAW") {
    return clonecore::Result<diskmodel::PartitionStyle>::success(
        diskmodel::PartitionStyle::raw);
  }
  if (text == L"MBR") {
    return clonecore::Result<diskmodel::PartitionStyle>::success(
        diskmodel::PartitionStyle::mbr);
  }
  if (text == L"GPT") {
    return clonecore::Result<diskmodel::PartitionStyle>::success(
        diskmodel::PartitionStyle::gpt);
  }
  return failure<diskmodel::PartitionStyle>(
      ErrorCode::invalid_data,
      ERROR_NOT_SUPPORTED,
      L"所有manifestのパーティション形式が不明です");
}

clonecore::Result<RescueUsbDataFileSystem> parse_data_file_system(
    const std::wstring_view text) {
  if (text == L"NTFS") {
    return clonecore::Result<RescueUsbDataFileSystem>::success(
        RescueUsbDataFileSystem::ntfs);
  }
  if (text == L"exFAT") {
    return clonecore::Result<RescueUsbDataFileSystem>::success(
        RescueUsbDataFileSystem::exfat);
  }
  return failure<RescueUsbDataFileSystem>(
      ErrorCode::invalid_data,
      ERROR_NOT_SUPPORTED,
      L"所有manifestのデータ領域ファイルシステムが不明です");
}

clonecore::Result<RescueMediaFileFingerprint> parse_manifest_file(
    JsonCursor& cursor) {
  if (!cursor.consume('{')) {
    return failure<RescueMediaFileFingerprint>(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"所有manifestのfile objectを読み取れません");
  }
  RescueMediaFileFingerprint file;
  std::uint8_t fields{};
  bool first = true;
  while (!cursor.consume('}')) {
    if (!first && !cursor.consume(',')) {
      return failure<RescueMediaFileFingerprint>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"所有manifestのfile区切りが不正です");
    }
    first = false;
    auto key = cursor.parse_string(kMaximumJsonKeyCharacters);
    if (!key || !cursor.consume(':')) {
      return clonecore::Result<RescueMediaFileFingerprint>::failure(
          key ? inspection_error(
                    ErrorCode::invalid_data, ERROR_INVALID_DATA,
                    L"所有manifestのfile key区切りがありません")
              : key.error());
    }
    std::uint8_t field{};
    if (key.value() == L"relativePath") {
      field = 1U;
      auto value = cursor.parse_string(kMaximumJsonStringCharacters);
      if (!value) {
        return clonecore::Result<RescueMediaFileFingerprint>::failure(
            value.error());
      }
      file.relative_path = value.take_value();
    } else if (key.value() == L"length") {
      field = 2U;
      auto value = cursor.parse_uint64();
      if (!value) {
        return clonecore::Result<RescueMediaFileFingerprint>::failure(
            value.error());
      }
      file.length = value.value();
    } else if (key.value() == L"sha256") {
      field = 4U;
      auto value = cursor.parse_string(64U);
      if (!value) {
        return clonecore::Result<RescueMediaFileFingerprint>::failure(
            value.error());
      }
      auto digest = parse_digest(value.value());
      if (!digest) {
        return clonecore::Result<RescueMediaFileFingerprint>::failure(
            digest.error());
      }
      file.sha256 = digest.value();
    } else {
      return failure<RescueMediaFileFingerprint>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"所有manifestのfile objectに未知fieldがあります");
    }
    if ((fields & field) != 0U) {
      return failure<RescueMediaFileFingerprint>(
          ErrorCode::invalid_data, ERROR_DUP_NAME,
          L"所有manifestのfile objectに重複fieldがあります");
    }
    fields |= field;
  }
  if (fields != 7U) {
    return failure<RescueMediaFileFingerprint>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"所有manifestのfile objectに必須fieldがありません");
  }
  file.reparse_point = false;
  file.hard_link_count = 1U;
  return clonecore::Result<RescueMediaFileFingerprint>::success(
      std::move(file));
}

clonecore::Result<RescueUsbPartitionLayoutEntry> parse_manifest_partition(
    JsonCursor& cursor) {
  if (!cursor.consume('{')) {
    return failure<RescueUsbPartitionLayoutEntry>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"所有manifestのpartition objectを読み取れません");
  }
  RescueUsbPartitionLayoutEntry partition;
  std::uint8_t fields{};
  bool first = true;
  while (!cursor.consume('}')) {
    if (!first && !cursor.consume(',')) {
      return failure<RescueUsbPartitionLayoutEntry>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"所有manifestのpartition区切りが不正です");
    }
    first = false;
    auto key = cursor.parse_string(kMaximumJsonKeyCharacters);
    if (!key || !cursor.consume(':')) {
      return clonecore::Result<RescueUsbPartitionLayoutEntry>::failure(
          key ? inspection_error(
                    ErrorCode::invalid_data, ERROR_INVALID_DATA,
                    L"所有manifestのpartition key区切りがありません")
              : key.error());
    }
    std::uint8_t field{};
    if (key.value() == L"number") {
      field = 1U;
      auto value = cursor.parse_uint64();
      if (!value || value.value() >
                        (std::numeric_limits<std::uint32_t>::max)()) {
        return failure<RescueUsbPartitionLayoutEntry>(
            ErrorCode::invalid_data, ERROR_ARITHMETIC_OVERFLOW,
            L"所有manifestのpartition番号が範囲外です");
      }
      partition.number = static_cast<std::uint32_t>(value.value());
    } else if (key.value() == L"style") {
      field = 2U;
      auto value = cursor.parse_string(16U);
      if (!value) {
        return clonecore::Result<RescueUsbPartitionLayoutEntry>::failure(
            value.error());
      }
      auto style = parse_partition_style(value.value());
      if (!style) {
        return clonecore::Result<RescueUsbPartitionLayoutEntry>::failure(
            style.error());
      }
      partition.style = style.value();
    } else if (key.value() == L"type") {
      field = 4U;
      auto value = cursor.parse_string(128U);
      if (!value) {
        return clonecore::Result<RescueUsbPartitionLayoutEntry>::failure(
            value.error());
      }
      partition.type = value.take_value();
    } else if (key.value() == L"offsetBytes") {
      field = 8U;
      auto value = cursor.parse_uint64();
      if (!value) {
        return clonecore::Result<RescueUsbPartitionLayoutEntry>::failure(
            value.error());
      }
      partition.offset_bytes = value.value();
    } else if (key.value() == L"sizeBytes") {
      field = 16U;
      auto value = cursor.parse_uint64();
      if (!value) {
        return clonecore::Result<RescueUsbPartitionLayoutEntry>::failure(
            value.error());
      }
      partition.size_bytes = value.value();
    } else if (key.value() == L"bootable") {
      field = 32U;
      auto value = cursor.parse_bool();
      if (!value) {
        return clonecore::Result<RescueUsbPartitionLayoutEntry>::failure(
            value.error());
      }
      partition.bootable = value.value();
    } else {
      return failure<RescueUsbPartitionLayoutEntry>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"所有manifestのpartition objectに未知fieldがあります");
    }
    if ((fields & field) != 0U) {
      return failure<RescueUsbPartitionLayoutEntry>(
          ErrorCode::invalid_data, ERROR_DUP_NAME,
          L"所有manifestのpartition objectに重複fieldがあります");
    }
    fields |= field;
  }
  if (fields != 63U) {
    return failure<RescueUsbPartitionLayoutEntry>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"所有manifestのpartition objectに必須fieldがありません");
  }
  return clonecore::Result<RescueUsbPartitionLayoutEntry>::success(
      std::move(partition));
}

clonecore::Result<RescueUsbCanonicalLayout> parse_manifest_layout(
    JsonCursor& cursor) {
  if (!cursor.consume('{')) {
    return failure<RescueUsbCanonicalLayout>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"所有manifestのcanonicalLayoutを読み取れません");
  }
  RescueUsbCanonicalLayout layout;
  std::uint8_t fields{};
  bool first = true;
  while (!cursor.consume('}')) {
    if (!first && !cursor.consume(',')) {
      return failure<RescueUsbCanonicalLayout>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"所有manifestのcanonicalLayout区切りが不正です");
    }
    first = false;
    auto key = cursor.parse_string(kMaximumJsonKeyCharacters);
    if (!key || !cursor.consume(':')) {
      return clonecore::Result<RescueUsbCanonicalLayout>::failure(
          key ? inspection_error(
                    ErrorCode::invalid_data, ERROR_INVALID_DATA,
                    L"所有manifestのcanonicalLayout key区切りがありません")
              : key.error());
    }
    std::uint8_t field{};
    if (key.value() == L"diskStyle") {
      field = 1U;
      auto value = cursor.parse_string(16U);
      if (!value) {
        return clonecore::Result<RescueUsbCanonicalLayout>::failure(
            value.error());
      }
      auto style = parse_partition_style(value.value());
      if (!style) {
        return clonecore::Result<RescueUsbCanonicalLayout>::failure(
            style.error());
      }
      layout.disk_style = style.value();
    } else if (key.value() == L"partitions") {
      field = 2U;
      if (!cursor.consume('[')) {
        return failure<RescueUsbCanonicalLayout>(
            ErrorCode::invalid_data, ERROR_INVALID_DATA,
            L"所有manifestのpartition配列を読み取れません");
      }
      bool first_partition = true;
      while (!cursor.consume(']')) {
        if (!first_partition && !cursor.consume(',')) {
          return failure<RescueUsbCanonicalLayout>(
              ErrorCode::invalid_data, ERROR_INVALID_DATA,
              L"所有manifestのpartition配列区切りが不正です");
        }
        first_partition = false;
        if (layout.partitions.size() >= kMaximumManifestPartitionCount) {
          return failure<RescueUsbCanonicalLayout>(
              ErrorCode::invalid_data, ERROR_TOO_MANY_OPEN_FILES,
              L"所有manifestのpartition件数が上限を超えています");
        }
        auto partition = parse_manifest_partition(cursor);
        if (!partition) {
          return clonecore::Result<RescueUsbCanonicalLayout>::failure(
              partition.error());
        }
        layout.partitions.push_back(partition.take_value());
      }
    } else {
      return failure<RescueUsbCanonicalLayout>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"所有manifestのcanonicalLayoutに未知fieldがあります");
    }
    if ((fields & field) != 0U) {
      return failure<RescueUsbCanonicalLayout>(
          ErrorCode::invalid_data, ERROR_DUP_NAME,
          L"所有manifestのcanonicalLayoutに重複fieldがあります");
    }
    fields |= field;
  }
  if (fields != 3U) {
    return failure<RescueUsbCanonicalLayout>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"所有manifestのcanonicalLayoutに必須fieldがありません");
  }
  std::sort(
      layout.partitions.begin(), layout.partitions.end(),
      [](const auto& left, const auto& right) {
        return std::tie(
                   left.number, left.style, left.type, left.offset_bytes,
                   left.size_bytes, left.bootable) <
            std::tie(
                   right.number, right.style, right.type, right.offset_bytes,
                   right.size_bytes, right.bootable);
      });
  return clonecore::Result<RescueUsbCanonicalLayout>::success(
      std::move(layout));
}

clonecore::Result<RescueMediaTreeIdentity> parse_tree_identity_fields(
    JsonCursor& cursor,
    std::vector<RescueMediaFileFingerprint>& files,
    std::vector<std::wstring>& directories) {
  if (!cursor.consume('{')) {
    return failure<RescueMediaTreeIdentity>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"所有manifestのownedBootTreeを読み取れません");
  }
  RescueMediaTreeIdentity identity;
  std::uint8_t fields{};
  bool first = true;
  while (!cursor.consume('}')) {
    if (!first && !cursor.consume(',')) {
      return failure<RescueMediaTreeIdentity>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"所有manifestのownedBootTree区切りが不正です");
    }
    first = false;
    auto key = cursor.parse_string(kMaximumJsonKeyCharacters);
    if (!key || !cursor.consume(':')) {
      return clonecore::Result<RescueMediaTreeIdentity>::failure(
          key ? inspection_error(
                    ErrorCode::invalid_data, ERROR_INVALID_DATA,
                    L"所有manifestのownedBootTree key区切りがありません")
              : key.error());
    }
    std::uint8_t field{};
    if (key.value() == L"entryCount" || key.value() == L"fileCount" ||
        key.value() == L"totalPathCharacters" ||
        key.value() == L"totalLogicalBytes") {
      auto value = cursor.parse_uint64();
      if (!value) {
        return clonecore::Result<RescueMediaTreeIdentity>::failure(
            value.error());
      }
      if (key.value() == L"entryCount") {
        field = 1U;
        identity.entry_count = value.value();
      } else if (key.value() == L"fileCount") {
        field = 2U;
        identity.file_count = value.value();
      } else if (key.value() == L"totalPathCharacters") {
        field = 4U;
        identity.total_path_characters = value.value();
      } else {
        field = 8U;
        identity.total_logical_bytes = value.value();
      }
    } else if (key.value() == L"rootDigest") {
      field = 16U;
      auto value = cursor.parse_string(64U);
      if (!value) {
        return clonecore::Result<RescueMediaTreeIdentity>::failure(
            value.error());
      }
      auto digest = parse_digest(value.value());
      if (!digest) {
        return clonecore::Result<RescueMediaTreeIdentity>::failure(
            digest.error());
      }
      identity.root_digest = digest.value();
    } else if (key.value() == L"files") {
      field = 32U;
      if (!cursor.consume('[')) {
        return failure<RescueMediaTreeIdentity>(
            ErrorCode::invalid_data, ERROR_INVALID_DATA,
            L"所有manifestのfiles配列を読み取れません");
      }
      bool first_file = true;
      while (!cursor.consume(']')) {
        if (!first_file && !cursor.consume(',')) {
          return failure<RescueMediaTreeIdentity>(
              ErrorCode::invalid_data, ERROR_INVALID_DATA,
              L"所有manifestのfiles配列区切りが不正です");
        }
        first_file = false;
        if (files.size() >= kRescueUsbMaximumBootTreeEntryCount) {
          return failure<RescueMediaTreeIdentity>(
              ErrorCode::invalid_data, ERROR_TOO_MANY_OPEN_FILES,
              L"所有manifestの起動ファイル件数が上限を超えています");
        }
        auto file = parse_manifest_file(cursor);
        if (!file) {
          return clonecore::Result<RescueMediaTreeIdentity>::failure(
              file.error());
        }
        files.push_back(file.take_value());
      }
    } else if (key.value() == L"directories") {
      field = 64U;
      if (!cursor.consume('[')) {
        return failure<RescueMediaTreeIdentity>(
            ErrorCode::invalid_data, ERROR_INVALID_DATA,
            L"所有manifestのdirectories配列を読み取れません");
      }
      bool first_directory = true;
      while (!cursor.consume(']')) {
        if (!first_directory && !cursor.consume(',')) {
          return failure<RescueMediaTreeIdentity>(
              ErrorCode::invalid_data, ERROR_INVALID_DATA,
              L"所有manifestのdirectories配列区切りが不正です");
        }
        first_directory = false;
        if (directories.size() >= kRescueUsbMaximumBootTreeEntryCount) {
          return failure<RescueMediaTreeIdentity>(
              ErrorCode::invalid_data, ERROR_TOO_MANY_OPEN_FILES,
              L"所有manifestの起動ディレクトリ件数が上限を超えています");
        }
        auto directory = cursor.parse_string(kMaximumJsonStringCharacters);
        if (!directory) {
          return clonecore::Result<RescueMediaTreeIdentity>::failure(
              directory.error());
        }
        directories.push_back(directory.take_value());
      }
    } else {
      return failure<RescueMediaTreeIdentity>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"所有manifestのownedBootTreeに未知fieldがあります");
    }
    if ((fields & field) != 0U) {
      return failure<RescueMediaTreeIdentity>(
          ErrorCode::invalid_data, ERROR_DUP_NAME,
          L"所有manifestのownedBootTreeに重複fieldがあります");
    }
    fields |= field;
  }
  if (fields != 127U) {
    return failure<RescueMediaTreeIdentity>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"所有manifestのownedBootTreeに必須fieldがありません");
  }
  const auto recalculated = make_rescue_usb_boot_tree_identity(
      files, directories);
  if (!recalculated || recalculated.value() != identity) {
    return clonecore::Result<RescueMediaTreeIdentity>::failure(
        recalculated
            ? inspection_error(
                  ErrorCode::verification_failed,
                  ERROR_DATA_CHECKSUM_ERROR,
                  L"所有manifestのownedBootTree集約値が内容と一致しません")
            : recalculated.error());
  }
  return clonecore::Result<RescueMediaTreeIdentity>::success(identity);
}

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(), left.end(), right.begin(),
          [](const wchar_t lhs, const wchar_t rhs) {
            return std::towupper(lhs) == std::towupper(rhs);
          });
}

bool same_cache_identity(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  return left.disk_number == right.disk_number &&
      left.model == right.model && left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.serial_suffix == right.serial_suffix &&
      left.device_instance_id == right.device_instance_id &&
      left.is_system_disk == right.is_system_disk;
}

struct VolumeGeometry final {
  wchar_t boot_drive_letter{};
  std::wstring boot_root;
  wchar_t data_drive_letter{};
  std::wstring data_root;
};

clonecore::Result<VolumeGeometry> resolve_owned_volume_geometry(
    const diskmodel::DiskInfo& target,
    const std::span<const DriveLetterVolume> volumes) {
  const auto layout = make_rescue_usb_canonical_layout(target);
  const auto completed = validate_rescue_usb_completed_layout(
      layout, target.size_bytes);
  if (!completed) {
    return clonecore::Result<VolumeGeometry>::failure(completed.error());
  }
  const auto& boot = target.partitions[0];
  const auto& data = target.partitions[1];
  std::optional<wchar_t> boot_letter;
  std::optional<wchar_t> data_letter;
  for (const auto& volume : volumes) {
    const std::size_t matching_extents = static_cast<std::size_t>(
        std::count_if(
            volume.extents.begin(), volume.extents.end(),
            [&](const DriveLetterDiskExtent& extent) {
              return extent.disk_number == target.disk_number;
            }));
    if (matching_extents == 0U) {
      continue;
    }
    if (volume.drive_letter < L'A' || volume.drive_letter > L'Z' ||
        volume.extents.size() != 1U || matching_extents != 1U) {
      return failure<VolumeGeometry>(
          ErrorCode::identity_mismatch, ERROR_INVALID_DRIVE,
          L"所有媒体の起動／データ領域対応が曖昧です");
    }
    const auto& extent = volume.extents.front();
    std::optional<wchar_t>* destination = nullptr;
    if (extent.starting_offset == boot.offset_bytes &&
        extent.length == boot.size_bytes) {
      destination = &boot_letter;
    } else if (extent.starting_offset == data.offset_bytes &&
               extent.length == data.size_bytes) {
      destination = &data_letter;
    } else {
      return failure<VolumeGeometry>(
          ErrorCode::identity_mismatch, ERROR_INVALID_DATA,
          L"所有媒体のボリューム範囲が完全レイアウトと一致しません");
    }
    if (destination->has_value()) {
      return failure<VolumeGeometry>(
          ErrorCode::identity_mismatch, ERROR_DUP_NAME,
          L"所有媒体の同一領域に複数ドライブ文字があります");
    }
    *destination = volume.drive_letter;
  }
  if (!boot_letter.has_value() || !data_letter.has_value() ||
      boot_letter.value() == data_letter.value()) {
    return failure<VolumeGeometry>(
        ErrorCode::identity_mismatch, ERROR_NOT_FOUND,
        L"所有媒体の起動／データ領域を一意に解決できません");
  }
  return clonecore::Result<VolumeGeometry>::success({
      .boot_drive_letter = boot_letter.value(),
      .boot_root = std::wstring{boot_letter.value(), L':', L'\\'},
      .data_drive_letter = data_letter.value(),
      .data_root = std::wstring{data_letter.value(), L':', L'\\'},
  });
}

clonecore::Result<std::wstring> query_volume_file_system(
    const std::wstring& root) {
  std::array<wchar_t, 64U> file_system{};
  if (GetVolumeInformationW(
          root.c_str(), nullptr, 0U, nullptr, nullptr, nullptr,
          file_system.data(), static_cast<DWORD>(file_system.size())) ==
      FALSE) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"レスキューUSBファイルシステムの読取り専用照会",
            GetLastError()));
  }
  if (file_system.front() == L'\0') {
    return failure<std::wstring>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"レスキューUSBのファイルシステム名が空です");
  }
  return clonecore::Result<std::wstring>::success(file_system.data());
}

std::wstring extended_path(
    const std::wstring& root,
    const std::wstring_view relative = {}) {
  std::wstring result = L"\\\\?\\" + root;
  if (!relative.empty()) {
    result.append(relative);
  }
  return result;
}

struct OpenedRegularFile final {
  clonecore::UniqueHandle handle;
  BY_HANDLE_FILE_INFORMATION before{};
  std::uint64_t length{};
};

clonecore::Result<OpenedRegularFile> open_regular_file_read_only(
    const std::wstring& path,
    const std::uint64_t maximum_bytes,
    const std::wstring_view role,
    const bool private_path) {
  clonecore::UniqueHandle handle(CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  if (!handle) {
    return clonecore::Result<OpenedRegularFile>::failure(
        inspection_error(
            ErrorCode::query_failed, GetLastError(),
            private_path
                ? L"保持データの非公開tree scanでファイルを開けません"
                : std::wstring(role) + L"を読み取り専用で開けません"));
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (GetFileInformationByHandle(handle.get(), &information) == FALSE) {
    return clonecore::Result<OpenedRegularFile>::failure(
        inspection_error(
            ErrorCode::query_failed, GetLastError(),
            private_path
                ? L"保持データの非公開tree scanで属性を確認できません"
                : std::wstring(role) + L"の属性を確認できません"));
  }
  const std::uint64_t length =
      (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
      information.nFileSizeLow;
  if ((information.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
      information.nNumberOfLinks != 1U || length > maximum_bytes) {
    return failure<OpenedRegularFile>(
        ErrorCode::unsupported_layout, ERROR_REPARSE_TAG_INVALID,
        private_path
            ? L"保持データの非公開tree scanで通常単一linkファイル以外を検出しました"
            : std::wstring(role) +
                  L"が通常単一linkファイルではないかサイズ上限外です");
  }
  return clonecore::Result<OpenedRegularFile>::success({
      .handle = std::move(handle),
      .before = information,
      .length = length,
  });
}

bool same_file_observation(
    const BY_HANDLE_FILE_INFORMATION& before,
    const BY_HANDLE_FILE_INFORMATION& after) noexcept {
  return before.dwVolumeSerialNumber == after.dwVolumeSerialNumber &&
      before.nFileIndexHigh == after.nFileIndexHigh &&
      before.nFileIndexLow == after.nFileIndexLow &&
      before.nFileSizeHigh == after.nFileSizeHigh &&
      before.nFileSizeLow == after.nFileSizeLow &&
      CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0 &&
      before.nNumberOfLinks == after.nNumberOfLinks &&
      (after.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U;
}

clonecore::Result<std::vector<std::byte>> read_exact_at(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::size_t length,
    const bool private_path,
    const std::atomic_bool* const cancellation_requested) {
  if (offset > static_cast<std::uint64_t>(
                   (std::numeric_limits<LONGLONG>::max)()) ||
      length > (std::numeric_limits<DWORD>::max)()) {
    return failure<std::vector<std::byte>>(
        ErrorCode::invalid_data, ERROR_ARITHMETIC_OVERFLOW,
        private_path ? L"保持データの非公開tree scan読取り範囲が不正です"
                     : L"起動領域ファイルの読取り範囲が不正です");
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == FALSE) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            private_path ? L"保持データの非公開tree scan seek"
                         : L"起動領域ファイルseek",
            GetLastError()));
  }
  std::vector<std::byte> buffer(length);
  std::size_t completed{};
  while (completed < length) {
    if (inspection_cancelled(cancellation_requested)) {
      return failure<std::vector<std::byte>>(
          ErrorCode::cancelled, ERROR_CANCELLED,
          L"レスキューUSB所有情報の検査を取り消しました");
    }
    const DWORD requested = static_cast<DWORD>((std::min)(
        length - completed,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read{};
    const BOOL read_succeeded = ReadFile(
        handle, buffer.data() + completed, requested, &read, nullptr);
    if (read_succeeded == FALSE || read == 0U || read > requested) {
      const DWORD native_code = read_succeeded == FALSE
          ? GetLastError()
          : ERROR_HANDLE_EOF;
      return clonecore::Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              ErrorCode::query_failed,
              private_path ? L"保持データの非公開tree scan読取り"
                           : L"起動領域ファイル読取り",
              native_code));
    }
    completed += read;
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(buffer));
}

clonecore::Result<std::vector<std::byte>> read_small_regular_file(
    const std::wstring& path,
    const std::uint64_t maximum_bytes,
    const std::wstring_view role,
    const std::atomic_bool* const cancellation_requested) {
  auto opened = open_regular_file_read_only(
      path, maximum_bytes, role, false);
  if (!opened) {
    return clonecore::Result<std::vector<std::byte>>::failure(opened.error());
  }
  if (opened.value().length == 0U ||
      opened.value().length >
          static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    return failure<std::vector<std::byte>>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        std::wstring(role) + L"が空またはアドレス空間上限外です");
  }
  auto bytes = read_exact_at(
      opened.value().handle.get(), 0U,
      static_cast<std::size_t>(opened.value().length), false,
      cancellation_requested);
  if (!bytes) {
    return bytes;
  }
  BY_HANDLE_FILE_INFORMATION after{};
  if (GetFileInformationByHandle(
          opened.value().handle.get(), &after) == FALSE ||
      !same_file_observation(opened.value().before, after)) {
    return failure<std::vector<std::byte>>(
        ErrorCode::identity_mismatch, ERROR_DATA_CHECKSUM_ERROR,
        std::wstring(role) + L"が読取り中に変化しました");
  }
  return bytes;
}

clonecore::Result<RescueMediaFileFingerprint> fingerprint_file(
    const std::wstring& path,
    std::wstring relative_path,
    const std::uint64_t maximum_bytes,
    const bool private_path,
    const std::atomic_bool* const cancellation_requested) {
  auto opened = open_regular_file_read_only(
      path, maximum_bytes,
      private_path ? L"保持データ" : L"起動領域ファイル",
      private_path);
  if (!opened) {
    return clonecore::Result<RescueMediaFileFingerprint>::failure(
        opened.error());
  }
  const imageformat::Sha256ReadCallback reader =
      [&](const std::uint64_t offset, const std::size_t length) {
        return read_exact_at(
            opened.value().handle.get(), offset, length, private_path,
            cancellation_requested);
      };
  auto digest = imageformat::sha256_from_reader(
      opened.value().length, kHashReadBlockBytes, reader);
  if (!digest) {
    return clonecore::Result<RescueMediaFileFingerprint>::failure(
        digest.error());
  }
  BY_HANDLE_FILE_INFORMATION after{};
  if (GetFileInformationByHandle(
          opened.value().handle.get(), &after) == FALSE ||
      !same_file_observation(opened.value().before, after)) {
    return failure<RescueMediaFileFingerprint>(
        ErrorCode::identity_mismatch, ERROR_DATA_CHECKSUM_ERROR,
        private_path
            ? L"保持データの非公開tree scan中にファイルが変化しました"
            : L"起動領域ファイルが読取り中に変化しました");
  }
  return clonecore::Result<RescueMediaFileFingerprint>::success({
      .relative_path = std::move(relative_path),
      .length = opened.value().length,
      .sha256 = digest.value(),
      .reparse_point = false,
      .hard_link_count = 1U,
  });
}

class UniqueFindHandle final {
 public:
  explicit UniqueFindHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueFindHandle() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      FindClose(handle_);
    }
  }
  UniqueFindHandle(const UniqueFindHandle&) = delete;
  UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct ScannedTree final {
  RescueMediaTreeIdentity identity;
  std::vector<RescueMediaFileFingerprint> files;
  std::vector<std::wstring> directories;
};

bool excluded_private_path(const std::wstring_view relative) noexcept {
  const std::size_t separator = relative.find(L'\\');
  const std::wstring_view first = relative.substr(0U, separator);
  return equals_case_insensitive(first, L"System Volume Information") ||
      equals_case_insensitive(first, L"$RECYCLE.BIN");
}

clonecore::Result<ScannedTree> scan_volume_tree(
    const std::wstring& root,
    const bool private_data,
    const std::atomic_bool* const cancellation_requested) {
  const std::size_t maximum_entries = private_data
      ? kRescueUsbMaximumDataTreeEntryCount
      : kRescueUsbMaximumBootTreeEntryCount;
  const std::uint64_t maximum_path_characters = private_data
      ? kRescueUsbMaximumDataPathCharacters
      : kRescueUsbMaximumBootPathCharacters;
  const std::uint64_t maximum_logical_bytes = private_data
      ? kRescueUsbMaximumDataLogicalBytes
      : kRescueUsbMaximumBootLogicalBytes;
  SetLastError(ERROR_SUCCESS);
  const DWORD root_attributes = GetFileAttributesW(
      extended_path(root).c_str());
  if (root_attributes == INVALID_FILE_ATTRIBUTES ||
      (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    const DWORD native_code = root_attributes == INVALID_FILE_ATTRIBUTES
        ? GetLastError()
        : ERROR_INVALID_DATA;
    return failure<ScannedTree>(
        ErrorCode::unsupported_layout, native_code,
        private_data
            ? L"保持データ領域の通常ルートを確認できません"
            : L"起動領域の通常ルートを確認できません");
  }

  struct PendingDirectory final {
    std::wstring relative;
  };
  std::vector<PendingDirectory> pending{{}};
  std::vector<RescueMediaFileFingerprint> files;
  std::vector<std::wstring> directories;
  std::uint64_t total_path_characters{};
  std::uint64_t total_logical_bytes{};
  while (!pending.empty()) {
    if (inspection_cancelled(cancellation_requested)) {
      return failure<ScannedTree>(
          ErrorCode::cancelled, ERROR_CANCELLED,
          L"レスキューUSB所有情報の検査を取り消しました");
    }
    PendingDirectory current = std::move(pending.back());
    pending.pop_back();
    const std::wstring search = extended_path(root, current.relative) +
        (current.relative.empty() ? L"*" : L"\\*");
    WIN32_FIND_DATAW found{};
    UniqueFindHandle handle(FindFirstFileExW(
        search.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
        nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (handle.get() == INVALID_HANDLE_VALUE) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND) {
        continue;
      }
      return clonecore::Result<ScannedTree>::failure(inspection_error(
          ErrorCode::query_failed, native_code,
          private_data
              ? L"保持データの非公開tree scanで列挙に失敗しました"
              : L"起動領域treeの列挙に失敗しました"));
    }
    for (;;) {
      const std::wstring_view name(found.cFileName);
      if (name != L"." && name != L"..") {
        std::wstring relative = current.relative.empty()
            ? std::wstring(name)
            : current.relative + L"\\" + std::wstring(name);
        if (!(private_data && excluded_private_path(relative)) &&
            !(!private_data && equals_case_insensitive(
                 relative, kRescueUsbManifestRelativePath))) {
          if (files.size() + directories.size() >= maximum_entries ||
              relative.size() >
                  maximum_path_characters - total_path_characters) {
            return failure<ScannedTree>(
                ErrorCode::invalid_data, ERROR_BUFFER_OVERFLOW,
                private_data
                    ? L"保持データの非公開tree scanが件数／パス上限を超えました"
                    : L"起動領域treeが件数／パス上限を超えました");
          }
          total_path_characters += relative.size();
          if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return failure<ScannedTree>(
                ErrorCode::unsupported_layout, ERROR_REPARSE_TAG_INVALID,
                private_data
                    ? L"保持データの非公開tree scanでreparse pointを検出しました"
                    : L"起動領域treeでreparse pointを検出しました");
          }
          if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            directories.push_back(relative);
            pending.push_back({std::move(relative)});
          } else {
            const std::uint64_t advertised_length =
                (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32U) |
                found.nFileSizeLow;
            if (advertised_length >
                maximum_logical_bytes - total_logical_bytes) {
              return failure<ScannedTree>(
                  ErrorCode::invalid_data, ERROR_DISK_FULL,
                  private_data
                      ? L"保持データの非公開tree scanが容量上限を超えました"
                      : L"起動領域treeが容量上限を超えました");
            }
            auto fingerprint = fingerprint_file(
                extended_path(root, relative), relative,
                maximum_logical_bytes - total_logical_bytes, private_data,
                cancellation_requested);
            if (!fingerprint) {
              return clonecore::Result<ScannedTree>::failure(
                  fingerprint.error());
            }
            total_logical_bytes += fingerprint.value().length;
            files.push_back(fingerprint.take_value());
          }
        }
      }
      if (FindNextFileW(handle.get(), &found) == FALSE) {
        const DWORD native_code = GetLastError();
        if (native_code != ERROR_NO_MORE_FILES) {
          return clonecore::Result<ScannedTree>::failure(inspection_error(
              ErrorCode::query_failed, native_code,
              private_data
                  ? L"保持データの非公開tree scanで列挙に失敗しました"
                  : L"起動領域treeの列挙に失敗しました"));
        }
        break;
      }
    }
  }
  const auto identity = private_data
      ? make_rescue_usb_private_data_tree_identity(files, directories)
      : make_rescue_usb_boot_tree_identity(files, directories);
  if (!identity) {
    return clonecore::Result<ScannedTree>::failure(identity.error());
  }
  return clonecore::Result<ScannedTree>::success({
      .identity = identity.value(),
      .files = std::move(files),
      .directories = std::move(directories),
  });
}

clonecore::Result<RescueMediaTreeIdentity>
scan_private_data_tree_identity(
    const std::wstring& root,
    const std::atomic_bool* const cancellation_requested) {
  auto tree = scan_volume_tree(root, true, cancellation_requested);
  if (!tree) {
    return clonecore::Result<RescueMediaTreeIdentity>::failure(tree.error());
  }
  // Private relative paths exist only inside this bounded enumeration helper.
  // They are discarded before the aggregate identity crosses the seam.
  return clonecore::Result<RescueMediaTreeIdentity>::success(
      tree.value().identity);
}

enum class PathPresence : std::uint8_t { absent, regular, invalid };

PathPresence probe_regular_file(
    const std::wstring& path,
    DWORD& native_code) noexcept {
  SetLastError(ERROR_SUCCESS);
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    native_code = GetLastError();
    return native_code == ERROR_FILE_NOT_FOUND ||
               native_code == ERROR_PATH_NOT_FOUND
        ? PathPresence::absent
        : PathPresence::invalid;
  }
  native_code = ERROR_SUCCESS;
  return (attributes &
          (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U
      ? PathPresence::regular
      : PathPresence::invalid;
}

RescueUsbInspectionResult blocked_result(std::wstring message) {
  return RescueUsbInspectionResult{
      .state = RescueUsbInspectionState::blocked,
      .message = std::move(message),
      .physical_write_started = false,
  };
}

RescueUsbInspectionResult unknown_result() {
  return RescueUsbInspectionResult{
      .state = RescueUsbInspectionState::unknown_media,
      .message =
          L"検証済みY-TEC所有情報がない媒体です。全消去初期化を推奨します。",
      .physical_write_started = false,
  };
}

}  // namespace

clonecore::Result<RescueUsbOwnershipManifest>
parse_rescue_usb_ownership_manifest(
    const std::span<const std::byte> utf8_json) {
  if (utf8_json.empty() ||
      utf8_json.size() > kRescueUsbMaximumOwnershipManifestBytes) {
    return failure<RescueUsbOwnershipManifest>(
        ErrorCode::invalid_data, ERROR_FILE_TOO_LARGE,
        L"レスキューUSB所有manifestが空またはサイズ上限外です");
  }
  const std::string_view input(
      reinterpret_cast<const char*>(utf8_json.data()), utf8_json.size());
  JsonCursor cursor(input);
  if (!cursor.consume('{')) {
    return failure<RescueUsbOwnershipManifest>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"レスキューUSB所有manifestのJSON objectを読み取れません");
  }
  RescueUsbOwnershipManifest manifest;
  std::uint16_t fields{};
  bool first = true;
  while (!cursor.consume('}')) {
    if (!first && !cursor.consume(',')) {
      return failure<RescueUsbOwnershipManifest>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"レスキューUSB所有manifestのJSON区切りが不正です");
    }
    first = false;
    auto key = cursor.parse_string(kMaximumJsonKeyCharacters);
    if (!key || !cursor.consume(':')) {
      return clonecore::Result<RescueUsbOwnershipManifest>::failure(
          key ? inspection_error(
                    ErrorCode::invalid_data, ERROR_INVALID_DATA,
                    L"レスキューUSB所有manifestのkey区切りがありません")
              : key.error());
    }
    std::uint16_t field{};
    if (key.value() == L"schemaVersion") {
      field = 1U;
      auto value = cursor.parse_uint64();
      if (!value || value.value() >
                        (std::numeric_limits<std::uint32_t>::max)()) {
        return failure<RescueUsbOwnershipManifest>(
            ErrorCode::invalid_data, ERROR_ARITHMETIC_OVERFLOW,
            L"所有manifestのschemaVersionが範囲外です");
      }
      manifest.schema_version = static_cast<std::uint32_t>(value.value());
    } else if (key.value() == L"purpose") {
      field = 2U;
      auto value = cursor.parse_string(256U);
      if (!value) {
        return clonecore::Result<RescueUsbOwnershipManifest>::failure(
            value.error());
      }
      manifest.purpose = value.take_value();
    } else if (key.value() == L"mediaId") {
      field = 4U;
      auto value = cursor.parse_string(64U);
      if (!value) {
        return clonecore::Result<RescueUsbOwnershipManifest>::failure(
            value.error());
      }
      manifest.media_id = value.take_value();
    } else if (key.value() == L"bootFileSystem") {
      field = 8U;
      auto value = cursor.parse_string(32U);
      if (!value) {
        return clonecore::Result<RescueUsbOwnershipManifest>::failure(
            value.error());
      }
      manifest.boot_file_system = value.take_value();
    } else if (key.value() == L"dataFileSystem") {
      field = 16U;
      auto value = cursor.parse_string(32U);
      if (!value) {
        return clonecore::Result<RescueUsbOwnershipManifest>::failure(
            value.error());
      }
      auto file_system = parse_data_file_system(value.value());
      if (!file_system) {
        return clonecore::Result<RescueUsbOwnershipManifest>::failure(
            file_system.error());
      }
      manifest.data_file_system = file_system.value();
    } else if (key.value() == L"bootPartitionBytes") {
      field = 32U;
      auto value = cursor.parse_uint64();
      if (!value) {
        return clonecore::Result<RescueUsbOwnershipManifest>::failure(
            value.error());
      }
      manifest.boot_partition_bytes = value.value();
    } else if (key.value() == L"canonicalLayout") {
      field = 64U;
      auto layout = parse_manifest_layout(cursor);
      if (!layout) {
        return clonecore::Result<RescueUsbOwnershipManifest>::failure(
            layout.error());
      }
      manifest.canonical_layout = layout.take_value();
    } else if (key.value() == L"ownedBootTree") {
      field = 128U;
      auto identity = parse_tree_identity_fields(
          cursor, manifest.owned_boot_files,
          manifest.owned_boot_directories);
      if (!identity) {
        return clonecore::Result<RescueUsbOwnershipManifest>::failure(
            identity.error());
      }
      manifest.owned_boot_tree_identity = identity.value();
    } else {
      return failure<RescueUsbOwnershipManifest>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"レスキューUSB所有manifestに未知fieldがあります");
    }
    if ((fields & field) != 0U) {
      return failure<RescueUsbOwnershipManifest>(
          ErrorCode::invalid_data, ERROR_DUP_NAME,
          L"レスキューUSB所有manifestに重複fieldがあります");
    }
    fields |= field;
  }
  if (!cursor.finished() || fields != 255U) {
    return failure<RescueUsbOwnershipManifest>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"レスキューUSB所有manifestの必須fieldまたは末尾が不正です");
  }
  if (manifest.schema_version != kRescueUsbOwnershipSchemaVersion ||
      manifest.purpose != kRescueUsbOwnershipPurpose ||
      manifest.media_id.size() != 36U ||
      manifest.boot_file_system != L"FAT32" ||
      manifest.boot_partition_bytes != kRescueUsbBootPartitionBytes) {
    return failure<RescueUsbOwnershipManifest>(
        ErrorCode::invalid_data, ERROR_REVISION_MISMATCH,
        L"レスキューUSB所有manifestが既知のschema-v2契約と一致しません");
  }
  bool nonzero_media_id = false;
  for (std::size_t index = 0U; index < manifest.media_id.size(); ++index) {
    const bool separator =
        index == 8U || index == 13U || index == 18U || index == 23U;
    const wchar_t character = manifest.media_id[index];
    if ((separator && character != L'-') ||
        (!separator &&
         !((character >= L'0' && character <= L'9') ||
           (character >= L'a' && character <= L'f')))) {
      return failure<RescueUsbOwnershipManifest>(
          ErrorCode::invalid_data, ERROR_INVALID_DATA,
          L"レスキューUSB所有manifestの媒体ID形式が不正です");
    }
    nonzero_media_id = nonzero_media_id ||
        (!separator && character != L'0');
  }
  if (!nonzero_media_id) {
    return failure<RescueUsbOwnershipManifest>(
        ErrorCode::invalid_data, ERROR_INVALID_DATA,
        L"レスキューUSB所有manifestの媒体IDが無効です");
  }
  return clonecore::Result<RescueUsbOwnershipManifest>::success(
      std::move(manifest));
}

clonecore::Result<RescueUsbOwnedMediaEvidence>
build_rescue_usb_owned_media_evidence(
    const diskmodel::DiskInfo& target,
    const RescueUsbOwnershipManifest& manifest,
    const std::span<const std::byte> marker_bytes,
    std::vector<RescueMediaFileFingerprint> observed_boot_files,
    std::vector<std::wstring> observed_boot_directories,
    const RescueMediaTreeIdentity& observed_data_tree_identity) {
  if (marker_bytes.size() != manifest.media_id.size() ||
      marker_bytes.size() != 36U ||
      !std::equal(
          marker_bytes.begin(), marker_bytes.end(), manifest.media_id.begin(),
          [](const std::byte byte, const wchar_t character) {
            return character <= 0x7FU &&
                std::to_integer<unsigned char>(byte) ==
                    static_cast<unsigned char>(character);
          })) {
    return failure<RescueUsbOwnedMediaEvidence>(
        ErrorCode::verification_failed, ERROR_DATA_CHECKSUM_ERROR,
        L"レスキューUSB媒体IDが所有manifestと一致しません");
  }
  const auto observed_boot_identity = make_rescue_usb_boot_tree_identity(
      observed_boot_files, observed_boot_directories);
  if (!observed_boot_identity) {
    return clonecore::Result<RescueUsbOwnedMediaEvidence>::failure(
        observed_boot_identity.error());
  }
  RescueUsbOwnedMediaInspection inspection{
      .schema_version = manifest.schema_version,
      .purpose = manifest.purpose,
      .media_id = manifest.media_id,
      .boot_file_system = manifest.boot_file_system,
      .data_file_system = manifest.data_file_system,
      .boot_partition_bytes = manifest.boot_partition_bytes,
      .manifest_layout = manifest.canonical_layout,
      .manifest_owned_boot_tree_identity =
          manifest.owned_boot_tree_identity,
      .manifest_owned_boot_files = manifest.owned_boot_files,
      .manifest_owned_boot_directories = manifest.owned_boot_directories,
      .observed_boot_tree_identity = observed_boot_identity.value(),
      .observed_boot_files = std::move(observed_boot_files),
      .observed_boot_directories = std::move(observed_boot_directories),
      .observed_data_tree_identity = observed_data_tree_identity,
  };
  auto plan = plan_rescue_usb_storage({
      .target = &target,
      .mode = RescueUsbProvisioningMode::preserve_data_refresh,
      .data_file_system = manifest.data_file_system,
      .owned_media = &inspection,
  });
  if (!plan) {
    return clonecore::Result<RescueUsbOwnedMediaEvidence>::failure(
        plan.error());
  }
  return clonecore::Result<RescueUsbOwnedMediaEvidence>::success({
      .cache_key = {
          .target = plan.value().expected_target,
          .canonical_layout = plan.value().reviewed_layout,
          .mode = plan.value().mode,
          .data_file_system = plan.value().data_file_system,
      },
      .owned_media = plan.value().reviewed_owned_media.value(),
      .physical_write_started = false,
  });
}

clonecore::Status validate_rescue_usb_inspection_target_binding(
    const RescueUsbOwnedMediaEvidence& evidence,
    const diskmodel::DiskInfo& selected_target) {
  if (evidence.physical_write_started ||
      evidence.cache_key.mode !=
          RescueUsbProvisioningMode::preserve_data_refresh ||
      evidence.owned_media.data_file_system !=
          evidence.cache_key.data_file_system) {
    return clonecore::Status::failure(inspection_error(
        ErrorCode::identity_mismatch, ERROR_REVISION_MISMATCH,
        L"所有媒体の検査cache契約が一致しません"));
  }
  const auto observed_identity =
      diskmodel::make_stable_disk_identity(selected_target, false);
  if (!observed_identity) {
    return clonecore::Status::failure(observed_identity.error());
  }
  const auto identity = clonecore::validate_stable_identity(
      evidence.cache_key.target, observed_identity.value(),
      L"レスキューUSB検査cache");
  if (!identity || !same_cache_identity(
                       evidence.cache_key.target,
                       observed_identity.value()) ||
      evidence.cache_key.canonical_layout !=
          make_rescue_usb_canonical_layout(selected_target)) {
    return clonecore::Status::failure(
        identity
            ? inspection_error(
                  ErrorCode::identity_mismatch, ERROR_DEVICE_NOT_CONNECTED,
                  L"USBの安定識別情報または完全レイアウトが検査時から変化しました")
            : identity.error());
  }
  auto plan = plan_rescue_usb_storage({
      .target = &selected_target,
      .mode = evidence.cache_key.mode,
      .data_file_system = evidence.cache_key.data_file_system,
      .owned_media = &evidence.owned_media,
  });
  if (!plan || plan.value().reviewed_layout !=
                   evidence.cache_key.canonical_layout ||
      !same_cache_identity(
          plan.value().expected_target, evidence.cache_key.target)) {
    return clonecore::Status::failure(
        plan
            ? inspection_error(
                  ErrorCode::identity_mismatch, ERROR_DATA_CHECKSUM_ERROR,
                  L"所有媒体の検査cacheを現在の選択へ再bindできません")
            : plan.error());
  }
  return clonecore::success_status();
}

clonecore::Status validate_rescue_usb_inspection_evidence(
    const RescueUsbOwnedMediaEvidence& evidence,
    const diskmodel::DiskInfo& selected_target,
    const RescueUsbProvisioningMode selected_mode,
    const RescueUsbDataFileSystem selected_file_system) {
  const auto target = validate_rescue_usb_inspection_target_binding(
      evidence, selected_target);
  if (!target) {
    return target;
  }
  if (selected_mode != evidence.cache_key.mode ||
      selected_file_system != evidence.cache_key.data_file_system) {
    return clonecore::Status::failure(inspection_error(
        ErrorCode::identity_mismatch, ERROR_REVISION_MISMATCH,
        L"所有媒体の検査cacheと選択モード／ファイルシステムが一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_rescue_usb_fresh_inspection_for_plan(
    const RescueUsbOwnedMediaEvidence& fresh_evidence,
    const RescueUsbStoragePlan& reviewed_plan) {
  const auto plan_binding =
      validate_rescue_usb_storage_plan_binding(reviewed_plan);
  if (!plan_binding) {
    return plan_binding;
  }
  if (reviewed_plan.mode !=
          RescueUsbProvisioningMode::preserve_data_refresh ||
      !reviewed_plan.reviewed_owned_media.has_value() ||
      fresh_evidence.physical_write_started ||
      fresh_evidence.cache_key.mode != reviewed_plan.mode ||
      fresh_evidence.cache_key.data_file_system !=
          reviewed_plan.data_file_system ||
      fresh_evidence.cache_key.canonical_layout !=
          reviewed_plan.reviewed_layout) {
    return clonecore::Status::failure(inspection_error(
        ErrorCode::identity_mismatch, ERROR_REVISION_MISMATCH,
        L"作成直前の所有検査がレビュー済み媒体計画と一致しません"));
  }
  const auto identity = clonecore::validate_stable_identity(
      reviewed_plan.expected_target,
      fresh_evidence.cache_key.target,
      L"レスキューUSB作成直前所有検査");
  if (!identity ||
      !same_cache_identity(
          reviewed_plan.expected_target,
          fresh_evidence.cache_key.target)) {
    return clonecore::Status::failure(
        identity
            ? inspection_error(
                  ErrorCode::identity_mismatch,
                  ERROR_DEVICE_NOT_CONNECTED,
                  L"作成直前のUSB安定識別情報がレビュー時から変化しました")
            : identity.error());
  }
  if (fresh_evidence.owned_media !=
      reviewed_plan.reviewed_owned_media.value()) {
    return clonecore::Status::failure(inspection_error(
        ErrorCode::verification_failed, ERROR_DATA_CHECKSUM_ERROR,
        L"作成直前の起動領域または保持データ集約値がレビュー時から変化しました"));
  }
  return clonecore::success_status();
}

clonecore::Result<RescueUsbOwnedVolumeRoots>
resolve_rescue_usb_owned_volume_roots(
    const diskmodel::DiskInfo& target,
    const std::span<const DriveLetterVolume> volumes,
    std::wstring boot_file_system,
    std::wstring data_file_system) {
  auto identity = diskmodel::make_stable_disk_identity(target, false);
  if (!identity) {
    return clonecore::Result<RescueUsbOwnedVolumeRoots>::failure(
        identity.error());
  }
  auto geometry = resolve_owned_volume_geometry(target, volumes);
  if (!geometry) {
    return clonecore::Result<RescueUsbOwnedVolumeRoots>::failure(
        geometry.error());
  }
  if (!equals_case_insensitive(boot_file_system, L"FAT32")) {
    return failure<RescueUsbOwnedVolumeRoots>(
        ErrorCode::unsupported_layout, ERROR_NOT_SUPPORTED,
        L"所有媒体の起動領域がFAT32ではありません");
  }
  RescueUsbDataFileSystem parsed_data{};
  if (equals_case_insensitive(data_file_system, L"NTFS")) {
    parsed_data = RescueUsbDataFileSystem::ntfs;
  } else if (equals_case_insensitive(data_file_system, L"exFAT")) {
    parsed_data = RescueUsbDataFileSystem::exfat;
  } else {
    return failure<RescueUsbOwnedVolumeRoots>(
        ErrorCode::unsupported_layout, ERROR_NOT_SUPPORTED,
        L"所有媒体のデータ領域がNTFS／exFATではありません");
  }
  return clonecore::Result<RescueUsbOwnedVolumeRoots>::success({
      .target_identity = identity.value(),
      .canonical_layout = make_rescue_usb_canonical_layout(target),
      .boot_drive_letter = geometry.value().boot_drive_letter,
      .boot_root = geometry.value().boot_root,
      .data_drive_letter = geometry.value().data_drive_letter,
      .data_root = geometry.value().data_root,
      .data_file_system = parsed_data,
      .physical_write_started = false,
  });
}

RescueUsbInspectionResult
inspect_rescue_usb_owned_media_with_windows_apis(
    const diskmodel::DiskInfo& target,
    const std::atomic_bool* const cancellation_requested) {
  try {
    if (inspection_cancelled(cancellation_requested)) {
      return blocked_result(
          L"レスキューUSB所有情報の検査を取り消しました");
    }
    const auto identity = diskmodel::make_stable_disk_identity(target, false);
    if (!identity) {
      return blocked_result(identity.error().message);
    }
    const auto completed = validate_rescue_usb_completed_layout(
        make_rescue_usb_canonical_layout(target), target.size_bytes);
    if (!completed) {
      const auto initialization = plan_rescue_usb_storage({
          .target = &target,
          .mode = RescueUsbProvisioningMode::initialize_all,
          .data_file_system = RescueUsbDataFileSystem::ntfs,
      });
      return initialization.has_value()
          ? unknown_result()
          : blocked_result(initialization.error().message);
    }
    auto volumes = enumerate_windows_drive_letter_volumes_read_only();
    if (!volumes) {
      return blocked_result(volumes.error().message);
    }
    auto geometry = resolve_owned_volume_geometry(target, volumes.value());
    if (!geometry) {
      return blocked_result(geometry.error().message);
    }
    auto boot_file_system = query_volume_file_system(
        geometry.value().boot_root);
    auto data_file_system = query_volume_file_system(
        geometry.value().data_root);
    if (!boot_file_system || !data_file_system) {
      return blocked_result(
          boot_file_system ? data_file_system.error().message
                           : boot_file_system.error().message);
    }
    auto roots = resolve_rescue_usb_owned_volume_roots(
        target, volumes.value(), boot_file_system.value(),
        data_file_system.value());
    if (!roots) {
      return blocked_result(roots.error().message);
    }
    const std::wstring manifest_path = extended_path(
        roots.value().boot_root, kRescueUsbManifestRelativePath);
    const std::wstring marker_path = extended_path(
        roots.value().boot_root, kRescueUsbMarkerRelativePath);
    DWORD manifest_native{};
    DWORD marker_native{};
    const PathPresence manifest_presence =
        probe_regular_file(manifest_path, manifest_native);
    const PathPresence marker_presence =
        probe_regular_file(marker_path, marker_native);
    if (manifest_presence == PathPresence::absent &&
        marker_presence == PathPresence::absent) {
      return unknown_result();
    }
    if (manifest_presence != PathPresence::regular ||
        marker_presence != PathPresence::regular) {
      return blocked_result(
          L"所有manifestと媒体IDが片方だけ存在するか通常ファイルではありません");
    }
    auto manifest_bytes = read_small_regular_file(
        manifest_path, kRescueUsbMaximumOwnershipManifestBytes,
        L"レスキューUSB所有manifest", cancellation_requested);
    auto marker_bytes = read_small_regular_file(
        marker_path, 36U, L"レスキューUSB媒体ID",
        cancellation_requested);
    if (!manifest_bytes || !marker_bytes) {
      return blocked_result(
          manifest_bytes ? marker_bytes.error().message
                         : manifest_bytes.error().message);
    }
    auto manifest = parse_rescue_usb_ownership_manifest(
        manifest_bytes.value());
    if (!manifest) {
      return blocked_result(manifest.error().message);
    }
    if (manifest.value().data_file_system !=
        roots.value().data_file_system) {
      return blocked_result(
          L"所有manifestと実データ領域のファイルシステムが一致しません");
    }
    auto boot_tree = scan_volume_tree(
        roots.value().boot_root, false, cancellation_requested);
    if (!boot_tree) {
      return blocked_result(boot_tree.error().message);
    }
    auto data_identity = scan_private_data_tree_identity(
        roots.value().data_root, cancellation_requested);
    if (!data_identity) {
      return blocked_result(data_identity.error().message);
    }
    auto evidence = build_rescue_usb_owned_media_evidence(
        target, manifest.value(), marker_bytes.value(),
        std::move(boot_tree.value().files),
        std::move(boot_tree.value().directories),
        data_identity.value());
    if (!evidence) {
      return blocked_result(evidence.error().message);
    }
    return RescueUsbInspectionResult{
        .state = RescueUsbInspectionState::verified_owned,
        .evidence = evidence.take_value(),
        .message =
            L"検証済みY-TEC媒体です。起動／アプリ領域だけを非上書き更新し、データ領域を完全保持できます。",
        .physical_write_started = false,
    };
  } catch (...) {
    return blocked_result(
        L"レスキューUSB所有情報の読取り専用検査で予期しない失敗が発生しました");
  }
}

clonecore::Status reinspect_rescue_usb_storage_plan_with_windows_apis(
    const RescueUsbStoragePlan& reviewed_plan,
    const std::atomic_bool* const cancellation_requested) {
  if (inspection_cancelled(cancellation_requested)) {
    return clonecore::Status::failure(inspection_error(
        ErrorCode::cancelled, ERROR_CANCELLED,
        L"レスキューUSBの作成直前検査を取り消しました"));
  }
  const auto plan_binding =
      validate_rescue_usb_storage_plan_binding(reviewed_plan);
  if (!plan_binding) {
    return plan_binding;
  }
  if (reviewed_plan.mode !=
      RescueUsbProvisioningMode::preserve_data_refresh) {
    return clonecore::success_status();
  }

  auto provider = diskmodel::make_windows_disk_inventory_provider(nullptr);
  auto inventory = provider->enumerate();
  if (!inventory) {
    return clonecore::Status::failure(inventory.error());
  }
  if (!inventory.value().issues.empty()) {
    return clonecore::Status::failure(inspection_error(
        ErrorCode::enumeration_failed, ERROR_PARTIAL_COPY,
        L"作成直前の物理ディスク再列挙に未解決の問題があります"));
  }

  const diskmodel::DiskInfo* exact_target = nullptr;
  for (const auto& candidate : inventory.value().disks) {
    const auto candidate_identity =
        diskmodel::make_stable_disk_identity(candidate, false);
    if (!candidate_identity) {
      continue;
    }
    const auto identity = clonecore::validate_stable_identity(
        reviewed_plan.expected_target,
        candidate_identity.value(),
        L"レスキューUSB作成直前再列挙");
    if (!identity ||
        !same_cache_identity(
            reviewed_plan.expected_target,
            candidate_identity.value())) {
      continue;
    }
    if (exact_target != nullptr) {
      return clonecore::Status::failure(inspection_error(
          ErrorCode::identity_mismatch, ERROR_DUP_NAME,
          L"レビュー済みUSBと一致する対象を一意に再識別できません"));
    }
    exact_target = &candidate;
  }
  if (exact_target == nullptr ||
      make_rescue_usb_canonical_layout(*exact_target) !=
          reviewed_plan.reviewed_layout) {
    return clonecore::Status::failure(inspection_error(
        ErrorCode::identity_mismatch, ERROR_DEVICE_NOT_CONNECTED,
        L"レビュー済みUSBの安定識別情報または完全レイアウトが変化しました"));
  }

  const auto fresh = inspect_rescue_usb_owned_media_with_windows_apis(
      *exact_target, cancellation_requested);
  if (fresh.state != RescueUsbInspectionState::verified_owned ||
      !fresh.evidence.has_value() || fresh.physical_write_started) {
    return clonecore::Status::failure(inspection_error(
        fresh.state == RescueUsbInspectionState::blocked &&
                inspection_cancelled(cancellation_requested)
            ? ErrorCode::cancelled
            : ErrorCode::verification_failed,
        inspection_cancelled(cancellation_requested)
            ? ERROR_CANCELLED
            : ERROR_DATA_CHECKSUM_ERROR,
        fresh.message.empty()
            ? L"作成直前に検証済みY-TEC媒体の所有情報を再確認できません"
            : fresh.message));
  }
  return validate_rescue_usb_fresh_inspection_for_plan(
      fresh.evidence.value(), reviewed_plan);
}

}  // namespace ytec::windowsapp

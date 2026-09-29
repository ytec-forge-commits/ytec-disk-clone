#include "ytec/clonecore/log_privacy.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::clonecore {
namespace {

constexpr std::wstring_view kPrivateMarker = L"[PRIVATE]";
constexpr std::wstring_view kPathMarker = L"[PATH]";
constexpr std::wstring_view kDocumentMarker = L"[DOCUMENT]";
constexpr std::wstring_view kDiskMarker = L"[DISK]";
constexpr std::wstring_view kDeviceMarker = L"[DEVICE]";
constexpr std::size_t kDiskSerialSuffixCharacters = 4U;
constexpr std::size_t kMaximumHashedIdentifierCharacters = 4096U;
constexpr std::size_t kMaximumMainLogCharacters = 8192U;
constexpr std::size_t kPrivacyInspectionLookaheadCharacters = 64U;

class AlgorithmHandle final {
 public:
  AlgorithmHandle() = default;
  ~AlgorithmHandle() {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0);
    }
  }

  AlgorithmHandle(const AlgorithmHandle&) = delete;
  AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
  AlgorithmHandle(AlgorithmHandle&&) = delete;
  AlgorithmHandle& operator=(AlgorithmHandle&&) = delete;

  [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

class HashHandle final {
 public:
  HashHandle() = default;
  ~HashHandle() {
    if (handle_ != nullptr) {
      BCryptDestroyHash(handle_);
    }
  }

  HashHandle(const HashHandle&) = delete;
  HashHandle& operator=(const HashHandle&) = delete;
  HashHandle(HashHandle&&) = delete;
  HashHandle& operator=(HashHandle&&) = delete;

  [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &handle_; }
  [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_{};
};

[[nodiscard]] wchar_t ascii_lower(const wchar_t value) noexcept {
  if (value >= L'A' && value <= L'Z') {
    return static_cast<wchar_t>(value + (L'a' - L'A'));
  }
  return value;
}

[[nodiscard]] bool ascii_equal_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.size(); ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::size_t> find_ascii_insensitive(
    const std::wstring_view text,
    const std::wstring_view pattern,
    const std::size_t start = 0U) noexcept {
  if (pattern.empty() || start > text.size() ||
      pattern.size() > text.size() - start) {
    return std::nullopt;
  }
  const std::size_t last = text.size() - pattern.size();
  for (std::size_t index = start; index <= last; ++index) {
    if (ascii_equal_insensitive(text.substr(index, pattern.size()), pattern)) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool contains_ascii_insensitive(
    const std::wstring_view text,
    const std::wstring_view pattern) noexcept {
  return find_ascii_insensitive(text, pattern).has_value();
}

[[nodiscard]] bool is_ascii_identifier_character(
    const wchar_t value) noexcept {
  return (value >= L'A' && value <= L'Z') ||
         (value >= L'a' && value <= L'z') ||
         (value >= L'0' && value <= L'9') || value == L'_';
}

[[nodiscard]] bool is_privacy_space(const wchar_t value) noexcept {
  const auto code = static_cast<unsigned int>(value);
  return code <= 0x20U || code == 0x85U || code == 0xa0U ||
         code == 0x1680U || (code >= 0x2000U && code <= 0x200aU) ||
         code == 0x2028U || code == 0x2029U || code == 0x202fU ||
         code == 0x205fU || code == 0x3000U;
}

[[nodiscard]] bool is_labeled_key_begin_boundary(
    const std::wstring_view text,
    const std::size_t begin) noexcept {
  if (begin == 0U || !is_ascii_identifier_character(text[begin - 1U])) {
    return true;
  }
  if (text[begin - 1U] == L'_') {
    return true;
  }
  return text[begin] >= L'A' && text[begin] <= L'Z' &&
         ((text[begin - 1U] >= L'a' && text[begin - 1U] <= L'z') ||
          (text[begin - 1U] >= L'0' && text[begin - 1U] <= L'9'));
}

[[nodiscard]] std::wstring_view trim_privacy_space_and_sentence_end(
    std::wstring_view value) noexcept {
  while (!value.empty() && is_privacy_space(value.front())) {
    value.remove_prefix(1U);
  }
  while (!value.empty() &&
         (is_privacy_space(value.back()) || value.back() == L'.' ||
          value.back() == L'!' || value.back() == L'。' ||
          value.back() == L'！')) {
    value.remove_suffix(1U);
  }
  return value;
}

[[nodiscard]] bool is_non_secret_state_description(
    const std::wstring_view value) noexcept {
  constexpr auto kDescriptions = std::to_array<std::wstring_view>({
      L"required",
      L"not required",
      L"absent",
      L"missing",
      L"unavailable",
      L"present",
      L"empty",
      L"cleared",
      L"redacted",
      L"disabled",
      L"enabled",
      L"configured",
      L"not configured",
      L"requested",
      L"supported",
      L"unsupported",
      L"authentication disabled",
      L"authentication enabled",
      L"support disabled",
      L"support enabled",
      L"scheme disabled",
      L"scheme enabled",
  });
  const std::wstring_view description =
      trim_privacy_space_and_sentence_end(value);
  for (const std::wstring_view safe : kDescriptions) {
    if (ascii_equal_insensitive(description, safe)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool contains_sensitive_value_marker(
    const std::wstring_view text,
    const std::wstring_view marker) noexcept {
  std::size_t search_from = 0U;
  while (search_from < text.size()) {
    const auto found = find_ascii_insensitive(text, marker, search_from);
    if (!found.has_value()) {
      return false;
    }
    const std::size_t begin = found.value();
    const bool begins_at_boundary =
        begin == 0U || !is_ascii_identifier_character(text[begin - 1U]);
    if (begins_at_boundary) {
      const std::size_t value_begin = begin + marker.size();
      if (marker == L"-----begin private key-----" ||
          marker.back() == L'=' || marker.back() == L':' ||
          !is_non_secret_state_description(text.substr(value_begin))) {
        return true;
      }
    }
    search_from = begin + 1U;
  }
  return false;
}

[[nodiscard]] bool contains_labeled_key(
    const std::wstring_view text,
    const std::wstring_view key) noexcept {
  std::size_t search_from = 0U;
  while (search_from < text.size()) {
    const auto found = find_ascii_insensitive(text, key, search_from);
    if (!found.has_value()) {
      return false;
    }
    const std::size_t begin = found.value();
    std::size_t cursor = begin + key.size();
    const bool begins_at_boundary =
        is_labeled_key_begin_boundary(text, begin);
    const bool ends_at_boundary =
        cursor == text.size() || !is_ascii_identifier_character(text[cursor]);
    if (begins_at_boundary && ends_at_boundary) {
      if (cursor < text.size() &&
          (text[cursor] == L'"' || text[cursor] == L'\'')) {
        ++cursor;
      }
      while (cursor < text.size() && is_privacy_space(text[cursor])) {
        ++cursor;
      }
      if (cursor < text.size() &&
          (text[cursor] == L':' || text[cursor] == L'=' ||
           text[cursor] == L'：' || text[cursor] == L'＝')) {
        return true;
      }
    }
    search_from = begin + 1U;
  }
  return false;
}

[[nodiscard]] std::wstring_view trim_ascii_space(
    std::wstring_view value) noexcept {
  while (!value.empty() &&
         (value.front() == L' ' || value.front() == L'\t' ||
          value.front() == L'\r' || value.front() == L'\n')) {
    value.remove_prefix(1U);
  }
  while (!value.empty() &&
         (value.back() == L' ' || value.back() == L'\t' ||
          value.back() == L'\r' || value.back() == L'\n')) {
    value.remove_suffix(1U);
  }
  return value;
}

[[nodiscard]] std::wstring sanitize_single_line(
    std::wstring_view message,
    std::size_t maximum_characters);

[[nodiscard]] bool hash_bytes(
    const BCRYPT_HASH_HANDLE hash,
    const void* const data,
    const std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::byte*>(data);
  std::size_t consumed = 0U;
  while (consumed < size) {
    const ULONG amount = static_cast<ULONG>((std::min)(
        size - consumed,
        static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
    const NTSTATUS status = BCryptHashData(
        hash,
        reinterpret_cast<PUCHAR>(
            const_cast<std::byte*>(bytes + consumed)),
        amount,
        0);
    if (!BCRYPT_SUCCESS(status)) {
      return false;
    }
    consumed += amount;
  }
  return true;
}

[[nodiscard]] std::optional<std::wstring> sha256_token(
    const std::wstring_view domain,
    const std::wstring_view value) {
  static_assert(sizeof(wchar_t) == 2U);

  AlgorithmHandle algorithm;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    return std::nullopt;
  }

  ULONG object_length{};
  ULONG returned{};
  status = BCryptGetProperty(
      algorithm.get(),
      BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length),
      &returned,
      0);
  if (!BCRYPT_SUCCESS(status) || returned != sizeof(object_length) ||
      object_length == 0U) {
    return std::nullopt;
  }

  ULONG hash_length{};
  status = BCryptGetProperty(
      algorithm.get(),
      BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&hash_length),
      sizeof(hash_length),
      &returned,
      0);
  if (!BCRYPT_SUCCESS(status) || returned != sizeof(hash_length) ||
      hash_length != 32U) {
    return std::nullopt;
  }

  std::vector<UCHAR> object(object_length);
  HashHandle hash;
  status = BCryptCreateHash(
      algorithm.get(),
      hash.put(),
      object.data(),
      object_length,
      nullptr,
      0,
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return std::nullopt;
  }

  constexpr wchar_t kSeparator = L'\0';
  if (domain.size() >
          (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) ||
      value.size() >
          (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) ||
      !hash_bytes(
          hash.get(), domain.data(), domain.size() * sizeof(wchar_t)) ||
      !hash_bytes(hash.get(), &kSeparator, sizeof(kSeparator)) ||
      !hash_bytes(
          hash.get(), value.data(), value.size() * sizeof(wchar_t))) {
    return std::nullopt;
  }

  std::array<std::byte, 32U> digest{};
  status = BCryptFinishHash(
      hash.get(),
      reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()),
      0);
  if (!BCRYPT_SUCCESS(status)) {
    return std::nullopt;
  }

  constexpr std::wstring_view kHex = L"0123456789abcdef";
  std::wstring encoded;
  encoded.reserve(digest.size() * 2U);
  for (const std::byte byte : digest) {
    const unsigned int value_byte = std::to_integer<unsigned int>(byte);
    encoded.push_back(kHex[(value_byte >> 4U) & 0x0fU]);
    encoded.push_back(kHex[value_byte & 0x0fU]);
  }
  return encoded;
}

[[nodiscard]] bool contains_sensitive_marker(
    const std::wstring_view message) noexcept {
  constexpr auto kLabeledKeys = std::to_array<std::wstring_view>({
      L"password",
      L"passphrase",
      L"passwd",
      L"pwd",
      L"recovery key",
      L"recovery-key",
      L"recovery_key",
      L"recoverykey",
      L"token",
      L"access_token",
      L"refresh_token",
      L"id_token",
      L"accesstoken",
      L"refreshtoken",
      L"idtoken",
      L"cookie",
      L"authorization",
      L"auth_header",
      L"auth-header",
      L"auth header",
      L"authheader",
      L"api_key",
      L"api-key",
      L"api key",
      L"apikey",
      L"client_secret",
      L"client secret",
      L"clientsecret",
      L"secret",
      L"credential",
      L"private_key",
      L"private-key",
      L"private key",
      L"パスワード",
      L"回復キー",
      L"認証ヘッダー",
  });
  for (const std::wstring_view key : kLabeledKeys) {
    if (contains_labeled_key(message, key)) {
      return true;
    }
  }
  constexpr auto kMarkers = std::to_array<std::wstring_view>({
      L"token value ",
      L"token value=",
      L"token value:",
      L"token is ",
      L"cookie value ",
      L"cookie value=",
      L"cookie value:",
      L"session cookie is ",
      L"proxy-authorization basic ",
      L"proxy-authorization bearer ",
      L"authorization basic ",
      L"authorization bearer ",
      L"auth header basic ",
      L"auth header bearer ",
      L"auth header is ",
      L"auth header value ",
      L"auth header value=",
      L"auth header value:",
      L"api key is ",
      L"api key value ",
      L"api key value=",
      L"api key value:",
      L"client secret is ",
      L"client secret value ",
      L"client secret value=",
      L"client secret value:",
      L"credential value ",
      L"credential value=",
      L"credential value:",
      L"private key is ",
      L"private key value ",
      L"private key value=",
      L"private key value:",
      L"-----begin private key-----",
      L"bearer ",
      L"password is ",
      L"password value ",
      L"password value=",
      L"password value:",
      L"passphrase is ",
      L"recovery key is ",
      L"access token is ",
      L"refresh token is ",
      L"id token is ",
      L"auth token is ",
      L"session token is ",
  });
  for (const std::wstring_view marker : kMarkers) {
    if (contains_sensitive_value_marker(message, marker)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool is_path_separator(const wchar_t value) noexcept {
  return value == L'\\' || value == L'/';
}

[[nodiscard]] bool is_path_prefix_boundary(
    const std::wstring_view message,
    const std::size_t index) noexcept {
  if (index == 0U) {
    return true;
  }
  return !is_ascii_identifier_character(message[index - 1U]);
}

[[nodiscard]] bool is_safe_serial_suffix_character(
    const wchar_t value) noexcept {
  return (value >= L'A' && value <= L'Z') ||
         (value >= L'a' && value <= L'z') ||
         (value >= L'0' && value <= L'9');
}

[[nodiscard]] bool contains_later_path_separator(
    const std::wstring_view message,
    const std::size_t begin) noexcept {
  for (std::size_t cursor = begin; cursor < message.size(); ++cursor) {
    const wchar_t value = message[cursor];
    if (is_path_separator(value)) {
      return true;
    }
    if (is_privacy_space(value) || value == L'"' || value == L'\'' ||
        value == L'<' || value == L'>' || value == L'|' || value == L'?' ||
        value == L'*') {
      return false;
    }
  }
  return false;
}

[[nodiscard]] bool contains_absolute_path(
    const std::wstring_view message) noexcept {
  for (std::size_t index = 0U; index < message.size(); ++index) {
    if (index + 2U < message.size() &&
        is_path_prefix_boundary(message, index) &&
        ((message[index] >= L'A' && message[index] <= L'Z') ||
         (message[index] >= L'a' && message[index] <= L'z')) &&
        message[index + 1U] == L':' &&
        is_path_separator(message[index + 2U])) {
      return true;
    }
    if (index + 1U < message.size() && message[index] == L'\\' &&
        message[index + 1U] == L'\\') {
      return true;
    }
    if (index + 1U < message.size() && message[index] == L'/' &&
        message[index + 1U] == L'/' &&
        (index == 0U || message[index - 1U] != L':')) {
      return true;
    }
    if (index + 1U < message.size() && message[index] == L'/' &&
        message[index + 1U] != L'/' &&
        (index == 0U ||
         (message[index - 1U] != L'/' && message[index - 1U] != L':')) &&
        is_path_prefix_boundary(message, index) &&
        contains_later_path_separator(message, index + 1U)) {
      return true;
    }
    if (index + 1U < message.size() && message[index] == L'\\' &&
        message[index + 1U] != L'\\' &&
        is_path_prefix_boundary(message, index)) {
      return true;
    }
  }
  return contains_ascii_insensitive(message, L"\\??\\") ||
         contains_ascii_insensitive(message, L"\\Device\\");
}

[[nodiscard]] bool is_document_extension_character(
    const wchar_t value) noexcept {
  return (value >= L'A' && value <= L'Z') ||
         (value >= L'a' && value <= L'z') ||
         (value >= L'0' && value <= L'9');
}

[[nodiscard]] bool contains_document_like_name(
    const std::wstring_view message) noexcept {
  constexpr auto kDocumentExtensions = std::to_array<std::wstring_view>({
      L"doc",
      L"docx",
      L"xls",
      L"xlsx",
      L"ppt",
      L"pptx",
      L"pdf",
      L"txt",
      L"rtf",
      L"csv",
      L"odt",
      L"ods",
      L"odp",
      L"pages",
      L"numbers",
      L"md",
      L"markdown",
      L"html",
      L"htm",
      L"epub",
      L"mobi",
      L"tex",
  });
  for (std::size_t dot = 1U; dot + 1U < message.size(); ++dot) {
    if (message[dot] != L'.' ||
        std::iswspace(message[dot - 1U]) != 0) {
      continue;
    }
    std::size_t end = dot + 1U;
    while (end < message.size() && end - dot <= 10U &&
           is_document_extension_character(message[end])) {
      ++end;
    }
    if (end == dot + 1U || end - dot > 10U) {
      continue;
    }
    const std::wstring_view extension =
        message.substr(dot + 1U, end - dot - 1U);
    for (const std::wstring_view document_extension : kDocumentExtensions) {
      if (ascii_equal_insensitive(extension, document_extension)) {
        return true;
      }
    }
  }
  return false;
}

[[nodiscard]] bool contains_raw_device_instance(
    const std::wstring_view message) noexcept {
  constexpr std::array<std::wstring_view, 7U> kPrefixes{
      L"USBSTOR\\",
      L"SCSI\\",
      L"IDE\\",
      L"STORAGE\\",
      L"PCI\\VEN_",
      L"USB\\VID_",
      L"ROOT\\",
  };
  for (const std::wstring_view prefix : kPrefixes) {
    if (contains_ascii_insensitive(message, prefix)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool contains_disk_identifier_marker(
    const std::wstring_view message) noexcept {
  constexpr auto kLabeledKeys = std::to_array<std::wstring_view>({
      L"serial",
      L"serial_number",
      L"serial-number",
      L"serial number",
      L"serialnumber",
      L"serial_no",
      L"serial no",
      L"serialno",
      L"serial_suffix",
      L"serial-suffix",
      L"disk_serial",
      L"disk-serial",
      L"シリアル",
      L"シリアル番号",
      L"シリアル末尾",
  });
  for (const std::wstring_view key : kLabeledKeys) {
    if (contains_labeled_key(message, key)) {
      return true;
    }
  }
  constexpr auto kMarkers = std::to_array<std::wstring_view>({
      L"disk serial is ",
      L"serial value",
  });
  for (const std::wstring_view marker : kMarkers) {
    if (contains_ascii_insensitive(message, marker)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool contains_device_identifier_marker(
    const std::wstring_view message) noexcept {
  constexpr auto kLabeledKeys = std::to_array<std::wstring_view>({
      L"device_instance_id",
      L"device-instance-id",
      L"device instance id",
      L"deviceinstanceid",
      L"デバイスインスタンス",
  });
  for (const std::wstring_view key : kLabeledKeys) {
    if (contains_labeled_key(message, key)) {
      return true;
    }
  }
  constexpr auto kMarkers = std::to_array<std::wstring_view>({
      L"device instance id is ",
  });
  for (const std::wstring_view marker : kMarkers) {
    if (contains_ascii_insensitive(message, marker)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool contains_document_identifier_marker(
    const std::wstring_view message) noexcept {
  constexpr auto kLabeledKeys = std::to_array<std::wstring_view>({
      L"document",
      L"document_name",
      L"document-name",
      L"document name",
      L"filename",
      L"file_name",
      L"file-name",
      L"文書",
      L"文書名",
      L"ファイル名",
  });
  for (const std::wstring_view key : kLabeledKeys) {
    if (contains_labeled_key(message, key)) {
      return true;
    }
  }
  constexpr auto kMarkers = std::to_array<std::wstring_view>({
      L"document name is ",
      L"file name is ",
  });
  for (const std::wstring_view marker : kMarkers) {
    if (contains_ascii_insensitive(message, marker)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::wstring sanitize_single_line(
    const std::wstring_view message,
    const std::size_t maximum_characters) {
  if (maximum_characters == 0U) {
    return {};
  }
  std::wstring result;
  result.reserve((std::min)(message.size(), maximum_characters));
  bool previous_space = false;
  bool truncated = false;
  const auto is_high_surrogate = [](const wchar_t value) noexcept {
    return value >= static_cast<wchar_t>(0xd800U) &&
           value <= static_cast<wchar_t>(0xdbffU);
  };
  const auto is_low_surrogate = [](const wchar_t value) noexcept {
    return value >= static_cast<wchar_t>(0xdc00U) &&
           value <= static_cast<wchar_t>(0xdfffU);
  };
  for (std::size_t index = 0U; index < message.size(); ++index) {
    const wchar_t original = message[index];
    if (is_high_surrogate(original)) {
      if (index + 1U < message.size() &&
          is_low_surrogate(message[index + 1U])) {
        if (result.size() + 2U > maximum_characters) {
          truncated = true;
          break;
        }
        result.push_back(original);
        result.push_back(message[index + 1U]);
        ++index;
        previous_space = false;
        continue;
      }
    }
    wchar_t character = original;
    if (is_high_surrogate(character) || is_low_surrogate(character)) {
      character = static_cast<wchar_t>(0xfffdU);
    }
    if (is_privacy_space(character)) {
      character = L' ';
    }
    if (character == L' ' && previous_space) {
      continue;
    }
    if (result.size() == maximum_characters) {
      truncated = true;
      break;
    }
    previous_space = character == L' ';
    result.push_back(character);
  }
  if (truncated && maximum_characters >= 3U) {
    std::size_t retained = (std::min)(
        result.size(), maximum_characters - 3U);
    if (retained > 0U && retained < result.size() &&
        is_high_surrogate(result[retained - 1U]) &&
        is_low_surrogate(result[retained])) {
      --retained;
    }
    result.resize(retained);
    result += L"...";
  }
  return result;
}

}  // namespace

std::wstring minimize_main_log_value(
    const MainLogPrivateValueKind kind,
    const std::wstring_view input) noexcept {
  try {
    const std::wstring_view value = trim_ascii_space(input);
    switch (kind) {
      case MainLogPrivateValueKind::secret:
        return std::wstring(kPrivateMarker);
      case MainLogPrivateValueKind::absolute_path:
        return std::wstring(kPathMarker);
      case MainLogPrivateValueKind::document_name:
        return std::wstring(kDocumentMarker);
      case MainLogPrivateValueKind::disk_serial: {
        if (value.empty() ||
            value.size() > kMaximumHashedIdentifierCharacters) {
          return std::wstring(kDiskMarker);
        }
        const auto token = sha256_token(L"YTEC-DISK-SERIAL-v1", value);
        if (!token.has_value()) {
          return std::wstring(kDiskMarker);
        }
        std::wstring result = L"[DISK#" + token.value();
        if (value.size() > kDiskSerialSuffixCharacters * 2U) {
          const std::wstring_view suffix =
              value.substr(value.size() - kDiskSerialSuffixCharacters);
          if (std::all_of(
                  suffix.begin(),
                  suffix.end(),
                  is_safe_serial_suffix_character)) {
            result += L"~";
            result.append(suffix);
          }
        }
        result += L"]";
        return result;
      }
      case MainLogPrivateValueKind::device_instance_id: {
        if (value.empty() ||
            value.size() > kMaximumHashedIdentifierCharacters) {
          return std::wstring(kDeviceMarker);
        }
        const auto token = sha256_token(L"YTEC-DEVICE-INSTANCE-v1", value);
        if (!token.has_value()) {
          return std::wstring(kDeviceMarker);
        }
        return L"[DEVICE#" + token.value() + L"]";
      }
    }
  } catch (...) {
    // Empty means "do not log" and cannot reveal the original value.
  }
  return {};
}

std::wstring sanitize_main_log_message(
    const std::wstring_view message,
    const std::size_t maximum_characters) noexcept {
  try {
    const std::size_t effective_maximum =
        (std::min)(maximum_characters, kMaximumMainLogCharacters);
    if (effective_maximum == 0U) {
      return {};
    }
    const std::size_t inspection_characters = (std::min)(
        message.size(),
        effective_maximum + kPrivacyInspectionLookaheadCharacters);
    const std::wstring_view inspected =
        message.substr(0U, inspection_characters);
    const std::wstring normalized =
        sanitize_single_line(inspected, inspection_characters);
    const std::wstring_view checked = normalized;
    if (contains_sensitive_marker(checked)) {
      return sanitize_single_line(kPrivateMarker, effective_maximum);
    }
    if (contains_disk_identifier_marker(checked)) {
      return sanitize_single_line(kDiskMarker, effective_maximum);
    }
    if (contains_device_identifier_marker(checked) ||
        contains_raw_device_instance(checked)) {
      return sanitize_single_line(kDeviceMarker, effective_maximum);
    }
    if (contains_absolute_path(checked)) {
      return sanitize_single_line(kPathMarker, effective_maximum);
    }
    if (contains_document_identifier_marker(checked) ||
        contains_document_like_name(checked)) {
      return sanitize_single_line(kDocumentMarker, effective_maximum);
    }
    return sanitize_single_line(checked, effective_maximum);
  } catch (...) {
    // A failed sanitizer must drop the record instead of exposing input.
    return {};
  }
}

}  // namespace ytec::clonecore

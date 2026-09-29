#include "ytec/windowsapp/manual_update.h"

#include "ytec/clonecore/error.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::string_view kProductId{"ytec-tsumugi-drive"};
constexpr std::uint32_t kSchemaVersion = 1U;
constexpr std::size_t kMaximumJsonKeyBytes = 32U;
constexpr std::size_t kMaximumJsonValueBytes = 256U;

clonecore::Error invalid_manifest(std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::invalid_data,
      .native_code = ERROR_INVALID_DATA,
      .operation = L"手動更新情報の検証",
      .message = std::move(message),
  };
}

clonecore::Error invalid_request(std::wstring message) {
  return clonecore::Error{
      .code = clonecore::ErrorCode::invalid_argument,
      .native_code = ERROR_INVALID_PARAMETER,
      .operation = L"手動更新確認",
      .message = std::move(message),
  };
}

bool is_json_whitespace(const char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool is_ascii_printable(const char value) noexcept {
  const auto byte = static_cast<unsigned char>(value);
  return byte >= 0x20U && byte <= 0x7EU;
}

class JsonCursor final {
 public:
  explicit JsonCursor(std::string_view input) : input_(input) {}

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

  [[nodiscard]] clonecore::Result<std::string> parse_ascii_string(
      const std::size_t maximum_bytes) {
    skip_whitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
      return clonecore::Result<std::string>::failure(
          invalid_manifest(L"JSON文字列を読み取れません"));
    }
    ++position_;

    std::string value;
    value.reserve(std::min(maximum_bytes, input_.size() - position_));
    while (position_ < input_.size()) {
      const char current = input_[position_++];
      if (current == '"') {
        return clonecore::Result<std::string>::success(std::move(value));
      }
      if (current == '\\') {
        if (position_ >= input_.size()) {
          break;
        }
        const char escaped = input_[position_++];
        if (escaped == '"' || escaped == '\\' || escaped == '/') {
          value.push_back(escaped);
        } else {
          return clonecore::Result<std::string>::failure(invalid_manifest(
              L"更新情報ではASCIIの引用符・逆斜線・スラッシュ以外の"
              L"JSONエスケープを許可しません"));
        }
      } else {
        if (!is_ascii_printable(current)) {
          return clonecore::Result<std::string>::failure(invalid_manifest(
              L"更新情報の文字列に許可されていない文字があります"));
        }
        value.push_back(current);
      }
      if (value.size() > maximum_bytes) {
        return clonecore::Result<std::string>::failure(
            invalid_manifest(L"更新情報の文字列が上限を超えています"));
      }
    }
    return clonecore::Result<std::string>::failure(
        invalid_manifest(L"JSON文字列が閉じられていません"));
  }

  [[nodiscard]] clonecore::Result<std::uint32_t> parse_uint32() {
    skip_whitespace();
    const std::size_t start = position_;
    while (position_ < input_.size() && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      ++position_;
    }
    if (start == position_) {
      return clonecore::Result<std::uint32_t>::failure(
          invalid_manifest(L"JSON整数を読み取れません"));
    }
    std::uint32_t value{};
    const auto parsed = std::from_chars(
        input_.data() + start, input_.data() + position_, value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != input_.data() + position_) {
      return clonecore::Result<std::uint32_t>::failure(
          invalid_manifest(L"JSON整数が範囲外です"));
    }
    return clonecore::Result<std::uint32_t>::success(value);
  }

 private:
  std::string_view input_;
  std::size_t position_{};
};

bool valid_sha256(std::string_view value) noexcept {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           const auto byte = static_cast<unsigned char>(character);
           return std::isdigit(byte) != 0 ||
                  (character >= 'a' && character <= 'f') ||
                  (character >= 'A' && character <= 'F');
         });
}

std::string uppercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    if (character >= 'a' && character <= 'z') {
      return static_cast<char>(character - ('a' - 'A'));
    }
    return character;
  });
  return value;
}

bool valid_published_utc(std::string_view value) noexcept {
  if (value.size() != 20U || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != 'Z') {
    return false;
  }
  constexpr std::array<std::size_t, 14> kDigitPositions{
      0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U};
  if (!std::all_of(
          kDigitPositions.begin(),
          kDigitPositions.end(),
          [&](const std::size_t position) {
            return value[position] >= '0' && value[position] <= '9';
          })) {
    return false;
  }
  const auto pair = [&](const std::size_t position) {
    return static_cast<unsigned int>(value[position] - '0') * 10U +
           static_cast<unsigned int>(value[position + 1U] - '0');
  };
  const unsigned int month = pair(5U);
  const unsigned int day = pair(8U);
  const unsigned int hour = pair(11U);
  const unsigned int minute = pair(14U);
  const unsigned int second = pair(17U);
  return month >= 1U && month <= 12U && day >= 1U && day <= 31U &&
         hour <= 23U && minute <= 59U && second <= 59U;
}

struct VersionIdentifier final {
  bool numeric{};
  std::uint32_t number{};
  std::string text;
};

struct ParsedVersion final {
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t patch{};
  std::vector<VersionIdentifier> prerelease;
};

clonecore::Result<std::uint32_t> parse_version_number(
    std::string_view value) {
  if (value.empty() || (value.size() > 1U && value.front() == '0')) {
    return clonecore::Result<std::uint32_t>::failure(
        invalid_manifest(L"版番号の数値要素が正しくありません"));
  }
  std::uint32_t number{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return clonecore::Result<std::uint32_t>::failure(
        invalid_manifest(L"版番号の数値要素が範囲外です"));
  }
  return clonecore::Result<std::uint32_t>::success(number);
}

clonecore::Result<ParsedVersion> parse_version(std::string_view value) {
  if (value.empty() || value.size() > 96U || value.find('+') != value.npos) {
    return clonecore::Result<ParsedVersion>::failure(
        invalid_manifest(L"版番号の形式が正しくありません"));
  }
  const std::size_t dash = value.find('-');
  const std::string_view core = value.substr(0U, dash);
  std::array<std::uint32_t, 3> numbers{};
  std::size_t start{};
  for (std::size_t index = 0U; index < numbers.size(); ++index) {
    const std::size_t dot = core.find('.', start);
    if ((index < 2U && dot == core.npos) ||
        (index == 2U && dot != core.npos)) {
      return clonecore::Result<ParsedVersion>::failure(
          invalid_manifest(L"版番号はmajor.minor.patch形式で指定してください"));
    }
    const std::size_t end = dot == core.npos ? core.size() : dot;
    auto parsed = parse_version_number(core.substr(start, end - start));
    if (!parsed.has_value()) {
      return clonecore::Result<ParsedVersion>::failure(parsed.error());
    }
    numbers[index] = parsed.value();
    start = end + 1U;
  }

  ParsedVersion parsed_version{
      .major = numbers[0],
      .minor = numbers[1],
      .patch = numbers[2],
  };
  if (dash == value.npos) {
    return clonecore::Result<ParsedVersion>::success(
        std::move(parsed_version));
  }
  std::string_view prerelease = value.substr(dash + 1U);
  if (prerelease.empty()) {
    return clonecore::Result<ParsedVersion>::failure(
        invalid_manifest(L"版番号の事前公開識別子が空です"));
  }
  start = 0U;
  while (start <= prerelease.size()) {
    const std::size_t dot = prerelease.find('.', start);
    const std::size_t end = dot == prerelease.npos ? prerelease.size() : dot;
    const std::string_view identifier =
        prerelease.substr(start, end - start);
    if (identifier.empty() || identifier.size() > 32U ||
        !std::all_of(identifier.begin(), identifier.end(), [](const char c) {
          return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                 (c >= 'a' && c <= 'z') || c == '-';
        })) {
      return clonecore::Result<ParsedVersion>::failure(
          invalid_manifest(L"版番号の事前公開識別子が正しくありません"));
    }
    const bool numeric = std::all_of(
        identifier.begin(), identifier.end(), [](const char c) {
          return c >= '0' && c <= '9';
        });
    VersionIdentifier version_identifier{
        .numeric = numeric,
        .text = std::string(identifier),
    };
    if (numeric) {
      auto number = parse_version_number(identifier);
      if (!number.has_value()) {
        return clonecore::Result<ParsedVersion>::failure(number.error());
      }
      version_identifier.number = number.value();
    }
    parsed_version.prerelease.push_back(std::move(version_identifier));
    if (dot == prerelease.npos) {
      break;
    }
    start = dot + 1U;
  }
  return clonecore::Result<ParsedVersion>::success(std::move(parsed_version));
}

int compare_versions(
    const ParsedVersion& left,
    const ParsedVersion& right) noexcept {
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> core{{
      {left.major, right.major},
      {left.minor, right.minor},
      {left.patch, right.patch},
  }};
  for (const auto& [left_number, right_number] : core) {
    if (left_number != right_number) {
      return left_number < right_number ? -1 : 1;
    }
  }
  if (left.prerelease.empty() || right.prerelease.empty()) {
    if (left.prerelease.empty() == right.prerelease.empty()) {
      return 0;
    }
    return left.prerelease.empty() ? 1 : -1;
  }
  const std::size_t common =
      std::min(left.prerelease.size(), right.prerelease.size());
  for (std::size_t index = 0U; index < common; ++index) {
    const auto& left_id = left.prerelease[index];
    const auto& right_id = right.prerelease[index];
    if (left_id.numeric && right_id.numeric) {
      if (left_id.number != right_id.number) {
        return left_id.number < right_id.number ? -1 : 1;
      }
      continue;
    }
    if (left_id.numeric != right_id.numeric) {
      return left_id.numeric ? -1 : 1;
    }
    if (left_id.text != right_id.text) {
      return left_id.text < right_id.text ? -1 : 1;
    }
  }
  if (left.prerelease.size() == right.prerelease.size()) {
    return 0;
  }
  return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
}

bool valid_json_content_type(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a')
                                  : c;
  });
  constexpr std::wstring_view kJson{L"application/json"};
  if (!value.starts_with(kJson)) {
    return false;
  }
  if (value.size() == kJson.size()) {
    return true;
  }
  return value[kJson.size()] == L';';
}

class UniqueInternetHandle final {
 public:
  UniqueInternetHandle() = default;
  explicit UniqueInternetHandle(HINTERNET handle) noexcept : handle_(handle) {}
  ~UniqueInternetHandle() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
  }

  UniqueInternetHandle(const UniqueInternetHandle&) = delete;
  UniqueInternetHandle& operator=(const UniqueInternetHandle&) = delete;

  UniqueInternetHandle(UniqueInternetHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  UniqueInternetHandle& operator=(UniqueInternetHandle&& other) noexcept {
    if (this != &other) {
      if (handle_ != nullptr) {
        WinHttpCloseHandle(handle_);
      }
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != nullptr;
  }

 private:
  HINTERNET handle_{};
};

clonecore::Error update_transport_error(
    std::wstring_view operation,
    const DWORD native_code) {
  auto error = clonecore::make_win32_error(
      clonecore::ErrorCode::query_failed, operation, native_code);
  error.message =
      L"更新情報を取得できませんでした。ネットワークを確認して、"
      L"利用者操作で再試行してください。 " +
      error.message;
  return error;
}

clonecore::Result<std::wstring> query_bounded_header(
    const HINTERNET request,
    const DWORD query,
    const std::size_t maximum_characters) {
  DWORD bytes{};
  SetLastError(ERROR_SUCCESS);
  if (WinHttpQueryHeaders(
          request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes,
          WINHTTP_NO_HEADER_INDEX) != FALSE) {
    return clonecore::Result<std::wstring>::failure(
        invalid_manifest(L"HTTP headerの長さ検査結果が不正です"));
  }
  const DWORD first_error = GetLastError();
  if (first_error != ERROR_INSUFFICIENT_BUFFER || bytes == 0U ||
      bytes % sizeof(wchar_t) != 0U ||
      bytes / sizeof(wchar_t) > maximum_characters + 1U) {
    return clonecore::Result<std::wstring>::failure(
        update_transport_error(L"更新情報HTTP headerの照会", first_error));
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
  DWORD actual_bytes = bytes;
  if (WinHttpQueryHeaders(
          request, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(),
          &actual_bytes, WINHTTP_NO_HEADER_INDEX) == FALSE) {
    return clonecore::Result<std::wstring>::failure(
        update_transport_error(
            L"更新情報HTTP headerの読取り", GetLastError()));
  }
  if (actual_bytes > bytes || actual_bytes % sizeof(wchar_t) != 0U) {
    return clonecore::Result<std::wstring>::failure(
        invalid_manifest(L"HTTP headerが検査後に変化しました"));
  }
  std::wstring value(buffer.data(), actual_bytes / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  if (value.empty() || value.size() > maximum_characters) {
    return clonecore::Result<std::wstring>::failure(
        invalid_manifest(L"HTTP headerが空または上限超過です"));
  }
  return clonecore::Result<std::wstring>::success(std::move(value));
}

clonecore::Result<ManualUpdateTransportResponse>
fetch_manual_update_manifest_with_winhttp(
    const std::wstring_view fixed_url,
    const std::size_t maximum_response_bytes) {
  if (fixed_url != kManualUpdateManifestUrl || maximum_response_bytes == 0U ||
      maximum_response_bytes > kMaximumManualUpdateManifestBytes) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        invalid_request(L"固定更新URLまたは応答上限が一致しません"));
  }

  UniqueInternetHandle session(WinHttpOpen(
      L"Y-TEC-Tsumugi-Drive/1.0 manual-update",
      WINHTTP_ACCESS_TYPE_NO_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0U));
  if (!session) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(L"WinHttpOpen(手動更新確認)", GetLastError()));
  }

  if (WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000) == FALSE) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"WinHttpSetTimeouts(手動更新確認)", GetLastError()));
  }
  DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
  if (WinHttpSetOption(
          session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &secure_protocols,
          sizeof(secure_protocols)) == FALSE) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"WinHttpSetOption(TLS 1.2限定)", GetLastError()));
  }

  UniqueInternetHandle connection(WinHttpConnect(
      session.get(), L"ytec.cloudfree.jp", INTERNET_DEFAULT_HTTPS_PORT, 0U));
  if (!connection) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"WinHttpConnect(Y-TEC手動更新)", GetLastError()));
  }

  const wchar_t* accept_types[]{L"application/json", nullptr};
  UniqueInternetHandle request(WinHttpOpenRequest(
      connection.get(), L"GET", L"/forge/updates/tsumugi-drive/update-v1.json",
      nullptr, WINHTTP_NO_REFERER, accept_types, WINHTTP_FLAG_SECURE));
  if (!request) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"WinHttpOpenRequest(Y-TEC手動更新)", GetLastError()));
  }

  DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (WinHttpSetOption(
          request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy,
          sizeof(redirect_policy)) == FALSE) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"WinHttpSetOption(redirect禁止)", GetLastError()));
  }
  DWORD disabled_features =
      WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  if (WinHttpSetOption(
          request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled_features,
          sizeof(disabled_features)) == FALSE) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"WinHttpSetOption(cookie/auth禁止)", GetLastError()));
  }

  constexpr wchar_t kHeaders[] =
      L"Accept: application/json\r\nCache-Control: no-cache\r\n";
  if (WinHttpSendRequest(
          request.get(), kHeaders,
          static_cast<DWORD>(std::size(kHeaders) - 1U),
          WINHTTP_NO_REQUEST_DATA, 0U, 0U, 0U) == FALSE ||
      WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"固定Y-TEC HTTPS更新情報の取得", GetLastError()));
  }

  DWORD status{};
  DWORD status_bytes = sizeof(status);
  if (WinHttpQueryHeaders(
          request.get(),
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_bytes,
          WINHTTP_NO_HEADER_INDEX) == FALSE ||
      status_bytes != sizeof(status)) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"更新情報HTTP statusの照会", GetLastError()));
  }

  auto content_type = query_bounded_header(
      request.get(), WINHTTP_QUERY_CONTENT_TYPE, 128U);
  if (!content_type.has_value()) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        content_type.error());
  }

  DWORD content_length{};
  DWORD length_bytes = sizeof(content_length);
  SetLastError(ERROR_SUCCESS);
  const BOOL has_length = WinHttpQueryHeaders(
      request.get(),
      WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &content_length, &length_bytes,
      WINHTTP_NO_HEADER_INDEX);
  const DWORD length_error = GetLastError();
  if (has_length == FALSE &&
      length_error != ERROR_WINHTTP_HEADER_NOT_FOUND) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        update_transport_error(
            L"更新情報Content-Lengthの照会", length_error));
  }
  if ((has_length != FALSE && length_bytes != sizeof(content_length)) ||
      (has_length != FALSE &&
       static_cast<std::size_t>(content_length) > maximum_response_bytes)) {
    return clonecore::Result<ManualUpdateTransportResponse>::failure(
        invalid_manifest(L"更新情報のContent-Lengthが上限を超えています"));
  }

  std::vector<std::byte> body;
  if (has_length != FALSE) {
    body.reserve(content_length);
  }
  while (true) {
    DWORD available{};
    if (WinHttpQueryDataAvailable(request.get(), &available) == FALSE) {
      return clonecore::Result<ManualUpdateTransportResponse>::failure(
          update_transport_error(
              L"更新情報の受信可能量照会", GetLastError()));
    }
    if (available == 0U) {
      break;
    }
    if (static_cast<std::size_t>(available) >
        maximum_response_bytes - body.size()) {
      return clonecore::Result<ManualUpdateTransportResponse>::failure(
          invalid_manifest(L"更新情報の応答が受信上限を超えました"));
    }
    const std::size_t offset = body.size();
    body.resize(offset + available);
    DWORD read{};
    if (WinHttpReadData(
            request.get(), body.data() + offset, available, &read) == FALSE) {
      return clonecore::Result<ManualUpdateTransportResponse>::failure(
          update_transport_error(L"更新情報の受信", GetLastError()));
    }
    if (read == 0U || read > available) {
      return clonecore::Result<ManualUpdateTransportResponse>::failure(
          invalid_manifest(L"更新情報の受信長が不正です"));
    }
    body.resize(offset + read);
  }

  return clonecore::Result<ManualUpdateTransportResponse>::success(
      ManualUpdateTransportResponse{
          .http_status = status,
          .final_url = std::wstring(kManualUpdateManifestUrl),
          .content_type = content_type.take_value(),
          .body = std::move(body),
      });
}

}  // namespace

clonecore::Result<ManualUpdateManifest> parse_manual_update_manifest(
    const std::span<const std::byte> body) {
  if (body.empty() || body.size() > kMaximumManualUpdateManifestBytes) {
    return clonecore::Result<ManualUpdateManifest>::failure(
        invalid_manifest(L"更新情報の応答サイズが許容範囲外です"));
  }

  std::string input;
  input.reserve(body.size());
  for (const std::byte byte : body) {
    const auto value = std::to_integer<unsigned char>(byte);
    if (value > 0x7FU) {
      return clonecore::Result<ManualUpdateManifest>::failure(invalid_manifest(
          L"更新情報は固定ASCII JSONスキーマである必要があります"));
    }
    input.push_back(static_cast<char>(value));
  }

  JsonCursor cursor(input);
  if (!cursor.consume('{')) {
    return clonecore::Result<ManualUpdateManifest>::failure(
        invalid_manifest(L"更新情報のJSON objectを読み取れません"));
  }

  ManualUpdateManifest manifest;
  enum Field : std::uint32_t {
    schema = 1U << 0U,
    product = 1U << 1U,
    latest = 1U << 2U,
    release_page = 1U << 3U,
    published = 1U << 4U,
    package_hash = 1U << 5U,
  };
  std::uint32_t fields{};
  constexpr std::uint32_t kRequiredFields =
      schema | product | latest | release_page | published | package_hash;

  bool first = true;
  while (true) {
    if (cursor.consume('}')) {
      break;
    }
    if (!first && !cursor.consume(',')) {
      return clonecore::Result<ManualUpdateManifest>::failure(
          invalid_manifest(L"更新情報のJSON区切りが正しくありません"));
    }
    first = false;

    auto key = cursor.parse_ascii_string(kMaximumJsonKeyBytes);
    if (!key.has_value() || !cursor.consume(':')) {
      return clonecore::Result<ManualUpdateManifest>::failure(
          key.has_value()
              ? invalid_manifest(L"更新情報のJSON key区切りがありません")
              : key.error());
    }

    std::uint32_t field{};
    if (key.value() == "schemaVersion") {
      field = schema;
      auto value = cursor.parse_uint32();
      if (!value.has_value()) {
        return clonecore::Result<ManualUpdateManifest>::failure(value.error());
      }
      manifest.schema_version = value.value();
    } else {
      auto value = cursor.parse_ascii_string(kMaximumJsonValueBytes);
      if (!value.has_value()) {
        return clonecore::Result<ManualUpdateManifest>::failure(value.error());
      }
      if (key.value() == "productId") {
        field = product;
        manifest.product_id = value.take_value();
      } else if (key.value() == "latestVersion") {
        field = latest;
        manifest.latest_version = value.take_value();
      } else if (key.value() == "releasePageUrl") {
        field = release_page;
        const std::string ascii = value.take_value();
        manifest.release_page_url.assign(ascii.begin(), ascii.end());
      } else if (key.value() == "publishedUtc") {
        field = published;
        manifest.published_utc = value.take_value();
      } else if (key.value() == "packageSha256") {
        field = package_hash;
        manifest.package_sha256 = value.take_value();
      } else {
        return clonecore::Result<ManualUpdateManifest>::failure(
            invalid_manifest(L"更新情報に未知のfieldがあります"));
      }
    }

    if ((fields & field) != 0U) {
      return clonecore::Result<ManualUpdateManifest>::failure(
          invalid_manifest(L"更新情報に重複fieldがあります"));
    }
    fields |= field;
  }

  if (!cursor.finished() || fields != kRequiredFields) {
    return clonecore::Result<ManualUpdateManifest>::failure(
        invalid_manifest(L"更新情報の必須fieldが不足または末尾が不正です"));
  }
  if (manifest.schema_version != kSchemaVersion ||
      manifest.product_id != kProductId ||
      manifest.release_page_url != kManualUpdateReleasePageUrl ||
      !valid_published_utc(manifest.published_utc) ||
      !valid_sha256(manifest.package_sha256)) {
    return clonecore::Result<ManualUpdateManifest>::failure(
        invalid_manifest(L"更新情報の固定値または形式が一致しません"));
  }
  auto version = parse_version(manifest.latest_version);
  if (!version.has_value()) {
    return clonecore::Result<ManualUpdateManifest>::failure(version.error());
  }
  manifest.package_sha256 = uppercase_ascii(manifest.package_sha256);
  return clonecore::Result<ManualUpdateManifest>::success(std::move(manifest));
}

clonecore::Result<ManualUpdateCheckReport> check_manual_update(
    const ManualUpdateCheckRequest& request,
    const ManualUpdateTransport& transport) {
  if (!request.user_initiated) {
    return clonecore::Result<ManualUpdateCheckReport>::failure(
        clonecore::Error{
            .code = clonecore::ErrorCode::confirmation_required,
            .native_code = ERROR_CANCELLED,
            .operation = L"手動更新確認",
            .message = L"利用者が更新確認を開始していません",
        });
  }
  if (!transport) {
    return clonecore::Result<ManualUpdateCheckReport>::failure(
        invalid_request(L"更新確認の通信Adapterがありません"));
  }
  auto current = parse_version(request.current_version);
  if (!current.has_value()) {
    return clonecore::Result<ManualUpdateCheckReport>::failure(
        invalid_request(L"現在の製品版番号を検証できません"));
  }

  auto response =
      transport(kManualUpdateManifestUrl, kMaximumManualUpdateManifestBytes);
  if (!response.has_value()) {
    return clonecore::Result<ManualUpdateCheckReport>::failure(response.error());
  }
  const auto& transport_response = response.value();
  if (transport_response.http_status != 200U ||
      transport_response.final_url != kManualUpdateManifestUrl ||
      !valid_json_content_type(transport_response.content_type) ||
      transport_response.body.empty() ||
      transport_response.body.size() > kMaximumManualUpdateManifestBytes) {
    return clonecore::Result<ManualUpdateCheckReport>::failure(invalid_manifest(
        L"更新情報のHTTP応答、最終URL、Content-Type、またはサイズを"
        L"検証できません"));
  }

  auto manifest = parse_manual_update_manifest(transport_response.body);
  if (!manifest.has_value()) {
    return clonecore::Result<ManualUpdateCheckReport>::failure(manifest.error());
  }
  auto latest = parse_version(manifest.value().latest_version);
  if (!latest.has_value()) {
    return clonecore::Result<ManualUpdateCheckReport>::failure(latest.error());
  }
  const int comparison = compare_versions(current.value(), latest.value());
  const auto disposition =
      comparison < 0
          ? ManualUpdateDisposition::update_available
          : comparison > 0 ? ManualUpdateDisposition::local_version_newer
                           : ManualUpdateDisposition::up_to_date;
  return clonecore::Result<ManualUpdateCheckReport>::success(
      ManualUpdateCheckReport{
          .disposition = disposition,
          .current_version = request.current_version,
          .manifest = manifest.take_value(),
      });
}

ManualUpdateTransport make_windows_manual_update_transport() {
  return [](const std::wstring_view fixed_url,
            const std::size_t maximum_response_bytes) {
    return fetch_manual_update_manifest_with_winhttp(
        fixed_url, maximum_response_bytes);
  };
}

clonecore::Result<ManualUpdateCheckReport>
check_manual_update_with_windows_apis(
    const ManualUpdateCheckRequest& request) {
  return check_manual_update(request, make_windows_manual_update_transport());
}

}  // namespace ytec::windowsapp

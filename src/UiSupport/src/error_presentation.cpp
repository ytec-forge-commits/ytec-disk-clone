#include "ytec/uisupport/error_presentation.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cwctype>
#include <string_view>
#include <utility>

namespace ytec::uisupport {
namespace {

constexpr std::size_t kMaximumSourceCharacters = 16U * 1024U;
constexpr std::size_t kMaximumDetailLineCharacters = 512U;
constexpr std::size_t kMaximumDetailLines = 32U;
constexpr std::wstring_view kOmissionMarker = L"＜詳細の一部を省略しました＞";
constexpr std::wstring_view kSecretMarker = L"＜秘密値を含む行を省略しました＞";
constexpr std::wstring_view kPathMarker = L"＜パスを含む行を省略しました＞";

std::wstring truncate_with_marker(
    const std::wstring_view value,
    const std::size_t maximum_characters) {
  if (value.size() <= maximum_characters) {
    return std::wstring(value);
  }
  if (maximum_characters == 0U) {
    return {};
  }
  constexpr std::wstring_view marker = L"…";
  if (maximum_characters <= marker.size()) {
    return std::wstring(marker.substr(0U, maximum_characters));
  }
  std::wstring result(
      value.substr(0U, maximum_characters - marker.size()));
  result += marker;
  return result;
}

wchar_t ascii_lower(const wchar_t value) noexcept {
  if (value >= L'A' && value <= L'Z') {
    return static_cast<wchar_t>(value - L'A' + L'a');
  }
  return value;
}

std::wstring ascii_lower_copy(const std::wstring_view value) {
  std::wstring result;
  result.reserve(value.size());
  for (const wchar_t character : value) {
    result.push_back(ascii_lower(character));
  }
  return result;
}

bool starts_with_ascii_ignore_case(
    const std::wstring_view value,
    const std::wstring_view prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < prefix.size(); ++index) {
    if (ascii_lower(value[index]) != ascii_lower(prefix[index])) {
      return false;
    }
  }
  return true;
}

std::wstring collapse_line(
    const std::wstring_view source,
    bool& truncated) {
  const std::size_t source_length =
      (std::min)(source.size(), kMaximumDetailLineCharacters);
  truncated = source.size() > source_length;
  std::wstring result;
  result.reserve(source_length);
  bool previous_space = true;
  for (std::size_t index = 0U; index < source_length; ++index) {
    wchar_t character = source[index];
    if (character == L'\0' || std::iswspace(character) != 0 ||
        character < L' ') {
      character = L' ';
    }
    if (character == L' ') {
      if (previous_space) {
        continue;
      }
      previous_space = true;
      result.push_back(character);
      continue;
    }
    previous_space = false;
    result.push_back(character);
  }
  while (!result.empty() && result.back() == L' ') {
    result.pop_back();
  }
  return result;
}

bool contains_secret_label(const std::wstring_view value) {
  static constexpr std::array<std::wstring_view, 28U> labels{
      L"password",
      L"passphrase",
      L"recovery key",
      L"recovery_key",
      L"bitlocker",
      L"api key",
      L"api_key",
      L"access token",
      L"access_token",
      L"refresh token",
      L"refresh_token",
      L"private key",
      L"private_key",
      L"secret",
      L"credential",
      L"authorization",
      L"bearer ",
      L"client secret",
      L"client_secret",
      L"x-api-key",
      L"cookie:",
      L"set-cookie",
      L"connection string",
      L"connection_string",
      L"認証情報",
      L"パスワード",
      L"回復キー",
      L"秘密鍵",
  };
  const std::wstring lower = ascii_lower_copy(value);
  return std::any_of(
      labels.begin(), labels.end(), [&](const std::wstring_view label) {
        return lower.find(label) != std::wstring::npos;
      });
}

bool contains_absolute_path(const std::wstring_view value) {
  const std::wstring lower = ascii_lower_copy(value);
  if (value.find(L"\\\\") != std::wstring_view::npos ||
      lower.find(L"\\device\\") != std::wstring_view::npos ||
      lower.find(L"\\??\\") != std::wstring_view::npos) {
    return true;
  }
  for (std::size_t index = 0U; index + 2U < value.size(); ++index) {
    const wchar_t drive = value[index];
    const bool ascii_letter =
        (drive >= L'A' && drive <= L'Z') ||
        (drive >= L'a' && drive <= L'z');
    if (ascii_letter && value[index + 1U] == L':' &&
        (value[index + 2U] == L'\\' || value[index + 2U] == L'/')) {
      return true;
    }
  }
  return false;
}

bool looks_like_stack_line(const std::wstring_view value) {
  const std::wstring lower = ascii_lower_copy(value);
  return starts_with_ascii_ignore_case(lower, L"at ") ||
         starts_with_ascii_ignore_case(lower, L"stack trace") ||
         starts_with_ascii_ignore_case(lower, L"stacktrace") ||
         starts_with_ascii_ignore_case(lower, L"scriptstacktrace") ||
         starts_with_ascii_ignore_case(lower, L"categoryinfo") ||
         starts_with_ascii_ignore_case(lower, L"fullyqualifiederrorid") ||
         starts_with_ascii_ignore_case(lower, L"invocationinfo") ||
         starts_with_ascii_ignore_case(lower, L"--- end") ||
         lower.find(L"parentcontainserrorrecordexception") !=
             std::wstring::npos;
}

std::wstring sanitize_primary_field(
    const std::wstring_view source,
    const std::size_t maximum_characters) {
  bool truncated = false;
  std::wstring value = collapse_line(source, truncated);
  if (contains_secret_label(value) || contains_absolute_path(value) ||
      looks_like_stack_line(value)) {
    return {};
  }
  return truncate_with_marker(value, maximum_characters);
}

void append_detail_line(
    std::wstring& output,
    const std::wstring_view line,
    bool& omitted) {
  if (line.empty()) {
    return;
  }
  const std::size_t separator = output.empty() ? 0U : 2U;
  if (output.size() + separator + line.size() >
      kMaximumErrorDetailCharacters) {
    omitted = true;
    return;
  }
  if (!output.empty()) {
    output += L"\r\n";
  }
  output += line;
}

std::wstring sanitize_detail_text(const std::wstring_view source) {
  const std::size_t bounded_length =
      (std::min)(source.size(), kMaximumSourceCharacters);
  const std::wstring_view bounded = source.substr(0U, bounded_length);
  bool omitted = source.size() > bounded_length;
  std::wstring output;
  std::size_t cursor = 0U;
  std::size_t line_count = 0U;
  while (cursor < bounded.size() && line_count < kMaximumDetailLines) {
    const std::size_t line_end = bounded.find_first_of(L"\r\n", cursor);
    const std::size_t length = line_end == std::wstring_view::npos
        ? bounded.size() - cursor
        : line_end - cursor;
    bool line_truncated = false;
    std::wstring line = collapse_line(
        bounded.substr(cursor, length), line_truncated);
    omitted = omitted || line_truncated;
    if (!line.empty()) {
      ++line_count;
      if (contains_secret_label(line)) {
        append_detail_line(output, kSecretMarker, omitted);
        omitted = true;
      } else if (contains_absolute_path(line)) {
        append_detail_line(output, kPathMarker, omitted);
        omitted = true;
      } else if (looks_like_stack_line(line)) {
        omitted = true;
      } else {
        append_detail_line(output, line, omitted);
      }
    }
    if (line_end == std::wstring_view::npos) {
      cursor = bounded.size();
    } else {
      cursor = line_end + 1U;
      if (bounded[line_end] == L'\r' && cursor < bounded.size() &&
          bounded[cursor] == L'\n') {
        ++cursor;
      }
    }
  }
  if (cursor < bounded.size()) {
    omitted = true;
  }
  if (omitted && output.find(kOmissionMarker) == std::wstring::npos) {
    append_detail_line(output, kOmissionMarker, omitted);
  }
  return truncate_with_marker(output, kMaximumErrorDetailCharacters);
}

std::wstring summary_for(const clonecore::ErrorCode code) {
  switch (code) {
    case clonecore::ErrorCode::invalid_argument:
      return L"入力内容を確認できませんでした";
    case clonecore::ErrorCode::invalid_data:
      return L"安全に扱えないデータを検出しました";
    case clonecore::ErrorCode::access_denied:
      return L"必要なアクセス権を確認できませんでした";
    case clonecore::ErrorCode::enumeration_failed:
    case clonecore::ErrorCode::query_failed:
      return L"接続中のディスク情報を確認できませんでした";
    case clonecore::ErrorCode::io_failed:
      return L"読み書き処理を安全に完了できませんでした";
    case clonecore::ErrorCode::identity_mismatch:
      return L"確認した対象が変わりました";
    case clonecore::ErrorCode::confirmation_required:
      return L"安全確認が完了していません";
    case clonecore::ErrorCode::unsupported_layout:
      return L"このディスク構成には対応していません";
    case clonecore::ErrorCode::verification_failed:
      return L"結果の検証に失敗しました";
    case clonecore::ErrorCode::unsupported_platform:
      return L"この環境では実行できません";
    case clonecore::ErrorCode::internal_error:
      return L"アプリ内部で処理を続けられませんでした";
    case clonecore::ErrorCode::cancelled:
      return L"操作を取り消しました";
  }
  return L"アプリ内部で処理を続けられませんでした";
}

std::wstring next_action_for(const clonecore::ErrorCode code) {
  switch (code) {
    case clonecore::ErrorCode::invalid_argument:
      return L"入力内容を確認して、もう一度選択してください。";
    case clonecore::ErrorCode::invalid_data:
      return L"対象を変更せず、再読み込みして内容を確認してください。";
    case clonecore::ErrorCode::access_denied:
      return L"保存場所と必要な権限を確認してから、やり直してください。";
    case clonecore::ErrorCode::enumeration_failed:
    case clonecore::ErrorCode::query_failed:
      return L"接続を確認して再読み込みし、対象を選び直してください。";
    case clonecore::ErrorCode::io_failed:
      return L"対象を完成扱いにせず、接続と診断ログを確認してください。";
    case clonecore::ErrorCode::identity_mismatch:
      return L"操作を開始せず、再読み込みして対象を選び直してください。";
    case clonecore::ErrorCode::confirmation_required:
      return L"表示された対象と注意事項を確認してから、やり直してください。";
    case clonecore::ErrorCode::unsupported_layout:
      return L"別の対応構成を選ぶか、レスキューメディアで確認してください。";
    case clonecore::ErrorCode::verification_failed:
      return L"対象を完成扱いにせず、詳細と診断ログを確認してください。";
    case clonecore::ErrorCode::unsupported_platform:
      return L"対応するWindowsまたはWinPE環境でやり直してください。";
    case clonecore::ErrorCode::internal_error:
      return L"操作を繰り返さず、詳細をコピーして診断に使用してください。";
    case clonecore::ErrorCode::cancelled:
      return L"必要な場合は対象を再確認して、最初からやり直してください。";
  }
  return L"操作を繰り返さず、詳細をコピーして診断に使用してください。";
}

std::wstring native_code_text(const DWORD code) {
  std::array<wchar_t, 11U> buffer{};
  const int written = swprintf_s(
      buffer.data(), buffer.size(), L"0x%08lX",
      static_cast<unsigned long>(code));
  return written > 0
      ? std::wstring(buffer.data(), static_cast<std::size_t>(written))
      : L"0x00000000";
}

std::wstring make_code_text(const clonecore::Error& error) {
  std::wstring code(clonecore::error_code_name(error.code));
  code += L" / native=";
  code += native_code_text(error.native_code);
  return truncate_with_marker(code, kMaximumErrorCodeCharacters);
}

void append_labeled_detail(
    std::wstring& details,
    const std::wstring_view label,
    const std::wstring_view value) {
  if (value.empty()) {
    return;
  }
  if (!details.empty()) {
    details += L"\r\n";
  }
  details += label;
  details += value;
}

std::wstring bounded_join(
    const std::wstring_view first,
    const std::wstring_view second,
    const std::size_t maximum_characters) {
  std::wstring value;
  value.reserve((std::min)(
      maximum_characters, first.size() + second.size()));
  value += first;
  value += second;
  return truncate_with_marker(value, maximum_characters);
}

}  // namespace

ErrorPresentation make_error_presentation(
    const clonecore::Error& error) {
  const std::wstring operation = sanitize_detail_text(error.operation);
  const std::wstring message = sanitize_detail_text(error.message);
  std::wstring details;
  append_labeled_detail(details, L"操作: ", operation);
  append_labeled_detail(details, L"内容: ", message);
  append_labeled_detail(
      details, L"内部コード: ", clonecore::error_code_name(error.code));
  append_labeled_detail(
      details, L"Nativeコード: ", native_code_text(error.native_code));
  details = truncate_with_marker(details, kMaximumErrorDetailCharacters);

  ErrorPresentation presentation{
      .summary = truncate_with_marker(
          summary_for(error.code), kMaximumErrorSummaryCharacters),
      .code = make_code_text(error),
      .next_action = truncate_with_marker(
          next_action_for(error.code), kMaximumErrorNextActionCharacters),
      .details = std::move(details),
  };
  presentation.details_expandable = !presentation.details.empty();
  presentation.details_copyable = !presentation.details.empty();
  return presentation;
}

std::wstring format_error_primary(
    const ErrorPresentation& presentation) {
  const std::wstring summary = sanitize_primary_field(
      presentation.summary, kMaximumErrorSummaryCharacters);
  const std::wstring code = sanitize_primary_field(
      presentation.code, kMaximumErrorCodeCharacters);
  const std::wstring next = sanitize_primary_field(
      presentation.next_action, kMaximumErrorNextActionCharacters);
  std::wstring primary;
  primary.reserve(summary.size() + code.size() + next.size() + 32U);
  primary += summary.empty() ? L"処理を続けられませんでした" : summary;
  primary += L"\r\nコード: ";
  primary += code.empty() ? L"internal_error" : code;
  primary += L"\r\n次の操作: ";
  primary += next.empty()
      ? L"詳細を確認し、対象を変更せずに停止してください。"
      : next;
  return truncate_with_marker(primary, kMaximumErrorPrimaryCharacters);
}

std::wstring format_error_details_for_copy(
    const ErrorPresentation& presentation) {
  std::wstring copy = format_error_primary(presentation);
  if (presentation.details_copyable && !presentation.details.empty()) {
    copy = bounded_join(copy, L"\r\n\r\n詳細:\r\n", kMaximumErrorCopyCharacters);
    const std::wstring sanitized_details =
        sanitize_detail_text(presentation.details);
    copy = bounded_join(
        copy,
        truncate_with_marker(
            sanitized_details, kMaximumErrorDetailCharacters),
        kMaximumErrorCopyCharacters);
  }
  return truncate_with_marker(copy, kMaximumErrorCopyCharacters);
}

}  // namespace ytec::uisupport

#include "ytec/clonecore/error.h"

#include <algorithm>
#include <vector>

namespace ytec::clonecore {
namespace {

void trim_trailing_whitespace(std::wstring& value) {
  while (!value.empty()) {
    const wchar_t last = value.back();
    if (last != L'\r' && last != L'\n' && last != L' ' && last != L'\t') {
      break;
    }
    value.pop_back();
  }
}

}  // namespace

std::wstring format_win32_message(const DWORD native_code) {
  if (native_code == ERROR_SUCCESS) {
    return L"成功";
  }

  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      native_code,
      0,
      reinterpret_cast<LPWSTR>(&buffer),
      0,
      nullptr);

  if (length == 0 || buffer == nullptr) {
    return L"Windows エラー情報を取得できませんでした";
  }

  std::wstring message(buffer, length);
  LocalFree(buffer);
  trim_trailing_whitespace(message);
  return message;
}

Error make_win32_error(
    const ErrorCode code,
    const std::wstring_view operation,
    const DWORD native_code) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::wstring(operation),
      .message = format_win32_message(native_code),
  };
}

std::wstring_view error_code_name(const ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::invalid_argument:
      return L"invalid_argument";
    case ErrorCode::invalid_data:
      return L"invalid_data";
    case ErrorCode::access_denied:
      return L"access_denied";
    case ErrorCode::enumeration_failed:
      return L"enumeration_failed";
    case ErrorCode::query_failed:
      return L"query_failed";
    case ErrorCode::io_failed:
      return L"io_failed";
    case ErrorCode::identity_mismatch:
      return L"identity_mismatch";
    case ErrorCode::confirmation_required:
      return L"confirmation_required";
    case ErrorCode::unsupported_layout:
      return L"unsupported_layout";
    case ErrorCode::verification_failed:
      return L"verification_failed";
    case ErrorCode::unsupported_platform:
      return L"unsupported_platform";
    case ErrorCode::internal_error:
      return L"internal_error";
    case ErrorCode::cancelled:
      return L"cancelled";
  }
  return L"internal_error";
}

}  // namespace ytec::clonecore

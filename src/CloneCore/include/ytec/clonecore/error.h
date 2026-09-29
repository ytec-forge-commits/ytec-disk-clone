#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ytec::clonecore {

enum class ErrorCode : std::uint8_t {
  invalid_argument,
  invalid_data,
  access_denied,
  enumeration_failed,
  query_failed,
  io_failed,
  identity_mismatch,
  confirmation_required,
  unsupported_layout,
  verification_failed,
  unsupported_platform,
  internal_error,
  cancelled,
};

struct Error final {
  ErrorCode code{ErrorCode::internal_error};
  DWORD native_code{ERROR_SUCCESS};
  std::wstring operation;
  std::wstring message;
};

[[nodiscard]] Error make_win32_error(
    ErrorCode code,
    std::wstring_view operation,
    DWORD native_code);

[[nodiscard]] std::wstring format_win32_message(DWORD native_code);
[[nodiscard]] std::wstring_view error_code_name(ErrorCode code) noexcept;

}  // namespace ytec::clonecore

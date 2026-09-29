#pragma once

#include "ytec/clonecore/error.h"

#include <cstddef>
#include <string>

namespace ytec::uisupport {

inline constexpr std::size_t kMaximumErrorSummaryCharacters = 80U;
inline constexpr std::size_t kMaximumErrorCodeCharacters = 80U;
inline constexpr std::size_t kMaximumErrorNextActionCharacters = 160U;
inline constexpr std::size_t kMaximumErrorDetailCharacters = 2048U;
inline constexpr std::size_t kMaximumErrorPrimaryCharacters = 512U;
inline constexpr std::size_t kMaximumErrorCopyCharacters = 3072U;

// Pure presentation data. The main text deliberately contains no raw engine
// message. A GUI may reveal details only after an explicit user action and may
// copy only format_error_details_for_copy() to the clipboard.
struct ErrorPresentation final {
  std::wstring summary;
  std::wstring code;
  std::wstring next_action;
  std::wstring details;
  bool details_expandable{};
  bool details_copyable{};
};

// Converts an engine error into bounded Japanese UI text. Recognized secret
// labels, absolute paths, stack-trace lines, control characters, and excessive
// input are omitted from expandable details. Raw error text is never used in
// summary, code, or next_action.
[[nodiscard]] ErrorPresentation make_error_presentation(
    const clonecore::Error& error);

// Formats the always-visible portion: short summary, stable code, next action.
[[nodiscard]] std::wstring format_error_primary(
    const ErrorPresentation& presentation);

// Formats a bounded support-oriented copy payload. It never returns more than
// kMaximumErrorCopyCharacters and includes only the already-sanitized details.
[[nodiscard]] std::wstring format_error_details_for_copy(
    const ErrorPresentation& presentation);

}  // namespace ytec::uisupport

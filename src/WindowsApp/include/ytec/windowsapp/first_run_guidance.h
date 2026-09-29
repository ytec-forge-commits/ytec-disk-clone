#pragma once

#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ytec::windowsapp {

inline constexpr std::wstring_view kFirstRunGuidanceSettingsFileName{
    L"tsumugi-ui-settings.bin"};
inline constexpr std::uint32_t kFirstRunGuidanceSchemaVersion = 1U;
inline constexpr std::size_t kFirstRunGuidanceDocumentBytes = 16U;
inline constexpr std::size_t kFirstRunGuidanceMaximumDocumentBytes = 64U;

struct FirstRunGuidanceItem final {
  std::wstring_view title;
  std::wstring_view description;
};

// Specification 11.1 deliberately limits the first-run message to these
// three short safety concepts. The same array is used for first display and
// the diagnostics-page redisplay so the wording cannot drift.
inline constexpr std::array<FirstRunGuidanceItem, 3U>
    kFirstRunGuidanceItems{{
        {
            L"対象確認",
            L"コピー元とコピー先のモデル・容量・接続方式・シリアル末尾を、実行前に確認します。",
        },
        {
            L"元ディスク保護",
            L"アプリはコピー元へ直接書き込みません。完全無変更が必要ならレスキューメディアを使います。",
        },
        {
            L"検証結果の意味",
            L"「検証完了」は書込みの読戻し確認です。実機での起動成功は別途確認します。",
        },
    }};

enum class FirstRunGuidanceDocumentState : std::uint8_t {
  missing,
  acknowledgement_pending,
  acknowledged,
  malformed,
  newer_schema,
  storage_unavailable,
};

struct FirstRunGuidanceDecision final {
  bool show_guidance{true};
  bool acknowledgement_may_be_saved{};
};

struct FirstRunGuidanceDiagnosticButtonLayout final {
  int update_left{};
  int update_width{};
  int guidance_left{};
  int guidance_width{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return update_width > 0 && guidance_width > 0 &&
           guidance_left >= update_left + update_width;
  }
};

struct FirstRunGuidanceInspection final {
  FirstRunGuidanceDocumentState state{
      FirstRunGuidanceDocumentState::storage_unavailable};
  FirstRunGuidanceDecision decision;
  std::wstring diagnostic;
  std::uint32_t native_code{};
};

enum class FirstRunGuidanceSaveDisposition : std::uint8_t {
  created,
  replaced,
  already_acknowledged,
};

struct FirstRunGuidanceSaveReport final {
  FirstRunGuidanceSaveDisposition disposition{
      FirstRunGuidanceSaveDisposition::created};
  std::wstring final_path;
  bool recovery_backup_retained{};
};

// Fixed-size binary schema: magic[8], little-endian schema[4], acknowledged
// byte, reserved[3]. No locale, BOM, text decoder, or permissive parser is
// involved.
[[nodiscard]] std::array<std::byte, kFirstRunGuidanceDocumentBytes>
serialize_first_run_guidance_document(bool acknowledged) noexcept;

[[nodiscard]] FirstRunGuidanceDocumentState
classify_first_run_guidance_document(
    std::span<const std::byte> bytes) noexcept;

[[nodiscard]] FirstRunGuidanceDecision plan_first_run_guidance(
    FirstRunGuidanceDocumentState state) noexcept;

// Splits the diagnostics secondary-action slot into two keyboard-reachable
// buttons. The primary action remains untouched for diagnostics refresh and
// future support-package actions.
[[nodiscard]] FirstRunGuidanceDiagnosticButtonLayout
calculate_first_run_guidance_diagnostic_button_layout(
    int secondary_left,
    int secondary_right,
    int gap = 10) noexcept;

// Production wrappers are bound to the current process executable and its
// exact adjacent existing "data" directory. They never create data and never
// consider AppData, the registry, environment variables, or another fallback.
[[nodiscard]] FirstRunGuidanceInspection
inspect_windows_first_run_guidance() noexcept;

[[nodiscard]] clonecore::Result<FirstRunGuidanceSaveReport>
save_windows_first_run_guidance_acknowledgement() noexcept;

}  // namespace ytec::windowsapp

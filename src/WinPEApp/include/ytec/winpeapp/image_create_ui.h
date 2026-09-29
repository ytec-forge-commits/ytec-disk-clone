#pragma once

#include "ytec/winpeapp/repair_layout.h"

#include <string_view>

namespace ytec::winpeapp {

struct WinPeImageCreateLayout final {
  UiRectangle source;
  UiRectangle destination;
  UiRectangle browse;
  UiRectangle rescue_mode;
  UiRectangle verification_mode;
  UiRectangle encryption;
  UiRectangle review;
  UiRectangle confirmation_token;
  UiRectangle execute;
  UiRectangle pause;
  UiRectangle cancel;
  UiRectangle output;
};

// Deterministic client-area layout used by the native WinPE GUI. All action
// controls remain inside a 1024x600 client area and gain vertical output space
// at 1280x720 without moving the confirmation buttons off screen.
[[nodiscard]] WinPeImageCreateLayout build_winpe_image_create_layout(
    int client_width,
    int client_height) noexcept;

struct WinPeImageCreateUiInput final {
  bool inventory_ready{};
  bool idle{};
  bool source_selected{};
  bool destination_entered{};
  bool verification_mode_selected{};
  bool reviewed{};
  bool progress_active{};
  std::wstring_view confirmation_text;
  bool cancellation_allowed{};
  bool cancellation_requested{};
};

struct WinPeImageCreateUiView final {
  bool source_enabled{};
  bool destination_enabled{};
  bool browse_enabled{};
  bool rescue_mode_enabled{};
  bool verification_mode_enabled{};
  bool encryption_enabled{};
  bool review_enabled{};
  bool confirmation_visible{};
  bool confirmation_enabled{};
  bool execute_visible{};
  bool execute_enabled{};
  bool cancel_visible{};
  bool cancel_enabled{};
};

[[nodiscard]] WinPeImageCreateUiView build_winpe_image_create_ui_view(
    const WinPeImageCreateUiInput& input) noexcept;

}  // namespace ytec::winpeapp

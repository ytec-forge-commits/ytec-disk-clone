#pragma once

#include "ytec/winpeapp/repair_layout.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace ytec::winpeapp {

struct WinPeImageRestoreLayout final {
  UiRectangle image_path;
  UiRectangle browse;
  UiRectangle verify;
  UiRectangle source_partition;
  UiRectangle target;
  UiRectangle target_partition;
  UiRectangle review;
  UiRectangle confirmation_token;
  UiRectangle execute;
  UiRectangle pause;
  UiRectangle cancel;
  UiRectangle output;
};

// Deterministic client-area layout. Every actionable control remains inside
// the compact 1024x600 client area and gains result space at 1280x720.
[[nodiscard]] WinPeImageRestoreLayout build_winpe_image_restore_layout(
    int client_width,
    int client_height) noexcept;

struct WinPeImageRestoreUiInput final {
  bool inventory_ready{};
  bool idle{};
  bool image_path_entered{};
  bool image_verified{};
  bool individual_partition_selected{};
  bool individual_target_selected{};
  bool target_selected{};
  bool target_reviewed{};
  bool reviewed_target_matches_selection{};
  bool image_storage_disk_known{};
  bool image_is_on_selected_target{};
  bool active_rescue_media_resolver_available{};
  bool progress_active{};
  std::wstring_view confirmation_text;
  bool cancellation_allowed{};
  bool cancellation_requested{};
};

struct WinPeImageRestoreUiView final {
  bool image_path_enabled{};
  bool browse_enabled{};
  bool verify_enabled{};
  bool source_partition_enabled{};
  bool target_enabled{};
  bool target_partition_enabled{};
  bool review_enabled{};
  bool confirmation_visible{};
  bool confirmation_enabled{};
  bool execute_visible{};
  bool execute_enabled{};
  bool cancel_visible{};
  bool cancel_enabled{};
};

[[nodiscard]] WinPeImageRestoreUiView build_winpe_image_restore_ui_view(
    const WinPeImageRestoreUiInput& input) noexcept;

[[nodiscard]] bool is_image_stored_on_restore_target(
    std::optional<std::uint32_t> image_storage_disk_number,
    std::optional<std::uint32_t> selected_target_disk_number) noexcept;

}  // namespace ytec::winpeapp

#pragma once

namespace ytec::winpeapp {

struct UiRectangle final {
  int left{};
  int top{};
  int right{};
  int bottom{};

  [[nodiscard]] constexpr int width() const noexcept {
    return right - left;
  }

  [[nodiscard]] constexpr int height() const noexcept {
    return bottom - top;
  }
};

struct WinPeRepairActionLayout final {
  UiRectangle note;
  UiRectangle winre_diagnostic_button;
  UiRectangle boot_inspect_button;
};

// Layout for the read-only actions at the bottom of the repair target card.
// The production window has a 1040 px minimum tracking width, but this stays
// deterministic so narrower synthetic widths can be rejected in tests.
[[nodiscard]] WinPeRepairActionLayout build_winpe_repair_action_layout(
    int client_width) noexcept;

}  // namespace ytec::winpeapp

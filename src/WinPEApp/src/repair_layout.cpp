#include "ytec/winpeapp/repair_layout.h"

#include <algorithm>

namespace ytec::winpeapp {

WinPeRepairActionLayout build_winpe_repair_action_layout(
    const int client_width) noexcept {
  constexpr int kContentLeft = 260;
  const int content_right = (std::max)(client_width - 28, 800);
  const int target_card_left = kContentLeft + 8;
  const int target_card_right = content_right - 8;
  return WinPeRepairActionLayout{
      .note =
          UiRectangle{
              .left = target_card_left + 16,
              .top = 248,
              .right = target_card_right - 394,
              .bottom = 290,
          },
      .winre_diagnostic_button =
          UiRectangle{
              .left = content_right - 374,
              .top = 253,
              .right = content_right - 210,
              .bottom = 285,
          },
      .boot_inspect_button =
          UiRectangle{
              .left = content_right - 196,
              .top = 253,
              .right = content_right - 32,
              .bottom = 285,
          },
  };
}

}  // namespace ytec::winpeapp

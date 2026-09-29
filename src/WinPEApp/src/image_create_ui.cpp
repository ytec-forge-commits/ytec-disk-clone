#include "ytec/winpeapp/image_create_ui.h"

#include <algorithm>

namespace ytec::winpeapp {

WinPeImageCreateLayout build_winpe_image_create_layout(
    const int client_width,
    const int client_height) noexcept {
  constexpr int kContentLeft = 260;
  const int content_right = (std::max)(client_width - 28, 800);
  const int field_left = kContentLeft + 22;
  const int field_right = content_right - 22;
  constexpr int kBrowseWidth = 134;
  constexpr int kActionWidth = 164;
  constexpr int kGap = 10;
  constexpr int kVerificationWidth = 238;
  const bool compact_height = client_height < 600;
  const int output_top = compact_height ? 472 : 516;
  const int output_bottom = compact_height
      ? (std::max)(client_height - 20, output_top + 1)
      : (std::max)(client_height - 32, 554);
  return WinPeImageCreateLayout{
      .source = {field_left, 201, field_right, 233},
      .destination = {
          field_left,
          261,
          field_right - kBrowseWidth - kGap,
          293,
      },
      .browse = {
          field_right - kBrowseWidth,
          261,
          field_right,
          293,
      },
      .rescue_mode = {
          field_left,
          303,
          field_right,
          331,
      },
      .verification_mode = {
          field_left,
          337,
          (std::min)(field_left + kVerificationWidth,
                     field_right - kActionWidth - (2 * kGap) - 180),
          369,
      },
      .encryption = {
          (std::min)(field_left + kVerificationWidth,
                     field_right - kActionWidth - (2 * kGap) - 180) + kGap,
          337,
          field_right - kActionWidth - kGap,
          369,
      },
      .review = {
          field_right - kActionWidth,
          337,
          field_right,
          369,
      },
      .confirmation_token = {
          field_left,
          403,
          field_right - kActionWidth - kGap,
          435,
      },
      .execute = {
          field_right - kActionWidth,
          403,
          field_right,
          435,
      },
      .pause = {
          field_right - (2 * kActionWidth) - kGap,
          437,
          field_right - kActionWidth - kGap,
          469,
      },
      .cancel = {
          field_right - kActionWidth,
          437,
          field_right,
          469,
      },
      .output = {
          field_left,
          output_top,
          field_right,
          output_bottom,
      },
  };
}

WinPeImageCreateUiView build_winpe_image_create_ui_view(
    const WinPeImageCreateUiInput& input) noexcept {
  const bool selectable = input.idle && !input.progress_active;
  const bool confirmation = input.reviewed && !input.progress_active;
  return WinPeImageCreateUiView{
      .source_enabled = selectable,
      .destination_enabled = selectable,
      .browse_enabled = selectable,
      .rescue_mode_enabled = selectable,
      .verification_mode_enabled = selectable,
      .encryption_enabled = selectable,
      .review_enabled = selectable && input.inventory_ready &&
          input.source_selected && input.destination_entered &&
          input.verification_mode_selected,
      .confirmation_visible = false,
      .confirmation_enabled = false,
      .execute_visible = confirmation,
      .execute_enabled = selectable && input.reviewed,
      .cancel_visible = input.progress_active,
      .cancel_enabled = !input.idle && input.progress_active &&
          input.cancellation_allowed && !input.cancellation_requested,
  };
}

}  // namespace ytec::winpeapp

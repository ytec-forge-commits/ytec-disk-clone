#include "ytec/windowsapp/layout.h"

#include <algorithm>
#include <limits>

namespace ytec::windowsapp {

CloneColumnLayout calculate_clone_column_layout(
    const int client_width) noexcept {
  constexpr int kContentLeft = 286;
  constexpr int kContentRightMargin = 36;
  constexpr int kColumnGap = 18;
  constexpr int kCardPadding = 18;

  const int content_right = (std::max)(kContentLeft, client_width - kContentRightMargin);
  const int available = content_right - kContentLeft;
  const int column_width = (std::max)(0, (available - kColumnGap) / 2);
  const int source_left = kContentLeft;
  const int target_left = source_left + column_width + kColumnGap;

  const HorizontalBounds source_card{
      source_left, source_left + column_width};
  const HorizontalBounds target_card{
      target_left, target_left + column_width};
  const HorizontalBounds source_control{
      source_card.left + kCardPadding,
      (std::max)(source_card.left + kCardPadding,
                 source_card.right - kCardPadding)};
  const HorizontalBounds target_control{
      target_card.left + kCardPadding,
      (std::max)(target_card.left + kCardPadding,
                 target_card.right - kCardPadding)};
  return CloneColumnLayout{
      .source_card = source_card,
      .target_card = target_card,
      .source_control = source_control,
      .target_control = target_control};
}

RescueMediaControlLayout calculate_rescue_media_control_layout(
    const int client_width) noexcept {
  constexpr int kCardLeft = 286;
  constexpr int kCardRightMargin = 36;
  constexpr int kInnerLeft = 312;
  constexpr int kInnerRightPadding = 18;
  constexpr int kControlGap = 18;
  constexpr int kBrowseButtonWidth = 156;

  const int card_right = (std::max)(kCardLeft, client_width - kCardRightMargin);
  const int inner_right = (std::max)(kInnerLeft, card_right - kInnerRightPadding);
  const int inner_width = inner_right - kInnerLeft;
  const int column_width = (std::max)(0, (inner_width - kControlGap) / 2);
  const int browse_left = (std::max)(
      kInnerLeft,
      inner_right - kBrowseButtonWidth);
  const int edit_right = (std::max)(
      kInnerLeft,
      browse_left - kControlGap);

  return RescueMediaControlLayout{
      .card = HorizontalBounds{kCardLeft, card_right},
      .kind_control = HorizontalBounds{
          kInnerLeft, kInnerLeft + column_width},
      .profile_control = HorizontalBounds{
          kInnerLeft + column_width + kControlGap,
          kInnerLeft + column_width * 2 + kControlGap},
      .mode_control = HorizontalBounds{
          kInnerLeft, kInnerLeft + column_width},
      .file_system_control = HorizontalBounds{
          kInnerLeft + column_width + kControlGap,
          kInnerLeft + column_width * 2 + kControlGap},
      .output_edit = HorizontalBounds{kInnerLeft, edit_right},
      .browse_button = HorizontalBounds{browse_left, inner_right}};
}

RescueMediaVerticalLayout calculate_rescue_media_vertical_layout(
    const int client_height,
    const bool usb_selected) noexcept {
  const bool compact = client_height < 680;
  if (compact) {
    return RescueMediaVerticalLayout{
        .compact = true,
        .kind_label_top = 246,
        .kind_control_top = 266,
        .option_label_top = 302,
        .option_control_top = 322,
        .destination_label_top = usb_selected ? 358 : 302,
        .destination_control_top = usb_selected ? 378 : 322,
    };
  }
  return RescueMediaVerticalLayout{
      .compact = false,
      .kind_label_top = 294,
      .kind_control_top = 318,
      .option_label_top = 359,
      .option_control_top = 382,
      .destination_label_top = usb_selected ? 423 : 359,
      .destination_control_top = usb_selected ? 446 : 382,
  };
}

BottomActionLayout calculate_bottom_action_layout(
    const int client_width) noexcept {
  constexpr int kContentLeft = 286;
  constexpr int kRightMargin = 36;
  constexpr int kButtonGap = 18;
  constexpr int kSecondaryWidth = 202;
  constexpr int kPreferredPrimaryWidth = 320;

  const int right = (std::max)(kContentLeft, client_width - kRightMargin);
  const int available_primary_width = (std::max)(
      0,
      right - kContentLeft - kSecondaryWidth - kButtonGap);
  const int primary_width = (std::min)(
      kPreferredPrimaryWidth,
      available_primary_width);
  const int primary_left = right - primary_width;
  const int secondary_right = (std::max)(
      kContentLeft,
      primary_left - kButtonGap);
  const int secondary_left = (std::max)(
      kContentLeft,
      secondary_right - kSecondaryWidth);

  return BottomActionLayout{
      .secondary_action =
          HorizontalBounds{secondary_left, secondary_right},
      .primary_action = HorizontalBounds{primary_left, right}};
}

DiagnosticsActionLayout calculate_diagnostics_action_layout(
    const int client_width) noexcept {
  constexpr int kContentLeft = 286;
  constexpr int kRightMargin = 36;
  constexpr int kButtonGap = 10;
  constexpr int kButtonCount = 4;

  const int right = (std::max)(kContentLeft, client_width - kRightMargin);
  const int available = (std::max)(
      0,
      right - kContentLeft - kButtonGap * (kButtonCount - 1));
  const int base_width = available / kButtonCount;
  const int first_left = kContentLeft;
  const int second_left = first_left + base_width + kButtonGap;
  const int third_left = second_left + base_width + kButtonGap;
  const int fourth_left = third_left + base_width + kButtonGap;
  return DiagnosticsActionLayout{
      .update_action = HorizontalBounds{
          first_left, first_left + base_width},
      .post_migration_action = HorizontalBounds{
          second_left, second_left + base_width},
      .guidance_action = HorizontalBounds{
          third_left, third_left + base_width},
      .support_action = HorizontalBounds{fourth_left, right},
  };
}

ImageCreateOptionLayout calculate_image_create_option_layout(
    const int client_width) noexcept {
  constexpr int kContentLeft = 312;
  constexpr int kRightMargin = 36;
  constexpr int kControlWidth = 266;
  constexpr int kControlGap = 10;

  const int right = (std::max)(kContentLeft, client_width - kRightMargin);
  const int transfer_left =
      (std::max)(kContentLeft, right - kControlWidth);
  const int verification_right =
      (std::max)(kContentLeft, transfer_left - kControlGap);
  const int verification_left =
      (std::max)(kContentLeft, verification_right - kControlWidth);
  return ImageCreateOptionLayout{
      .verification_control =
          HorizontalBounds{verification_left, verification_right},
      .transfer_control = HorizontalBounds{transfer_left, right},
  };
}

ClonePartitionCapacityDialogLayout
calculate_clone_partition_capacity_dialog_layout(
    const int work_width,
    const int work_height,
    const unsigned int dpi) noexcept {
  const unsigned int safe_dpi = (std::clamp)(dpi, 96U, 192U);
  const auto scaled = [safe_dpi](const int value) noexcept {
    const auto product = static_cast<long long>(value) * safe_dpi;
    return static_cast<int>((std::min)(
        product / 96LL,
        static_cast<long long>((std::numeric_limits<int>::max)())));
  };
  const int outer = (std::clamp)(scaled(12), 12, 24);
  const int available_width = (std::max)(0, work_width - outer * 2);
  const int available_height = (std::max)(0, work_height - outer * 2);
  const int desired_width = scaled(920);
  const int desired_height = scaled(650);
  const int client_width = (std::min)(available_width, desired_width);
  const int client_height = (std::min)(available_height, desired_height);
  const int padding = (std::clamp)(scaled(16), 12, 24);
  const int gap = (std::clamp)(scaled(8), 8, 12);
  const int guidance_height = (std::clamp)(scaled(42), 42, 70);
  const int row_height = (std::clamp)(scaled(27), 27, 38);
  const int status_height = (std::clamp)(scaled(28), 28, 44);
  const int button_height = (std::clamp)(scaled(32), 32, 44);
  const int button_width = (std::clamp)(scaled(116), 116, 176);
  const int label_width = (std::clamp)(scaled(178), 158, 270);

  const DialogBounds client{0, 0, client_width, client_height};
  const int content_left = (std::min)(padding, client_width);
  const int content_right = (std::max)(content_left, client_width - padding);
  const int content_bottom = (std::max)(padding, client_height - padding);
  const int cancel_left = (std::max)(content_left, content_right - button_width);
  const int accept_right = (std::max)(content_left, cancel_left - gap);
  const int accept_left = (std::max)(content_left, accept_right - button_width);
  const DialogBounds cancel{
      cancel_left,
      (std::max)(padding, content_bottom - button_height),
      content_right,
      content_bottom};
  const DialogBounds accept{
      accept_left, cancel.top, accept_right, cancel.bottom};
  const DialogBounds status{
      content_left,
      (std::max)(padding, accept.top - gap - status_height),
      content_right,
      (std::max)(padding, accept.top - gap)};
  const DialogBounds target_row{
      content_left,
      (std::max)(padding, status.top - gap - row_height),
      content_right,
      (std::max)(padding, status.top - gap)};
  const DialogBounds policy_row{
      content_left,
      (std::max)(padding, target_row.top - gap - row_height),
      content_right,
      (std::max)(padding, target_row.top - gap)};
  const int label_right = (std::min)(
      content_right, content_left + label_width);
  const int control_left = (std::min)(
      content_right, label_right + gap);
  const DialogBounds guidance{
      content_left,
      padding,
      content_right,
      (std::min)(policy_row.top, padding + guidance_height)};
  const DialogBounds partition_list{
      content_left,
      (std::min)(policy_row.top, guidance.bottom + gap),
      content_right,
      (std::max)(guidance.bottom + gap, policy_row.top - gap)};

  return ClonePartitionCapacityDialogLayout{
      .client_width = client_width,
      .client_height = client_height,
      .client = client,
      .guidance = guidance,
      .partition_list = partition_list,
      .surplus_label = DialogBounds{
          policy_row.left, policy_row.top, label_right, policy_row.bottom},
      .surplus_policy = DialogBounds{
          control_left, policy_row.top, policy_row.right, policy_row.bottom},
      .surplus_target_label = DialogBounds{
          target_row.left, target_row.top, label_right, target_row.bottom},
      .surplus_target = DialogBounds{
          control_left, target_row.top, target_row.right, target_row.bottom},
      .status = status,
      .accept_button = accept,
      .cancel_button = cancel,
  };
}

}  // namespace ytec::windowsapp

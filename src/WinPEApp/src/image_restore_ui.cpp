#include "ytec/winpeapp/image_restore_ui.h"

#include <algorithm>

namespace ytec::winpeapp {

WinPeImageRestoreLayout build_winpe_image_restore_layout(
    const int client_width,
    const int client_height) noexcept {
  constexpr int kContentLeft = 260;
  const int content_right = (std::max)(client_width - 28, 800);
  const int field_left = kContentLeft + 22;
  const int field_right = content_right - 22;
  constexpr int kBrowseWidth = 134;
  constexpr int kActionWidth = 176;
  constexpr int kGap = 10;
  const int partition_middle = field_left +
      (field_right - field_left) / 2;
  const bool compact_height = client_height < 600;
  const int output_top = compact_height ? 472 : 506;
  const int output_bottom = compact_height
      ? (std::max)(client_height - 20, output_top + 1)
      : (std::max)(client_height - 32, 554);
  return WinPeImageRestoreLayout{
      .image_path = {
          field_left,
          201,
          field_right - kBrowseWidth - kGap,
          233,
      },
      .browse = {
          field_right - kBrowseWidth,
          201,
          field_right,
          233,
      },
      .verify = {
          field_right - kActionWidth,
          241,
          field_right,
          273,
      },
      .source_partition = {
          field_left,
          295,
          partition_middle - (kGap / 2),
          327,
      },
      .target = {
          field_left,
          352,
          field_right - kActionWidth - kGap,
          384,
      },
      .target_partition = {
          partition_middle + (kGap / 2),
          295,
          field_right,
          327,
      },
      .review = {
          field_right - kActionWidth,
          352,
          field_right,
          384,
      },
      .confirmation_token = {
          field_left,
          413,
          field_right - kActionWidth - kGap,
          445,
      },
      .execute = {
          field_right - kActionWidth,
          413,
          field_right,
          445,
      },
      .pause = {
          field_right - (2 * kActionWidth) - kGap,
          413,
          field_right - kActionWidth - kGap,
          445,
      },
      .cancel = {
          field_right - kActionWidth,
          413,
          field_right,
          445,
      },
      .output = {field_left, output_top, field_right, output_bottom},
  };
}

WinPeImageRestoreUiView build_winpe_image_restore_ui_view(
    const WinPeImageRestoreUiInput& input) noexcept {
  const bool selectable = input.idle && !input.progress_active;
  const bool storage_safe = input.image_storage_disk_known &&
      !input.image_is_on_selected_target;
  const bool placement_ready = !input.individual_partition_selected ||
      input.individual_target_selected;
  const bool reviewed = input.target_reviewed &&
      input.reviewed_target_matches_selection && storage_safe &&
      input.active_rescue_media_resolver_available;
  const bool confirmation_matches = input.confirmation_text == L"OK";
  return WinPeImageRestoreUiView{
      .image_path_enabled = selectable,
      .browse_enabled = selectable,
      .verify_enabled = selectable && input.image_path_entered,
      .source_partition_enabled = selectable && input.image_verified,
      .target_enabled = selectable && input.image_verified,
      .target_partition_enabled = selectable && input.image_verified &&
          input.individual_partition_selected && input.target_selected,
      .review_enabled = selectable && input.inventory_ready &&
          input.image_verified && input.target_selected && placement_ready &&
          storage_safe,
      .confirmation_visible = reviewed && !input.progress_active,
      .confirmation_enabled = selectable && reviewed,
      .execute_visible = reviewed && !input.progress_active,
      .execute_enabled = selectable && reviewed && confirmation_matches,
      .cancel_visible = input.progress_active,
      .cancel_enabled = !input.idle && input.progress_active &&
          input.cancellation_allowed && !input.cancellation_requested,
  };
}

bool is_image_stored_on_restore_target(
    const std::optional<std::uint32_t> image_storage_disk_number,
    const std::optional<std::uint32_t> selected_target_disk_number) noexcept {
  return image_storage_disk_number.has_value() &&
      selected_target_disk_number.has_value() &&
      image_storage_disk_number.value() == selected_target_disk_number.value();
}

}  // namespace ytec::winpeapp

#pragma once

namespace ytec::windowsapp {

struct HorizontalBounds final {
  int left{};
  int right{};

  [[nodiscard]] int width() const noexcept {
    return right > left ? right - left : 0;
  }

  [[nodiscard]] bool contains(const HorizontalBounds& other) const noexcept {
    return other.left >= left && other.right <= right;
  }
};

struct CloneColumnLayout final {
  HorizontalBounds source_card;
  HorizontalBounds target_card;
  HorizontalBounds source_control;
  HorizontalBounds target_control;
};

struct RescueMediaControlLayout final {
  HorizontalBounds card;
  HorizontalBounds kind_control;
  HorizontalBounds profile_control;
  HorizontalBounds mode_control;
  HorizontalBounds file_system_control;
  HorizontalBounds output_edit;
  HorizontalBounds browse_button;
};

struct RescueMediaVerticalLayout final {
  bool compact{};
  int kind_label_top{};
  int kind_control_top{};
  int option_label_top{};
  int option_control_top{};
  int destination_label_top{};
  int destination_control_top{};
};

struct BottomActionLayout final {
  HorizontalBounds secondary_action;
  HorizontalBounds primary_action;
};

// The diagnostics page has four equally important keyboard-reachable actions.
// It uses the complete content row instead of squeezing extra controls into
// the normal two-action layout.
struct DiagnosticsActionLayout final {
  HorizontalBounds update_action;
  HorizontalBounds post_migration_action;
  HorizontalBounds guidance_action;
  HorizontalBounds support_action;
};

struct ImageCreateOptionLayout final {
  HorizontalBounds verification_control;
  HorizontalBounds transfer_control;
};

struct DialogBounds final {
  int left{};
  int top{};
  int right{};
  int bottom{};

  [[nodiscard]] int width() const noexcept {
    return right > left ? right - left : 0;
  }
  [[nodiscard]] int height() const noexcept {
    return bottom > top ? bottom - top : 0;
  }
  [[nodiscard]] bool contains(const DialogBounds& other) const noexcept {
    return other.left >= left && other.top >= top &&
        other.right <= right && other.bottom <= bottom;
  }
};

struct ClonePartitionCapacityDialogLayout final {
  int client_width{};
  int client_height{};
  DialogBounds client;
  DialogBounds guidance;
  DialogBounds partition_list;
  DialogBounds surplus_label;
  DialogBounds surplus_policy;
  DialogBounds surplus_target_label;
  DialogBounds surplus_target;
  DialogBounds status;
  DialogBounds accept_button;
  DialogBounds cancel_button;
};

[[nodiscard]] CloneColumnLayout calculate_clone_column_layout(
    int client_width) noexcept;

[[nodiscard]] RescueMediaControlLayout
calculate_rescue_media_control_layout(int client_width) noexcept;

[[nodiscard]] RescueMediaVerticalLayout
calculate_rescue_media_vertical_layout(
    int client_height,
    bool usb_selected) noexcept;

[[nodiscard]] BottomActionLayout calculate_bottom_action_layout(
    int client_width) noexcept;

[[nodiscard]] DiagnosticsActionLayout
calculate_diagnostics_action_layout(int client_width) noexcept;

[[nodiscard]] ImageCreateOptionLayout
calculate_image_create_option_layout(int client_width) noexcept;

// Fits inside the monitor work area at 100-200% DPI. The partition ListView
// owns vertical scrolling, so no row count can push buttons off-screen.
[[nodiscard]] ClonePartitionCapacityDialogLayout
calculate_clone_partition_capacity_dialog_layout(
    int work_width,
    int work_height,
    unsigned int dpi) noexcept;

}  // namespace ytec::windowsapp

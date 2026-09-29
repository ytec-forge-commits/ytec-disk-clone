#include "ytec/winpeapp/image_restore_ui.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool inside(
    const ytec::winpeapp::UiRectangle& value,
    const int width,
    const int height) {
  return value.left >= 260 && value.top >= 94 && value.right <= width - 20 &&
      value.bottom <= height - 20 && value.width() > 0 && value.height() > 0;
}

bool separated(
    const ytec::winpeapp::UiRectangle& upper,
    const ytec::winpeapp::UiRectangle& lower) {
  return upper.bottom <= lower.top;
}

void layout_fits(const int width, const int height) {
  const auto layout = ytec::winpeapp::build_winpe_image_restore_layout(
      width, height);
  const std::vector<ytec::winpeapp::UiRectangle> rectangles{
      layout.image_path,
      layout.browse,
      layout.verify,
      layout.source_partition,
      layout.target,
      layout.target_partition,
      layout.review,
      layout.confirmation_token,
      layout.execute,
      layout.pause,
      layout.cancel,
      layout.output,
  };
  for (const auto& rectangle : rectangles) {
    check(inside(rectangle, width, height),
          "every image-restore control must stay inside the client area");
  }
  check(layout.browse.width() >= 120 && layout.verify.width() >= 170 &&
            layout.review.width() >= 170 && layout.execute.width() >= 170 &&
            layout.pause.width() >= 160 &&
            layout.cancel.width() >= 160,
        "all Japanese action labels need a non-truncating button width");
  check(separated(layout.image_path, layout.verify) &&
            separated(layout.verify, layout.source_partition) &&
            separated(layout.verify, layout.target_partition) &&
            separated(layout.source_partition, layout.target) &&
            separated(layout.target_partition, layout.target) &&
            separated(layout.target, layout.confirmation_token) &&
            separated(layout.review, layout.confirmation_token) &&
            separated(layout.execute, layout.output) &&
            separated(layout.pause, layout.output) &&
            separated(layout.cancel, layout.output),
        "vertical restore control groups must not overlap");
  check(layout.image_path.right + 10 <= layout.browse.left &&
            layout.source_partition.right + 10 <=
                layout.target_partition.left &&
            layout.target.right + 10 <= layout.review.left &&
            layout.confirmation_token.right + 10 <= layout.execute.left,
        "same-row fields and buttons need a visible gap");
  check(layout.pause.right + 10 <= layout.cancel.left,
        "pause and cancel buttons need a visible gap");
}

void individual_restore_requires_an_existing_target_partition() {
  using ytec::winpeapp::build_winpe_image_restore_ui_view;
  auto view = build_winpe_image_restore_ui_view({
      .inventory_ready = true,
      .idle = true,
      .image_path_entered = true,
      .image_verified = true,
      .individual_partition_selected = true,
      .target_selected = true,
      .image_storage_disk_known = true,
      .active_rescue_media_resolver_available = true,
  });
  check(view.source_partition_enabled && view.target_partition_enabled &&
            !view.review_enabled,
        "individual restore must wait for a compatible existing target partition");

  view = build_winpe_image_restore_ui_view({
      .inventory_ready = true,
      .idle = true,
      .image_path_entered = true,
      .image_verified = true,
      .individual_partition_selected = true,
      .individual_target_selected = true,
      .target_selected = true,
      .image_storage_disk_known = true,
      .active_rescue_media_resolver_available = true,
  });
  check(view.review_enabled,
        "a compatible existing target partition should unlock read-only review");
}

void compact_and_standard_layouts_fit() {
  layout_fits(960, 516);
  layout_fits(1024, 600);
  layout_fits(1280, 720);
}

void full_verification_precedes_target_review_and_exact_ok() {
  using ytec::winpeapp::build_winpe_image_restore_ui_view;
  auto view = build_winpe_image_restore_ui_view({
      .inventory_ready = true,
      .idle = true,
      .image_path_entered = true,
  });
  check(view.verify_enabled && !view.target_enabled && !view.review_enabled &&
            !view.execute_visible,
        "only full image verification may follow initial file selection");

  view = build_winpe_image_restore_ui_view({
      .inventory_ready = true,
      .idle = true,
      .image_path_entered = true,
      .image_verified = true,
      .target_selected = true,
      .image_storage_disk_known = true,
      .active_rescue_media_resolver_available = true,
  });
  check(view.target_enabled && view.review_enabled && !view.execute_visible,
        "a verified image and selected target should enable read-only review");

  view = build_winpe_image_restore_ui_view({
      .inventory_ready = true,
      .idle = true,
      .image_path_entered = true,
      .image_verified = true,
      .target_selected = true,
      .target_reviewed = true,
      .reviewed_target_matches_selection = true,
      .image_storage_disk_known = true,
      .active_rescue_media_resolver_available = true,
      .confirmation_text = L"ok",
  });
  check(view.execute_visible && !view.execute_enabled,
        "lowercase confirmation must never enable restore");

  view = build_winpe_image_restore_ui_view({
      .inventory_ready = true,
      .idle = true,
      .image_path_entered = true,
      .image_verified = true,
      .target_selected = true,
      .target_reviewed = true,
      .reviewed_target_matches_selection = true,
      .image_storage_disk_known = true,
      .active_rescue_media_resolver_available = true,
      .confirmation_text = L"OK",
  });
  check(view.execute_enabled,
        "only exact uppercase OK should enable the reviewed restore");
}

void same_media_target_change_and_missing_boot_resolver_fail_closed() {
  using ytec::winpeapp::build_winpe_image_restore_ui_view;
  check(ytec::winpeapp::is_image_stored_on_restore_target(3U, 3U) &&
            !ytec::winpeapp::is_image_stored_on_restore_target(3U, 4U) &&
            !ytec::winpeapp::is_image_stored_on_restore_target(
                std::nullopt, 3U),
        "same physical image/target media comparison must require exact known disks");

  auto base = ytec::winpeapp::WinPeImageRestoreUiInput{
      .inventory_ready = true,
      .idle = true,
      .image_path_entered = true,
      .image_verified = true,
      .target_selected = true,
      .target_reviewed = true,
      .reviewed_target_matches_selection = true,
      .image_storage_disk_known = true,
      .active_rescue_media_resolver_available = true,
      .confirmation_text = L"OK",
  };
  base.image_is_on_selected_target = true;
  auto view = build_winpe_image_restore_ui_view(base);
  check(!view.review_enabled && !view.execute_visible,
        "an image stored on the erase target must fail before confirmation");

  base.image_is_on_selected_target = false;
  base.reviewed_target_matches_selection = false;
  view = build_winpe_image_restore_ui_view(base);
  check(!view.execute_visible,
        "changing target after review must invalidate execution");

  base.reviewed_target_matches_selection = true;
  base.active_rescue_media_resolver_available = false;
  view = build_winpe_image_restore_ui_view(base);
  check(!view.execute_visible,
        "missing strict active-rescue-media resolver must fail closed");
}

void progress_locks_every_selection_and_exposes_safe_cancel() {
  auto view = ytec::winpeapp::build_winpe_image_restore_ui_view({
      .idle = false,
      .progress_active = true,
      .cancellation_allowed = true,
  });
  check(!view.image_path_enabled && !view.browse_enabled &&
            !view.verify_enabled && !view.target_enabled &&
            !view.review_enabled && !view.execute_visible &&
            view.cancel_visible && view.cancel_enabled,
        "active restore must lock selection and expose safe cancellation");

  view = ytec::winpeapp::build_winpe_image_restore_ui_view({
      .idle = false,
      .progress_active = true,
      .cancellation_allowed = true,
      .cancellation_requested = true,
  });
  check(view.cancel_visible && !view.cancel_enabled,
        "repeated cancellation requests must be blocked");
}

}  // namespace

int main() {
  try {
    compact_and_standard_layouts_fit();
    full_verification_precedes_target_review_and_exact_ok();
    individual_restore_requires_an_existing_target_partition();
    same_media_target_change_and_missing_boot_resolver_fail_closed();
    progress_locks_every_selection_and_exposes_safe_cancel();
    std::cout << "winpe image restore ui tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "winpe image restore ui tests: FAIL: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
}

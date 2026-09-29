#include "ytec/winpeapp/image_create_ui.h"

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
  const auto layout = ytec::winpeapp::build_winpe_image_create_layout(
      width, height);
  const std::vector<ytec::winpeapp::UiRectangle> rectangles{
      layout.source,
      layout.destination,
      layout.browse,
      layout.rescue_mode,
      layout.verification_mode,
      layout.encryption,
      layout.review,
      layout.confirmation_token,
      layout.execute,
      layout.pause,
      layout.cancel,
      layout.output,
  };
  for (const auto& rectangle : rectangles) {
    check(inside(rectangle, width, height),
          "every image-create control must stay inside the client area");
  }
  check(layout.browse.width() >= 120 && layout.review.width() >= 140 &&
            layout.execute.width() >= 160 && layout.pause.width() >= 140 &&
            layout.cancel.width() >= 140,
        "all action labels need a non-truncating button width");
  check(layout.source.height() >= 30 && layout.destination.height() >= 30 &&
            layout.confirmation_token.height() >= 30,
        "keyboard fields need a usable height");
  check(separated(layout.source, layout.destination) &&
            separated(layout.destination, layout.rescue_mode) &&
            separated(layout.rescue_mode, layout.verification_mode) &&
            separated(layout.rescue_mode, layout.encryption) &&
            separated(layout.rescue_mode, layout.review) &&
            separated(layout.review, layout.confirmation_token) &&
            separated(layout.execute, layout.output) &&
            separated(layout.pause, layout.output) &&
            separated(layout.cancel, layout.output),
        "vertical control groups must not overlap");
  check(layout.destination.right + 10 <= layout.browse.left &&
            layout.confirmation_token.right + 10 <= layout.execute.left,
        "same-row fields and buttons need a visible gap");
  check(layout.verification_mode.right + 10 <= layout.encryption.left &&
            layout.encryption.right + 10 <= layout.review.left,
        "verification, encryption, and review controls need visible gaps");
  check(layout.pause.right + 10 <= layout.cancel.left,
        "pause and cancel buttons need a visible gap");
}

void standard_and_compact_layouts_fit() {
  layout_fits(960, 516);
  layout_fits(1280, 720);
  layout_fits(1024, 600);
}

void ui_state_requires_review_without_destructive_ok_token() {
  using ytec::winpeapp::WinPeImageCreateUiInput;
  auto view = ytec::winpeapp::build_winpe_image_create_ui_view({
      .inventory_ready = true,
      .idle = true,
      .source_selected = true,
      .destination_entered = true,
      .verification_mode_selected = true,
  });
  check(view.review_enabled && !view.execute_visible &&
            !view.cancel_visible && view.rescue_mode_enabled,
        "selection should enable review only");

  view = ytec::winpeapp::build_winpe_image_create_ui_view({
      .inventory_ready = true,
      .idle = true,
      .source_selected = true,
      .destination_entered = true,
      .verification_mode_selected = true,
      .reviewed = true,
      .confirmation_text = L"ok",
  });
  check(!view.confirmation_visible && !view.confirmation_enabled &&
            view.execute_visible && view.execute_enabled,
        "source-to-file review should enable execution without a destructive-target OK token");

  view = ytec::winpeapp::build_winpe_image_create_ui_view({
      .inventory_ready = true,
      .idle = true,
      .source_selected = true,
      .destination_entered = true,
      .verification_mode_selected = true,
      .reviewed = true,
      .confirmation_text = L"OK ",
  });
  check(view.execute_enabled && !view.confirmation_visible,
        "legacy confirmation text must not gate source-to-file execution");

  view = ytec::winpeapp::build_winpe_image_create_ui_view({
      .inventory_ready = true,
      .idle = true,
      .source_selected = true,
      .destination_entered = true,
      .verification_mode_selected = true,
      .reviewed = true,
      .confirmation_text = L"OK",
  });
  check(view.execute_enabled && !view.confirmation_visible,
        "a reviewed source-to-file operation should remain executable without OK");

  view = ytec::winpeapp::build_winpe_image_create_ui_view({
      .inventory_ready = true,
      .idle = true,
      .source_selected = true,
      .destination_entered = true,
      .verification_mode_selected = false,
  });
  check(!view.review_enabled && view.verification_mode_enabled,
        "an unknown verification mode must fail closed before review");
}

void progress_locks_selection_and_exposes_safe_cancel() {
  auto view = ytec::winpeapp::build_winpe_image_create_ui_view({
      .inventory_ready = true,
      .idle = false,
      .source_selected = true,
      .destination_entered = true,
      .verification_mode_selected = true,
      .progress_active = true,
      .cancellation_allowed = true,
  });
  check(!view.source_enabled && !view.destination_enabled &&
            !view.browse_enabled && !view.rescue_mode_enabled &&
            !view.verification_mode_enabled && !view.review_enabled &&
            !view.execute_visible && view.cancel_visible &&
            view.cancel_enabled,
        "active creation must lock selection and expose safe cancel");

  view = ytec::winpeapp::build_winpe_image_create_ui_view({
      .idle = false,
      .progress_active = true,
      .cancellation_allowed = true,
      .cancellation_requested = true,
  });
  check(view.cancel_visible && !view.cancel_enabled,
        "a repeated cancellation request must be blocked");
}

}  // namespace

int main() {
  try {
    standard_and_compact_layouts_fit();
    ui_state_requires_review_without_destructive_ok_token();
    progress_locks_selection_and_exposes_safe_cancel();
    std::cout << "winpe image create ui tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "winpe image create ui tests: FAIL: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
}

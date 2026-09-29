#pragma once

#include "ytec/windowsapp/support_zip.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

struct SupportZipReviewModel final {
  std::wstring output_file_name;
  std::wstring summary;
  std::vector<std::wstring> entry_rows;
  std::wstring privacy_notice;
};

// Produces the complete pre-creation review without exposing a source or
// destination directory. Only the final basename, archive basenames, masked
// sizes, and aggregate exclusion count cross this UI seam.
[[nodiscard]] SupportZipReviewModel build_support_zip_review_model(
    std::wstring_view final_path,
    std::span<const SupportZipDisplayEntry> entries,
    std::uint64_t masked_total_bytes,
    std::size_t excluded_log_count);

struct SupportZipUiBounds final {
  int left{};
  int top{};
  int right{};
  int bottom{};

  [[nodiscard]] int width() const noexcept { return right - left; }
  [[nodiscard]] int height() const noexcept { return bottom - top; }
  [[nodiscard]] bool contains(const SupportZipUiBounds& child) const noexcept {
    return child.left >= left && child.top >= top &&
        child.right <= right && child.bottom <= bottom;
  }
};

struct SupportZipReviewLayout final {
  int client_width{};
  int client_height{};
  SupportZipUiBounds summary;
  SupportZipUiBounds entries;
  SupportZipUiBounds privacy_notice;
  SupportZipUiBounds create_button;
  SupportZipUiBounds cancel_button;
};

inline constexpr int kSupportZipReviewOuterGap = 24;
inline constexpr int kSupportZipReviewFrameReserve = 40;

// The returned client remains inside the available work area, including the
// 960 x 516 logical viewport used by the 1024 x 600 / 200%-DPI acceptance
// profile. The file list scrolls instead of growing the dialog.
[[nodiscard]] SupportZipReviewLayout calculate_support_zip_review_layout(
    int available_width,
    int available_height) noexcept;

// Static acceptance contract for the product entry. Kept outside main.cpp so
// the no-I/O UI tests can prove keyboard and background behavior without
// launching a file picker or reading product logs.
struct SupportZipUiContract final {
  std::wstring_view action_label;
  std::wstring_view create_label;
  bool save_as_local{};
  bool planning_in_background{};
  bool creation_in_background{};
  bool enter_accepts_review{};
  bool escape_cancels_review{};
  bool previous_focus_restored{};
  bool automatic_send{};
};

[[nodiscard]] constexpr SupportZipUiContract support_zip_ui_contract()
    noexcept {
  return SupportZipUiContract{
      .action_label = L"サポートZIP保存",
      .create_label = L"ローカルZIPを作成",
      .save_as_local = true,
      .planning_in_background = true,
      .creation_in_background = true,
      .enter_accepts_review = true,
      .escape_cancels_review = true,
      .previous_focus_restored = true,
      .automatic_send = false,
  };
}

}  // namespace ytec::windowsapp

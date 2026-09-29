#include "ytec/windowsapp/support_zip_ui.h"

#include "ytec/windowsapp/progress.h"

#include <algorithm>

namespace ytec::windowsapp {
namespace {

std::wstring basename_only(const std::wstring_view path) {
  const std::size_t separator = path.find_last_of(L"\\/");
  const std::wstring_view name = separator == std::wstring_view::npos
      ? path
      : path.substr(separator + 1U);
  return name.empty() ? std::wstring(L"support.zip") : std::wstring(name);
}

}  // namespace

SupportZipReviewModel build_support_zip_review_model(
    const std::wstring_view final_path,
    const std::span<const SupportZipDisplayEntry> entries,
    const std::uint64_t masked_total_bytes,
    const std::size_t excluded_log_count) {
  SupportZipReviewModel model;
  model.output_file_name = basename_only(final_path);
  model.summary =
      L"保存ファイル: " + model.output_file_name + L"\r\n" +
      L"含有ログ: " + std::to_wstring(entries.size()) +
      L"件　追加マスク後合計: " + format_bytes(masked_total_bytes) +
      L"　選外: " + std::to_wstring(excluded_log_count) + L"件";
  model.entry_rows.reserve(entries.size());
  for (const auto& entry : entries) {
    model.entry_rows.push_back(
        basename_only(entry.archive_entry_name) +
        L"　マスク後 " + format_bytes(entry.masked_size_bytes));
  }
  model.privacy_notice =
      L"一覧は製品ログのファイル名と追加マスク後のサイズだけです。"
      L"元の保存パス、パスワード、回復キー、文書名は表示・格納せず、"
      L"ZIPは選択したローカル先へ保存するだけで自動送信しません。";
  return model;
}

SupportZipReviewLayout calculate_support_zip_review_layout(
    const int available_width,
    const int available_height) noexcept {
  constexpr int kPreferredWidth = 720;
  constexpr int kPreferredHeight = 460;
  constexpr int kMargin = 20;
  constexpr int kSummaryHeight = 54;
  constexpr int kPrivacyHeight = 52;
  constexpr int kButtonWidth = 180;
  constexpr int kButtonHeight = 38;
  constexpr int kButtonGap = 12;
  constexpr int kRowGap = 10;

  const int usable_width = (std::max)(
      0,
      available_width - kSupportZipReviewOuterGap * 2 -
          kSupportZipReviewFrameReserve);
  const int usable_height = (std::max)(
      0,
      available_height - kSupportZipReviewOuterGap * 2 -
          kSupportZipReviewFrameReserve);
  const int client_width = (std::min)(kPreferredWidth, usable_width);
  const int client_height = (std::min)(kPreferredHeight, usable_height);
  const int content_right = client_width - kMargin;
  const int button_top = client_height - kMargin - kButtonHeight;
  const int privacy_bottom = button_top - kRowGap;
  const int privacy_top = privacy_bottom - kPrivacyHeight;
  const int entries_top = kMargin + kSummaryHeight + kRowGap;
  const int entries_bottom = (std::max)(entries_top, privacy_top - kRowGap);
  const int cancel_left = content_right - kButtonWidth;
  const int create_right = cancel_left - kButtonGap;
  const int create_left = create_right - kButtonWidth;

  return SupportZipReviewLayout{
      .client_width = client_width,
      .client_height = client_height,
      .summary = {kMargin, kMargin, content_right, kMargin + kSummaryHeight},
      .entries = {kMargin, entries_top, content_right, entries_bottom},
      .privacy_notice = {
          kMargin, privacy_top, content_right, privacy_bottom},
      .create_button = {
          create_left, button_top, create_right, button_top + kButtonHeight},
      .cancel_button = {
          cancel_left, button_top, content_right, button_top + kButtonHeight},
  };
}

}  // namespace ytec::windowsapp

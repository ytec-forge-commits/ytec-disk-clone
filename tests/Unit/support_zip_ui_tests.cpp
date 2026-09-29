#include "ytec/windowsapp/support_zip_ui.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

void check(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool overlaps(
    const ytec::windowsapp::SupportZipUiBounds& left,
    const ytec::windowsapp::SupportZipUiBounds& right) {
  return left.left < right.right && left.right > right.left &&
      left.top < right.bottom && left.bottom > right.top;
}

void verify_layout(const int width, const int height) {
  const auto layout =
      ytec::windowsapp::calculate_support_zip_review_layout(width, height);
  const ytec::windowsapp::SupportZipUiBounds client{
      0, 0, layout.client_width, layout.client_height};
  const std::array controls{
      layout.summary,
      layout.entries,
      layout.privacy_notice,
      layout.create_button,
      layout.cancel_button,
  };
  check(
      layout.client_width <=
              width - ytec::windowsapp::kSupportZipReviewOuterGap * 2 -
                  ytec::windowsapp::kSupportZipReviewFrameReserve &&
          layout.client_height <=
              height - ytec::windowsapp::kSupportZipReviewOuterGap * 2 -
                  ytec::windowsapp::kSupportZipReviewFrameReserve,
      "support ZIP review must stay inside the available work area");
  for (const auto& control : controls) {
    check(client.contains(control), "support ZIP control must stay in client");
  }
  check(layout.entries.height() >= 100, "file list needs a useful viewport");
  check(
      !overlaps(layout.summary, layout.entries) &&
          !overlaps(layout.entries, layout.privacy_notice) &&
          !overlaps(layout.privacy_notice, layout.create_button) &&
          !overlaps(layout.create_button, layout.cancel_button),
      "support ZIP review controls must not overlap");
  check(
      layout.create_button.width() >= 180,
      "explicit create label needs a non-truncating button");
}

void verify_review_model() {
  const std::array<ytec::windowsapp::SupportZipDisplayEntry, 2> entries{{
      {.archive_entry_name = L"logs\\tsumugi-20260813-010203.log",
       .source_size_bytes = 8192,
       .masked_size_bytes = 4096},
      {.archive_entry_name = L"tsumugi-failure-20260812-235959.log",
       .source_size_bytes = 16384,
       .masked_size_bytes = 12288},
  }};
  const auto model = ytec::windowsapp::build_support_zip_review_model(
      L"D:\\Synthetic\\Support\\Tsumugi-support.zip",
      entries,
      16384,
      7);
  check(
      model.output_file_name == L"Tsumugi-support.zip",
      "review exposes only the output basename");
  check(
      model.summary.find(L"D:\\Synthetic") == std::wstring::npos,
      "review must not expose destination path");
  check(
      model.summary.find(L"選外: 7件") != std::wstring::npos,
      "review must show excluded count");
  check(model.entry_rows.size() == 2, "every included log must be listed");
  check(
      model.entry_rows[0].find(L"logs\\") == std::wstring::npos,
      "entry review must expose only archive basename");
  check(
      model.entry_rows[0].find(L"D:\\Synthetic") == std::wstring::npos &&
          model.entry_rows[1].find(L"D:\\Synthetic") ==
              std::wstring::npos,
      "entry review must never expose source path");
  check(
      model.entry_rows[0].find(L"マスク後") != std::wstring::npos,
      "entry review must label masked size truthfully");
  check(
      model.privacy_notice.find(L"自動送信しません") != std::wstring::npos,
      "review must state local-only behavior");
}

void verify_ui_contract() {
  constexpr auto contract = ytec::windowsapp::support_zip_ui_contract();
  check(
      contract.action_label == L"サポートZIP保存" &&
          contract.create_label == L"ローカルZIPを作成",
      "product actions need explicit Japanese labels");
  check(
      contract.save_as_local && contract.planning_in_background &&
          contract.creation_in_background,
      "Save As planning and creation must use the bounded background flow");
  check(
      contract.enter_accepts_review && contract.escape_cancels_review &&
          contract.previous_focus_restored,
      "review must preserve Enter Esc and focus behavior");
  check(!contract.automatic_send, "support ZIP must never auto-send");
}

}  // namespace

int main() {
  verify_layout(960, 516);
  verify_layout(1024, 600);
  verify_layout(1280, 720);
  verify_review_model();
  verify_ui_contract();
  std::cout << "support zip UI tests: PASS\n";
  return 0;
}

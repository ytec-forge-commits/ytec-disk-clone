#include "ytec/windowsapp/media_preflight.h"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string_view>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kMaximumDisplayedDiagnostics = 6U;

std::wstring widen_ascii(std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

std::wstring ready_text(bool ready) {
  return ready ? L"確認済み" : L"未確認";
}

void append_diagnostics(
    std::wostringstream& output,
    const mediabuilder::AdkDiscoveryReport& report) {
  std::size_t displayed{};
  std::size_t first_candidate{};
  std::size_t candidate_count = report.candidates.size();
  if (report.selected_candidate_index &&
      *report.selected_candidate_index < report.candidates.size()) {
    first_candidate = *report.selected_candidate_index;
    candidate_count = 1U;
  }
  for (std::size_t candidate_index = first_candidate;
       candidate_index < first_candidate + candidate_count;
       ++candidate_index) {
    const auto& candidate = report.candidates[candidate_index];
    for (const auto& diagnostic : candidate.diagnostics) {
      if (diagnostic.severity ==
          mediabuilder::DiagnosticSeverity::information) {
        continue;
      }
      if (displayed == 0U) {
        output << L"\n主な診断:\n";
      }
      output << L"・[" << widen_ascii(diagnostic.code) << L"] "
             << diagnostic.message << L'\n';
      ++displayed;
      if (displayed >= kMaximumDisplayedDiagnostics) {
        return;
      }
    }
  }
}

}  // namespace

MediaPreflightView summarize_media_preflight(
    const mediabuilder::AdkDiscoveryReport& report) {
  MediaPreflightView view{
      .base_layout_ready = report.base_layout_ready(),
      .bootex_layout_ready = report.bootex_layout_ready(),
      .media_creation_permitted = report.media_creation_permitted(),
  };

  if (view.media_creation_permitted) {
    view.status = L"レスキューメディアの作成準備を確認できました。";
  } else if (view.base_layout_ready) {
    view.status =
        L"ADK/WinPEは検出しましたが、安全な作成を許可できません。";
  } else {
    view.status =
        L"必要なADK/WinPE構成を確認できませんでした。";
  }

  std::wostringstream details;
  details << L"検査アーキテクチャ: "
          << (report.architecture.empty() ? L"不明" : report.architecture)
          << L"\n検査候補: " << report.candidates.size() << L"件\n";

  if (report.selected_candidate_index &&
      *report.selected_candidate_index < report.candidates.size()) {
    const auto& selected =
        report.candidates[*report.selected_candidate_index];
    details << L"検出先: " << selected.root.native() << L"\n"
            << L"ADK Deployment Tools: "
            << (selected.deployment_tools_version.empty()
                    ? L"版を確認できません"
                    : selected.deployment_tools_version)
            << L"\n";
  } else {
    details << L"検出先: なし\n";
  }

  details << L"BIOS/UEFI用の基本構成: "
          << ready_text(view.base_layout_ready) << L"\n"
          << L"UEFI 2023 CA用 /bootex: "
          << ready_text(view.bootex_layout_ready) << L"\n"
          << L"検証済みバージョンと必須更新: "
          << ready_text(view.media_creation_permitted) << L"\n"
          << L"メディア作成ゲート: "
          << (view.media_creation_permitted ? L"許可" : L"停止")
          << L"\n\nSecure Bootの実起動可否は、対象PCの"
             L"ファームウェア証明書と失効状態も含めて"
             L"VM・実機で最終確認します。";
  append_diagnostics(details, report);
  view.details = details.str();

  std::wostringstream screen_details;
  screen_details
      << L"BIOS/UEFI用の基本構成: "
      << ready_text(view.base_layout_ready) << L"\n\n"
      << L"UEFI 2023 CA用 /bootex: "
      << ready_text(view.bootex_layout_ready) << L"\n\n"
      << L"検証済みバージョンと必須更新: "
      << ready_text(view.media_creation_permitted) << L"\n\n"
      << L"メディア作成ゲート: "
      << (view.media_creation_permitted ? L"許可" : L"停止")
      << L"\n\n詳細とSecure Bootに関する注意事項は、"
         L"診断結果ダイアログに表示しました。";
  view.screen_details = screen_details.str();
  return view;
}

MediaPreflightView inspect_local_windows_media_environment() {
  auto environment = mediabuilder::make_windows_adk_environment();
  return summarize_media_preflight(
      mediabuilder::detect_windows_adk(*environment, L"amd64"));
}

}  // namespace ytec::windowsapp

#include "ytec/winpeapp/winre_diagnostic_view.h"

#include "ytec/bootrepair/mbr2gpt_layout.h"

#include <array>
#include <cstdint>
#include <string>

namespace ytec::winpeapp {
namespace {

std::wstring format_size(const std::uint64_t bytes) {
  constexpr std::array<const wchar_t*, 5> kUnits{
      L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1U < kUnits.size()) {
    value /= 1024.0;
    ++unit;
  }
  wchar_t buffer[64]{};
  if (unit == 0U) {
    swprintf_s(buffer, L"%llu %s", bytes, kUnits[unit]);
  } else {
    swprintf_s(buffer, L"%.1f %s", value, kUnits[unit]);
  }
  return buffer;
}

bool evidence_is_conclusive(
    const bootrepair::WinReDiagnosticReport& report) {
  return report.microsoft_signature_verified &&
      report.read_only_command && report.exit_code == 0U &&
      report.source_state != bootrepair::WinReSourceState::unknown;
}

}  // namespace

WinReDiagnosticView build_winre_diagnostic_view(
    const bootrepair::WinReDiagnosticReport& report) {
  WinReDiagnosticView view;
  view.conclusive = evidence_is_conclusive(report);
  if (!view.conclusive) {
    view.headline = L"WinREを安全に確定できませんでした";
    view.details =
        L"Microsoft署名、読取り専用コマンド、終了状態、登録先の"
        L"いずれかを確定できません。\r\n"
        L"この結果から回復パーティションの再利用・補完を判断せず、"
        L"MBR→GPT計画は停止します。";
    return view;
  }

  if (report.source_state ==
      bootrepair::WinReSourceState::registered_partition) {
    view.recovery_source_available =
        report.registered_location_reported &&
        report.registered_location_matches_expected_disk &&
        report.registered_image_present &&
        report.registered_partition_number != 0U &&
        report.winre_image_size_bytes != 0U;
    if (!view.recovery_source_available) {
      view.conclusive = false;
      view.headline = L"登録済みWinREの照合が不完全です";
      view.details =
          L"登録先、対象ディスク、Winre.wimのすべてを一致確認できません。\r\n"
          L"既存の回復パーティションは再利用せず、MBR→GPT計画は停止します。";
      return view;
    }
    view.headline = L"登録済みWinREを読み取り確認しました";
    view.details =
        L"・Microsoft署名済みREAgentC: 確認済み\r\n"
        L"・実行内容: /info のみ（変更なし）\r\n"
        L"・登録先: 対象ディスクのパーティション " +
        std::to_wstring(report.registered_partition_number) +
        L"\r\n・Winre.wim: " +
        format_size(report.winre_image_size_bytes) +
        L"\r\n\r\n将来の別ディスクMBR→GPT再構築では、"
        L"この回復環境を検証付きコピー候補として扱えます。"
        L"この診断だけでは変換や書込みを開始しません。";
    return view;
  }

  if (report.source_state ==
      bootrepair::WinReSourceState::image_available_in_windows) {
    view.recovery_source_available =
        report.fallback_image_present &&
        report.winre_image_size_bytes != 0U;
    if (!view.recovery_source_available) {
      view.conclusive = false;
      view.headline = L"Windows内のWinRE候補を確認できません";
      view.details =
          L"回復パーティションを補完する根拠となるWinre.wimを"
          L"通常ファイルとして読み取り確認できません。\r\n"
          L"MBR→GPT計画は停止します。";
      return view;
    }
    view.headline = L"回復パーティションを補完できるWinRE候補があります";
    view.details =
        L"・Microsoft署名済みREAgentC: 確認済み\r\n"
        L"・実行内容: /info のみ（変更なし）\r\n"
        L"・Windows内のWinre.wim: " +
        format_size(report.winre_image_size_bytes) +
        L"\r\n\r\n将来の別ディスクMBR→GPT再構築では、"
        L"新しい回復パーティションを作成して配置する候補になります。"
        L"この診断だけでは変換や書込みを開始しません。";
    return view;
  }

  view.headline = L"利用できるWinREイメージが見つかりません";
  view.details =
      L"診断は正常に完了しましたが、登録済み回復領域にもWindows内にも"
      L"Winre.wimを確認できませんでした。\r\n"
      L"回復ツールを含むMBR→GPT再構築は開始せず、"
      L"WinREの復旧または別の正規ソース確認が必要です。";
  return view;
}

}  // namespace ytec::winpeapp

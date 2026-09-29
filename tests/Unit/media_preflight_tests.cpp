#include "ytec/windowsapp/media_preflight.h"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestFailure final {
  std::string message;
};

void check(bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

ytec::mediabuilder::AdkDiscoveryReport make_report(
    bool layout_ready,
    bool bootex_ready,
    bool creation_permitted) {
  ytec::mediabuilder::AdkDiscoveryReport report;
  report.architecture = L"amd64";
  ytec::mediabuilder::AdkCandidateReport candidate;
  candidate.root = L"C:\\MockADK";
  candidate.deployment_tools_version = L"10.1.26100.2454";
  candidate.base_layout_ready = layout_ready;
  candidate.bootex_layout_ready = bootex_ready;
  candidate.media_creation_permitted = creation_permitted;
  report.candidates.push_back(std::move(candidate));
  if (layout_ready) {
    report.selected_candidate_index = 0U;
  }
  return report;
}

void test_ready_report_is_clear_and_permitted() {
  auto report = make_report(true, true, true);
  ytec::mediabuilder::AdkCandidateReport unused_candidate;
  unused_candidate.root = L"C:\\UnusedADK";
  unused_candidate.diagnostics.push_back(
      ytec::mediabuilder::AdkDiagnostic{
          .severity =
              ytec::mediabuilder::DiagnosticSeverity::error,
          .code = "ADK_ROOT_NOT_FOUND",
          .path = unused_candidate.root,
          .message = L"使用しない候補が見つかりません。",
          .native_code = 2U,
      });
  report.candidates.push_back(std::move(unused_candidate));
  const auto view = ytec::windowsapp::summarize_media_preflight(
      report);
  check(view.base_layout_ready, "Base layout should be ready");
  check(view.bootex_layout_ready, "Bootex layout should be ready");
  check(
      view.media_creation_permitted,
      "Verified report should permit media creation");
  check(
      view.screen_details.find(
          L"BIOS/UEFI用の基本構成: 確認済み") !=
          std::wstring::npos,
      "Summary should state BIOS/UEFI readiness");
  check(
      view.screen_details.find(
          L"UEFI 2023 CA用 /bootex: 確認済み") !=
          std::wstring::npos,
      "Summary should state 2023 CA readiness");
  check(
      view.details.find(L"ADK_ROOT_NOT_FOUND") == std::wstring::npos,
      "Unused candidate errors should not pollute a successful result");
}

void test_version_gate_blocks_even_when_layout_exists() {
  const auto view = ytec::windowsapp::summarize_media_preflight(
      make_report(true, true, false));
  check(view.base_layout_ready, "Base layout should remain visible");
  check(
      !view.media_creation_permitted,
      "Unverified servicing must block media creation");
  check(
      view.status.find(L"許可できません") != std::wstring::npos,
      "Blocked status should be explicit");
  check(
      view.screen_details.find(L"メディア作成ゲート: 停止") !=
          std::wstring::npos,
      "Blocked gate should be explicit");
}

void test_missing_environment_surfaces_diagnostic() {
  auto report = make_report(false, false, false);
  report.candidates[0].diagnostics.push_back(
      ytec::mediabuilder::AdkDiagnostic{
          .severity =
              ytec::mediabuilder::DiagnosticSeverity::error,
          .code = "ADK_WINPE_ADDON_MISSING",
          .path = L"C:\\MockADK",
          .message = L"Windows PE Add-onが見つかりません。",
          .native_code = 2U,
      });
  const auto view =
      ytec::windowsapp::summarize_media_preflight(report);
  check(
      !view.base_layout_ready,
      "Missing environment should not be ready");
  check(
      view.status.find(L"確認できませんでした") !=
          std::wstring::npos,
      "Missing environment status should be explicit");
  check(
      view.details.find(L"ADK_WINPE_ADDON_MISSING") !=
          std::wstring::npos,
      "Actionable diagnostic code should be shown");
  check(
      view.details.find(L"実機で最終確認") != std::wstring::npos,
      "Secure Boot must not be overclaimed");
}

}  // namespace

int main() {
  try {
    test_ready_report_is_clear_and_permitted();
    test_version_gate_blocks_even_when_layout_exists();
    test_missing_environment_surfaces_diagnostic();
    std::cout << "media preflight tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "media preflight tests: FAIL: "
              << failure.message << '\n';
    return 1;
  }
}

#include "ytec/mediabuilder/adk_detection.h"

#include <iostream>

int main() {
  auto environment = ytec::mediabuilder::make_windows_adk_environment();
  const auto roots = ytec::mediabuilder::windows_adk_candidate_roots();
  if (roots.empty()) {
    std::cerr << "FAIL: no bounded Windows ADK candidate roots were produced\n";
    return 1;
  }

  const auto report = ytec::mediabuilder::detect_windows_adk(*environment);
  if (report.candidates.size() != roots.size()) {
    std::cerr << "FAIL: not all bounded ADK candidates were inspected\n";
    return 1;
  }

  if (report.selected_candidate_index) {
    if (*report.selected_candidate_index >= report.candidates.size() ||
        !report.base_layout_ready()) {
      std::cerr << "FAIL: selected ADK candidate is not ready\n";
      return 1;
    }
    std::cout << "PASS: installed ADK/WinPE Add-on passed safe detection\n";
    return 0;
  }

  for (const auto& candidate : report.candidates) {
    if (candidate.base_layout_ready) {
      std::cerr << "FAIL: ready candidate was not selected\n";
      return 1;
    }
    bool has_error = false;
    for (const auto& diagnostic : candidate.diagnostics) {
      if (diagnostic.severity ==
          ytec::mediabuilder::DiagnosticSeverity::error) {
        has_error = true;
        break;
      }
    }
    if (!has_error) {
      std::cerr << "FAIL: incomplete ADK candidate lacks an error diagnostic\n";
      return 1;
    }
  }

  std::cout << "PASS: absent/incomplete ADK failed safely without installation\n";
  return 0;
}

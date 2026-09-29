#pragma once

#include "ytec/bootrepair/winre_diagnostic.h"

#include <string>

namespace ytec::winpeapp {

struct WinReDiagnosticView final {
  bool conclusive{};
  bool recovery_source_available{};
  std::wstring headline;
  std::wstring details;
};

// Converts the read-only WinRE evidence into product-facing text. This view
// never enables MBR-to-GPT execution; it only states whether a future
// separate-target rebuild plan has a verified recovery source.
[[nodiscard]] WinReDiagnosticView build_winre_diagnostic_view(
    const bootrepair::WinReDiagnosticReport& report);

}  // namespace ytec::winpeapp

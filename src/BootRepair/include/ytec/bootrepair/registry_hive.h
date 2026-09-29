#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <string>

namespace ytec::bootrepair {

struct WindowsCurrentVersionValues final {
  std::uint32_t major{};
  std::wstring build;
  std::wstring installation_type;
};

// Reads only these three values from an untrusted standalone SOFTWARE hive:
// CurrentMajorVersionNumber, CurrentBuildNumber, and InstallationType. The
// file is opened GENERIC_READ without write/delete sharing. All hive bins,
// cells, indices, lengths, and value types reached by the fixed path are
// bounds checked. Transaction logs are never replayed and no bytes are saved.
[[nodiscard]] clonecore::Result<WindowsCurrentVersionValues>
read_windows_current_version_from_hive(const std::wstring& hive_path);

}  // namespace ytec::bootrepair

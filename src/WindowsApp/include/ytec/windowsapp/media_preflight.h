#pragma once

#include "ytec/mediabuilder/adk_detection.h"

#include <string>

namespace ytec::windowsapp {

struct MediaPreflightView final {
  bool base_layout_ready{};
  bool bootex_layout_ready{};
  bool media_creation_permitted{};
  std::wstring status;
  std::wstring screen_details;
  std::wstring details;
};

[[nodiscard]] MediaPreflightView summarize_media_preflight(
    const mediabuilder::AdkDiscoveryReport& report);

[[nodiscard]] MediaPreflightView
inspect_local_windows_media_environment();

}  // namespace ytec::windowsapp

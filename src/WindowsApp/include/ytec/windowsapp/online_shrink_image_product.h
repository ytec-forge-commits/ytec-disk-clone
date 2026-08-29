#pragma once

#include "ytec/windowsapp/online_shrink_image_plan.h"

#include <optional>
#include <string>
#include <string_view>

namespace ytec::windowsapp {

struct WindowsOnlineShrinkImageProductRequest final {
  diskmodel::DiskInfo selected_source;
  std::wstring final_path;
  bool administrator{};
  std::uint32_t windows_major{};
  std::uint32_t windows_minor{};
  std::uint32_t windows_build{};
  std::string windows_architecture;
  std::string created_utc;
  std::string app_version;
  std::optional<std::string_view> encryption_password;
  imageformat::TsumugiCreateVerificationMode verification_mode{
      imageformat::TsumugiCreateVerificationMode::complete};
  bool replace_existing{};
  vssrequester::AsyncWaitOptions async_wait;
  clonecore::DiskOperationCallbacks callbacks;
  vssrequester::VssDiffAreaReviewCallback diff_area_review_callback;
  const clonecore::Logger* logger{};
  // The active product log is either one proved-disjoint local file or the
  // process-wide bounded RAM router. No AppData fallback is inferred here.
  std::wstring persistent_log_path;
  bool log_is_ram_only{};
};

// Shared read-only observer for shrink creation and shrink restore work
// artifacts. It canonicalizes every path, rejects reparse ancestors, maps the
// path to one physical disk before/after inventory, and returns stable
// identities without creating a file.
[[nodiscard]] clonecore::Result<WindowsShrinkWorkPlacementObservation>
observe_windows_shrink_work_placement_with_windows_apis(
    const WindowsShrinkWorkPaths& paths);

// Product adapter for the audited Windows APIs. It performs a read-only source
// re-identification and analysis, then delegates all VSS/capture/container
// lifecycle rules to execute_windows_online_shrink_image_create().
[[nodiscard]] clonecore::Result<vssrequester::OnlineTsumugiBackupReport>
execute_windows_online_shrink_image_create_with_windows_apis(
    const WindowsOnlineShrinkImageProductRequest& request);

}  // namespace ytec::windowsapp

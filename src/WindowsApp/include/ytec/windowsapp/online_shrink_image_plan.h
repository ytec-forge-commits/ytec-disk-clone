#pragma once

#include "ytec/windowsapp/windows_ntfs_shrink_capture.h"
#include "ytec/windowsshrink/source_analysis.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ytec::windowsapp {

struct WindowsOnlineShrinkImagePlanRequest final {
  windowsshrink::ShrinkSourceAnalysis analysis;
  std::wstring final_path;
  imageformat::TsumugiImageStorageFileSystem storage_file_system{
      imageformat::TsumugiImageStorageFileSystem::unknown};
  bool administrator{};
  std::optional<std::string_view> encryption_password;
  imageformat::TsumugiCreateVerificationMode verification_mode{
      imageformat::TsumugiCreateVerificationMode::complete};
  bool replace_existing{};
  std::uint32_t chunk_size{imageformat::kImageChunkSize16MiB};
};

struct WindowsOnlineShrinkImagePlan final {
  vssrequester::WorkflowRequest workflow;
  imageformat::TsumugiImageCreateRequest image_template;
  WindowsNtfsShrinkCapturePlan capture;
};

// Pure conversion from one read-only, stable source observation to every
// immutable input consumed by the VSS shrink controller.  It performs no VSS,
// filesystem, destination, or physical-disk I/O.
[[nodiscard]] clonecore::Result<WindowsOnlineShrinkImagePlan>
plan_windows_online_shrink_image(
    const WindowsOnlineShrinkImagePlanRequest& request);

}  // namespace ytec::windowsapp

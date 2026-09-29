#pragma once

#include "ytec/windowsapp/first_run_guidance.h"

namespace ytec::windowsapp::detail {

// Private real-Win32 seam used by synthetic tests. Product UI must use only
// the current-executable wrappers from the public header.
using FirstRunGuidanceBeforeReplaceHook = void (*)(void*) noexcept;

[[nodiscard]] FirstRunGuidanceInspection
inspect_first_run_guidance_in_existing_data_directory(
    const std::wstring& data_directory) noexcept;

[[nodiscard]] clonecore::Result<FirstRunGuidanceSaveReport>
save_first_run_guidance_in_existing_data_directory(
    const std::wstring& data_directory,
    FirstRunGuidanceBeforeReplaceHook before_replace = nullptr,
    void* hook_context = nullptr) noexcept;

}  // namespace ytec::windowsapp::detail

#pragma once

#include "ytec/windowsapp/windows_adk_eula_extractor.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>

namespace ytec::windowsapp::detail {

// source_handle is an already identity-locked, single-link, non-reparse file
// owned by WindowsAdkAcquisitionPlatform. The platform retains the stage-root
// handle and source handle without delete/write sharing for this entire call.
[[nodiscard]] clonecore::Result<WindowsAdkEulaExtractionResult>
extract_windows_adk_eula_from_verified_owned_handle(
    HANDLE source_handle,
    const std::filesystem::path& owned_stage_root,
    const AdkPinnedPayload& pinned_bootstrap,
    const AdkVerifiedPayload& verified_bootstrap,
    const AdkEmbeddedEulaPin& pin);

}  // namespace ytec::windowsapp::detail

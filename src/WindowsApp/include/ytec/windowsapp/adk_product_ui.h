#pragma once

#include "ytec/windowsapp/adk_consent_review.h"
#include "ytec/windowsapp/adk_management.h"

#include <Windows.h>

#include <filesystem>
#include <string_view>

namespace ytec::windowsapp {

// Opens a user-initiated filesystem folder picker. The returned path has not
// been read; the caller must pass it through the action-specific pure/core
// validation before any platform operation.
[[nodiscard]] clonecore::Result<std::filesystem::path>
select_adk_product_folder(
    HWND owner,
    std::wstring_view title);

// Presents the complete verified in-memory RTF in a read-only RichEdit.
// Acceptance is disabled until the document end has been observed and the
// dedicated checkbox is checked. The RTF is never written to disk.
[[nodiscard]] clonecore::Result<AdkConsentReviewAcknowledgement>
show_adk_product_consent_dialog(
    HWND owner,
    const AdkReleaseManifest& manifest,
    const AdkVerifiedEulaDocument& document);

}  // namespace ytec::windowsapp

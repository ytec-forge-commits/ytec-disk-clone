#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/restore_image_inspection.h"

#include <string>

namespace ytec::migrationengine {

// Fully verifies a manifest.dcmig directory bundle and exposes the same
// read-only summary shape used by normal .dcimg restore preflight.
[[nodiscard]] clonecore::Result<imageformat::RestoreImageInspectionReport>
inspect_shrink_bundle_for_restore(const std::wstring& manifest_path);

}  // namespace ytec::migrationengine

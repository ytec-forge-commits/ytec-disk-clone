#pragma once

#include "ytec/imageformat/shrink_image_manifest.h"
#include "ytec/windowsshrink/source_analysis.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ytec::migrationengine {

using WindowsSourceVersion = windowsshrink::WindowsSourceVersion;
using ShrinkSourceAnalysisContext = windowsshrink::ShrinkSourceAnalysisContext;

// Compatibility-only view for the quarantined .dcmig implementation.  The
// released product paths use windowsshrink::ShrinkSourceAnalysis directly.
struct AnalyzedShrinkVolume final {
  std::uint32_t source_table_index{};
  std::wstring volume_guid_path;
  std::string payload_file_name;
};

struct ShrinkSourceAnalysis final {
  imageformat::ShrinkImageManifest manifest;
  std::vector<AnalyzedShrinkVolume> content_volumes;
};

[[nodiscard]] clonecore::Result<ShrinkSourceAnalysis>
analyze_gpt_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::GptDisk& layout,
    const ShrinkSourceAnalysisContext& context);

[[nodiscard]] clonecore::Result<ShrinkSourceAnalysis>
analyze_mbr_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::MbrDisk& layout,
    const ShrinkSourceAnalysisContext& context);

}  // namespace ytec::migrationengine

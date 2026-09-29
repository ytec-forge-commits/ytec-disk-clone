#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/migrationengine/source_analysis.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ytec::migrationengine {

struct ShrinkCaptureSource final {
  std::uint32_t source_table_index{};
  std::wstring capture_root;
};

struct ShrinkBundleCaptureRequest final {
  ShrinkSourceAnalysis analysis;
  std::vector<ShrinkCaptureSource> capture_sources;
  std::wstring final_bundle_directory;
  std::wstring scratch_directory;
  clonecore::DiskOperationCallbacks callbacks;
};

struct ShrinkBundleCaptureReport final {
  std::wstring bundle_directory;
  std::wstring manifest_path;
  std::uint64_t total_payload_bytes{};
  std::uint32_t captured_volume_count{};
  imageformat::Sha256Digest manifest_sha256{};
  bool committed_after_complete_verification{};
};

// Captures each supplied read-only volume to a separate WIM in a sibling
// staging directory. The final .dcmig directory is created by one no-replace
// rename only after every payload and the manifest verify successfully.
[[nodiscard]] clonecore::Result<ShrinkBundleCaptureReport>
capture_shrink_bundle(
    const ShrinkBundleCaptureRequest& request,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner);

[[nodiscard]] clonecore::Result<ShrinkBundleCaptureReport>
capture_shrink_bundle_with_windows_apis(
    const ShrinkBundleCaptureRequest& request);

}  // namespace ytec::migrationengine

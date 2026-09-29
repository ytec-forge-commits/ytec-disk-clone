#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/shrink_image_manifest.h"

#include <string>
#include <vector>

namespace ytec::migrationengine {

inline constexpr wchar_t kShrinkBundleManifestFileName[] = L"manifest.dcmig";

struct VerifiedShrinkPayload final {
  std::string file_name;
  std::wstring absolute_path;
  std::uint64_t length_bytes{};
  imageformat::Sha256Digest sha256{};
  clonecore::UniqueHandle locked_read_handle;
};

struct LockedFileHash final {
  std::wstring absolute_path;
  std::uint64_t length_bytes{};
  imageformat::Sha256Digest sha256{};
  clonecore::UniqueHandle locked_read_handle;
};

// Keeps the manifest, payload files, and bundle directory locked against
// replacement for the lifetime of the report. It is intentionally movable
// but not copyable through UniqueHandle.
struct VerifiedShrinkBundle final {
  std::wstring bundle_directory;
  std::wstring manifest_path;
  imageformat::ShrinkImageManifest manifest;
  std::uint64_t manifest_length_bytes{};
  imageformat::Sha256Digest manifest_sha256{};
  std::vector<VerifiedShrinkPayload> payloads;
  clonecore::UniqueHandle locked_manifest_handle;
  clonecore::UniqueHandle locked_directory_handle;
};

// The input is untrusted. Requires a drive-letter local path named
// manifest.dcmig in a reparse-free .dcmig directory on one physical disk,
// rejects undeclared entries, and validates all WIM lengths and SHA-256 values
// before succeeding.
[[nodiscard]] clonecore::Result<VerifiedShrinkBundle>
verify_shrink_bundle_read_only(const std::wstring& manifest_path);

// Opens a regular non-reparse file without write/delete sharing, then hashes
// the same held handle. Callers can retain the returned handle across a
// security-sensitive transition.
[[nodiscard]] clonecore::Result<LockedFileHash>
hash_regular_file_locked_read_only(const std::wstring& absolute_path);

}  // namespace ytec::migrationengine

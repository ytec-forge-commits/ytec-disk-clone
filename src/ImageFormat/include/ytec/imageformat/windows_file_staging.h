#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/windows_tsumugi_destination.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::imageformat {

struct WindowsFileStagingRequest final {
  std::wstring final_path;
  clonecore::StableDiskIdentity expected_source_disk;
  std::optional<clonecore::StableDiskIdentity> expected_clone_target_disk;
};

// Temporary legacy source compatibility. New .tsumugi code must include
// windows_tsumugi_destination.h and use WindowsImageDestinationObservation.
using WindowsFileDestinationObservation =
    WindowsImageDestinationObservation;

// Test seam for file and environment failures. Product code must use
// make_windows_file_staging_target(), which installs the audited Win32 backend.
class IWindowsFileStagingBackend {
 public:
  virtual ~IWindowsFileStagingBackend() = default;

  [[nodiscard]] virtual clonecore::Result<
      WindowsFileDestinationObservation>
  observe_destination(const std::wstring& requested_final_path) = 0;

  [[nodiscard]] virtual clonecore::Status create_new_restricted_partial(
      const std::wstring& partial_path,
      std::uint64_t expected_length) = 0;
  [[nodiscard]] virtual bool owns_partial() const noexcept = 0;

  [[nodiscard]] virtual clonecore::Status write_at(
      std::uint64_t offset,
      std::span<const std::byte> bytes) = 0;
  [[nodiscard]] virtual clonecore::Status resize_before_verification(
      std::uint64_t final_length) = 0;
  [[nodiscard]] virtual clonecore::Result<std::vector<std::byte>> read_at(
      std::uint64_t offset,
      std::size_t length) = 0;
  [[nodiscard]] virtual clonecore::Status flush() = 0;
  [[nodiscard]] virtual clonecore::Status close_file() = 0;

  // Must fail when the final path already exists. Replacement is forbidden.
  [[nodiscard]] virtual clonecore::Status commit_no_replace(
      const std::wstring& partial_path,
      const std::wstring& final_path) = 0;
  [[nodiscard]] virtual clonecore::Status remove_owned_partial(
      const std::wstring& partial_path) = 0;
};

// Creates an inert target. No file is created until IDcimgStagingTarget::begin.
// begin re-enumerates disk identity, verifies free space/path boundaries, and
// creates only "<final>.partial" with CREATE_NEW and a protected DACL.
[[nodiscard]] clonecore::Result<std::unique_ptr<IDcimgStagingTarget>>
make_windows_file_staging_target(const WindowsFileStagingRequest& request);

// Mock-only construction seam. It applies the same orchestration and identity
// gates, but the supplied backend performs all observations and I/O.
[[nodiscard]] clonecore::Result<std::unique_ptr<IDcimgStagingTarget>>
make_windows_file_staging_target_with_backend(
    const WindowsFileStagingRequest& request,
    std::unique_ptr<IWindowsFileStagingBackend> backend);

}  // namespace ytec::imageformat

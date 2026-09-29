#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/tsumugi_image_service.h"
#include "ytec/imageformat/windows_tsumugi_destination.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ytec::imageformat {

struct WindowsTsumugiRescueStagingRequest final {
  // The staging file is derived as
  // "<canonical final_path>.rescue-stage.partial". It is never a caller-
  // supplied arbitrary cleanup path.
  std::wstring final_path;
  clonecore::StableDiskIdentity expected_source_disk;
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  Sha256Digest source_model_hash{};
  Sha256Digest source_serial_hash{};
  Sha256Digest source_state_hash{};
  // Must cover both the full raw staging file and the caller's conservative
  // upper bound for the final image. This prevents a late capacity failure
  // after repeatedly reading a failing source.
  std::uint64_t required_available_bytes{};
  bool replace_existing{};
};

// Test seam for destination changes and file failures. Product code must use
// make_windows_tsumugi_rescue_staging_session(), which installs the audited
// Win32 backend. The backend owns one CREATE_NEW file by stable handle ID and
// must never delete a path that no longer identifies that exact object.
class IWindowsTsumugiRescueStagingBackend {
 public:
  virtual ~IWindowsTsumugiRescueStagingBackend() = default;

  [[nodiscard]] virtual clonecore::Result<
      WindowsImageDestinationObservation>
  observe_destination(const std::wstring& final_path) = 0;

  [[nodiscard]] virtual clonecore::Status create_new_owned_staging(
      const std::wstring& staging_path,
      std::uint64_t expected_length) = 0;
  [[nodiscard]] virtual bool owns_staging() const noexcept = 0;
  [[nodiscard]] virtual bool sealed_for_read() const noexcept = 0;
  [[nodiscard]] virtual clonecore::Status write_at(
      std::uint64_t offset,
      std::span<const std::byte> bytes) = 0;
  [[nodiscard]] virtual clonecore::Result<std::vector<std::byte>> read_at(
      std::uint64_t offset,
      std::size_t length) const = 0;
  [[nodiscard]] virtual clonecore::Status flush() = 0;
  [[nodiscard]] virtual clonecore::Status seal_read_only(
      const std::wstring& staging_path,
      std::uint64_t expected_length) = 0;
  [[nodiscard]] virtual clonecore::Status discard_exact_owned_staging() = 0;
};

// Validates source/destination separation and total capacity before creating
// one adjacent owned staging file. The returned session is writable only until
// seal_for_image_read(), and readable as an image source only after sealing.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<ITsumugiRescueStagingSession>>
make_windows_tsumugi_rescue_staging_session(
    const WindowsTsumugiRescueStagingRequest& request);

// Mock-only construction seam. It applies the same orchestration and identity
// gates while the supplied backend performs observations and I/O.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<ITsumugiRescueStagingSession>>
make_windows_tsumugi_rescue_staging_session_with_backend(
    const WindowsTsumugiRescueStagingRequest& request,
    std::unique_ptr<IWindowsTsumugiRescueStagingBackend> backend);

}  // namespace ytec::imageformat

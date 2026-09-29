#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::winpeapp {

inline constexpr std::wstring_view kActiveRescueMediaRuntimeMarkerPath =
    L"X:\\YtecDiskClone\\rescue-media-id.txt";
inline constexpr std::wstring_view kActiveRescueMediaMarkerRelativePath =
    L"YtecDiskClone\\rescue-media-id.txt";

// All dependencies are read-only. Marker absence is represented by nullopt;
// an unreadable, malformed, or uncertain candidate must be returned as an
// error so the resolver can fail closed.
struct ActiveRescueMediaDependencies final {
  std::function<clonecore::Result<std::optional<std::string>>(
      const std::wstring&)>
      read_marker;
  std::function<clonecore::Result<std::vector<std::wstring>>()>
      enumerate_local_drive_roots;
  std::function<clonecore::Result<std::uint32_t>(const std::wstring&)>
      query_local_drive_type;
  std::function<clonecore::Result<std::uint32_t>(const std::wstring&)>
      query_single_disk_number_for_path;
  std::function<clonecore::Result<diskmodel::InventoryReport>()>
      enumerate_disks;
};

// Freshly resolved storage that owns the WinPE runtime marker. marker_path is
// the unique local-root marker (never the X: runtime copy). A CD-ROM has no
// physical identity; fixed/removable media must have one. The boolean records
// that the marker identity was obtained from the dependency's opened-handle
// read contract rather than from path attributes alone.
struct ActiveRescueMediaStorageObservation final {
  std::wstring marker_path;
  std::uint32_t drive_type{};
  std::optional<clonecore::StableDiskIdentity> physical_identity;
  bool marker_identity_from_open_handle{};
};

// Resolves the one local fixed/removable/CD-ROM root whose normal,
// non-reparse marker matches the 36-byte runtime marker embedded in X:. The
// marker and drive type are re-observed before any physical identity is
// returned. This function is read-only and fails closed on absence, drift, or
// ambiguity.
[[nodiscard]] clonecore::Result<ActiveRescueMediaStorageObservation>
resolve_active_rescue_media_storage(
    const ActiveRescueMediaDependencies& dependencies);

// Resolves the currently booted Y-TEC rescue medium and compares its freshly
// observed stable identity with the selected target. The confirmation object
// is accepted only to match the destructive controller callback signature; it
// is never interpreted and this function performs no write or state change.
[[nodiscard]] clonecore::Result<bool> resolve_active_rescue_media_target(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    const ActiveRescueMediaDependencies& dependencies);

[[nodiscard]] ActiveRescueMediaDependencies
make_active_rescue_media_windows_dependencies();

[[nodiscard]] clonecore::Result<ActiveRescueMediaStorageObservation>
query_active_rescue_media_storage_with_windows_apis();

[[nodiscard]] clonecore::Result<bool>
query_active_rescue_media_target_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation);

}  // namespace ytec::winpeapp

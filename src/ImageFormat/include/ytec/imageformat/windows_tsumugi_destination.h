#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::imageformat {

enum class WindowsImageDestinationFileSystem : std::uint8_t {
  unknown = 0U,
  ntfs = 1U,
  exfat = 2U,
  other = 3U,
};

// Read-only identity obtained from a handle opened with
// FILE_FLAG_OPEN_REPARSE_POINT. It is observation evidence only; the native
// .tsumugi writer independently retains and rechecks its owned file handle.
struct WindowsImagePathIdentity final {
  std::uint64_t volume_serial_number{};
  std::array<std::byte, 16> file_id{};
  std::uint64_t file_size{};
};

struct WindowsImageDestinationObservation final {
  std::wstring canonical_final_path;
  std::wstring partial_path;
  clonecore::StableDiskIdentity destination_disk;
  std::vector<clonecore::StableDiskIdentity> connected_disks;
  std::uint64_t available_bytes{};
  std::uint32_t physical_disk_extent_count{};
  WindowsImageDestinationFileSystem file_system{
      WindowsImageDestinationFileSystem::unknown};
  // Product observation checks every existing ancestor, not only the direct
  // parent. The flag exists so focused tests cannot bypass that requirement.
  bool parent_is_reparse{};
  bool final_exists{};
  bool partial_exists{};
  std::optional<WindowsImagePathIdentity> parent_identity;
  std::optional<WindowsImagePathIdentity> final_identity;
  std::optional<WindowsImagePathIdentity> partial_identity;
};

enum class WindowsTsumugiDestinationGuardPhase : std::uint8_t {
  before_stage = 1U,
  before_commit_owned_partial = 2U,
};

struct WindowsTsumugiDestinationGuardRequest final {
  std::wstring final_path;
  clonecore::StableDiskIdentity expected_source_disk;
  std::uint64_t required_available_bytes{};
  bool replace_existing{};
  WindowsTsumugiDestinationGuardPhase phase{
      WindowsTsumugiDestinationGuardPhase::before_stage};
  // Required only for before_commit_owned_partial. The guard reopens the
  // adjacent regular file and requires this exact observed length. The native
  // writer separately compares its stable file ID before rename or cleanup.
  std::uint64_t expected_owned_partial_bytes{};
};

// Product guard for a single local .tsumugi destination. It canonicalizes the
// path, rejects every reparse ancestor, requires one physical-disk extent on
// NTFS/exFAT, reidentifies source and destination, and reopens parent/final/
// partial objects without following reparses. Invoke before staging and again
// immediately before TsumugiStagedImageV1::commit_verified().
[[nodiscard]] clonecore::Status validate_windows_tsumugi_destination(
    const WindowsTsumugiDestinationGuardRequest& request);

// Read-only product observation used by Windows file adapters that must retain
// and compare the exact parent and destination identities across their own
// handle lifecycle. The returned fields are still untrusted until passed to a
// matching validator.
[[nodiscard]] clonecore::Result<WindowsImageDestinationObservation>
observe_windows_tsumugi_destination(const std::wstring& final_path);

// Pure validation seam. All fields are treated as untrusted observations.
[[nodiscard]] clonecore::Status
validate_windows_tsumugi_destination_observation(
    const WindowsTsumugiDestinationGuardRequest& request,
    const WindowsImageDestinationObservation& observation);

}  // namespace ytec::imageformat

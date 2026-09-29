#pragma once

#include "ytec/clonecore/disk_identity.h"

#include <functional>
#include <string>

namespace ytec::windowsapp {

struct WindowsShrinkWorkPaths final {
  std::wstring scratch_directory;
  std::wstring checkpoint_path;
  std::wstring log_path;
  // The process-wide bounded RAM logger has no filesystem artifact. When
  // true log_path must remain empty and no fabricated path is observed.
  bool log_is_ram_only{};
};

struct WindowsShrinkWorkPathObservation final {
  std::wstring canonical_path;
  clonecore::StableDiskIdentity backing_disk;
  bool local_volume{};
  bool parent_is_reparse{};
};

struct WindowsShrinkWorkPlacementObservation final {
  WindowsShrinkWorkPathObservation scratch;
  WindowsShrinkWorkPathObservation checkpoint;
  WindowsShrinkWorkPathObservation log;
};

using WindowsShrinkWorkPlacementObserver = std::function<clonecore::Result<
    WindowsShrinkWorkPlacementObservation>(const WindowsShrinkWorkPaths&)>;

// Pure safety boundary shared by Windows capture/restore and WinPE restore.
// Every filesystem-backed work artifact must resolve to a local, non-reparse
// location on a disk other than protected_source. The caller supplies a fresh
// physical-disk observation; this function performs no I/O.
[[nodiscard]] clonecore::Status
validate_windows_shrink_work_placement_observation(
    const clonecore::StableDiskIdentity& protected_source,
    const WindowsShrinkWorkPaths& requested,
    const WindowsShrinkWorkPlacementObservation& observed);

// Read-only Windows observer shared by creation and restore products. It
// canonicalizes each path, rejects a reparse ancestor, maps it to exactly one
// physical disk on both sides of a full inventory scan, and returns stable
// identities without creating any file or directory.
[[nodiscard]] clonecore::Result<WindowsShrinkWorkPlacementObservation>
observe_windows_shrink_work_placement_with_windows_apis(
    const WindowsShrinkWorkPaths& paths);

}  // namespace ytec::windowsapp

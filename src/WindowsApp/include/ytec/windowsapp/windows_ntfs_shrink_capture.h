#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/windowsapp/online_shrink_image.h"
#include "ytec/windowsdism/dism.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct WindowsNtfsShrinkVolumeSelection final {
  std::uint32_t source_table_index{};
  std::wstring original_volume_guid_path;
};

// Immutable reviewed input used to construct the online capture Adapter.
// The manifest is retained so the Snapshot callback cannot substitute a
// different partition extent after VSS has started.
struct WindowsNtfsShrinkCapturePlan final {
  clonecore::StableDiskIdentity source_disk;
  imageformat::TsumugiManifest reviewed_manifest;
  std::vector<WindowsNtfsShrinkVolumeSelection> snapshot_volumes;
};

// Owns one private scratch child and the WIM files created in it. seal_wim()
// must retain a no-write/no-delete-sharing handle until discard_owned(). The
// latter deletes only objects proved to be owned by this instance and must
// leave an unknown replacement untouched.
class IWindowsNtfsShrinkWimStaging {
 public:
  virtual ~IWindowsNtfsShrinkWimStaging() = default;

  [[nodiscard]] virtual clonecore::Result<std::wstring> reserve_wim_path(
      std::uint32_t source_table_index) = 0;
  [[nodiscard]] virtual clonecore::Result<std::uint64_t> seal_wim(
      std::uint32_t source_table_index) = 0;
  [[nodiscard]] virtual clonecore::Result<std::vector<std::byte>> read_wim(
      std::uint32_t source_table_index,
      std::uint64_t offset,
      std::size_t length) const = 0;
  [[nodiscard]] virtual clonecore::Status discard_owned() noexcept = 0;
};

using WindowsNtfsShrinkSourceOpener = std::function<clonecore::Result<
    diskmodel::ReadOnlyPhysicalDiskHandle>(
    const clonecore::StableDiskIdentity&)>;

using WindowsNtfsShrinkVolumeResolver = std::function<clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>(
    const diskmodel::DiskInfo&,
    std::span<const diskmodel::VolumePartitionLocation>)>;

using WindowsNtfsShrinkStagingFactory = std::function<clonecore::Result<
    std::unique_ptr<IWindowsNtfsShrinkWimStaging>>(
    const std::wstring& canonical_scratch_directory)>;

using WindowsNtfsShrinkDismCaptureExecutor = std::function<clonecore::Result<
    windowsdism::DismExecutionReport>(
    const windowsdism::DismCaptureRequest&)>;

struct WindowsNtfsShrinkCaptureDependencies final {
  // Called once inside the active Snapshot callback and once again before the
  // capture session releases its WIM staging. Both calls must freshly
  // re-identify the stable source and return a read-only physical handle.
  WindowsNtfsShrinkSourceOpener open_read_only_source;
  WindowsNtfsShrinkVolumeResolver resolve_snapshot_volumes;
  WindowsNtfsShrinkStagingFactory create_wim_staging;
  // Product binding delegates to execute_dism_capture(), which verifies the
  // fixed System32 dism.exe Authenticode signature before and after launch.
  WindowsNtfsShrinkDismCaptureExecutor capture_wim;
};

// Creates the GUI-independent Snapshot callback Adapter. Validation is pure:
// unsupported filesystems, any BitLocker marker, invalid partition extents,
// and a mismatched Volume GUID plan are rejected here before a VSS workflow or
// any dependency I/O is started by the caller.
[[nodiscard]] clonecore::Result<WindowsShrinkCaptureExecutor>
make_windows_ntfs_shrink_capture_executor(
    WindowsNtfsShrinkCapturePlan plan,
    WindowsNtfsShrinkCaptureDependencies dependencies);

// Product binding using the audited physical-disk re-identification, exact
// Volume extent resolver, private no-reparse scratch staging, and signed
// System32 DISM execution. It does not start VSS or perform capture by itself.
[[nodiscard]] clonecore::Result<WindowsShrinkCaptureExecutor>
make_windows_ntfs_shrink_capture_executor_with_windows_apis(
    WindowsNtfsShrinkCapturePlan plan);

}  // namespace ytec::windowsapp

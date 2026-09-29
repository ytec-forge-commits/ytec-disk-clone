#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/windowsapp/shrink_restore_transaction.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace ytec::windowsapp {

// Everything reviewed before the destructive adapter is constructed.  The
// platform still performs a fresh inventory/layout check in begin().
struct WindowsTsumugiShrinkRestorePlatformRequest final {
  clonecore::StableDiskIdentity expected_target;
  clonecore::TargetConfirmation confirmation;
  imageformat::Sha256Digest expected_target_layout_hash{};
  bool target_is_active_rescue_media{};
  clonecore::DiskOperationCallbacks callbacks;
};

struct WindowsTsumugiShrinkTargetObservation final {
  diskmodel::ReidentifiedPhysicalTarget physical;
  imageformat::TsumugiRestoreDiskIdentity restore_identity;
};

struct WindowsTsumugiShrinkVolumeBinding final {
  std::uint32_t final_target_number{};
  std::uint32_t disk_number{};
  std::uint64_t target_offset{};
  std::uint64_t target_size{};
  std::wstring volume_device_path;
};

// Evidence returned only after reopening the exact bound volume.  An applied
// WIM is accepted only when the complete visible namespace was enumerated and
// every non-reparse ordinary file's unnamed data stream was read in bounded
// blocks through its stable EOF. Reparse targets and alternate data streams
// are not followed by this v1 evidence. The
// authenticated WIM plus DISM /CheckIntegrity /Verify remains the source-side
// integrity proof; these counters are target readback evidence, not a claim
// that manifest v1 contains a canonical filesystem-tree digest.
struct WindowsTsumugiShrinkFileSystemReadbackEvidence final {
  std::uint64_t directory_count{};
  std::uint64_t regular_file_count{};
  std::uint64_t regular_file_bytes_read{};
  std::uint64_t reparse_point_count{};
  bool file_system_metadata_verified{};
  bool namespace_fully_enumerated{};
  bool every_regular_file_read_to_eof{};
};

struct WindowsTsumugiShrinkNtfsExtensionEvidence final {
  std::uint64_t previous_file_system_bytes{};
  std::uint64_t final_file_system_bytes{};
  std::uint64_t final_partition_extent_bytes{};
  bool exact_single_extent_reverified{};
  bool ntfs_sector_count_reverified{};
  bool flushed{};
};

// Narrow Win32 seam.  The production implementation below uses the audited
// DiskModel opener, exact Volume extents, Microsoft VDS/DISM and an owned
// locked WIM. Tests replace only this boundary and never touch a disk.
class IWindowsTsumugiShrinkRestorePlatformIo {
 public:
  virtual ~IWindowsTsumugiShrinkRestorePlatformIo() = default;

  [[nodiscard]] virtual clonecore::Result<
      WindowsTsumugiShrinkTargetObservation>
  observe_original_target(
      const imageformat::Sha256Digest& connection_instance_hash) = 0;
  [[nodiscard]] virtual clonecore::Status validate_work_paths_disjoint(
      const WindowsShrinkWorkPaths& work_paths,
      std::uint32_t current_target_disk_number) = 0;
  [[nodiscard]] virtual clonecore::Status set_target_offline(bool offline) = 0;
  [[nodiscard]] virtual clonecore::Result<diskmodel::PhysicalTargetHandle>
  open_offline_target() = 0;
  [[nodiscard]] virtual clonecore::Status notify_layout_changed() = 0;

  [[nodiscard]] virtual clonecore::Result<
      WindowsTsumugiShrinkVolumeBinding>
  bind_online_volume(
      std::uint32_t final_target_number,
      std::uint64_t target_offset,
      std::uint64_t target_size) = 0;
  [[nodiscard]] virtual clonecore::Status format_volume(
      const WindowsTsumugiShrinkVolumeBinding& volume,
      imageformat::TsumugiManifestFileSystem file_system,
      std::uint64_t cluster_size) = 0;
  [[nodiscard]] virtual clonecore::Result<
      WindowsTsumugiShrinkFileSystemReadbackEvidence>
  verify_volume_readback(
      const WindowsTsumugiShrinkVolumeBinding& volume,
      imageformat::TsumugiManifestFileSystem file_system,
      std::uint64_t cluster_size,
      bool require_complete_applied_content_readback) = 0;
  // Extends only the already-bound NTFS filesystem to its exact, newly
  // published partition extent. Implementations must recheck the one-disk
  // extent and NTFS sector count before and after the exact extension control.
  [[nodiscard]] virtual clonecore::Result<
      WindowsTsumugiShrinkNtfsExtensionEvidence>
  extend_ntfs_volume_to_exact_extent_and_verify(
      const WindowsTsumugiShrinkVolumeBinding& volume,
      std::uint64_t previous_partition_size,
      std::uint64_t cluster_size) = 0;
  [[nodiscard]] virtual clonecore::Status dismount_and_offline_volume(
      const WindowsTsumugiShrinkVolumeBinding& volume) = 0;

  [[nodiscard]] virtual clonecore::Status begin_owned_staged_wim(
      const std::wstring& scratch_directory,
      std::uint32_t source_table_index,
      std::uint64_t expected_length) = 0;
  [[nodiscard]] virtual clonecore::Status append_owned_staged_wim(
      std::uint64_t archive_offset,
      std::uint64_t length,
      bool zero_fill,
      std::span<const std::byte> plaintext) = 0;
  [[nodiscard]] virtual clonecore::Result<imageformat::Sha256Digest>
  verify_and_lock_single_image_wim(
      std::uint32_t source_table_index) = 0;
  [[nodiscard]] virtual clonecore::Status apply_locked_wim(
      const WindowsTsumugiShrinkVolumeBinding& volume,
      const std::wstring& scratch_directory,
      const imageformat::Sha256Digest& locked_hash) = 0;
  [[nodiscard]] virtual clonecore::Status discard_owned_staged_wim() = 0;
};

// Shared production Win32 boundary.  Construction validates the reviewed
// request and allocates helpers only; it does not enumerate, online/offline,
// format, open, or write a disk.  Direct shrink reuses this exact target
// re-identification, VDS/DISM apply and filesystem-readback implementation.
[[nodiscard]] clonecore::Result<std::unique_ptr<
    IWindowsTsumugiShrinkRestorePlatformIo>>
make_windows_tsumugi_shrink_restore_platform_io(
    const WindowsTsumugiShrinkRestorePlatformRequest& request);

// Production Win32 adapter.  Construction itself performs no destructive I/O.
[[nodiscard]] clonecore::Result<std::unique_ptr<
    IWindowsTsumugiShrinkRestorePlatform>>
make_windows_tsumugi_shrink_restore_platform(
    const WindowsTsumugiShrinkRestorePlatformRequest& request);

// Unit-test seam using the same state machine and validation as production.
[[nodiscard]] clonecore::Result<std::unique_ptr<
    IWindowsTsumugiShrinkRestorePlatform>>
make_windows_tsumugi_shrink_restore_platform_with_io(
    const WindowsTsumugiShrinkRestorePlatformRequest& request,
    std::unique_ptr<IWindowsTsumugiShrinkRestorePlatformIo> io);

}  // namespace ytec::windowsapp

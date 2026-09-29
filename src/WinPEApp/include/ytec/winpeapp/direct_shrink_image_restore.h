#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/tsumugi_image_service.h"
#include "ytec/winpeapp/active_rescue_media.h"
#include "ytec/windowsapp/shrink_work_placement.h"

#include <functional>
#include <optional>

namespace ytec::winpeapp {

// Immutable review of the physical data area beside the currently booted
// Y-TEC rescue marker. Shrink restore never falls back to X:, AppData, or an
// unbound drive letter: this must be a proven third physical disk.
struct DirectShrinkImageRestoreWorkReview final {
  windowsapp::WindowsShrinkWorkPaths paths;
  windowsapp::WindowsShrinkWorkPlacementObservation observation;
  clonecore::StableDiskIdentity active_rescue_disk;
};

struct DirectShrinkImageRestoreRequest final {
  imageformat::TsumugiImageVerifyRequest image;
  imageformat::Sha256Digest expected_image_global_hash{};
  imageformat::Sha256Digest expected_source_state_hash{};
  imageformat::TsumugiManifest reviewed_manifest;
  bool reviewed_image_partial_loss{};
  clonecore::StableDiskIdentity image_backing_disk;
  diskmodel::DiskInfo reviewed_target;
  clonecore::StableDiskIdentity expected_target;
  // Present only when the reviewed target's model/serial hashes identify it
  // as the physical source recorded by the authenticated image. This binds
  // the explicit "restore back to the original disk" review to the exact
  // destructive target; the ordinary two-step target acknowledgement and
  // uppercase OK remain mandatory.
  std::optional<clonecore::StableDiskIdentity>
      reviewed_original_source_target;
  imageformat::Sha256Digest expected_target_layout_hash{};
  imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 reviewed_layout;
  DirectShrinkImageRestoreWorkReview work;
  clonecore::TargetConfirmation confirmation;
  bool administrator{};
  clonecore::DiskOperationCallbacks callbacks;
};

struct DirectShrinkImageRestoreReport final {
  imageformat::TsumugiShrinkRestoreReport restore;
  bool image_completely_reverified{};
  bool image_backing_reidentified_before_write{};
  bool active_rescue_media_checked{};
  bool target_reidentified_before_plan{};
  bool work_placement_reidentified_before_write{};
  bool target_left_offline{};
  bool direct_execution_only{};
  bool boot_repair_offer_required{};
};

using DirectShrinkActiveStorageQuery = std::function<clonecore::Result<
    ActiveRescueMediaStorageObservation>()>;
using DirectShrinkImageBackingObserver = std::function<clonecore::Result<
    clonecore::StableDiskIdentity>(const std::wstring&)>;
using DirectShrinkTargetReidentifier = std::function<clonecore::Result<
    diskmodel::ReidentifiedPhysicalTarget>(
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&)>;
using DirectShrinkRestoreExecutor = std::function<clonecore::Result<
    DirectShrinkImageRestoreReport>(
        const DirectShrinkImageRestoreRequest&,
        const diskmodel::ReidentifiedPhysicalTarget&,
        const clonecore::StableDiskIdentity&,
        const windowsapp::WindowsShrinkWorkPlacementObservation&)>;

struct DirectShrinkImageRestoreDependencies final {
  DirectShrinkActiveStorageQuery query_active_storage;
  DirectShrinkImageBackingObserver identify_image_backing;
  DirectShrinkTargetReidentifier reidentify_target;
  windowsapp::WindowsShrinkWorkPlacementObserver observe_work_placement;
  DirectShrinkRestoreExecutor execute_reviewed_restore;
};

// Read-only stable identity for a local image path. The path-to-disk mapping
// is queried before and after a complete inventory scan.
[[nodiscard]] clonecore::Result<clonecore::StableDiskIdentity>
identify_direct_shrink_image_backing_with_windows_apis(
    const std::wstring& image_path);

// Read-only review of YtecDiskClone\data on the active rescue medium. The
// image, target, and work disks must be three distinct stable identities.
[[nodiscard]] clonecore::Result<DirectShrinkImageRestoreWorkReview>
review_direct_shrink_active_rescue_work_with_windows_apis(
    const clonecore::StableDiskIdentity& image_backing_disk,
    const clonecore::StableDiskIdentity& expected_target);

// Creates one immutable, target-specific whole-disk shrink layout while
// preserving the authenticated source partition-table style. This slice does
// not expose MBR-to-GPT selection and never performs target I/O.
[[nodiscard]] clonecore::Result<
    imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
make_direct_shrink_image_restore_layout_with_windows_apis(
    const imageformat::TsumugiManifest& manifest,
    const diskmodel::DiskInfo& target,
    const clonecore::StableDiskIdentity& expected_target,
    const std::optional<clonecore::StableDiskIdentity>&
        reviewed_original_source_target);

// Direct WinPE product controller. Every identity/layout/work gate is
// completed before execute_reviewed_restore is called. There is no job-file
// or persistent-resume fallback for shrink restore.
[[nodiscard]] clonecore::Result<DirectShrinkImageRestoreReport>
execute_direct_shrink_image_restore(
    const DirectShrinkImageRestoreRequest& request,
    const DirectShrinkImageRestoreDependencies& dependencies);

[[nodiscard]] DirectShrinkImageRestoreDependencies
make_direct_shrink_image_restore_windows_dependencies();

[[nodiscard]] clonecore::Result<DirectShrinkImageRestoreReport>
execute_direct_shrink_image_restore_with_windows_apis(
    const DirectShrinkImageRestoreRequest& request);

}  // namespace ytec::winpeapp

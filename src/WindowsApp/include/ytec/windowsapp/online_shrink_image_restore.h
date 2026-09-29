#pragma once

#include "ytec/operationcore/operation.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/windowsapp/online_shrink_image.h"
#include "ytec/windowsapp/restore_preflight.h"

#include <functional>
#include <optional>

namespace ytec::windowsapp {

struct WindowsOnlineShrinkRestoreRequest final {
  TsumugiRestoreImagePreflightReport reviewed_image;
  diskmodel::DiskInfo reviewed_target;
  imageformat::TsumugiImageVerifyRequest image;
  clonecore::StableDiskIdentity image_backing_disk;
  clonecore::StableDiskIdentity expected_target;
  imageformat::Sha256Digest expected_target_layout_hash{};
  imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 reviewed_layout;
  WindowsShrinkWorkPaths work_paths;
  WindowsShrinkWorkPlacementObservation observed_work;
  clonecore::TargetConfirmation confirmation;
  bool administrator{};
  operationcore::OperationId operation_id{};
  clonecore::DiskOperationCallbacks callbacks;
};

struct WindowsOnlineShrinkRestoreExecutionReport final {
  imageformat::TsumugiShrinkRestoreReport restore;
  bool image_completely_reverified{};
  bool target_reidentified_before_plan{};
  bool work_placement_reidentified_before_write{};
  bool target_left_offline{};
  bool boot_repair_offer_required{};
};

struct WindowsOnlineShrinkRestoreOperationReport final {
  operationcore::OperationPlan plan;
  operationcore::OperationResult lifecycle;
  std::optional<WindowsOnlineShrinkRestoreExecutionReport> restore;
};

using WindowsOnlineShrinkRestoreTargetReidentifier =
    std::function<clonecore::Result<diskmodel::ReidentifiedPhysicalTarget>(
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&)>;
using WindowsOnlineShrinkRestoreProtectedTargetQuery =
    std::function<clonecore::Result<bool>(
        const clonecore::StableDiskIdentity&)>;
using WindowsOnlineShrinkRestoreExecutor =
    std::function<clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>(
        const WindowsOnlineShrinkRestoreRequest&)>;

struct WindowsOnlineShrinkRestoreDependencies final {
  WindowsOnlineShrinkRestoreTargetReidentifier reidentify_target;
  WindowsOnlineShrinkRestoreProtectedTargetQuery
      is_protected_rescue_media;
  WindowsShrinkWorkPlacementObserver observe_work_placement;
  WindowsOnlineShrinkRestoreExecutor execute_reviewed_restore;
};

// Builds a target-specific reviewed layout. Random disk/partition identifiers
// are created now and remain bound to the OperationPlan through completion.
// The default target style preserves GPT/MBR; GPT-to-MBR is never offered.
[[nodiscard]] clonecore::Result<
    imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
make_windows_online_shrink_restore_layout_with_windows_apis(
    const TsumugiRestoreImagePreflightReport& image,
    const diskmodel::DiskInfo& target,
    const clonecore::StableDiskIdentity& expected_target);

[[nodiscard]] clonecore::Result<operationcore::OperationPlan>
make_windows_online_shrink_restore_operation_plan(
    const WindowsOnlineShrinkRestoreRequest& request);

// A successful wrapper Result means the immutable lifecycle ran. Destructive
// failure/cancellation is represented by report.lifecycle, never as a false
// completed result.
[[nodiscard]] clonecore::Result<
    WindowsOnlineShrinkRestoreOperationReport>
execute_windows_online_shrink_restore_operation(
    const WindowsOnlineShrinkRestoreRequest& request,
    const WindowsOnlineShrinkRestoreDependencies& dependencies);

[[nodiscard]] WindowsOnlineShrinkRestoreDependencies
make_windows_online_shrink_restore_dependencies();

[[nodiscard]] clonecore::Result<
    WindowsOnlineShrinkRestoreOperationReport>
execute_windows_online_shrink_restore_operation_with_windows_apis(
    const WindowsOnlineShrinkRestoreRequest& request);

}  // namespace ytec::windowsapp

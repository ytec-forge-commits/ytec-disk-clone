#pragma once

#include "ytec/operationcore/operation.h"
#include "ytec/windowsapp/online_image_restore.h"
#include "ytec/windowsapp/restore_preflight.h"

#include <optional>

namespace ytec::windowsapp {

// Immutable review snapshot used to bridge the product UI to the shared
// OperationPlan lifecycle. The reviewed image and target layout are bound by
// hashes; the destructive controller still performs its own fresh checks.
struct OnlineImageRestoreOperationRequest final {
  TsumugiRestoreImagePreflightReport reviewed_image;
  diskmodel::DiskInfo reviewed_target;
  OnlineImageRestoreRequest restore;
  operationcore::OperationId operation_id{};
};

struct OnlineImageRestoreOperationReport final {
  operationcore::OperationPlan plan;
  operationcore::OperationResult lifecycle;
  std::optional<OnlineImageRestoreReport> restore;
};

[[nodiscard]] clonecore::Result<operationcore::OperationId>
make_online_image_restore_operation_id_with_windows_apis();

[[nodiscard]] clonecore::Result<operationcore::OperationPlan>
make_online_image_restore_operation_plan(
    const OnlineImageRestoreOperationRequest& request);

// A successful Result means setup produced an immutable plan. Inspect
// report.lifecycle for completed/failed/cancelled; destructive failures are
// deliberately represented by OperationResult and never hidden as success.
[[nodiscard]] clonecore::Result<OnlineImageRestoreOperationReport>
execute_online_image_restore_operation(
    const OnlineImageRestoreOperationRequest& request,
    const OnlineImageRestoreDependencies& dependencies);

[[nodiscard]] clonecore::Result<OnlineImageRestoreOperationReport>
execute_online_image_restore_operation_with_windows_apis(
    const OnlineImageRestoreOperationRequest& request);

}  // namespace ytec::windowsapp

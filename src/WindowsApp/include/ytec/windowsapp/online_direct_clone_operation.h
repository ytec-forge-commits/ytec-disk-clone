#pragma once

#include "ytec/operationcore/operation.h"
#include "ytec/windowsapp/online_direct_clone.h"

#include <optional>

namespace ytec::windowsapp {

// Immutable review snapshot used by the Windows direct-clone UI. DiskInfo is
// retained only to prove that the stable identities and both layout hashes
// came from the exact cards shown before the uppercase OK confirmation.
struct OnlineDirectCloneOperationRequest final {
  diskmodel::DiskInfo reviewed_source;
  diskmodel::DiskInfo reviewed_target;
  OnlineDirectCloneRequest clone;
  operationcore::OperationId operation_id{};
};

struct OnlineDirectCloneOperationReport final {
  operationcore::OperationPlan plan;
  operationcore::OperationResult lifecycle;
  std::optional<OnlineDirectCloneReport> clone;
};

[[nodiscard]] clonecore::Result<operationcore::OperationId>
make_online_direct_clone_operation_id_with_windows_apis();

[[nodiscard]] clonecore::Result<operationcore::OperationPlan>
make_online_direct_clone_operation_plan(
    const OnlineDirectCloneOperationRequest& request);

// A successful Result means that an immutable plan was formed and its
// lifecycle ran. Inspect lifecycle.outcome before consuming clone evidence.
[[nodiscard]] clonecore::Result<OnlineDirectCloneOperationReport>
execute_online_direct_clone_operation(
    const OnlineDirectCloneOperationRequest& request,
    const OnlineDirectCloneDependencies& dependencies);

[[nodiscard]] clonecore::Result<OnlineDirectCloneOperationReport>
execute_online_direct_clone_operation_with_windows_apis(
    const OnlineDirectCloneOperationRequest& request);

}  // namespace ytec::windowsapp

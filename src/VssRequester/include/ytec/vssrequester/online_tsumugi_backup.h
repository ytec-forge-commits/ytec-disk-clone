#pragma once

#include "ytec/vssrequester/tsumugi_snapshot.h"
#include "ytec/vssrequester/windows_backend.h"
#include "ytec/vssrequester/windows_diff_area_observer.h"
#include "ytec/vssrequester/workflow.h"

#include <functional>
#include <memory>

namespace ytec::vssrequester {

struct PreparedOnlineTsumugiBackup final {
  WorkflowRequest workflow;
  TsumugiSnapshotImageRequest image;
  // nullptr means the pre-Snapshot guard. A non-null report means a fully
  // verified owned partial exists and the callback is running immediately
  // before its delayed final-name commit.
  std::function<clonecore::Status(
      const imageformat::TsumugiImageCreateReport*)>
      revalidate_destination;
};

struct OnlineTsumugiBackupReport final {
  WorkflowReport workflow;
  imageformat::TsumugiImageCreateReport image;
  bool final_file_committed_after_vss{};
};

using TsumugiSnapshotPrepareExecutor = std::function<clonecore::Result<
    imageformat::TsumugiStagedImageV1>(
    const TsumugiSnapshotImageRequest&,
    const SnapshotCopyContext&)>;

using TsumugiWorkflowBackendFactory = std::function<clonecore::Result<
    std::unique_ptr<IWorkflowBackend>>(SnapshotCopyCallback)>;

// Mockable lifecycle seam. The copy callback may only prepare and completely
// verify a neighbouring .partial. The completed .tsumugi name is committed
// after BackupComplete and exact Snapshot-set deletion have both succeeded.
[[nodiscard]] clonecore::Result<OnlineTsumugiBackupReport>
execute_prepared_online_tsumugi_backup(
    const PreparedOnlineTsumugiBackup& request,
    const TsumugiSnapshotPrepareExecutor& prepare_snapshot,
    const TsumugiWorkflowBackendFactory& backend_factory);

struct WindowsOnlineTsumugiBackupRequest final {
  PreparedOnlineTsumugiBackup prepared;
  AsyncWaitOptions async_wait;
  clonecore::DiskOperationCallbacks callbacks;
  VssDiffAreaReviewCallback diff_area_review_callback;
  const clonecore::Logger* logger{};
};

[[nodiscard]] clonecore::Result<OnlineTsumugiBackupReport>
execute_windows_online_tsumugi_backup(
    const WindowsOnlineTsumugiBackupRequest& request);

}  // namespace ytec::vssrequester

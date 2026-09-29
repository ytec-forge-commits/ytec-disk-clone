#pragma once

#include "ytec/imageformat/windows_file_staging.h"
#include "ytec/vssrequester/snapshot_plan.h"
#include "ytec/vssrequester/windows_backend.h"

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace ytec::vssrequester {

struct OnlineImageBackupReport final {
  WorkflowReport workflow;
  imageformat::DcimgStreamBuildReport image;
  bool final_file_committed_after_vss{};
};

using SnapshotImageCopyExecutor = std::function<clonecore::Result<
    imageformat::DcimgStreamBuildReport>(
    const SnapshotImageCopyRequest&,
    std::span<const std::wstring>,
    imageformat::IDcimgStagingTarget&)>;

using WorkflowBackendFactory = std::function<clonecore::Result<
    std::unique_ptr<IWorkflowBackend>>(SnapshotCopyCallback)>;

// Mockable orchestration seam. The staging target's commit is deferred until
// the VSS workflow has completed BackupComplete and deleted its Snapshot set.
[[nodiscard]] clonecore::Result<OnlineImageBackupReport>
execute_prepared_snapshot_image_backup(
    const PreparedSnapshotImagePlan& plan,
    std::unique_ptr<imageformat::IDcimgStagingTarget> staging_target,
    const SnapshotImageCopyExecutor& copy_executor,
    const WorkflowBackendFactory& backend_factory);

struct WindowsOnlineImageBackupRequest final {
  PreparedSnapshotImagePlan plan;
  imageformat::WindowsFileStagingRequest staging;
  AsyncWaitOptions async_wait;
  const clonecore::Logger* logger{};
};

// Product adapter. This is the only VSS-to-file entry point and requires an
// administrator plan. All physical source access remains read-only.
[[nodiscard]] clonecore::Result<OnlineImageBackupReport>
execute_windows_online_image_backup(
    const WindowsOnlineImageBackupRequest& request);

}  // namespace ytec::vssrequester

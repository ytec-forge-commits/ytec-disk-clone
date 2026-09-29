#pragma once

#include "ytec/clonecore/log.h"
#include "ytec/vssrequester/diff_area_monitor.h"
#include "ytec/vssrequester/workflow.h"

#include <functional>
#include <memory>
#include <string>

namespace ytec::vssrequester {

// Must freshly re-identify the physical source and reviewed layout behind the
// supplied Volume GUID without writing to it. The returned opaque token is
// compared before and after every COM observation. Exceptions fail closed.
using WindowsVssSourceIdentityProbe = std::function<clonecore::Result<
    std::wstring>(const VssDiffAreaSnapshotBinding&)>;

// Injectable only for deterministic failure tests. Product construction uses
// the built-in read-only Windows probe when this callback is empty.
using WindowsVssBackingVolumeSpaceProbe = std::function<clonecore::Result<
    VssDiffAreaObservation>(
    const VssDiffAreaObservation&)>;

struct WindowsVssDiffAreaObserverOptions final {
  WindowsVssSourceIdentityProbe probe_source_identity;
  WindowsVssBackingVolumeSpaceProbe probe_backing_volume_space;
  const clonecore::Logger* logger{};
};

struct WindowsVssDiffAreaOperationMonitorOptions final {
  VssDiffAreaMonitorPolicy policy;
  std::wstring expected_source_identity_token;
  WindowsVssSourceIdentityProbe probe_source_identity;
  VssDiffAreaReviewCallback review_callback;
  const clonecore::Logger* logger{};
};

using WindowsVssDiffAreaOperationMonitorFactory = std::function<
    clonecore::Result<std::unique_ptr<VssDiffAreaOperationMonitor>>(
        const SnapshotCopyContext&)>;

using WindowsVssDiffAreaObserverFactory = std::function<clonecore::Result<
    std::unique_ptr<IVssDiffAreaObserver>>(
    WindowsVssDiffAreaObserverOptions)>;

// Converts the already verified active Snapshot callback context into the
// complete immutable monitor binding. Provider and creation timestamp come
// from the same GetSnapshotProperties observation as the device mapping.
[[nodiscard]] clonecore::Result<VssDiffAreaMonitorBinding>
make_windows_vss_diff_area_monitor_binding(
    const SnapshotCopyContext& context,
    const std::wstring& expected_source_identity_token);

// Product factory for one active Snapshot callback. It converts the verified
// callback context, owns the production read-only observer, and rejects a
// missing source probe or review UI before any output mutation may begin.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<VssDiffAreaOperationMonitor>>
make_windows_vss_diff_area_operation_monitor(
    const SnapshotCopyContext& context,
    WindowsVssDiffAreaOperationMonitorOptions options);

// Production, read-only Windows adapter. It uses QuerySnapshotsByVolume and
// IVssDifferentialSoftwareSnapshotMgmt::QueryDiffAreasForSnapshot; it never
// calls AddDiffArea, ChangeDiffAreaMaximumSize, migration, deletion, or another
// mutating VSS API. Unknown/unbounded sizes, COM errors, missing or replaced
// snapshots, provider drift, and source identity drift fail closed.
class WindowsVssDiffAreaObserver final : public IVssDiffAreaObserver {
 public:
  explicit WindowsVssDiffAreaObserver(
      WindowsVssDiffAreaObserverOptions options);
  ~WindowsVssDiffAreaObserver() override;

  WindowsVssDiffAreaObserver(const WindowsVssDiffAreaObserver&) = delete;
  WindowsVssDiffAreaObserver& operator=(
      const WindowsVssDiffAreaObserver&) = delete;
  WindowsVssDiffAreaObserver(WindowsVssDiffAreaObserver&&) = delete;
  WindowsVssDiffAreaObserver& operator=(
      WindowsVssDiffAreaObserver&&) = delete;

  [[nodiscard]] clonecore::Result<std::vector<VssDiffAreaObservation>>
  observe(const VssDiffAreaMonitorBinding& binding) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ytec::vssrequester

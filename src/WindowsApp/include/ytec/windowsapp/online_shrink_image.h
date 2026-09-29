#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/imageformat/tsumugi_image_service.h"
#include "ytec/vssrequester/online_tsumugi_backup.h"
#include "ytec/windowsapp/shrink_work_placement.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace ytec::windowsapp {

enum class WindowsShrinkCapturedPayloadKind : std::uint8_t {
  vss_snapshot_wim = 1U,
  locked_read_only_exact_raw = 2U,
};

struct WindowsShrinkCapturedPayload final {
  std::uint32_t source_table_index{};
  WindowsShrinkCapturedPayloadKind kind{
      WindowsShrinkCapturedPayloadKind::vss_snapshot_wim};
  // Exact RAW only: the immutable source-disk extent represented by the
  // session bytes. It must equal the reviewed partition source offset.
  std::uint64_t original_source_offset{};
  std::uint64_t session_source_offset{};
  std::uint64_t length{};
  // WIM evidence. All three values are empty for exact RAW and must exactly
  // match one mapping in the active Snapshot Set for a WIM payload.
  std::wstring original_volume_guid_path;
  std::wstring snapshot_id;
  std::wstring snapshot_device_path;
};

// A capture implementation may stage one WIM file per partition, but exposes
// them as one immutable virtual byte source while the .tsumugi writer reads
// them. The source_state_hash must bind the Snapshot Set, every Snapshot ID,
// and any independently locked exact-RAW source generation.
class IWindowsShrinkCapturedSession
    : public imageformat::ITsumugiImageSourceSession {
 public:
  ~IWindowsShrinkCapturedSession() override = default;

  [[nodiscard]] virtual std::span<const WindowsShrinkCapturedPayload>
  payloads() const noexcept = 0;

  // Freshly observed after Snapshot creation / RAW locking. The controller
  // rejects the session unless this still matches the reviewed source disk.
  [[nodiscard]] virtual const clonecore::StableDiskIdentity&
  observed_source_disk() const noexcept = 0;

  // Removes only staging artifacts owned by this capture. It must not touch
  // an unknown replacement. A failure prevents the completed image name from
  // being committed.
  [[nodiscard]] virtual clonecore::Status discard_owned_staging() noexcept = 0;
};

struct WindowsOnlineShrinkImageCreateRequest final {
  clonecore::StableDiskIdentity source_disk;
  vssrequester::WorkflowRequest workflow;
  imageformat::TsumugiImageCreateRequest image_template;
  WindowsShrinkWorkPaths work_paths;
  clonecore::DiskOperationCallbacks callbacks;
};

using WindowsShrinkCaptureExecutor = std::function<clonecore::Result<
    std::unique_ptr<IWindowsShrinkCapturedSession>>(
    const vssrequester::SnapshotCopyContext&,
    std::span<const imageformat::TsumugiManifestPartition>,
    const WindowsShrinkWorkPaths&,
    const clonecore::DiskOperationCallbacks&)>;

using WindowsShrinkImagePrepareExecutor = std::function<clonecore::Result<
    imageformat::TsumugiStagedImageV1>(
    const imageformat::TsumugiImageCreateRequest&,
    const clonecore::DiskOperationCallbacks&)>;

// Called before VSS with nullptr and again with the completely verified staged
// report. The product implementation must resolve final_path without following
// a substituted reparse target, re-identify its backing disk, and reject the
// source disk or a changed destination before returning success.
using WindowsShrinkDestinationValidator = std::function<clonecore::Status(
    const imageformat::TsumugiImageCreateReport*)>;

struct WindowsOnlineShrinkImageCreateDependencies final {
  WindowsShrinkWorkPlacementObserver observe_work_placement;
  WindowsShrinkCaptureExecutor capture_snapshot_payloads;
  WindowsShrinkImagePrepareExecutor prepare_image;
  vssrequester::TsumugiWorkflowBackendFactory backend_factory;
  WindowsShrinkDestinationValidator revalidate_destination;
  vssrequester::WindowsVssDiffAreaOperationMonitorFactory
      make_diff_area_monitor;
};

struct WindowsOnlineShrinkImageCreateReport final {
  vssrequester::WorkflowReport workflow;
  imageformat::TsumugiImageCreateReport image;
  std::uint32_t wim_payload_count{};
  std::uint32_t exact_raw_payload_count{};
  bool every_wim_bound_to_active_snapshot_set{};
  bool work_placement_revalidated_before_commit{};
  bool staging_discarded_before_commit{};
  bool final_file_committed_after_vss{};
};

// GUI-independent Windows controller. It validates work placement before VSS,
// captures archive bytes only inside the active Snapshot callback, completely
// verifies one adjacent .tsumugi.partial, completes and deletes the Snapshot
// Set, revalidates every work/destination boundary, and commits the final name
// last. No .dcmig or persisted operation job is read or written.
[[nodiscard]] clonecore::Result<WindowsOnlineShrinkImageCreateReport>
execute_windows_online_shrink_image_create(
    const WindowsOnlineShrinkImageCreateRequest& request,
    const WindowsOnlineShrinkImageCreateDependencies& dependencies);

}  // namespace ytec::windowsapp

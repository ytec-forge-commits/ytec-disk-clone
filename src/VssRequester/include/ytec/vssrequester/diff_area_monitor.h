#pragma once

#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::vssrequester {

// VSS_ASSOC_NO_MAX_SPACE is deliberately represented instead of being
// converted to an artificial byte count. Both unbounded and unknown limits are
// terminal, fail-closed observations for this product.
enum class VssDiffAreaMaximumKind : std::uint8_t {
  bounded,
  unbounded,
  unknown,
};

enum class VssDiffAreaMonitorDirective : std::uint8_t {
  continue_operation,
  review_required,
  safe_cancel_required,
};

enum class VssDiffAreaReviewAction : std::uint8_t {
  resume_once,
  safe_cancel,
};

// This policy is copied into one monitor at construction and has no mutation
// API. Product routes must therefore put the reviewed values into their
// immutable operation plan before the VSS Snapshot Set is created.
struct VssDiffAreaMonitorPolicy final {
  std::uint64_t poll_interval_ms{5'000U};
  std::uint32_t danger_used_basis_points{8'000U};
  // Reserve required both inside the configured VSS maximum and on the
  // filesystem that actually backs the differential area. A zero policy may
  // disable the reserve, but an observed zero-byte backing reserve is still
  // dangerous and requires review.
  std::uint64_t minimum_remaining_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct VssDiffAreaSnapshotBinding final {
  std::wstring snapshot_id;
  std::wstring original_volume_guid_path;
  std::wstring snapshot_device_path;
  std::wstring provider_id;
  std::int64_t creation_timestamp{};

  // Opaque, privacy-preserving token produced by a fresh read-only source
  // re-identification. It should bind the stable source identity and reviewed
  // layout generation; disk number or drive letter alone is not sufficient.
  std::wstring expected_source_identity_token;
};

struct VssDiffAreaMonitorBinding final {
  std::wstring snapshot_set_id;
  std::vector<VssDiffAreaSnapshotBinding> snapshots;
};

struct VssDiffAreaObservation final {
  std::wstring snapshot_set_id;
  std::wstring snapshot_id;
  std::wstring original_volume_guid_path;
  std::wstring snapshot_device_path;
  std::wstring provider_id;
  std::int64_t creation_timestamp{};
  std::wstring observed_source_identity_token;
  std::wstring diff_area_volume_guid_path;
  // The canonical unique volume name is resolved from the exact VSS
  // association above. GUID, serial and total capacity form the immutable
  // backing-volume latch. Initial free/available values remain in first-poll
  // evidence and both mutable counters are freshly resampled on every poll.
  std::wstring backing_volume_guid_path;
  std::uint32_t backing_volume_serial_number{};
  std::uint64_t backing_volume_total_bytes{};
  std::uint64_t backing_volume_free_bytes{};
  std::uint64_t backing_volume_available_bytes{};
  VssDiffAreaMaximumKind maximum_kind{VssDiffAreaMaximumKind::unknown};
  std::uint64_t maximum_bytes{};
  std::uint64_t allocated_bytes{};
  std::uint64_t used_bytes{};
};

class IVssDiffAreaObserver {
 public:
  virtual ~IVssDiffAreaObserver() = default;

  // Implementations must observe only the exact supplied Snapshot Set and
  // source bindings. This method is read-only and must not resize, move, or
  // create a VSS differential area.
  [[nodiscard]] virtual clonecore::Result<
      std::vector<VssDiffAreaObservation>>
  observe(const VssDiffAreaMonitorBinding& binding) = 0;
};

struct VssDiffAreaSampleEvidence final {
  VssDiffAreaObservation observation;
  std::uint64_t danger_threshold_bytes{};
  std::uint64_t remaining_bytes{};
  // min(total free, caller-available) is used so quota or filesystem
  // pressure cannot be hidden by the other counter.
  std::uint64_t effective_backing_volume_free_bytes{};
  bool used_threshold_reached{};
  bool remaining_reserve_reached{};
  bool backing_volume_reserve_reached{};
};

struct VssDiffAreaPollEvidence final {
  std::uint64_t sequence{};
  std::uint64_t observed_elapsed_ms{};
  VssDiffAreaMonitorPolicy policy;
  VssDiffAreaMonitorDirective directive{
      VssDiffAreaMonitorDirective::safe_cancel_required};
  std::vector<VssDiffAreaSampleEvidence> samples;
  // Present on evidence produced by resolve_review(). It is the exact
  // previously displayed sequence against which the action was submitted.
  std::optional<std::uint64_t> reviewed_evidence_sequence;
  std::optional<VssDiffAreaReviewAction> review_action;
  std::optional<clonecore::Error> failure;
};

// The sink is part of the safety boundary: every performed observation and
// every explicit review action must be durably or otherwise appropriately
// evidenced by the product route. A sink failure makes the monitor terminal.
using VssDiffAreaEvidenceSink =
    std::function<clonecore::Status(const VssDiffAreaPollEvidence&)>;

struct VssDiffAreaPollResult final {
  VssDiffAreaMonitorDirective directive{
      VssDiffAreaMonitorDirective::safe_cancel_required};
  bool observation_performed{};
  std::uint64_t latest_evidence_sequence{};
};

// Single-worker state machine. Calls are intentionally synchronous so the
// owning copy loop can poll at a verified safe boundary before continuing.
// Once review_required is returned, later poll calls cannot clear it. Only an
// explicit resolve_review() for the exact evidence sequence can attempt to
// resume. resume_once always performs a fresh observation and resumes for one
// interval only when used/remaining values are no worse than those displayed.
class VssDiffAreaMonitor final {
 public:
  [[nodiscard]] static clonecore::Result<std::unique_ptr<VssDiffAreaMonitor>>
  create(
      VssDiffAreaMonitorPolicy policy,
      VssDiffAreaMonitorBinding binding,
      IVssDiffAreaObserver* observer,
      VssDiffAreaEvidenceSink evidence_sink);

  ~VssDiffAreaMonitor();

  VssDiffAreaMonitor(const VssDiffAreaMonitor&) = delete;
  VssDiffAreaMonitor& operator=(const VssDiffAreaMonitor&) = delete;
  VssDiffAreaMonitor(VssDiffAreaMonitor&&) = delete;
  VssDiffAreaMonitor& operator=(VssDiffAreaMonitor&&) = delete;

  [[nodiscard]] clonecore::Result<VssDiffAreaPollResult> poll(
      std::uint64_t elapsed_ms);

  // Bypasses only the time interval. Identity, provider, finite-capacity,
  // diff-area-volume and evidence checks remain identical. Product routes use
  // this solely immediately before their first output mutation and once more
  // before the active Snapshot callback returns.
  [[nodiscard]] clonecore::Result<VssDiffAreaPollResult> poll_now(
      std::uint64_t elapsed_ms);

  [[nodiscard]] clonecore::Result<VssDiffAreaPollResult> resolve_review(
      std::uint64_t evidence_sequence,
      std::uint64_t elapsed_ms,
      VssDiffAreaReviewAction action);

  [[nodiscard]] VssDiffAreaMonitorDirective directive() const noexcept;
  [[nodiscard]] const VssDiffAreaMonitorPolicy& policy() const noexcept;
  [[nodiscard]] const VssDiffAreaMonitorBinding& binding() const noexcept;
  [[nodiscard]] const std::optional<VssDiffAreaPollEvidence>&
  latest_evidence() const noexcept;

 private:
  VssDiffAreaMonitor(
      VssDiffAreaMonitorPolicy policy,
      VssDiffAreaMonitorBinding binding,
      IVssDiffAreaObserver* observer,
      VssDiffAreaEvidenceSink evidence_sink);

  [[nodiscard]] clonecore::Result<VssDiffAreaPollResult> poll_internal(
      std::uint64_t elapsed_ms,
      bool force_observation);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

enum class VssDiffAreaOperationPollReason : std::uint8_t {
  initial_before_output,
  safe_boundary,
  review_resume,
  completion_before_snapshot_callback_return,
};

struct VssDiffAreaReviewDecision final {
  std::uint64_t displayed_evidence_sequence{};
  VssDiffAreaReviewAction action{VssDiffAreaReviewAction::safe_cancel};
};

using VssDiffAreaReviewCallback = std::function<clonecore::Result<
    VssDiffAreaReviewDecision>(const VssDiffAreaPollEvidence&)>;

using VssDiffAreaElapsedMillisecondsProvider =
    std::function<std::uint64_t()>;

struct VssDiffAreaOperationPollEvidence final {
  VssDiffAreaOperationPollReason reason{
      VssDiffAreaOperationPollReason::initial_before_output};
  std::optional<clonecore::DiskOperationSafeBoundary> safe_boundary;
  VssDiffAreaPollEvidence monitor;
};

struct VssDiffAreaOperationReviewEvidence final {
  std::uint64_t displayed_evidence_sequence{};
  std::uint64_t submitted_evidence_sequence{};
  VssDiffAreaReviewAction action{VssDiffAreaReviewAction::safe_cancel};
  bool accepted{};
};

// Complete same-process evidence owned by one active VSS Snapshot Set. This
// is deliberately not an OperationCheckpoint and must never be presented as
// a persistent resume capability: VSS_CTX_BACKUP snapshots are non-persistent.
struct VssDiffAreaOperationEvidence final {
  VssDiffAreaMonitorPolicy policy;
  VssDiffAreaMonitorBinding binding;
  std::vector<VssDiffAreaOperationPollEvidence> polls;
  std::vector<VssDiffAreaOperationReviewEvidence> reviews;
  VssDiffAreaMonitorDirective final_directive{
      VssDiffAreaMonitorDirective::safe_cancel_required};
  bool initial_poll_completed_before_output{};
  bool only_polled_at_disk_operation_safe_boundaries{};
  bool completion_poll_completed_before_snapshot_callback_return{};
  bool same_process_only{};
  std::optional<clonecore::Error> terminal_failure;
};

// Owns the observer and the core state machine for one product operation.
// initial_poll() and completion_poll() force fresh observations. Between those
// calls the observer is reached only from callbacks().safe_boundary; progress
// and timer/cancellation callbacks never perform a VSS query.
class VssDiffAreaOperationMonitor final {
 public:
  [[nodiscard]] static clonecore::Result<
      std::unique_ptr<VssDiffAreaOperationMonitor>>
  create(
      VssDiffAreaMonitorPolicy policy,
      VssDiffAreaMonitorBinding binding,
      std::unique_ptr<IVssDiffAreaObserver> observer,
      VssDiffAreaReviewCallback review_callback,
      VssDiffAreaElapsedMillisecondsProvider elapsed_ms = {});

  ~VssDiffAreaOperationMonitor();

  VssDiffAreaOperationMonitor(const VssDiffAreaOperationMonitor&) = delete;
  VssDiffAreaOperationMonitor& operator=(
      const VssDiffAreaOperationMonitor&) = delete;
  VssDiffAreaOperationMonitor(VssDiffAreaOperationMonitor&&) = delete;
  VssDiffAreaOperationMonitor& operator=(
      VssDiffAreaOperationMonitor&&) = delete;

  [[nodiscard]] clonecore::Status initial_poll();
  [[nodiscard]] clonecore::Status completion_poll();
  [[nodiscard]] clonecore::DiskOperationCallbacks callbacks(
      clonecore::DiskOperationCallbacks existing);
  [[nodiscard]] const VssDiffAreaOperationEvidence& evidence()
      const noexcept;

 private:
  class Impl;
  explicit VssDiffAreaOperationMonitor(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Pure completion gate used by every successful product route before it
// returns evidence to the UI.
[[nodiscard]] clonecore::Status
validate_completed_vss_diff_area_operation_evidence(
    const VssDiffAreaOperationEvidence& evidence);

// Encodes already privacy-preserving immutable source/layout bytes as the
// opaque source-epoch token used by the observer. No disk number, drive
// letter, model or serial is introduced by this helper.
[[nodiscard]] clonecore::Result<std::wstring>
encode_vss_diff_area_source_epoch_token(
    std::span<const std::byte> source_epoch_bytes);

}  // namespace ytec::vssrequester

#pragma once

#include "ytec/operationcore/resume_slot.h"
#include "ytec/imageformat/tsumugi_physical_restore_resume.h"
#include "ytec/winpeapp/direct_image_restore.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ytec::winpeapp {

// Resume is deliberately a separate controller from the legacy physical
// restore adapter.  Only the production backend that can deterministically
// reconstruct and prove an exact/rescue whole-disk incomplete layout may opt
// in.  Shrink and individual-partition restore remain outside this contract.
enum class DirectImageRestoreResumeAction : std::uint8_t {
  inspect_only,
  start_new,
  resume_existing,
  discard_existing,
  cancel,
};

enum class DirectImageRestoreResumeOutcomeKind : std::uint8_t {
  no_slot,
  decision_required,
  cancelled,
  discarded,
  completed,
  completed_checkpoint_retained,
};

// These are opened-storage identities, not paths, drive letters, disk
// numbers, or UI claims. A platform probe must obtain all four from handles
// and must fail rather than returning an all-zero or uncertain identity.
struct DirectImageRestoreResumeStorageProof final {
  operationcore::Sha256Digest checkpoint_storage_identity_hash{};
  operationcore::Sha256Digest image_storage_identity_hash{};
  operationcore::Sha256Digest target_storage_identity_hash{};
  operationcore::Sha256Digest active_rescue_storage_identity_hash{};
  // Reopened observation of the selected image file.  It must equal the
  // identity captured from the immutable handle that performs complete image
  // verification before the controller may bind a plan.
  operationcore::Sha256Digest image_file_object_identity_hash{};
  bool all_identities_from_open_handles{};
};

enum class DirectImageRestoreTargetResumeState : std::uint8_t {
  reviewed_initial_layout,
  preparation_original_or_zero_bound_to_operation,
  incomplete_layout_bound_to_operation,
  commit_publication_bound_to_operation,
  completed_layout_bound_to_operation,
};

// Returned only after one immutable image handle has completed full container,
// manifest, global-hash, chunk-hash and (when applicable) GCM verification.
// image_file_object_identity_hash identifies that same open file object.
struct DirectImageRestoreResumeEvidence final {
  imageformat::TsumugiVerifiedImage image;
  operationcore::Sha256Digest image_file_object_identity_hash{};
  // Normalized physical storage domain derived from the offline writer's
  // opened target identity. The controller compares it with the independent
  // four-storage proof before creating or advancing a checkpoint.
  operationcore::Sha256Digest target_storage_identity_hash{};
  operationcore::ReidentifiedOperation observed_operation;
  imageformat::Sha256Digest observed_initial_target_layout_hash{};
  DirectImageRestoreTargetResumeState target_state{
      DirectImageRestoreTargetResumeState::reviewed_initial_layout};
  operationcore::Sha256Digest incomplete_layout_plan_hash{};
  std::vector<imageformat::TsumugiPhysicalResumePreparationSectorV1>
      preparation_sectors;
  std::optional<imageformat::TsumugiRestoreLayoutPublicationInspectionV1>
      publication;
  // Authenticated target-write mapping.  Staged/final GPT/MBR metadata is
  // absent, so the durable cursor never mistakes intentionally withheld
  // metadata for a payload chunk that may be skipped after restart.
  std::vector<imageformat::TsumugiPhysicalResumePayloadSegmentV1>
      payload_segments;
  bool complete_image_verified_on_one_immutable_handle{};
  bool image_file_identity_from_that_handle{};
  bool target_state_from_locked_handle{};
  bool active_rescue_media_excluded_by_stable_identity{};
  bool exact_or_rescue_whole_disk_layout_only{};
};

struct DirectImageRestoreResumeCursor final {
  operationcore::OperationId operation_id{};
  std::uint64_t verified_logical_bytes{};
  std::uint64_t verified_chunk_count{};
  std::uint64_t expected_logical_bytes{};
  std::uint64_t expected_chunk_count{};
  operationcore::Sha256Digest plan_hash{};
  imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase durable_phase{
      imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase::preparing};
  operationcore::ResumeIdentityBinding identities{};
  std::wstring continuity_token;
  std::vector<imageformat::TsumugiPhysicalResumePayloadSegmentV1>
      payload_segments;
  std::vector<imageformat::TsumugiPhysicalResumePreparationSectorV1>
      preparation_sectors;
};

// The adapter invokes this exactly once for the next authenticated payload
// segment and only after its entire target range was flushed and read back
// successfully. Skips, duplicates, out-of-order segments and substitution are
// rejected before the checkpoint can advance.
using DirectImageRestoreChunkReadbackCommit =
    std::function<clonecore::Status(
        std::uint64_t,
        const imageformat::TsumugiPhysicalResumePayloadSegmentV1&)>;

// Durable phase callbacks replace the single active checkpoint and return
// only after atomic publish/readback. Preparation completion is committed
// before payload; commit-ready is committed after the full payload prefix is
// authenticated and before any partition-table publication.
using DirectImageRestoreResumePhaseCommit =
    std::function<clonecore::Status()>;

struct DirectImageRestoreResumeTransferReport final {
  std::uint64_t resumed_verified_logical_bytes{};
  std::uint64_t resumed_verified_chunk_count{};
  std::uint64_t final_verified_logical_bytes{};
  std::uint64_t final_verified_chunk_count{};
  bool full_image_reverified_on_same_handle_before_first_write{};
  bool target_and_incomplete_layout_reidentified_before_first_write{};
  bool verified_prefix_was_not_rewritten{};
  bool every_new_chunk_flushed_and_read_back{};
  bool final_layout_committed{};
  bool target_left_offline{};
  // Rescue completion never upgrades authenticated unreadable zero-fill to a
  // normal exact result.  These fields must exactly match the fully verified
  // image evidence even after process restart.
  bool rescue_mode{};
  bool partial_loss{};
  std::vector<clonecore::ByteRange> unreadable_ranges;
};

using DirectImageRestoreResumeStorageProbe = std::function<
    clonecore::Result<DirectImageRestoreResumeStorageProof>(
        const DirectImageRestoreRequest&)>;

using DirectImageRestoreResumeEvidenceProbe = std::function<
    clonecore::Result<DirectImageRestoreResumeEvidence>(
        const DirectImageRestoreRequest&,
        const std::optional<operationcore::ResumeSlotRecord>&)>;

// A production adapter must reopen the target's exact/rescue incomplete layout
// transaction and honor the authenticated payload-segment cursor.  A legacy or
// test adapter that cannot prove those properties must leave
// persistent_exact_resume_capable false so the controller stops before target
// I/O.
using DirectImageRestoreResumeTransferExecutor = std::function<
    clonecore::Result<DirectImageRestoreResumeTransferReport>(
        const DirectImageRestoreRequest&,
        const DirectImageRestoreResumeEvidence&,
        const DirectImageRestoreResumeCursor&,
        const DirectImageRestoreResumePhaseCommit&,
        const DirectImageRestoreChunkReadbackCommit&,
        const DirectImageRestoreResumePhaseCommit&)>;

struct DirectImageRestoreResumeDependencies final {
  operationcore::IResumeSlotPlatform* slot_platform{};
  const DirectImageRestoreDependencies* physical_dependencies{};
  DirectImageRestoreResumeStorageProbe prove_storage_separation;
  DirectImageRestoreResumeEvidenceProbe collect_evidence;
  DirectImageRestoreResumeTransferExecutor execute_transfer;
};

struct DirectImageRestoreResumeCommand final {
  DirectImageRestoreResumeAction action{
      DirectImageRestoreResumeAction::inspect_only};
  // Used only for start_new. It must be generated once by the current UI
  // controller and remain stable for this operation.
  operationcore::OperationId new_operation_id{};
  // Required for resume/discard. It binds a displayed decision to the exact
  // slot record so a stale prompt cannot mutate a replacement checkpoint.
  std::optional<operationcore::ResumeSlotBinding> reviewed_existing_slot;
};

struct DirectImageRestoreResumeOutcome final {
  DirectImageRestoreResumeOutcomeKind kind{
      DirectImageRestoreResumeOutcomeKind::no_slot};
  std::optional<operationcore::ResumeSlotBinding> existing_slot;
  std::uint64_t verified_logical_bytes{};
  std::uint64_t verified_chunk_count{};
  // Bounded, path-free startup summary.  The UI may show these counters
  // before an image or target has been selected, but must never expose the
  // operation id or any identity/checkpoint digest as user-facing text.
  std::uint64_t expected_logical_bytes{};
  std::optional<operationcore::CheckpointPhase> checkpoint_phase;
  std::optional<operationcore::ResumeCapability> capability;
  std::optional<DirectImageRestoreResumeTransferReport> transfer;
  bool rescue_mode{};
  bool partial_loss{};
  std::vector<clonecore::ByteRange> unreadable_ranges;
  // A completed restore is still reported as completed if cleanup of the
  // already-obsolete checkpoint fails. The exact checkpoint remains for an
  // explicit, binding-checked discard and is never silently removed.
  std::optional<clonecore::Error> checkpoint_cleanup_error;
};

// Produces the only startup text that may be shown before an image and target
// have been reselected.  It intentionally consumes only the bounded public
// counters/classification above and never renders an operation id, identity
// digest, checkpoint path, or owned-partial path.
[[nodiscard]] clonecore::Result<std::wstring>
format_direct_image_restore_resume_startup_review_v1(
    const DirectImageRestoreResumeOutcome& outcome);

// Pure/controller boundary for the first persistent-resume product slice.
// It performs no physical I/O itself. inspect_only and discard_existing need
// only the fixed-slot platform: they never require a current image/target,
// invoke the four-storage proof, verify an image, or call the transfer
// executor. start_new/resume_existing require those dependencies and prove
// storage separation before any evidence or target I/O. Existing
// unknown/corrupt state is returned as failure and remains untouched.
[[nodiscard]] clonecore::Result<DirectImageRestoreResumeOutcome>
control_direct_image_restore_resume_v1(
    const DirectImageRestoreRequest& request,
    const DirectImageRestoreResumeCommand& command,
    const DirectImageRestoreResumeDependencies& dependencies);

}  // namespace ytec::winpeapp

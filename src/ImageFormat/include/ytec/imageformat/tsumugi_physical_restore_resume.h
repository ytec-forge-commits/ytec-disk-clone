#pragma once

#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/imageformat/tsumugi_restore_layout_io.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace ytec::imageformat {

// A container record may intersect target-only GPT/MBR metadata that remains
// deliberately withheld until final commit.  The durable cursor therefore
// advances over these authenticated payload segments, never over raw
// container-record indexes.  record_plaintext_offset binds a segment back to
// the authenticated record without copying or modifying that record.
struct TsumugiPhysicalResumePayloadSegmentV1 final {
  std::uint64_t record_index{};
  std::uint64_t record_plaintext_offset{};
  std::uint64_t target_offset{};
  std::uint64_t length{};

  [[nodiscard]] bool operator==(
      const TsumugiPhysicalResumePayloadSegmentV1&) const noexcept = default;
};

struct TsumugiPhysicalResumeLayoutSeedV1 final {
  std::array<std::byte, 16U> operation_id{};
  Sha256Digest plan_hash{};
};

// This production backend intentionally supports 512-byte logical sectors
// only. 4Kn completion remains fail-closed until a separately reviewed layout
// and checkpoint boundary is implemented.
// Persistent preparation records one digest per sector in the two bounded
// invalidation ranges.  Raw target metadata is never copied into the slot.
// Two 1 MiB ranges at the supported 512-byte sector size require at most 4096
// entries; callers and the checkpoint parser enforce the same bound.
inline constexpr std::size_t
    kTsumugiPhysicalResumeMaximumPreparationSectorsV1 = 4096U;

struct TsumugiPhysicalResumePreparationSectorV1 final {
  std::uint64_t offset{};
  std::uint32_t length{};
  Sha256Digest original_hash{};

  [[nodiscard]] bool operator==(
      const TsumugiPhysicalResumePreparationSectorV1&) const noexcept =
      default;
};

enum class TsumugiPhysicalResumePreparationStateV1 : std::uint8_t {
  all_original,
  original_or_zero,
  all_zero,
};

struct TsumugiPhysicalResumePreparationInspectionV1 final {
  TsumugiPhysicalResumePreparationStateV1 state{
      TsumugiPhysicalResumePreparationStateV1::all_original};
  std::uint64_t original_sector_count{};
  std::uint64_t zero_sector_count{};
};

// Captures only SHA-256 evidence from the already-open target.  It performs
// no target write and must run after the reviewed initial layout and stable
// target identity have been checked, but before the first destructive write.
[[nodiscard]] clonecore::Result<std::vector<
    TsumugiPhysicalResumePreparationSectorV1>>
capture_tsumugi_physical_resume_preparation_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    clonecore::ITargetDiskWriter& target);

// Read-only restart inspection. Every sector must still match its durable
// original digest or be completely zero. Foreign/torn bytes fail closed.
[[nodiscard]] clonecore::Result<
    TsumugiPhysicalResumePreparationInspectionV1>
inspect_tsumugi_physical_resume_preparation_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    std::span<const TsumugiPhysicalResumePreparationSectorV1> evidence,
    clonecore::ITargetDiskWriter& target);

// Commit-ready restart verification for the remainder of the broad
// invalidation ranges. Exact staged/final metadata sectors are classified by
// inspect_tsumugi_whole_disk_restore_layout_publication_v1; every other
// preparation sector must remain zero.
[[nodiscard]] clonecore::Status
verify_tsumugi_physical_resume_nonpublication_zero_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    std::span<const TsumugiPhysicalResumePreparationSectorV1> evidence,
    clonecore::ITargetDiskWriter& target);

// After a complete read-only inspection, zeros each exact invalidation sector
// with flush/readback. Interruption leaves the durable preparing evidence
// usable because every completed sector is still one of the two allowed
// states. Payload writes remain forbidden until the caller durably advances
// the checkpoint to prepared.
[[nodiscard]] clonecore::Status
prepare_tsumugi_physical_resume_layout_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    std::span<const TsumugiPhysicalResumePreparationSectorV1> evidence,
    clonecore::ITargetDiskWriter& target,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Builds the exact payload/checkpoint mapping by subtracting every staged and
// final GPT/MBR metadata write from the authenticated container ranges.  This
// function performs no I/O.  The caller authenticates the returned boundaries
// in its immutable operation plan.
[[nodiscard]] clonecore::Result<
    std::vector<TsumugiPhysicalResumePayloadSegmentV1>>
make_tsumugi_physical_resume_payload_segments_v1(
    std::span<const TsumugiChunkRecord> records,
    const TsumugiWholeDiskRestoreLayoutPlan& layout);

// Reconstructs one final exact/rescue layout from durable operation material.
// GPT GUIDs and the MBR signature are deterministic for that operation; an
// MBR collision fails closed rather than selecting a different post-restart
// layout.
[[nodiscard]] clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>
make_tsumugi_physical_resume_layout_plan_v1(
    const TsumugiVerifiedImage& image,
    const TsumugiRestoreDiskIdentity& target,
    const TsumugiPhysicalResumeLayoutSeedV1& seed,
    std::span<const std::uint32_t> disallowed_mbr_signatures = {});

// Reads only from the already-open target.  Success means every staged/final
// metadata range is still zero and the exact deterministic transaction can be
// resumed without repeating initial invalidation or rewriting a verified
// payload prefix.
[[nodiscard]] clonecore::Status
verify_tsumugi_physical_resume_layout_withheld_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    clonecore::ITargetDiskWriter& target,
    std::size_t verification_block_bytes = 4U * 1024U * 1024U);

struct TsumugiPhysicalResumeCursorV1 final {
  enum class DurablePhase : std::uint8_t {
    preparing,
    prepared,
    commit_ready,
  };

  TsumugiPhysicalResumeLayoutSeedV1 layout_seed;
  DurablePhase durable_phase{DurablePhase::preparing};
  // Empty is accepted only for a legacy v1 checkpoint whose target was
  // proven wholly initial or wholly zero. New v2 preparing checkpoints always
  // carry the complete bounded sector evidence.
  std::vector<TsumugiPhysicalResumePreparationSectorV1>
      preparation_sectors;
  std::uint64_t verified_payload_bytes{};
  std::uint64_t verified_segment_count{};
  std::uint64_t expected_payload_bytes{};
  std::uint64_t expected_segment_count{};
  std::vector<TsumugiPhysicalResumePayloadSegmentV1> segments;
};

using TsumugiPhysicalResumeCheckpointCommitV1 =
    std::function<clonecore::Status(
        std::uint64_t,
        const TsumugiPhysicalResumePayloadSegmentV1&)>;

using TsumugiPhysicalResumePhaseCommitV1 =
    std::function<clonecore::Status()>;

// Injectable only for isolated synthetic tests.  The production factory
// binds this seam directly to read_verified_tsumugi_file_v1.
using TsumugiPhysicalResumeImageReaderV1 = std::function<clonecore::Result<
    TsumugiStreamRestoreReport>(
    const TsumugiStreamVerifyRequest&,
    const TsumugiVerifiedChunkCallback&,
    const clonecore::DiskOperationCallbacks&,
    const TsumugiVerifiedInspectionGate&)>;

struct TsumugiPhysicalResumeEngineDependenciesV1 final {
  TsumugiPhysicalResumeImageReaderV1 read_verified_image;
};

struct TsumugiPhysicalResumeEngineReportV1 final {
  std::uint64_t resumed_verified_payload_bytes{};
  std::uint64_t resumed_verified_segment_count{};
  std::uint64_t final_verified_payload_bytes{};
  std::uint64_t final_verified_segment_count{};
  bool full_image_reverified_on_same_handle_before_first_write{};
  bool target_and_incomplete_layout_reidentified_before_first_write{};
  bool verified_prefix_was_not_rewritten{};
  bool every_new_segment_flushed_and_read_back{};
  bool final_layout_committed{};
};

// Low-level exact/rescue whole-disk resume engine.  It owns the opened target
// writer, re-verifies the image with a strict two-pass reader, re-reads every
// durable prefix segment, writes only the suffix, and advances the checkpoint
// callback only after a complete segment flush/readback.  Layout publication
// remains the last operation.
[[nodiscard]] clonecore::Result<TsumugiPhysicalResumeEngineReportV1>
execute_tsumugi_physical_whole_disk_resume_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    std::span<const std::uint32_t> disallowed_mbr_signatures,
    const TsumugiPhysicalResumeCursorV1& cursor,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const TsumugiPhysicalResumePhaseCommitV1& preparation_commit,
    const TsumugiPhysicalResumeCheckpointCommitV1& checkpoint_commit,
    const TsumugiPhysicalResumePhaseCommitV1& commit_ready_commit,
    const TsumugiPhysicalResumeEngineDependenciesV1& dependencies,
    const clonecore::DiskOperationCallbacks& callbacks = {});

[[nodiscard]] clonecore::Result<TsumugiPhysicalResumeEngineReportV1>
execute_tsumugi_physical_whole_disk_resume_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    std::span<const std::uint32_t> disallowed_mbr_signatures,
    const TsumugiPhysicalResumeCursorV1& cursor,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const TsumugiPhysicalResumePhaseCommitV1& preparation_commit,
    const TsumugiPhysicalResumeCheckpointCommitV1& checkpoint_commit,
    const TsumugiPhysicalResumePhaseCommitV1& commit_ready_commit,
    const clonecore::DiskOperationCallbacks& callbacks = {});

}  // namespace ytec::imageformat

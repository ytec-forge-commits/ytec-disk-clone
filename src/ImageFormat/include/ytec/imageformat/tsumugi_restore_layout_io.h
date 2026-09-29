#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/tsumugi_restore_layout.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ytec::imageformat {

struct TsumugiRestoreLayoutIoReport final {
  std::uint64_t invalidated_bytes{};
  std::uint64_t staged_metadata_bytes{};
  std::uint64_t committed_metadata_bytes{};
  bool every_write_read_back_verified{};
  bool primary_partition_table_committed{};
  bool target_left_incomplete{};
  // True only when a failed final metadata publication was followed by a
  // successful flush/readback-verified removal of every exposed table sector.
  bool emergency_reinvalidation_verified{};
};

enum class TsumugiRestoreLayoutPublicationStateV1 : std::uint8_t {
  all_zero,
  known_write_prefix,
  all_final,
};

struct TsumugiRestoreLayoutPublicationInspectionV1 final {
  TsumugiRestoreLayoutPublicationStateV1 state{
      TsumugiRestoreLayoutPublicationStateV1::all_zero};
  std::uint64_t published_write_count{};
  std::uint64_t total_write_count{};
};

// Read-only restart classifier for the deterministic staged+commit sequence.
// Every complete metadata write must be either all zero or byte-for-byte equal
// to this operation's expected bytes, and expected writes must form one prefix.
// A torn, foreign, ambiguous, or out-of-order range fails without target write.
[[nodiscard]] clonecore::Result<
    TsumugiRestoreLayoutPublicationInspectionV1>
inspect_tsumugi_whole_disk_restore_layout_publication_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& plan,
    clonecore::ITargetDiskWriter& target,
    std::size_t verification_block_bytes = 4U * 1024U * 1024U);

// Stateful target-only metadata transaction. prepare() invalidates both table
// locations. commit() stages non-recognizable GPT entry arrays, then performs
// the non-cancellable header/MBR commit sequence. Payload writes are performed
// by TsumugiBlockRestoreTransaction between these two calls.
class TsumugiWholeDiskRestoreLayoutTransaction final {
 public:
  TsumugiWholeDiskRestoreLayoutTransaction(
      TsumugiWholeDiskRestoreLayoutPlan plan,
      clonecore::ITargetDiskWriter& target) noexcept;

  TsumugiWholeDiskRestoreLayoutTransaction(
      const TsumugiWholeDiskRestoreLayoutTransaction&) = delete;
  TsumugiWholeDiskRestoreLayoutTransaction& operator=(
      const TsumugiWholeDiskRestoreLayoutTransaction&) = delete;

  [[nodiscard]] clonecore::Result<TsumugiRestoreLayoutIoReport> prepare(
      const clonecore::DiskOperationCallbacks& callbacks = {});
  // Reopens an already-invalidated transaction after process restart.  It
  // performs no write and succeeds only when every exact staged/final
  // metadata range is still zero on the same opened target.
  [[nodiscard]] clonecore::Result<TsumugiRestoreLayoutIoReport>
  resume_prepared(
      std::size_t verification_block_bytes = 4U * 1024U * 1024U);
  // Reinvalidates a previously inspected, operation-owned publication prefix
  // before replaying the canonical commit order.  The caller must first prove
  // the durable full-payload/commit-ready checkpoint and known prefix state.
  [[nodiscard]] clonecore::Result<TsumugiRestoreLayoutIoReport>
  reinvalidate_publication_prefix(
      std::size_t verification_block_bytes = 4U * 1024U * 1024U);
  [[nodiscard]] clonecore::Result<TsumugiRestoreLayoutIoReport> commit(
      const clonecore::DiskOperationCallbacks& callbacks = {});

  // A partially restored whole disk must stay offline and unrecognizable.
  // Original metadata is deliberately not restored over overwritten payload.
  void abort() noexcept;

  [[nodiscard]] const TsumugiRestoreLayoutIoReport& report() const noexcept;

 private:
  TsumugiWholeDiskRestoreLayoutPlan plan_;
  clonecore::ITargetDiskWriter* target_{};
  TsumugiRestoreLayoutIoReport report_{};
  bool prepared_{};
  bool committed_{};
  bool terminal_failure_{};
  bool metadata_may_be_recognizable_{};
};

struct TsumugiPreservingPartitionLayoutIoReportV1 final {
  std::uint64_t published_metadata_bytes{};
  std::uint64_t rollback_metadata_bytes{};
  bool every_write_read_back_verified{};
  bool partition_table_committed{};
  bool rollback_attempted{};
  bool rollback_read_back_verified{};
  bool original_layout_preserved_after_failure{};
  bool target_left_incomplete{};
};

// Publishes exactly one new GPT/MBR partition after its payload has passed
// readback. GPT always updates and verifies the backup copy before touching
// the primary copy. Any runtime failure restores the exact captured metadata;
// MBR uses the same rollback rule for its single sector.
class TsumugiPreservingPartitionLayoutTransactionV1 final {
 public:
  TsumugiPreservingPartitionLayoutTransactionV1(
      TsumugiPreservingPartitionLayoutPlanV1 plan,
      clonecore::ITargetDiskWriter& target) noexcept;

  TsumugiPreservingPartitionLayoutTransactionV1(
      const TsumugiPreservingPartitionLayoutTransactionV1&) = delete;
  TsumugiPreservingPartitionLayoutTransactionV1& operator=(
      const TsumugiPreservingPartitionLayoutTransactionV1&) = delete;

  [[nodiscard]] clonecore::Result<
      TsumugiPreservingPartitionLayoutIoReportV1>
  prepare(const clonecore::DiskOperationCallbacks& callbacks = {});

  [[nodiscard]] clonecore::Result<
      TsumugiPreservingPartitionLayoutIoReportV1>
  commit(const clonecore::DiskOperationCallbacks& callbacks = {});

  void abort() noexcept;

  [[nodiscard]] const TsumugiPreservingPartitionLayoutIoReportV1& report()
      const noexcept;

 private:
  [[nodiscard]] clonecore::Status rollback() noexcept;

  TsumugiPreservingPartitionLayoutPlanV1 plan_;
  clonecore::ITargetDiskWriter* target_{};
  TsumugiPreservingPartitionLayoutIoReportV1 report_{};
  bool prepared_{};
  bool committed_{};
  bool terminal_failure_{};
  bool rollback_required_{};
};

// Audit state for the one-partition-at-a-time construction transaction used
// by shrink restore. A published temporary GPT is intentionally recognizable
// so that the platform can format and apply that one volume, but it is never a
// boot-capable or final layout. metadata_safely_withheld is true only after
// readback-verified removal of every temporary GPT metadata range.
struct TsumugiShrinkRestoreLayoutIoReportV1 final {
  TsumugiRestoreLayoutIoReport final_layout;
  std::uint64_t temporary_metadata_published_bytes{};
  std::uint64_t temporary_metadata_retired_bytes{};
  std::size_t construction_layout_count{};
  std::size_t published_construction_layouts{};
  std::size_t retired_construction_layouts{};
  std::optional<std::uint32_t> active_final_target_number;
  std::optional<std::uint32_t> active_source_table_index;
  bool every_temporary_write_read_back_verified{};
  bool temporary_layout_active{};
  bool metadata_safely_withheld{};
  bool target_left_incomplete{};
  bool final_partition_table_committed{};
};

// Target-only state machine for shrink restore metadata. prepare() removes the
// old target tables. Exactly one construction GPT may then be published at a
// time. Its publication and retirement writes are non-cancellable once they
// start. The final reviewed GPT/MBR can be committed only after every
// construction GPT has been retired with flush/readback verification.
//
// Filesystem formatting/application and payload verification stay in the
// platform/service layer; this transaction owns only the target metadata
// safety boundary and never accesses the source disk.
class TsumugiShrinkRestoreLayoutTransactionV1 final {
 public:
  TsumugiShrinkRestoreLayoutTransactionV1(
      TsumugiShrinkWholeDiskRestoreLayoutPlanV1 final_plan,
      std::vector<TsumugiShrinkConstructionLayoutPlanV1>
          construction_plans,
      clonecore::ITargetDiskWriter& target);

  TsumugiShrinkRestoreLayoutTransactionV1(
      const TsumugiShrinkRestoreLayoutTransactionV1&) = delete;
  TsumugiShrinkRestoreLayoutTransactionV1& operator=(
      const TsumugiShrinkRestoreLayoutTransactionV1&) = delete;

  [[nodiscard]] clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>
  prepare(const clonecore::DiskOperationCallbacks& callbacks = {});

  [[nodiscard]] clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>
  publish_construction(
      std::uint32_t final_target_number,
      const clonecore::DiskOperationCallbacks& callbacks = {});

  [[nodiscard]] clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>
  retire_construction(
      std::uint32_t final_target_number,
      const clonecore::DiskOperationCallbacks& callbacks = {});

  [[nodiscard]] clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>
  commit_final(const clonecore::DiskOperationCallbacks& callbacks = {});

  // If a temporary GPT is active (or its first cleanup was not verifiable),
  // abort retries exact retirement before delegating to final-layout abort.
  // Repeated calls are safe and keep retrying an unresolved cleanup.
  void abort() noexcept;

  [[nodiscard]] const TsumugiShrinkRestoreLayoutIoReportV1& report()
      const noexcept;

  [[nodiscard]] const TsumugiShrinkConstructionLayoutPlanV1*
  active_construction_plan() const noexcept;

 private:
  enum class ConstructionState : std::uint8_t {
    pending,
    active,
    retired,
  };

  [[nodiscard]] std::optional<std::size_t> find_construction(
      std::uint32_t final_target_number) const noexcept;
  void refresh_report() noexcept;

  TsumugiShrinkWholeDiskRestoreLayoutPlanV1 final_plan_;
  std::vector<TsumugiShrinkConstructionLayoutPlanV1> construction_plans_;
  std::vector<ConstructionState> construction_states_;
  clonecore::ITargetDiskWriter* target_{};
  TsumugiWholeDiskRestoreLayoutTransaction final_transaction_;
  TsumugiShrinkRestoreLayoutIoReportV1 report_{};
  std::optional<std::size_t> active_construction_index_;
  bool combined_plan_validated_{};
  bool prepared_{};
  bool final_committed_{};
  bool terminal_failure_{};
  bool active_construction_published_{};
  bool temporary_cleanup_unverified_{};
};

}  // namespace ytec::imageformat

#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/directshrink/target_contract.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/operationcore/operation.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::winpeapp {

// Canonical source-volume identity captured while the physical source is
// already held read-only by WinPE.  table_index remains the GPT source-table
// identity; a UI row number or a drive letter is never accepted as a binding.
struct WinPeOfflineNtfsVolumeBinding final {
  std::uint32_t source_table_index{};
  std::uint64_t source_offset_bytes{};
  std::uint64_t source_size_bytes{};
  std::wstring volume_guid_path;
};

// Complete source epoch used by planning and repeated before target I/O,
// before every WIM capture, and immediately before final GPT publication.
// canonical_epoch_hash is recomputed by
// hash_winpe_offline_ntfs_source_epoch_v1(); caller-provided digests are never
// trusted on their own.
struct WinPeOfflineNtfsSourceEpochEvidence final {
  clonecore::StableDiskIdentity observed_source;
  imageformat::Sha256Digest source_layout_hash{};
  imageformat::Sha256Digest source_partition_snapshot_hash{};
  imageformat::Sha256Digest source_analysis_hash{};
  imageformat::Sha256Digest canonical_epoch_hash{};
  std::vector<WinPeOfflineNtfsVolumeBinding> ntfs_volumes;
  std::uint32_t logical_sector_size{};
  bool stable_identity_reidentified{};
  bool source_os_read_only{};
  bool physical_handle_read_only{};
  bool gpt_source{};
  bool whole_disk_analysis{};
  bool bitlocker_fully_decrypted{};
};

// Additional product-boundary facts which are not encoded by a partition
// layout alone.  Every field is retained in the immutable WinPE plan hash.
struct WinPeOfflineNtfsPlanningEvidence final {
  WinPeOfflineNtfsSourceEpochEvidence source_epoch;
  std::string analysis_created_utc;
  std::string app_version;
  bool winpe_environment_verified{};
  bool source_contains_windows{};
  bool source_supported_basic_disk{};
  bool source_health_allows_standard_clone{};
  bool target_supported_fixed_disk{};
  bool target_non_system{};
  bool target_health_allows_destructive_clone{};
};

// The immutable partition/capacity decision is the already-normalized shared
// direct-shrink plan retained inside target_plan.  The wrapper changes the
// common OperationPlan environment to WinPE and additionally binds the exact
// read-only source epoch.  It never invokes or stores a VSS workflow result.
class WinPeOfflineNtfsDirectShrinkPlan final {
 public:
  WinPeOfflineNtfsDirectShrinkPlan(
      const WinPeOfflineNtfsDirectShrinkPlan&) = default;
  WinPeOfflineNtfsDirectShrinkPlan(
      WinPeOfflineNtfsDirectShrinkPlan&&) noexcept = default;
  WinPeOfflineNtfsDirectShrinkPlan& operator=(
      const WinPeOfflineNtfsDirectShrinkPlan&) = delete;
  WinPeOfflineNtfsDirectShrinkPlan& operator=(
      WinPeOfflineNtfsDirectShrinkPlan&&) = delete;

  [[nodiscard]] const operationcore::OperationPlan& operation_plan()
      const noexcept {
    return operation_plan_;
  }
  [[nodiscard]] const directshrink::TargetPlan& target_plan()
      const noexcept {
    return *target_plan_;
  }
  [[nodiscard]] const WinPeOfflineNtfsPlanningEvidence& planning_evidence()
      const noexcept {
    return planning_evidence_;
  }

 private:
  WinPeOfflineNtfsDirectShrinkPlan() = default;

  operationcore::OperationPlan operation_plan_;
  std::shared_ptr<const directshrink::TargetPlan> target_plan_;
  WinPeOfflineNtfsPlanningEvidence planning_evidence_;

  friend clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>
  build_winpe_offline_ntfs_direct_shrink_plan(
      directshrink::TargetPlan,
      WinPeOfflineNtfsPlanningEvidence);
};

// Pure canonical hash.  Bindings are sorted by source_table_index so the
// caller's enumeration order is not an identity input; duplicate indexes or
// duplicate Volume GUID roots fail closed.
[[nodiscard]] clonecore::Result<imageformat::Sha256Digest>
hash_winpe_offline_ntfs_source_epoch_v1(
    const WinPeOfflineNtfsSourceEpochEvidence& evidence);

// Pure wrapper construction.  The current product slice is deliberately
// narrow: whole-disk GPT preserve, 512-byte logical sectors, selected NTFS
// payloads only, fully decrypted source, separate fixed non-system target,
// and the shared three-way surplus policy.  Required partitions cannot be
// omitted by the retained mapping.
[[nodiscard]] clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>
build_winpe_offline_ntfs_direct_shrink_plan(
    directshrink::TargetPlan target_plan,
    WinPeOfflineNtfsPlanningEvidence planning_evidence);

struct WinPeOfflineNtfsCaptureLeaseEvidence final {
  WinPeOfflineNtfsSourceEpochEvidence source_epoch;
  WinPeOfflineNtfsVolumeBinding volume;
  // Product WinPE uses the exact canonical Volume GUID root.  It is not a
  // ShadowCopy path and must remain fixed while the lease object is alive.
  std::wstring capture_device_path;
  std::wstring capture_identity;
  bool source_disk_handle_held_read_only{};
  bool source_volume_handle_held_read_only{};
  bool source_volume_extent_reverified{};
};

// Lifetime guard for the read-only physical source and exact source volume.
// The target WIM capture must finish before this object is destroyed.
class IWinPeOfflineNtfsCaptureLease {
 public:
  virtual ~IWinPeOfflineNtfsCaptureLease() = default;
  [[nodiscard]] virtual const WinPeOfflineNtfsCaptureLeaseEvidence& evidence()
      const noexcept = 0;
};

// Source-only WinPE boundary.  No method returns a Writer or writable source
// handle.  lock_source_read_only_and_revalidate() sets the OS disk attribute
// read-only and deliberately does not restore it at completion or failure.
class IWinPeOfflineNtfsSourceGuard {
 public:
  virtual ~IWinPeOfflineNtfsSourceGuard() = default;

  [[nodiscard]] virtual clonecore::Result<
      WinPeOfflineNtfsSourceEpochEvidence>
  lock_source_read_only_and_revalidate(
      const WinPeOfflineNtfsDirectShrinkPlan& plan) = 0;

  [[nodiscard]] virtual clonecore::Result<std::unique_ptr<
      IWinPeOfflineNtfsCaptureLease>>
  acquire_capture_lease_after_revalidation(
      const WinPeOfflineNtfsDirectShrinkPlan& plan,
      const directshrink::PartitionTask& task) = 0;

  [[nodiscard]] virtual clonecore::Result<
      WinPeOfflineNtfsSourceEpochEvidence>
  revalidate_immediately_before_final_commit(
      const WinPeOfflineNtfsDirectShrinkPlan& plan) = 0;

  [[nodiscard]] virtual bool source_left_os_read_only() const noexcept = 0;
};

using WinPeOfflineNtfsSelectionReidentifier = std::function<
    clonecore::Result<diskmodel::ReidentifiedPhysicalClone>(
        const clonecore::StableDiskIdentity&,
        const clonecore::StableDiskIdentity&)>;

using WinPeOfflineNtfsConfirmedReidentifier = std::function<
    clonecore::Result<diskmodel::ReidentifiedPhysicalClone>(
        const clonecore::StableDiskIdentity&,
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&)>;

using WinPeOfflineNtfsSourceGuardFactory = std::function<clonecore::Result<
    std::unique_ptr<IWinPeOfflineNtfsSourceGuard>>(
        const WinPeOfflineNtfsDirectShrinkPlan&,
        const diskmodel::ReidentifiedPhysicalClone&)>;

// Target-only construction factory.  The concrete adapter may share the
// audited construction/checkpoint/boot/commit-last implementation, but the
// source controller above replaces the Windows VSS workflow completely.
using WinPeOfflineNtfsTargetPlatformFactory = std::function<clonecore::Result<
    std::unique_ptr<directshrink::ITargetPlatform>>(
        const WinPeOfflineNtfsDirectShrinkPlan&,
        const diskmodel::ReidentifiedPhysicalClone&)>;

using WinPeOfflineNtfsReadOnlySourceOpener = std::function<
    clonecore::Result<diskmodel::ReadOnlyPhysicalDiskHandle>(
        const clonecore::StableDiskIdentity&)>;

struct WinPeOfflineNtfsDirectShrinkDependencies final {
  WinPeOfflineNtfsSelectionReidentifier reidentify_selection;
  WinPeOfflineNtfsConfirmedReidentifier reidentify_confirmed;
  WinPeOfflineNtfsSourceGuardFactory make_source_guard;
  WinPeOfflineNtfsTargetPlatformFactory make_target_platform;
  WinPeOfflineNtfsReadOnlySourceOpener open_read_only_raw_source;
};

struct WinPeOfflineNtfsDirectShrinkExecutionOptions final {
  clonecore::TargetConfirmation confirmation;
  clonecore::DiskOperationCallbacks callbacks;
};

struct WinPeOfflineNtfsDirectShrinkExecutionReport final {
  std::uint64_t applied_archive_count{};
  std::uint64_t copied_exact_raw_count{};
  std::uint64_t source_epoch_revalidation_count{};
  std::uint64_t verified_target_bytes{};
  imageformat::Sha256Digest aggregate_write_digest{};
  imageformat::Sha256Digest precomputed_retired_completion_hash{};
  imageformat::Sha256Digest precomputed_pending_completion_hash{};
  imageformat::Sha256Digest completion_evidence_hash{};
  directshrink::CheckpointEvidence commit_ready_checkpoint;
  directshrink::BootEvidence boot;
  directshrink::FinalCommitEvidence final_commit;
  WinPeOfflineNtfsSourceEpochEvidence final_source_epoch;
  bool source_locked_read_only_before_target_io{};
  bool every_capture_used_exact_read_only_lease{};
  bool source_epoch_rechecked_after_boot_before_final_commit{};
  bool construction_layout_non_bootable{};
  bool durable_checkpoint_preceded_payload{};
  bool final_gpt_committed_last{};
  bool source_left_os_read_only{};
  bool target_left_offline{};
  bool real_boot_not_proven{};
};

struct WinPeOfflineNtfsDirectShrinkOperationReport final {
  operationcore::OperationPlan plan;
  operationcore::OperationResult lifecycle;
  std::optional<WinPeOfflineNtfsDirectShrinkExecutionReport> execution;
};

// Executes the WinPE state machine without VSS:
// source read-only/epoch gate -> non-boot BasicData construction -> durable
// checkpoint -> one exact offline NTFS WIM capture/apply/readback at a time ->
// final extents -> optional Boot/WinRE -> final source epoch gate -> final GPT
// commit last -> target offline.  Any pre-publication failure or cancellation
// invokes abort_keep_offline_incomplete().
[[nodiscard]] clonecore::Result<
    WinPeOfflineNtfsDirectShrinkOperationReport>
execute_winpe_offline_ntfs_direct_shrink_clone(
    const WinPeOfflineNtfsDirectShrinkPlan& plan,
    const WinPeOfflineNtfsDirectShrinkExecutionOptions& options,
    const WinPeOfflineNtfsDirectShrinkDependencies& dependencies);

}  // namespace ytec::winpeapp

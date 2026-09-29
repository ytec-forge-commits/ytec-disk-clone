#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/tsumugi_restore_layout.h"
#include "ytec/migrationcore/file_system_recreate.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::migrationengine {

// A production Win32 target factory exists below, but the public product path
// remains deliberately disconnected until Windows/WinPE orchestration can
// supply the complete reviewed whole-disk construction plan and surface every
// typed failure state.  A callable test seam or standalone factory alone must
// never turn this product-reachability flag on.
inline constexpr bool
    kWindowsFileSystemRecreateProductionTargetAdapterConnected = false;

inline constexpr std::size_t
    kDefaultWindowsFileSystemRecreateMaximumDepth = 256U;
inline constexpr std::size_t
    kDefaultWindowsFileSystemRecreateTransferBytes = 1024U * 1024U;
inline constexpr std::size_t
    kMaximumWindowsFileSystemRecreateStreamQueryBytes = 1024U * 1024U;

enum class WindowsFileSystemRecreateSourceRootKind : std::uint8_t {
  canonical_volume_guid,
  vss_snapshot_device,
};

struct WindowsFileSystemRecreateSourceLimits final {
  std::size_t maximum_entries{
      migrationcore::kMaximumFileSystemRecreateEntries};
  std::size_t maximum_depth{
      kDefaultWindowsFileSystemRecreateMaximumDepth};
  std::uint32_t maximum_path_utf16_units{
      migrationcore::kMaximumFileSystemRecreatePathUtf16Units};
  std::uint64_t maximum_file_bytes{
      migrationcore::kExfatMaximumRecreatedFileBytes};
  std::size_t maximum_stream_query_bytes{
      kMaximumWindowsFileSystemRecreateStreamQueryBytes};
};

using WindowsFileSystemRecreateEpochTokenObserver = std::function<
    clonecore::Result<migrationcore::FileSystemRecreateSha256>()>;

// expected_source_epoch_token_sha256 is owned by the surrounding VSS or
// read-only-source lifecycle.  observe_source_epoch_token must freshly return
// that lifecycle token before and after namespace observation, and again at
// every target mutation boundary.  A remembered token without an observer is
// rejected.
struct WindowsFileSystemRecreateSourceRequest final {
  clonecore::StableDiskIdentity expected_source_disk;
  std::uint32_t source_table_index{};
  std::uint64_t source_partition_offset_bytes{};
  std::uint64_t source_partition_length_bytes{};
  migrationcore::MigrationFileSystem expected_file_system{
      migrationcore::MigrationFileSystem::fat32};
  std::wstring source_root_path;
  migrationcore::FileSystemRecreateSha256
      expected_source_epoch_token_sha256{};
  WindowsFileSystemRecreateEpochTokenObserver
      observe_source_epoch_token;
  WindowsFileSystemRecreateSourceLimits limits;
};

struct FileSystemRecreateSourceEpochEvidence final {
  clonecore::StableDiskIdentity observed_source_disk;
  migrationcore::FileSystemRecreateSha256 enumeration_epoch_sha256{};
  std::uint32_t source_table_index{};
  std::uint64_t source_partition_offset_bytes{};
  std::uint64_t source_partition_length_bytes{};
  std::uint64_t freshness_sequence{};
  bool root_file_id_stable{};
  bool exact_single_extent{};
  bool source_token_reidentified{};
};

// A source file is reopened component-by-component below the retained root
// handle.  read_next() is sequential and bounded.  finish_and_verify() only
// succeeds after exact EOF, an unchanged FileId/size/all-times observation,
// and a SHA-256 match with the sealed canonical tree entry.
class IFileSystemRecreateSourceFile {
 public:
  virtual ~IFileSystemRecreateSourceFile() = default;

  [[nodiscard]] virtual std::uint64_t expected_size_bytes() const noexcept = 0;

  [[nodiscard]] virtual clonecore::Result<std::vector<std::byte>> read_next(
      std::size_t maximum_bytes) = 0;

  [[nodiscard]] virtual clonecore::Status finish_and_verify() = 0;
};

class IFileSystemRecreateSourceSession {
 public:
  virtual ~IFileSystemRecreateSourceSession() = default;

  [[nodiscard]] virtual const clonecore::StableDiskIdentity& source_disk()
      const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t source_table_index() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t source_partition_offset_bytes()
      const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t source_partition_length_bytes()
      const noexcept = 0;
  [[nodiscard]] virtual const migrationcore::CanonicalFileSystemTree&
  canonical_tree() const noexcept = 0;

  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateSourceEpochEvidence>
  revalidate_source_epoch() = 0;

  [[nodiscard]] virtual clonecore::Result<
      std::unique_ptr<IFileSystemRecreateSourceFile>>
  open_regular_file(
      std::size_t canonical_entry_index,
      const migrationcore::CanonicalFileSystemTreeEntry& expected_entry) = 0;
};

// Opens only a canonical Volume GUID root or a VSS snapshot device root.  It
// uses a retained root handle, RootDirectory-relative NtCreateFile one
// component at a time, FILE_OPEN_REPARSE_POINT, handle enumeration and CNG
// SHA-256.  It never opens a source object for write and performs no target I/O.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<IFileSystemRecreateSourceSession>>
open_windows_file_system_recreate_source_session_read_only(
    const WindowsFileSystemRecreateSourceRequest& request);

struct FileSystemRecreateTargetSelection final {
  clonecore::StableDiskIdentity expected_target_disk;
  std::uint32_t target_partition_number{};
  std::uint64_t target_partition_offset_bytes{};
  std::uint64_t target_partition_length_bytes{};
  bool reviewed_as_active_rescue_media{};
};

// The target cannot honestly remain physically offline while Windows formats
// and populates a filesystem.  This typed state distinguishes the two safe
// phases instead of treating a limited construction volume as an offline
// disk.  The online state is accepted only while a retained, non-reparse
// Volume GUID root is freshly bound to the exact reviewed single-disk extent.
enum class FileSystemRecreateTargetIsolationState : std::uint8_t {
  physical_disk_offline = 1U,
  construction_volume_online_exclusive = 2U,
};

// Adds source/target stable identities and the exact target extent to the pure
// MigrationCore plan.  The resulting hashes are the guard values that every
// future mutation adapter must revalidate internally.
class WindowsFileSystemRecreateExecutionPlan final {
 public:
  WindowsFileSystemRecreateExecutionPlan(
      const WindowsFileSystemRecreateExecutionPlan&) = default;
  WindowsFileSystemRecreateExecutionPlan(
      WindowsFileSystemRecreateExecutionPlan&&) noexcept = default;
  WindowsFileSystemRecreateExecutionPlan& operator=(
      const WindowsFileSystemRecreateExecutionPlan&) = delete;
  WindowsFileSystemRecreateExecutionPlan& operator=(
      WindowsFileSystemRecreateExecutionPlan&&) = delete;

  [[nodiscard]] const migrationcore::FileSystemRecreatePlan& core_plan()
      const noexcept {
    return core_plan_;
  }
  [[nodiscard]] const clonecore::StableDiskIdentity& source_disk()
      const noexcept {
    return source_disk_;
  }
  [[nodiscard]] const FileSystemRecreateTargetSelection& target()
      const noexcept {
    return target_;
  }
  [[nodiscard]] const migrationcore::FileSystemRecreateSha256&
  target_binding_sha256() const noexcept {
    return target_binding_sha256_;
  }
  [[nodiscard]] const migrationcore::FileSystemRecreateSha256&
  execution_plan_sha256() const noexcept {
    return execution_plan_sha256_;
  }

 private:
  WindowsFileSystemRecreateExecutionPlan(
      migrationcore::FileSystemRecreatePlan core_plan,
      clonecore::StableDiskIdentity source_disk,
      FileSystemRecreateTargetSelection target,
      migrationcore::FileSystemRecreateSha256 target_binding_sha256,
      migrationcore::FileSystemRecreateSha256 execution_plan_sha256);

  migrationcore::FileSystemRecreatePlan core_plan_;
  clonecore::StableDiskIdentity source_disk_;
  FileSystemRecreateTargetSelection target_;
  migrationcore::FileSystemRecreateSha256 target_binding_sha256_{};
  migrationcore::FileSystemRecreateSha256 execution_plan_sha256_{};

  friend clonecore::Result<WindowsFileSystemRecreateExecutionPlan>
  bind_windows_file_system_recreate_execution_plan(
      const migrationcore::FileSystemRecreatePlan&,
      const IFileSystemRecreateSourceSession&,
      const FileSystemRecreateTargetSelection&);
};

[[nodiscard]] clonecore::Result<WindowsFileSystemRecreateExecutionPlan>
bind_windows_file_system_recreate_execution_plan(
    const migrationcore::FileSystemRecreatePlan& core_plan,
    const IFileSystemRecreateSourceSession& source,
    const FileSystemRecreateTargetSelection& target);

struct FileSystemRecreateTargetObservation final {
  clonecore::StableDiskIdentity observed_target_disk;
  std::uint32_t target_partition_number{};
  std::uint64_t target_partition_offset_bytes{};
  std::uint64_t target_partition_length_bytes{};
  std::uint64_t freshness_sequence{};
  bool freshly_reidentified{};
  bool reviewed_extent_within_disk{};
  bool exact_partition_extent{};
  FileSystemRecreateTargetIsolationState isolation_state{
      FileSystemRecreateTargetIsolationState::physical_disk_offline};
  bool exact_target_handle_retained{};
  bool root_file_id_stable{};
  bool root_is_non_reparse{};
  bool active_rescue_media{};
};

struct FileSystemRecreateMutationGuard final {
  migrationcore::FileSystemRecreateSha256 execution_plan_sha256{};
  migrationcore::FileSystemRecreateSha256 source_epoch_sha256{};
  migrationcore::FileSystemRecreateSha256 target_binding_sha256{};
};

struct FileSystemRecreateMutationEvidence final {
  FileSystemRecreateMutationGuard accepted_guard;
  bool guard_revalidated_inside_adapter{};
  bool exact_target_handle_retained{};
  FileSystemRecreateTargetIsolationState isolation_state{
      FileSystemRecreateTargetIsolationState::physical_disk_offline};
  bool completion_incomplete{};
  bool flushed{};
};

struct FileSystemRecreateFormattedTargetObservation final {
  FileSystemRecreateTargetObservation target;
  migrationcore::FileSystemRecreateFormatGeometry actual_geometry;
  std::uint32_t root_reparse_tag{};
  bool root_is_directory{};
  bool root_opened_handle_identity_stable{};
};

struct FileSystemRecreateTargetFile final {
  std::uint64_t opened_handle_token{};
  FileSystemRecreateMutationEvidence mutation;
};

struct FileSystemRecreateTargetWrite final {
  std::size_t bytes_written{};
  FileSystemRecreateMutationEvidence mutation;
};

struct FileSystemRecreateCompletionEvidence final {
  FileSystemRecreateMutationGuard accepted_guard;
  migrationcore::FileSystemRecreateSha256 verified_manifest_sha256{};
  migrationcore::FileSystemRecreateSha256 target_epoch_sha256{};
  bool guard_revalidated_inside_adapter{};
  bool exact_target_reidentified{};
  bool complete_readback_verified{};
  bool completion_committed_last{};
  bool target_offline{};
  // Publication is a one-way safety boundary.  cleanup_pending is restricted
  // to non-mutating handle/mount cleanup after a fully verified latch; it does
  // not permit a target mutation after completion_committed_last.
  bool publication_attempted{};
  bool publication_latched{};
  bool publication_readback_verified{};
  bool cleanup_pending{};
  bool incomplete_use_prohibited{};
};

enum class FileSystemRecreateCommitDisposition : std::uint8_t {
  completed,
  prepublication_failure,
  partial_publication_use_prohibited,
};

// A production target is deliberately narrower than the pure planner.  The
// first connected slice accepts one reviewed FAT32/exFAT data partition in a
// complete whole-disk shrink layout.  Supporting a mixed disk requires an
// outer multi-partition transaction; this factory must not publish a partial
// mixed layout on its own.
struct WindowsFileSystemRecreateProductionTargetRequest final {
  clonecore::TargetConfirmation confirmation;
  imageformat::Sha256Digest expected_original_target_layout_sha256{};
  imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 reviewed_layout;
  std::vector<imageformat::TsumugiShrinkConstructionLayoutPlanV1>
      reviewed_construction_layouts;
  bool target_is_active_rescue_media{};
  clonecore::DiskOperationCallbacks callbacks;
};

struct WindowsFileSystemRecreateTargetDiskObservation final {
  diskmodel::ReidentifiedPhysicalTarget physical;
  imageformat::Sha256Digest current_layout_sha256{};
  bool target_class_accepted{};
};

struct WindowsFileSystemRecreateConstructionVolumeBinding final {
  std::uint32_t final_target_number{};
  std::uint32_t disk_number{};
  std::uint64_t target_offset{};
  std::uint64_t target_size{};
  std::wstring canonical_volume_guid_path;
  bool exact_single_disk_extent{};
};

struct WindowsFileSystemRecreateFormattedRootObservation final {
  WindowsFileSystemRecreateConstructionVolumeBinding volume;
  migrationcore::FileSystemRecreateFormatGeometry actual_geometry;
  std::uint32_t root_reparse_tag{};
  bool root_is_directory{};
  bool root_opened_handle_identity_stable{};
};

// Narrow Win32 seam.  The production implementation owns the exact Volume
// GUID root and all created object handles.  Tests may replace this seam, but
// doing so never changes the production-connected flag.
class IWindowsFileSystemRecreateTargetIo {
 public:
  virtual ~IWindowsFileSystemRecreateTargetIo() = default;

  [[nodiscard]] virtual clonecore::Result<
      WindowsFileSystemRecreateTargetDiskObservation>
  observe_target_read_only() = 0;
  [[nodiscard]] virtual clonecore::Status set_target_offline(bool offline) = 0;
  [[nodiscard]] virtual clonecore::Result<diskmodel::PhysicalTargetHandle>
  open_offline_target() = 0;
  [[nodiscard]] virtual clonecore::Status notify_layout_changed() = 0;

  [[nodiscard]] virtual clonecore::Result<
      WindowsFileSystemRecreateConstructionVolumeBinding>
  bind_online_construction_volume(
      std::uint32_t final_target_number,
      std::uint64_t target_offset,
      std::uint64_t target_size) = 0;
  [[nodiscard]] virtual clonecore::Status format_exact_volume(
      const WindowsFileSystemRecreateConstructionVolumeBinding& volume,
      const migrationcore::FileSystemRecreateFormatGeometry& desired) = 0;
  [[nodiscard]] virtual clonecore::Result<
      WindowsFileSystemRecreateFormattedRootObservation>
  open_and_inspect_formatted_root(
      const WindowsFileSystemRecreateConstructionVolumeBinding& volume,
      const migrationcore::FileSystemRecreateFormatGeometry& desired) = 0;
  [[nodiscard]] virtual clonecore::Result<
      WindowsFileSystemRecreateFormattedRootObservation>
  revalidate_formatted_root_read_only() = 0;

  [[nodiscard]] virtual clonecore::Status create_directory_no_replace(
      const migrationcore::CanonicalFileSystemTreeEntry& entry) = 0;
  [[nodiscard]] virtual clonecore::Result<std::uint64_t>
  create_file_no_replace(
      const migrationcore::CanonicalFileSystemTreeEntry& entry) = 0;
  [[nodiscard]] virtual clonecore::Result<std::size_t> write_file_chunk(
      std::uint64_t opened_handle_token,
      std::uint64_t offset,
      std::span<const std::byte> bytes) = 0;
  [[nodiscard]] virtual clonecore::Status
  finalize_file_metadata_flush_and_hold(
      std::uint64_t opened_handle_token,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) = 0;
  [[nodiscard]] virtual clonecore::Status
  apply_directory_metadata_and_flush(
      const migrationcore::CanonicalFileSystemTreeEntry& entry) = 0;
  [[nodiscard]] virtual clonecore::Status flush_target_namespace() = 0;
  [[nodiscard]] virtual clonecore::Result<
      migrationcore::FileSystemRecreateTargetReadback>
  enumerate_complete_target_readback_read_only(
      const migrationcore::FileSystemRecreatePlan& plan) = 0;

  // Releases every namespace handle before locking and dismounting the exact
  // Volume.  Success also proves the physical target is offline again.
  [[nodiscard]] virtual clonecore::Status
  close_namespace_dismount_and_offline(
      const WindowsFileSystemRecreateConstructionVolumeBinding& volume) = 0;
};

enum class WindowsFileSystemRecreateTargetLifecycleState : std::uint8_t {
  ready = 1U,
  offline_incomplete = 2U,
  construction_volume_online = 3U,
  namespace_readback_verified = 4U,
  construction_retired_offline = 5U,
  publication_attempted = 6U,
  completed_offline = 7U,
  aborted_offline_incomplete = 8U,
  partial_publication_use_prohibited = 9U,
};

// Allocation-free commit result.  There is deliberately no generic Result
// failure: the adapter must classify every outcome, including failures after
// publication starts.  Only prepublication_failure is abort-safe.  Once
// publication_attempted is true, the controller must never issue a destructive
// rollback/abort; partial state stays offline and prohibited from use.
struct FileSystemRecreateCommitOutcome final {
  FileSystemRecreateCommitDisposition disposition{
      FileSystemRecreateCommitDisposition::prepublication_failure};
  FileSystemRecreateCompletionEvidence evidence;
  clonecore::ErrorCode failure_code{clonecore::ErrorCode::io_failed};
  std::uint32_t native_failure_code{};
};

struct FileSystemRecreateAbortEvidence final {
  FileSystemRecreateMutationGuard accepted_guard;
  bool guard_revalidated_inside_adapter{};
  bool exact_target_handle_retained{};
  bool target_offline{};
  bool completion_incomplete{};
};

// Destructive target-only seam.  The Win32 implementation below is narrower
// than the pure planner and is not yet reachable from a product UI.
// Implementations must retain the
// exact target/root/file handles behind opaque tokens; pathname-only identity
// decisions do not satisfy this contract.  Each mutating method must compare
// all three guard hashes immediately before its own mutation and return the
// accepted values.  No-replace creation is mandatory.
class IFileSystemRecreateTargetPlatform {
 public:
  virtual ~IFileSystemRecreateTargetPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateTargetObservation>
  reidentify_target_read_only() = 0;

  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateMutationEvidence>
  begin_incomplete_target(
      const FileSystemRecreateMutationGuard& guard) = 0;

  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateMutationEvidence>
  format_target_file_system(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::FileSystemRecreateFormatGeometry& desired) = 0;

  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateFormattedTargetObservation>
  inspect_formatted_target_read_only() = 0;

  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateMutationEvidence>
  create_directory_no_replace(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) = 0;

  [[nodiscard]] virtual clonecore::Result<FileSystemRecreateTargetFile>
  create_file_no_replace(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) = 0;

  [[nodiscard]] virtual clonecore::Result<FileSystemRecreateTargetWrite>
  write_file_chunk(
      const FileSystemRecreateMutationGuard& guard,
      const FileSystemRecreateTargetFile& file,
      std::uint64_t offset,
      std::span<const std::byte> bytes) = 0;

  // Applies portable attributes/times through the retained file handle,
  // verifies exact final length, flushes data+metadata, and keeps the handle
  // until the full namespace readback has completed.
  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateMutationEvidence>
  finalize_file_metadata_flush_and_close(
      const FileSystemRecreateMutationGuard& guard,
      const FileSystemRecreateTargetFile& file,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) = 0;

  // Applies metadata in reverse depth order through each retained,
  // non-reparse, write-through directory handle and reads the identity back.
  // The exact Volume handle closes the whole-filesystem flush boundary before
  // dismount/offline.
  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateMutationEvidence>
  apply_directory_metadata_and_flush(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::CanonicalFileSystemTreeEntry& entry) = 0;

  [[nodiscard]] virtual clonecore::Result<
      FileSystemRecreateMutationEvidence>
  flush_target_namespace(
      const FileSystemRecreateMutationGuard& guard) = 0;

  // Must perform the same bounded, opened-handle, no-reparse enumeration and
  // stable-EOF SHA-256 readback as the source observer, over the whole target.
  [[nodiscard]] virtual clonecore::Result<
      migrationcore::FileSystemRecreateTargetReadback>
  enumerate_complete_target_readback_read_only(
      const migrationcore::FileSystemRecreatePlan& plan) = 0;

  // Called only after MigrationCore exact tree/content verification.  The
  // completion marker/layout publication is the final mutation and the target
  // remains offline on success.
  [[nodiscard]] virtual FileSystemRecreateCommitOutcome
  commit_completion_last(
      const FileSystemRecreateMutationGuard& guard,
      const migrationcore::FileSystemRecreateVerification& verification)
      noexcept = 0;

  // Idempotent and allocation-free.  It must be safe after a partial format,
  // short write, cancellation, exception, failed readback, or a classified
  // prepublication failure.  It MUST NOT be called after publication was
  // attempted, latched, or classified partial/use-prohibited.  It may only
  // touch the exact target handle retained by begin_incomplete_target and must
  // compare the immutable execution/source/target guard before doing so.
  [[nodiscard]] virtual FileSystemRecreateAbortEvidence
  abort_keep_offline_incomplete(
      const FileSystemRecreateMutationGuard& guard) noexcept = 0;
};

class IWindowsFileSystemRecreateProductionTargetPlatform
    : public IFileSystemRecreateTargetPlatform {
 public:
  [[nodiscard]] virtual WindowsFileSystemRecreateTargetLifecycleState
  lifecycle_state() const noexcept = 0;
};

// Factory construction performs validation and allocation only.  The first
// destructive write remains begin_incomplete_target().
[[nodiscard]] clonecore::Result<std::unique_ptr<
    IWindowsFileSystemRecreateProductionTargetPlatform>>
make_windows_file_system_recreate_target_platform(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    const WindowsFileSystemRecreateProductionTargetRequest& request);

// Synthetic seam using the identical state machine.  It is intentionally not
// evidence that a product UI or production target path is connected.
[[nodiscard]] clonecore::Result<std::unique_ptr<
    IWindowsFileSystemRecreateProductionTargetPlatform>>
make_windows_file_system_recreate_target_platform_with_io(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    const WindowsFileSystemRecreateProductionTargetRequest& request,
    std::unique_ptr<IWindowsFileSystemRecreateTargetIo> io);

struct FileSystemRecreateExecutionOptions final {
  std::size_t maximum_transfer_bytes{
      kDefaultWindowsFileSystemRecreateTransferBytes};
  clonecore::DiskOperationCallbacks callbacks;
};

struct FileSystemRecreateExecutionReport final {
  migrationcore::FileSystemRecreateVerification verification;
  FileSystemRecreateCompletionEvidence completion;
  std::uint64_t directory_count{};
  std::uint64_t regular_file_count{};
  std::uint64_t copied_file_bytes{};
  std::uint64_t target_reidentification_count{};
  bool every_mutation_guard_revalidated{};
  bool every_file_flushed{};
  bool full_namespace_read_back{};
  bool commit_was_last_mutation{};
  bool target_left_offline{};
  bool publication_latched{};
  bool cleanup_pending{};
  bool incomplete_use_prohibited{};
  bool production_target_adapter_connected{
      kWindowsFileSystemRecreateProductionTargetAdapterConnected};
};

enum class FileSystemRecreateTransactionDisposition : std::uint8_t {
  completed,
  aborted_incomplete,
  partial_publication_use_prohibited,
};

// Pre-I/O validation may still use Result failure.  Once the first target
// mutation is attempted, the controller always returns this typed value.  A
// caller must explicitly require disposition == completed and a populated
// completed_report; Result::has_value() alone never means recreation completed.
struct FileSystemRecreateTransactionOutcome final {
  FileSystemRecreateTransactionDisposition disposition{
      FileSystemRecreateTransactionDisposition::aborted_incomplete};
  std::optional<FileSystemRecreateExecutionReport> completed_report;
  std::optional<FileSystemRecreateCompletionEvidence> completion;
  std::optional<FileSystemRecreateAbortEvidence> abort;
  std::optional<clonecore::Error> error;
};

// Executes only through the injected target contract.  FAT/exFAT in-place
// shrink is not represented.  NTFS remains on the WIM route; raw/unknown data
// remains exact-only and cannot be bound into this controller.
[[nodiscard]] clonecore::Result<FileSystemRecreateTransactionOutcome>
execute_file_system_recreation_on_injected_target(
    const WindowsFileSystemRecreateExecutionPlan& plan,
    IFileSystemRecreateSourceSession& source,
    IFileSystemRecreateTargetPlatform& target,
    const FileSystemRecreateExecutionOptions& options = {});

}  // namespace ytec::migrationengine

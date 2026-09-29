#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/log.h"
#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/vssrequester/snapshot_reader.h"
#include "ytec/vssrequester/windows_backend.h"
#include "ytec/vssrequester/windows_diff_area_observer.h"
#include "ytec/vssrequester/workflow.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

enum class OnlineDirectClonePartitionStyle : std::uint8_t {
  gpt,
  mbr,
};

// This request contains only the immutable selection/confirmation values that
// an OperationPlan needs. It intentionally contains no reservation file,
// disk-number-only handoff, or hidden reboot state.
struct OnlineDirectCloneRequest final {
  bool administrator{};
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  // Read-only inventory layouts shown on the final review screen. Both are
  // re-hashed before VSS work, immediately before target offline, and on the
  // exact opened target handle.
  imageformat::Sha256Digest expected_source_layout_hash{};
  imageformat::Sha256Digest expected_target_layout_hash{};
  clonecore::TargetConfirmation confirmation;
  std::size_t maximum_chunk_bytes{1024U * 1024U};
  vssrequester::AsyncWaitOptions async_wait;
  clonecore::DiskOperationCallbacks callbacks;
  vssrequester::VssDiffAreaReviewCallback diff_area_review_callback;
  const clonecore::Logger* logger{};
};

struct OnlineDirectSnapshotPartition final {
  std::uint32_t partition_index{};
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  std::wstring volume_guid_path;
};

enum class OnlineDirectLockedFileSystem : std::uint8_t {
  fat32,
  exfat,
};

// FAT32/exFAT do not have the VSS consistency contract used for NTFS.  A
// Windows exact clone may include them only while this exact single-extent
// Volume GUID is exclusively locked and retained for the complete copy.
struct OnlineDirectLockedPartition final {
  std::uint32_t partition_index{};
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  std::wstring volume_guid_path;
  OnlineDirectLockedFileSystem file_system{
      OnlineDirectLockedFileSystem::fat32};
};

struct OnlineDirectSourceLayout final {
  OnlineDirectClonePartitionStyle partition_style{
      OnlineDirectClonePartitionStyle::gpt};
  std::vector<OnlineDirectSnapshotPartition> snapshot_partitions;
  std::vector<OnlineDirectLockedPartition> locked_partitions;
  std::vector<clonecore::ByteRange> static_physical_ranges;
};

struct OnlineDirectSnapshotReader final {
  std::uint32_t partition_index{};
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  std::unique_ptr<clonecore::ISourceDiskReader> reader;
};

struct OnlineDirectLockedReader final {
  std::uint32_t partition_index{};
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  OnlineDirectLockedFileSystem file_system{
      OnlineDirectLockedFileSystem::fat32};
  std::unique_ptr<clonecore::ISourceDiskReader> reader;
};

struct OnlineDirectLockedVolumeOpenRequest final {
  std::uint32_t physical_disk_number{};
  std::uint32_t partition_index{};
  std::uint64_t disk_offset{};
  std::uint64_t length{};
  std::uint32_t logical_sector_size{};
  std::wstring volume_guid_path;
  OnlineDirectLockedFileSystem expected_file_system{
      OnlineDirectLockedFileSystem::fat32};
};

// Parses and classifies one freshly opened, read-only Windows source. Every
// mutable NTFS partition must have exactly one Volume GUID binding. Only known
// boot/recovery regions are permitted to use the physical read-only reader.
[[nodiscard]] clonecore::Result<OnlineDirectSourceLayout>
build_online_direct_source_layout(
    const diskmodel::DiskInfo& observed_source,
    const clonecore::ISourceDiskReader& read_only_source,
    std::span<const clonecore::VolumeBitmapBinding> volume_bindings);

// Creates a whole-disk reader whose NTFS partition reads are translated into
// VSS Snapshot readers. A read that is not completely contained in either one
// Snapshot partition or one explicitly approved static range fails closed.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>
make_online_direct_composite_reader(
    const clonecore::ISourceDiskReader* read_only_physical_source,
    std::vector<OnlineDirectSnapshotReader> snapshot_readers,
    std::vector<clonecore::ByteRange> static_physical_ranges);

// Extended route used by the product controller.  Every locked reader owns a
// retained exclusive OS volume-lock lease; destroying the composite releases
// it.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>
make_online_direct_composite_reader(
    const clonecore::ISourceDiskReader* read_only_physical_source,
    std::vector<OnlineDirectSnapshotReader> snapshot_readers,
    std::vector<OnlineDirectLockedReader> locked_readers,
    std::vector<clonecore::ByteRange> static_physical_ranges);

// Opens a canonical Volume GUID read-only, proves one exact physical extent,
// obtains an exclusive volume lock, and re-identifies FAT32/exFAT through the
// same retained handle before returning a bounded partition reader.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>
open_locked_file_system_volume_with_windows_apis(
    const OnlineDirectLockedVolumeOpenRequest& request);

struct OnlineDirectCloneEngineReport final {
  std::uint64_t copied_data_bytes{};
  std::uint32_t copied_partition_count{};
  std::uint32_t recreated_partition_count{};
  imageformat::Sha256Digest verified_write_digest{};
  bool read_back_verified{};
  bool partition_table_committed{};
};

struct OnlineDirectCloneEngineContext final {
  OnlineDirectClonePartitionStyle partition_style{
      OnlineDirectClonePartitionStyle::gpt};
  const OnlineDirectCloneRequest* request{};
  const clonecore::StableDiskIdentity* observed_source{};
  const clonecore::StableDiskIdentity* observed_target{};
  const clonecore::ISourceDiskReader* source{};
  clonecore::ITargetDiskWriter* target{};
  clonecore::INtfsUsedRangeProvider* snapshot_bitmap_provider{};
  std::span<const std::uint32_t> connected_mbr_signatures;
};

struct OnlineDirectCloneReport final {
  OnlineDirectClonePartitionStyle partition_style{
      OnlineDirectClonePartitionStyle::gpt};
  std::uint64_t copied_data_bytes{};
  std::uint32_t copied_partition_count{};
  std::uint32_t recreated_partition_count{};
  imageformat::Sha256Digest verified_write_digest{};
  bool read_back_verified{};
  bool partition_table_committed{};
  bool snapshot_backup_completed{};
  bool snapshots_deleted{};
  bool used_vss_snapshot{};
  std::uint32_t locked_volume_count{};
  bool source_consistency_verified{};
  bool target_left_offline{};
  // Phase-one backend truth: the native clone engine verifies disk data, but
  // the BCD fresh-rebuild transaction is a separate controller connection.
  // Callers must not describe a system target as boot-ready while this is
  // false.
  bool boot_finalization_required{true};
  bool boot_finalization_completed{};
};

using OnlineDirectCloneReidentifier = std::function<clonecore::Result<
    diskmodel::ReidentifiedPhysicalClone>(
    const clonecore::StableDiskIdentity&,
    const clonecore::StableDiskIdentity&,
    const clonecore::TargetConfirmation&)>;

using OnlineDirectCloneSelectionReidentifier =
    std::function<clonecore::Result<diskmodel::ReidentifiedPhysicalClone>(
        const clonecore::StableDiskIdentity&,
        const clonecore::StableDiskIdentity&)>;

using OnlineDirectSourceOpener = std::function<clonecore::Result<
    diskmodel::ReadOnlyPhysicalDiskHandle>(
    const clonecore::StableDiskIdentity&)>;

using OnlineDirectGptBindingQuery = std::function<clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>(
    const diskmodel::DiskInfo&,
    const clonecore::GptDisk&)>;

using OnlineDirectMbrBindingQuery = std::function<clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>(
    const diskmodel::DiskInfo&,
    const clonecore::MbrDisk&)>;

using OnlineDirectSnapshotWorkflowRunner = std::function<clonecore::Result<
    vssrequester::WorkflowReport>(
    const vssrequester::WorkflowRequest&,
    const vssrequester::AsyncWaitOptions&,
    const clonecore::Logger*,
    vssrequester::SnapshotCopyCallback)>;

using OnlineDirectSnapshotReaderOpener = std::function<clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>(
    const vssrequester::SnapshotVolumeOpenRequest&)>;

using OnlineDirectLockedVolumeOpener = std::function<clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>(
    const OnlineDirectLockedVolumeOpenRequest&)>;

using OnlineDirectBitmapProviderFactory = std::function<clonecore::Result<
    std::unique_ptr<clonecore::INtfsUsedRangeProvider>>(
    std::vector<clonecore::SnapshotVolumeBitmapBinding>)>;

using OnlineDirectCloneTargetOfflineSetter = std::function<clonecore::Status(
    const clonecore::StableDiskIdentity&,
    const clonecore::StableDiskIdentity&,
    const clonecore::TargetConfirmation&,
    bool)>;

using OnlineDirectPhysicalTargetOfflineSetter =
    std::function<clonecore::Status(
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&,
        bool)>;

using OnlineDirectTargetOpener = std::function<clonecore::Result<
    diskmodel::PhysicalTargetHandle>(
    const clonecore::StableDiskIdentity&,
    const clonecore::TargetConfirmation&)>;

using OnlineDirectMbrSignatureCollector = std::function<clonecore::Result<
    std::vector<std::uint32_t>>(
    const clonecore::StableDiskIdentity&,
    const clonecore::MbrDisk&)>;

using OnlineDirectCloneEngine = std::function<clonecore::Result<
    OnlineDirectCloneEngineReport>(
    const OnlineDirectCloneEngineContext&)>;

using OnlineDirectBootFinalizer = std::function<clonecore::Status(
    const clonecore::StableDiskIdentity&,
    OnlineDirectClonePartitionStyle)>;

struct OnlineDirectCloneDependencies final {
  // Read-only inventory path used by OperationCore before it validates the
  // typed confirmation. It must never accept or perform a destructive action.
  OnlineDirectCloneSelectionReidentifier reidentify_clone_selection;
  OnlineDirectCloneReidentifier reidentify_clone;
  OnlineDirectSourceOpener open_read_only_source;
  OnlineDirectGptBindingQuery query_gpt_bindings;
  OnlineDirectMbrBindingQuery query_mbr_bindings;
  OnlineDirectSnapshotWorkflowRunner run_snapshot_workflow;
  OnlineDirectSnapshotReaderOpener open_snapshot_reader;
  OnlineDirectLockedVolumeOpener open_locked_volume;
  OnlineDirectBitmapProviderFactory make_snapshot_bitmap_provider;
  OnlineDirectCloneTargetOfflineSetter set_clone_target_offline;
  OnlineDirectPhysicalTargetOfflineSetter set_physical_target_offline;
  OnlineDirectTargetOpener open_offline_target;
  OnlineDirectMbrSignatureCollector collect_mbr_signatures;
  OnlineDirectCloneEngine execute_clone_engine;
  OnlineDirectBootFinalizer finalize_target_boot;
  vssrequester::WindowsVssDiffAreaOperationMonitorFactory
      make_diff_area_monitor;
};

// Product dependency adapter shared by the direct controller and the common
// OperationPlan bridge. Constructing it performs no disk I/O and no UAC
// request; every destructive call still passes through the verified
// reidentification/offline/open boundaries above.
[[nodiscard]] OnlineDirectCloneDependencies
make_online_direct_clone_windows_dependencies();

// Executes the destructive target phase only while all VSS Snapshot readers
// are alive. The source and target are freshly reidentified again immediately
// before the target is taken offline. Success and failure both leave the
// target offline once the destructive phase has started.
[[nodiscard]] clonecore::Result<OnlineDirectCloneReport>
execute_online_direct_clone(
    const OnlineDirectCloneRequest& request,
    const OnlineDirectCloneDependencies& dependencies);

// Product adapter. It performs no UAC request and never writes to the source.
[[nodiscard]] clonecore::Result<OnlineDirectCloneReport>
execute_online_direct_clone_with_windows_apis(
    const OnlineDirectCloneRequest& request);

}  // namespace ytec::windowsapp

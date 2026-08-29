#include "ytec/directshrink/windows_target_platform.h"

// The target transaction is already audited and exercised in WindowsApp, but
// its public interface still carries Windows/VSS-specific plan and evidence
// names. Compile that implementation once more against the extracted common
// target contract, then adapt only the capture-source seam from VSS Snapshot
// metadata to a WinPE read-only Volume lease. WindowsApp remains untouched
// while both products execute the same checkpoint/GPT/DISM/BootRepair code.
#include "ytec/windowsapp/windows_direct_shrink_clone_platform.h"

#include <Windows.h>

#include <memory>
#include <span>
#include <string>
#include <utility>

namespace ytec::windowsapp {

// Compatibility surface used only in this translation unit. All transaction
// evidence except the source-capture descriptor is the extracted common type.
class IWinPeDirectShrinkCompatibilityPlatform {
 public:
  virtual ~IWinPeDirectShrinkCompatibilityPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<directshrink::CheckpointEvidence>
  begin_target_owned_staging(
      const directshrink::TargetPlan& plan,
      const operationcore::Sha256Digest& operation_plan_hash) = 0;
  [[nodiscard]] virtual clonecore::Result<
      directshrink::TargetPreparationEvidence>
  prepare_non_archive_partitions_and_verify(
      std::span<const directshrink::PartitionTask> tasks) = 0;
  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkStagedArchiveEvidence>
  capture_ntfs_wim_to_owned_staging(
      const directshrink::PartitionTask& task,
      const vssrequester::SnapshotMapping& capture) = 0;
  [[nodiscard]] virtual clonecore::Result<
      directshrink::AppliedPartitionEvidence>
  apply_staged_ntfs_wim_and_verify(
      const directshrink::PartitionTask& task,
      const WindowsDirectShrinkStagedArchiveEvidence& archive) = 0;
  [[nodiscard]] virtual clonecore::Result<
      directshrink::ExactRawPartitionEvidence>
  copy_exact_raw_and_verify(
      const directshrink::PartitionTask& task,
      const clonecore::ISourceDiskReader& read_only_source) = 0;
  [[nodiscard]] virtual clonecore::Status discard_exact_staged_archive(
      const WindowsDirectShrinkStagedArchiveEvidence& archive) = 0;
  [[nodiscard]] virtual clonecore::Result<directshrink::CheckpointEvidence>
  persist_prepared_partitions_checkpoint(
      const directshrink::CheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;
  [[nodiscard]] virtual clonecore::Result<directshrink::CheckpointEvidence>
  persist_progress_checkpoint(
      const directshrink::CheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;
  [[nodiscard]] virtual clonecore::Result<directshrink::BootEvidence>
  finalize_boot_from_staged_layout_and_verify(
      const directshrink::TargetPlan& plan) = 0;
  [[nodiscard]] virtual clonecore::Result<directshrink::CheckpointEvidence>
  seal_commit_ready_checkpoint(
      const directshrink::CheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;
  [[nodiscard]] virtual clonecore::Result<directshrink::CheckpointEvidence>
  prepare_final_extents_keep_incomplete_and_verify(
      const directshrink::TargetPlan& plan,
      const directshrink::CheckpointEvidence& expected) = 0;
  [[nodiscard]] virtual clonecore::Result<directshrink::CheckpointEvidence>
  revalidate_before_final_commit(
      const directshrink::TargetPlan& plan,
      const directshrink::CheckpointEvidence& expected) = 0;
  [[nodiscard]] virtual clonecore::Result<directshrink::FinalCommitEvidence>
  commit_final_layout_last(
      const directshrink::TargetPlan& plan,
      const directshrink::CheckpointEvidence& commit_ready) = 0;
  virtual void abort_keep_offline_incomplete() noexcept = 0;
};

clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>
reject_winpe_direct_shrink_mbr_safety(
    const clonecore::StableDiskIdentity&,
    const clonecore::StableDiskIdentity&,
    bool) {
  return clonecore::Result<WindowsDirectShrinkMbrSafetyEvidence>::failure({
      .code = clonecore::ErrorCode::unsupported_layout,
      .native_code = ERROR_NOT_SUPPORTED,
      .operation = L"WinPE direct-shrink MBR safety",
      .message = L"WinPE offline NTFS直接縮小はGPT preserveだけを扱います",
  });
}

}  // namespace ytec::windowsapp

// Compile the existing target-only transaction against the extracted types.
// Its WIM capture implementation reads only snapshot_device_path, which the
// adapter below supplies from a still-held WinPE Volume lease; no VSS API is
// invoked by this target module.
#define WindowsDirectShrinkClonePlan ::ytec::directshrink::TargetPlan
#define WindowsDirectShrinkPartitionTask \
  ::ytec::directshrink::PartitionTask
#define WindowsDirectShrinkPartitionTaskKind \
  ::ytec::directshrink::PartitionTaskKind
#define WindowsDirectShrinkSourcePartitionMapping \
  ::ytec::directshrink::SourcePartitionMapping
#define WindowsDirectShrinkSourcePartitionDisposition \
  ::ytec::directshrink::SourcePartitionDisposition
#define WindowsDirectShrinkCheckpointPhase \
  ::ytec::directshrink::CheckpointPhase
#define WindowsDirectShrinkCheckpointEvidence \
  ::ytec::directshrink::CheckpointEvidence
#define WindowsDirectShrinkTargetPreparationEvidence \
  ::ytec::directshrink::TargetPreparationEvidence
#define WindowsDirectShrinkAppliedPartitionEvidence \
  ::ytec::directshrink::AppliedPartitionEvidence
#define WindowsDirectShrinkExactRawPartitionEvidence \
  ::ytec::directshrink::ExactRawPartitionEvidence
#define WindowsDirectShrinkBootEvidence ::ytec::directshrink::BootEvidence
#define WindowsDirectShrinkFinalCommitEvidence \
  ::ytec::directshrink::FinalCommitEvidence
#define IWindowsDirectShrinkClonePlatform \
  IWinPeDirectShrinkCompatibilityPlatform
#define kWindowsDirectShrinkCheckpointOffsetBytes \
  ::ytec::directshrink::kCheckpointOffsetBytes
#define kWindowsDirectShrinkCheckpointRecordBytes \
  ::ytec::directshrink::kCheckpointRecordBytes
#define kWindowsDirectShrinkStagingAlignmentBytes \
  ::ytec::directshrink::kStagingAlignmentBytes
#define kWindowsDirectShrinkStagingControlReserveBytes \
  ::ytec::directshrink::kStagingControlReserveBytes
#define kWindowsDirectShrinkStagingFileSystemReserveBytes \
  ::ytec::directshrink::kStagingFileSystemReserveBytes
#define make_windows_direct_shrink_clone_platform \
  make_winpe_compatibility_direct_shrink_target_platform
#define make_windows_direct_shrink_clone_platform_with_dependencies \
  make_winpe_compatibility_direct_shrink_target_platform_with_dependencies
#define observe_windows_direct_shrink_mbr_safety_with_windows_apis \
  reject_winpe_direct_shrink_mbr_safety

#include "../../WindowsApp/src/windows_direct_shrink_clone_platform.cpp"

#undef observe_windows_direct_shrink_mbr_safety_with_windows_apis
#undef make_windows_direct_shrink_clone_platform_with_dependencies
#undef make_windows_direct_shrink_clone_platform
#undef kWindowsDirectShrinkStagingFileSystemReserveBytes
#undef kWindowsDirectShrinkStagingControlReserveBytes
#undef kWindowsDirectShrinkStagingAlignmentBytes
#undef kWindowsDirectShrinkCheckpointRecordBytes
#undef kWindowsDirectShrinkCheckpointOffsetBytes
#undef IWindowsDirectShrinkClonePlatform
#undef WindowsDirectShrinkFinalCommitEvidence
#undef WindowsDirectShrinkBootEvidence
#undef WindowsDirectShrinkAppliedPartitionEvidence
#undef WindowsDirectShrinkExactRawPartitionEvidence
#undef WindowsDirectShrinkTargetPreparationEvidence
#undef WindowsDirectShrinkCheckpointEvidence
#undef WindowsDirectShrinkCheckpointPhase
#undef WindowsDirectShrinkSourcePartitionDisposition
#undef WindowsDirectShrinkSourcePartitionMapping
#undef WindowsDirectShrinkPartitionTaskKind
#undef WindowsDirectShrinkPartitionTask
#undef WindowsDirectShrinkClonePlan

namespace ytec::windowsapp {
namespace {

clonecore::Result<diskmodel::ReidentifiedPhysicalClone>
reidentify_winpe_source_for_target_transaction(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  auto observed = diskmodel::reidentify_physical_clone(
      expected_source,
      expected_target,
      confirmation,
      *inventory,
      false);
  if (!observed) {
    return observed;
  }
  const auto& source = observed.value().source;
  if (!source.read_only.has_value() || !source.read_only.value() ||
      !source.offline.has_value() || source.offline.value() ||
      !source.removable.has_value() || source.removable.value() ||
      source.is_system_disk != expected_source.is_system_disk ||
      source.logical_sector_size != 512U) {
    return clonecore::Result<
        diskmodel::ReidentifiedPhysicalClone>::failure({
        .code = clonecore::ErrorCode::identity_mismatch,
        .native_code = ERROR_WRITE_PROTECT,
        .operation = L"WinPE target transaction source再識別",
        .message =
            L"実sourceのOS-level read-only、online fixed状態、system役割、または512-byte sectorが計画と一致しません",
    });
  }

  // The reused target-only transaction historically requires an online,
  // non-read-only source observation although it never receives a source
  // handle. Mask only this legacy presentation bit after proving the stronger
  // WinPE read-only fact above; every fresh reidentification repeats the proof.
  auto compatible = observed.take_value();
  compatible.source.read_only = false;
  return clonecore::Result<diskmodel::ReidentifiedPhysicalClone>::success(
      std::move(compatible));
}

clonecore::Result<std::unique_ptr<IWinPeDirectShrinkCompatibilityPlatform>>
make_winpe_read_only_source_compatibility_platform(
    const directshrink::TargetPlan& plan,
    diskmodel::ReidentifiedPhysicalClone observed,
    const WindowsDirectShrinkClonePlatformRequest& request) {
  WindowsTsumugiShrinkRestorePlatformRequest io_request{
      .expected_target = plan.expected_target(),
      .confirmation = request.confirmation,
      .expected_target_layout_hash = plan.expected_target_layout_hash(),
      .target_is_active_rescue_media = plan.target_is_active_rescue_media(),
      .callbacks = request.callbacks,
  };
  auto io = make_windows_tsumugi_shrink_restore_platform_io(io_request);
  auto guid = clonecore::make_windows_guid_generator();
  auto connection =
      imageformat::make_tsumugi_connection_instance_hash_with_windows_apis();
  if (!io || !guid || !connection) {
    return !io
        ? clonecore::Result<std::unique_ptr<
              IWinPeDirectShrinkCompatibilityPlatform>>::failure(io.error())
        : !connection
              ? clonecore::Result<std::unique_ptr<
                    IWinPeDirectShrinkCompatibilityPlatform>>::failure(
                    connection.error())
              : clonecore::Result<std::unique_ptr<
                    IWinPeDirectShrinkCompatibilityPlatform>>::failure({
                    .code = clonecore::ErrorCode::internal_error,
                    .native_code = ERROR_INVALID_HANDLE,
                    .operation = L"WinPE target transaction GUID generator",
                    .message = L"Windows GUID generatorを作成できません",
                });
  }

  WindowsDirectShrinkClonePlatformDependencies dependencies{
      .target_io = io.take_value(),
      .guid_generator = std::move(guid),
      .make_wim_store =
          [](const std::wstring& root,
             const std::uint64_t capacity,
             const std::uint64_t maximum_archive,
             const clonecore::DiskOperationCallbacks& callbacks) {
            return WindowsDirectShrinkOwnedWimStore::create(
                root, capacity, maximum_archive, callbacks);
          },
      .connection_instance_hash = connection.take_value(),
      .reidentify_confirmed =
          reidentify_winpe_source_for_target_transaction,
      .finalize_boot = finalize_boot_with_windows_apis,
      .finalize_winre = finalize_winre_with_windows_apis,
      .observe_mbr_safety = reject_winpe_direct_shrink_mbr_safety,
  };

  // See reidentify_winpe_source_for_target_transaction(): the common WinPE
  // factory has already checked the real read-only bit. Only the private
  // compatibility instance sees the legacy non-read-only presentation.
  observed.source.read_only = false;
  return make_winpe_compatibility_direct_shrink_target_platform_with_dependencies(
      plan, observed, request, std::move(dependencies));
}

}  // namespace
}  // namespace ytec::windowsapp

namespace ytec::directshrink {
namespace {

template <typename T>
clonecore::Result<T> adapter_failure(
    std::wstring operation,
    std::wstring message,
    const DWORD native_code = ERROR_INVALID_PARAMETER) {
  return clonecore::Result<T>::failure({
      .code = clonecore::ErrorCode::invalid_argument,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  });
}

windowsapp::WindowsDirectShrinkStagedArchiveEvidence to_compatibility_archive(
    const StagedArchiveEvidence& archive) {
  return windowsapp::WindowsDirectShrinkStagedArchiveEvidence{
      .source_table_index = archive.source_table_index,
      .target_number = archive.target_number,
      .snapshot_id = archive.capture_identity,
      .snapshot_device_path = archive.read_device_path,
      .archive_length = archive.archive_length,
      .archive_hash = archive.archive_hash,
      .sealed_no_write_delete_sharing =
          archive.sealed_no_write_delete_sharing,
      .flushed = archive.flushed,
      .complete_read_back_hash_verified =
          archive.complete_read_back_hash_verified,
      .target_offline = archive.target_offline,
  };
}

class WindowsTargetPlatformAdapter final : public ITargetPlatform {
 public:
  explicit WindowsTargetPlatformAdapter(std::unique_ptr<
      windowsapp::IWinPeDirectShrinkCompatibilityPlatform> implementation)
      : implementation_(std::move(implementation)) {}

  [[nodiscard]] clonecore::Result<CheckpointEvidence>
  begin_target_owned_staging(
      const TargetPlan& plan,
      const operationcore::Sha256Digest& operation_plan_hash) override {
    return implementation_->begin_target_owned_staging(
        plan, operation_plan_hash);
  }

  [[nodiscard]] clonecore::Result<TargetPreparationEvidence>
  prepare_non_archive_partitions_and_verify(
      const std::span<const PartitionTask> tasks) override {
    return implementation_->prepare_non_archive_partitions_and_verify(tasks);
  }

  [[nodiscard]] clonecore::Result<StagedArchiveEvidence>
  capture_ntfs_wim_to_owned_staging(
      const PartitionTask& task,
      const CaptureSource& source) override {
    if (source.kind != CaptureSourceKind::winpe_read_only_volume ||
        !task.source_table_index.has_value() ||
        source.source_table_index != *task.source_table_index ||
        source.source_offset_bytes != task.source_offset_bytes ||
        source.source_size_bytes != task.source_size_bytes ||
        source.original_volume_guid_path != task.original_volume_guid_path ||
        source.capture_identity.empty() || source.read_device_path.empty() ||
        !source.source_os_read_only ||
        !source.source_disk_handle_held_read_only ||
        !source.source_volume_handle_held_read_only ||
        !source.source_volume_extent_reverified) {
      return adapter_failure<StagedArchiveEvidence>(
          L"WinPE target WIM capture lease",
          L"source table index、extent、Volume GUID、read-only handles、またはcapture identityが計画と一致しません");
    }
    auto captured = implementation_->capture_ntfs_wim_to_owned_staging(
        task,
        vssrequester::SnapshotMapping{
            .original_volume_guid_path = source.original_volume_guid_path,
            .snapshot_id = source.capture_identity,
            .snapshot_device_path = source.read_device_path,
            .provider_id = L"WINPE-READ-ONLY-VOLUME",
            .creation_timestamp = 0,
        });
    if (!captured) {
      return clonecore::Result<StagedArchiveEvidence>::failure(
          captured.error());
    }
    return clonecore::Result<StagedArchiveEvidence>::success({
        .source_table_index = captured.value().source_table_index,
        .target_number = captured.value().target_number,
        .capture_identity = captured.value().snapshot_id,
        .read_device_path = captured.value().snapshot_device_path,
        .archive_length = captured.value().archive_length,
        .archive_hash = captured.value().archive_hash,
        .sealed_no_write_delete_sharing =
            captured.value().sealed_no_write_delete_sharing,
        .flushed = captured.value().flushed,
        .complete_read_back_hash_verified =
            captured.value().complete_read_back_hash_verified,
        .target_offline = captured.value().target_offline,
    });
  }

  [[nodiscard]] clonecore::Result<AppliedPartitionEvidence>
  apply_staged_ntfs_wim_and_verify(
      const PartitionTask& task,
      const StagedArchiveEvidence& archive) override {
    return implementation_->apply_staged_ntfs_wim_and_verify(
        task, to_compatibility_archive(archive));
  }

  [[nodiscard]] clonecore::Result<ExactRawPartitionEvidence>
  copy_exact_raw_and_verify(
      const PartitionTask& task,
      const clonecore::ISourceDiskReader& read_only_source) override {
    return implementation_->copy_exact_raw_and_verify(
        task, read_only_source);
  }

  [[nodiscard]] clonecore::Status discard_exact_staged_archive(
      const StagedArchiveEvidence& archive) override {
    return implementation_->discard_exact_staged_archive(
        to_compatibility_archive(archive));
  }

  [[nodiscard]] clonecore::Result<CheckpointEvidence>
  persist_prepared_partitions_checkpoint(
      const CheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) override {
    return implementation_->persist_prepared_partitions_checkpoint(
        previous,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
  }

  [[nodiscard]] clonecore::Result<CheckpointEvidence>
  persist_progress_checkpoint(
      const CheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) override {
    return implementation_->persist_progress_checkpoint(
        previous,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
  }

  [[nodiscard]] clonecore::Result<BootEvidence>
  finalize_boot_from_staged_layout_and_verify(
      const TargetPlan& plan) override {
    return implementation_->finalize_boot_from_staged_layout_and_verify(plan);
  }

  [[nodiscard]] clonecore::Result<CheckpointEvidence>
  seal_commit_ready_checkpoint(
      const CheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) override {
    return implementation_->seal_commit_ready_checkpoint(
        previous,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
  }

  [[nodiscard]] clonecore::Result<CheckpointEvidence>
  prepare_final_extents_keep_incomplete_and_verify(
      const TargetPlan& plan,
      const CheckpointEvidence& expected) override {
    return implementation_->prepare_final_extents_keep_incomplete_and_verify(
        plan, expected);
  }

  [[nodiscard]] clonecore::Result<CheckpointEvidence>
  revalidate_before_final_commit(
      const TargetPlan& plan,
      const CheckpointEvidence& expected) override {
    return implementation_->revalidate_before_final_commit(plan, expected);
  }

  [[nodiscard]] clonecore::Result<FinalCommitEvidence>
  commit_final_layout_last(
      const TargetPlan& plan,
      const CheckpointEvidence& commit_ready) override {
    return implementation_->commit_final_layout_last(plan, commit_ready);
  }

  void abort_keep_offline_incomplete() noexcept override {
    implementation_->abort_keep_offline_incomplete();
  }

 private:
  std::unique_ptr<
      windowsapp::IWinPeDirectShrinkCompatibilityPlatform> implementation_;
};

}  // namespace

clonecore::Result<std::unique_ptr<ITargetPlatform>>
make_windows_target_platform_for_winpe(
    const TargetPlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const WindowsTargetPlatformRequest& request) {
  auto identities = clonecore::validate_clone_identities(
      plan.expected_source(),
      observed.source_identity,
      plan.expected_target(),
      observed.target_identity,
      request.confirmation,
      false);
  if (!identities) {
    return clonecore::Result<std::unique_ptr<ITargetPlatform>>::failure(
        identities.error());
  }
  if (plan.source_partition_style() !=
          migrationcore::MigrationPartitionStyle::gpt ||
      plan.partition_style() != migrationcore::MigrationPartitionStyle::gpt ||
      plan.partition_style_choice() !=
          migrationcore::DirectClonePartitionStyleChoice::preserve ||
      plan.mbr_preserve_binding().has_value() ||
      plan.target_is_active_rescue_media() ||
      plan.expected_source().is_system_disk ||
      !observed.source.read_only.has_value() ||
      !observed.source.read_only.value() ||
      !observed.source.offline.has_value() || observed.source.offline.value() ||
      !observed.source.removable.has_value() ||
      observed.source.removable.value() || observed.source.is_system_disk ||
      observed.source.logical_sector_size != 512U ||
      !observed.target.offline.has_value() ||
      !observed.target.read_only.has_value() ||
      observed.target.read_only.value() ||
      !observed.target.removable.has_value() ||
      observed.target.removable.value() || observed.target.is_system_disk ||
      observed.target.logical_sector_size != 512U) {
    return adapter_failure<std::unique_ptr<ITargetPlatform>>(
        L"WinPE production target platform",
        L"OS-level read-only GPT source、GPT preserve、分離fixed non-system target、512-byte sector、および非rescue targetが必要です",
        ERROR_NOT_SUPPORTED);
  }

  auto implementation = windowsapp::
      make_winpe_read_only_source_compatibility_platform(
          plan,
          observed,
          windowsapp::WindowsDirectShrinkClonePlatformRequest{
              .confirmation = request.confirmation,
              .callbacks = request.callbacks,
          });
  if (!implementation) {
    return clonecore::Result<std::unique_ptr<ITargetPlatform>>::failure(
        implementation.error());
  }
  std::unique_ptr<ITargetPlatform> result =
      std::make_unique<WindowsTargetPlatformAdapter>(
          implementation.take_value());
  return clonecore::Result<std::unique_ptr<ITargetPlatform>>::success(
      std::move(result));
}

}  // namespace ytec::directshrink

#include "ytec/vssrequester/online_backup.h"

#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/vssrequester/snapshot_copy.h"

#include <Windows.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace ytec::vssrequester {
namespace {

clonecore::Error backup_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

class DeferredCommitTarget final
    : public imageformat::IDcimgStagingTarget {
 public:
  explicit DeferredCommitTarget(
      imageformat::IDcimgStagingTarget& target) noexcept
      : target_(target) {}

  clonecore::Status begin(const std::uint64_t expected_length) override {
    if (begun_ || prepared_ || finalized_) {
      return invalid_state(L"遅延確定出力開始");
    }
    const auto result = target_.begin(expected_length);
    if (result) {
      begun_ = true;
    }
    return result;
  }

  clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (!begun_ || prepared_ || finalized_) {
      return invalid_state(L"遅延確定出力書込み");
    }
    return target_.write_at(offset, bytes);
  }

  clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!begun_ || finalized_) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          backup_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_STATE,
              L"遅延確定出力読戻し",
              L"出力が開始前または確定済みです"));
    }
    return target_.read_at(offset, length);
  }

  clonecore::Status resize_before_verification(
      const std::uint64_t final_length) override {
    if (!begun_ || prepared_ || finalized_) {
      return invalid_state(L"遅延確定出力最終長");
    }
    return target_.resize_before_verification(final_length);
  }

  clonecore::Status flush() override {
    if (!begun_ || prepared_ || finalized_) {
      return invalid_state(L"遅延確定出力flush");
    }
    return target_.flush();
  }

  clonecore::Status commit_verified() override {
    if (!begun_ || prepared_ || finalized_) {
      return invalid_state(L"遅延確定準備");
    }
    prepared_ = true;
    return clonecore::success_status();
  }

  clonecore::Status abort_incomplete() override {
    if (finalized_) {
      return invalid_state(L"確定済み出力の破棄拒否");
    }
    if (!begun_) {
      return clonecore::success_status();
    }
    const auto result = target_.abort_incomplete();
    if (result) {
      begun_ = false;
      prepared_ = false;
    }
    return result;
  }

  clonecore::Status finalize_after_vss() {
    if (!begun_ || !prepared_ || finalized_) {
      return invalid_state(L"VSS完了後の最終確定");
    }
    const auto result = target_.commit_verified();
    if (result) {
      finalized_ = true;
      begun_ = false;
    }
    return result;
  }

  [[nodiscard]] bool prepared() const noexcept { return prepared_; }

 private:
  static clonecore::Status invalid_state(std::wstring operation) {
    return clonecore::Status::failure(backup_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        std::move(operation),
        L"出力の状態遷移が不正です"));
  }

  imageformat::IDcimgStagingTarget& target_;
  bool begun_{};
  bool prepared_{};
  bool finalized_{};
};

clonecore::Error with_abort_failure(
    clonecore::Error primary,
    const clonecore::Status& abort) {
  if (!abort) {
    primary.message +=
        L"。未完了ファイルの破棄にも失敗しました: " +
        abort.error().operation;
  }
  return primary;
}

clonecore::Status validate_plan(const PreparedSnapshotImagePlan& plan) {
  if (!plan.workflow.administrator ||
      plan.workflow.volumes.empty() ||
      plan.workflow.volumes.size() !=
          plan.image_copy.volumes.size() ||
      plan.snapshot_partition_count !=
          plan.image_copy.volumes.size() ||
      plan.raw_partition_count !=
          plan.image_copy.raw_regions.size() ||
      plan.image_copy.source_disk_size == 0) {
    return clonecore::Status::failure(backup_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンラインイメージ計画",
        L"管理者ゲートまたはVSS/固定領域の対応件数が不正です"));
  }
  const auto manifest =
      imageformat::inspect_backup_manifest_v1(
          plan.image_copy.manifest);
  const auto snapshot =
      imageformat::inspect_partition_snapshot_v1(
          plan.image_copy.partition_table_snapshot);
  if (!manifest || !snapshot ||
      manifest.value().source.size_bytes !=
          plan.image_copy.source_disk_size ||
      manifest.value().source.logical_sector_size !=
          plan.image_copy.logical_sector_size ||
      manifest.value().physical_sector_size !=
          plan.image_copy.physical_sector_size ||
      manifest.value().chunk_size != plan.image_copy.chunk_size ||
      manifest.value().compression != plan.image_copy.compression ||
      snapshot.value().source_disk_size !=
          plan.image_copy.source_disk_size ||
      snapshot.value().logical_sector_size !=
          plan.image_copy.logical_sector_size ||
      (manifest.value().partition_style ==
       imageformat::BackupPartitionStyle::gpt) !=
          (snapshot.value().style ==
           imageformat::PartitionTableStyle::gpt)) {
    return clonecore::Status::failure(backup_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"オンラインイメージ メタデータ整合",
        L"マニフェスト、パーティション表、またはコピー計画の寸法が一致しません"));
  }
  std::size_t expected_snapshot = 0;
  std::size_t expected_raw = 0;
  std::size_t expected_recreated = 0;
  for (const auto& partition : manifest.value().partitions) {
    if (partition.role ==
        imageformat::BackupPartitionRole::microsoft_reserved) {
      ++expected_recreated;
      continue;
    }
    if (partition.role ==
        imageformat::BackupPartitionRole::windows_ntfs) {
      const bool found = std::any_of(
          plan.image_copy.volumes.begin(),
          plan.image_copy.volumes.end(),
          [&](const auto& volume) {
            return volume.partition_entry_index ==
                       partition.table_index &&
                   volume.disk_offset == partition.offset_bytes &&
                   volume.partition_length == partition.length_bytes;
          });
      if (!found) {
        return clonecore::Status::failure(backup_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"オンラインイメージ NTFS対応",
            L"マニフェストのWindows NTFSに対応するVSS計画がありません"));
      }
      ++expected_snapshot;
      continue;
    }
    const bool found = std::any_of(
        plan.image_copy.raw_regions.begin(),
        plan.image_copy.raw_regions.end(),
        [&](const auto& region) {
          return region.disk_offset == partition.offset_bytes &&
                 region.length == partition.length_bytes &&
                 region.source_reader != nullptr;
        });
    if (!found) {
      return clonecore::Status::failure(backup_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"オンラインイメージ固定領域対応",
          L"マニフェストのEFI/回復/FAT32に対応する読取り専用領域がありません"));
    }
    ++expected_raw;
  }
  if (expected_snapshot != plan.snapshot_partition_count ||
      expected_raw != plan.raw_partition_count ||
      expected_recreated != plan.recreated_partition_count) {
    return clonecore::Status::failure(backup_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"オンラインイメージ パーティション件数",
        L"マニフェストとコピー計画のパーティション件数が一致しません"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<OnlineImageBackupReport>
execute_prepared_snapshot_image_backup(
    const PreparedSnapshotImagePlan& plan,
    std::unique_ptr<imageformat::IDcimgStagingTarget> staging_target,
    const SnapshotImageCopyExecutor& copy_executor,
    const WorkflowBackendFactory& backend_factory) {
  const auto valid = validate_plan(plan);
  if (!valid) {
    return clonecore::Result<OnlineImageBackupReport>::failure(
        valid.error());
  }
  if (!staging_target || !copy_executor || !backend_factory) {
    return clonecore::Result<OnlineImageBackupReport>::failure(backup_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンラインイメージ依存境界",
        L"出力、コピー処理、またはVSS Backend Factoryがありません"));
  }

  DeferredCommitTarget deferred(*staging_target);
  std::optional<imageformat::DcimgStreamBuildReport> image_report;
  auto backend = backend_factory(
      [&](const SnapshotCopyContext& context) {
        if (image_report.has_value()) {
          return clonecore::Status::failure(backup_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_STATE,
              L"VSS Snapshotコピー回数",
              L"同じオンラインイメージ処理でコピーが複数回要求されました"));
        }
        std::vector<std::wstring> snapshot_paths;
        snapshot_paths.reserve(context.mappings.size());
        for (const auto& mapping : context.mappings) {
          snapshot_paths.push_back(mapping.snapshot_device_path);
        }
        auto copied = copy_executor(
            plan.image_copy, snapshot_paths, deferred);
        if (!copied) {
          return clonecore::Status::failure(copied.error());
        }
        image_report = copied.take_value();
        if (!image_report->committed ||
            !image_report->all_chunks_read_back_verified ||
            !image_report->global_hash_read_back_verified ||
            !deferred.prepared()) {
          return clonecore::Status::failure(backup_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"VSS Snapshotイメージ全検証",
              L"全チャンク・全体ハッシュ・遅延確定準備が完了していません"));
        }
        return clonecore::success_status();
      });
  if (!backend || !backend.value()) {
    return clonecore::Result<OnlineImageBackupReport>::failure(
        backend ? backup_error(
                      clonecore::ErrorCode::internal_error,
                      ERROR_INVALID_HANDLE,
                      L"VSS Backend生成",
                      L"VSS Backend Factoryが空のBackendを返しました")
                : backend.error());
  }

  auto workflow =
      execute_backup_workflow(plan.workflow, *backend.value());
  if (!workflow) {
    const auto aborted = deferred.abort_incomplete();
    return clonecore::Result<OnlineImageBackupReport>::failure(
        with_abort_failure(workflow.error(), aborted));
  }
  if (!image_report.has_value()) {
    const auto aborted = deferred.abort_incomplete();
    return clonecore::Result<OnlineImageBackupReport>::failure(
        with_abort_failure(
            backup_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_STATE,
                L"VSS Snapshotイメージ結果",
                L"VSS Workflow成功時にイメージ結果がありません"),
            aborted));
  }

  const auto finalized = deferred.finalize_after_vss();
  if (!finalized) {
    const auto aborted = deferred.abort_incomplete();
    return clonecore::Result<OnlineImageBackupReport>::failure(
        with_abort_failure(finalized.error(), aborted));
  }
  image_report->committed = true;
  return clonecore::Result<OnlineImageBackupReport>::success(
      OnlineImageBackupReport{
          .workflow = workflow.take_value(),
          .image = std::move(*image_report),
          .final_file_committed_after_vss = true,
      });
}

clonecore::Result<OnlineImageBackupReport>
execute_windows_online_image_backup(
    const WindowsOnlineImageBackupRequest& request) {
  auto staging =
      imageformat::make_windows_file_staging_target(request.staging);
  if (!staging) {
    return clonecore::Result<OnlineImageBackupReport>::failure(
        staging.error());
  }
  return execute_prepared_snapshot_image_backup(
      request.plan,
      staging.take_value(),
      [](const SnapshotImageCopyRequest& copy_request,
         const std::span<const std::wstring> snapshot_paths,
         imageformat::IDcimgStagingTarget& target) {
        return copy_snapshot_devices_to_dcimg_v1_with_windows_apis(
            copy_request, snapshot_paths, target);
      },
      [&](SnapshotCopyCallback callback) {
        std::unique_ptr<IWorkflowBackend> backend =
            std::make_unique<WindowsVssBackend>(
                WindowsVssBackendOptions{
                    .async_wait = request.async_wait,
                    .copy_snapshot_data = std::move(callback),
                    .logger = request.logger,
                });
        return clonecore::Result<
            std::unique_ptr<IWorkflowBackend>>::success(
            std::move(backend));
      });
}

}  // namespace ytec::vssrequester

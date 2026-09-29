#include "ytec/vssrequester/online_tsumugi_backup.h"

#include <Windows.h>

#include <cwchar>
#include <optional>
#include <string>
#include <utility>

namespace ytec::vssrequester {
namespace {

clonecore::Error online_error(
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

clonecore::Error with_abort_failure(
    clonecore::Error primary,
    imageformat::TsumugiStagedImageV1& staged) {
  const auto aborted = staged.abort_incomplete();
  if (!aborted) {
    primary.message += L"。検証済み.partialの破棄にも失敗しました: " +
        aborted.error().message;
  }
  return primary;
}

clonecore::Status validate_request(
    const PreparedOnlineTsumugiBackup& request) {
  if (!request.workflow.administrator ||
      request.workflow.volumes.empty() ||
      request.workflow.volumes.size() != request.image.volumes.size() ||
      !request.revalidate_destination) {
    return clonecore::Status::failure(online_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンラインTsumugi計画",
        L"管理者ゲートまたはVSS Volume計画の件数が不正です"));
  }
  for (std::size_t index = 0U;
       index < request.workflow.volumes.size(); ++index) {
    if (_wcsicmp(
            request.workflow.volumes[index].file_system.c_str(),
            L"NTFS") != 0 ||
        _wcsicmp(
            request.workflow.volumes[index].volume_guid_path.c_str(),
            request.image.volumes[index].original_volume_guid_path.c_str()) !=
            0) {
      return clonecore::Status::failure(online_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"オンラインTsumugi Volume対応",
          L"Workflowと画像計画のNTFS Volume順序が一致しません"));
    }
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<OnlineTsumugiBackupReport>
execute_prepared_online_tsumugi_backup(
    const PreparedOnlineTsumugiBackup& request,
    const TsumugiSnapshotPrepareExecutor& prepare_snapshot,
    const TsumugiWorkflowBackendFactory& backend_factory) {
  const auto valid = validate_request(request);
  if (!valid) {
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        valid.error());
  }
  if (!prepare_snapshot || !backend_factory) {
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        online_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"オンラインTsumugi依存境界",
            L"Snapshot準備処理またはVSS Backend Factoryがありません"));
  }

  const auto initial_destination = request.revalidate_destination(nullptr);
  if (!initial_destination) {
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        initial_destination.error());
  }

  std::optional<imageformat::TsumugiStagedImageV1> staged;
  std::wstring copied_snapshot_set_id;
  auto backend = backend_factory(
      [&](const SnapshotCopyContext& context) {
        if (staged.has_value() || context.snapshot_set_id.empty()) {
          return clonecore::Status::failure(online_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_STATE,
              L"オンラインTsumugi Snapshotコピー回数",
              L"同じ処理のSnapshotコピーは有効なContextで1回だけです"));
        }
        auto prepared = prepare_snapshot(request.image, context);
        if (!prepared) {
          return clonecore::Status::failure(prepared.error());
        }
        if (!prepared.value().pending() ||
            !imageformat::selected_tsumugi_creation_verification_passed(
                prepared.value().report()) ||
            prepared.value().report().stream.committed) {
          return clonecore::Status::failure(online_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"オンラインTsumugi作成時検証",
              L"完成名未確定の選択済み作成時検証結果が揃っていません"));
        }
        copied_snapshot_set_id = context.snapshot_set_id;
        staged.emplace(prepared.take_value());
        return clonecore::success_status();
      });
  if (!backend || !backend.value()) {
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        backend ? online_error(
                      clonecore::ErrorCode::internal_error,
                      ERROR_INVALID_HANDLE,
                      L"オンラインTsumugi VSS Backend生成",
                      L"VSS Backend Factoryが空のBackendを返しました")
                : backend.error());
  }

  auto workflow = execute_backup_workflow(request.workflow, *backend.value());
  if (!workflow) {
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        staged.has_value()
            ? with_abort_failure(workflow.error(), *staged)
            : workflow.error());
  }
  if (!staged.has_value() ||
      copied_snapshot_set_id != workflow.value().snapshot_set_id ||
      !workflow.value().snapshot_data_copied ||
      !workflow.value().backup_completed ||
      !workflow.value().snapshots_deleted) {
    auto error = online_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_STATE,
        L"オンラインTsumugi VSS完了照合",
        L"検証済み画像と完了したSnapshot SetのIdentityまたは状態が一致しません");
    if (staged.has_value()) {
      error = with_abort_failure(std::move(error), *staged);
    }
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        std::move(error));
  }

  const auto final_destination =
      request.revalidate_destination(&staged->report());
  if (!final_destination) {
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        with_abort_failure(final_destination.error(), *staged));
  }

  auto committed = staged->commit_verified();
  if (!committed) {
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        with_abort_failure(committed.error(), *staged));
  }
  return clonecore::Result<OnlineTsumugiBackupReport>::success(
      OnlineTsumugiBackupReport{
          .workflow = workflow.take_value(),
          .image = committed.take_value(),
          .final_file_committed_after_vss = true,
      });
}

clonecore::Result<OnlineTsumugiBackupReport>
execute_windows_online_tsumugi_backup(
    const WindowsOnlineTsumugiBackupRequest& request) {
  auto source_token = encode_vss_diff_area_source_epoch_token(
      std::span<const std::byte>(
          request.prepared.image.locked_source_state_hash));
  if (!source_token ||
      !request.prepared.image.revalidate_locked_layout ||
      !request.diff_area_review_callback) {
    return clonecore::Result<OnlineTsumugiBackupReport>::failure(
        source_token
            ? online_error(
                  clonecore::ErrorCode::invalid_argument,
                  ERROR_INVALID_PARAMETER,
                  L"オンラインTsumugi VSS差分領域監視",
                  L"固定source epoch、fresh layout probe、またはreview UIがありません")
            : source_token.error());
  }
  const std::wstring expected_source_token = source_token.take_value();
  return execute_prepared_online_tsumugi_backup(
      request.prepared,
      [&, expected_source_token](
          const TsumugiSnapshotImageRequest& image,
          const SnapshotCopyContext& context) {
        if (clonecore::disk_operation_cancellation_requested(
                request.callbacks)) {
          return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
              online_error(
                  clonecore::ErrorCode::cancelled,
                  ERROR_CANCELLED,
                  L"オンラインTsumugi VSS差分領域初回poll",
                  L"初回output変更前に取消要求を確認しました"));
        }
        auto monitor = make_windows_vss_diff_area_operation_monitor(
            context,
            WindowsVssDiffAreaOperationMonitorOptions{
                .expected_source_identity_token = expected_source_token,
                .probe_source_identity =
                    [revalidate = image.revalidate_locked_layout,
                     expected_source_token](
                        const VssDiffAreaSnapshotBinding&) {
                      const auto status = revalidate();
                      return status
                          ? clonecore::Result<std::wstring>::success(
                                expected_source_token)
                          : clonecore::Result<std::wstring>::failure(
                                status.error());
                    },
                .review_callback = request.diff_area_review_callback,
                .logger = request.logger,
            });
        if (!monitor || !monitor.value()) {
          return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
              monitor
                  ? online_error(
                        clonecore::ErrorCode::internal_error,
                        ERROR_INVALID_HANDLE,
                        L"オンラインTsumugi VSS差分領域monitor",
                        L"製品monitor factoryが空のmonitorを返しました")
                  : monitor.error());
        }
        auto active_monitor = monitor.take_value();
        auto status = active_monitor->initial_poll();
        if (!status) {
          return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
              status.error());
        }
        auto prepared = prepare_tsumugi_snapshot_image_v1_with_windows_apis(
            image,
            context,
            active_monitor->callbacks(request.callbacks));
        if (!prepared) {
          return prepared;
        }
        status = active_monitor->completion_poll();
        if (status) {
          status = validate_completed_vss_diff_area_operation_evidence(
              active_monitor->evidence());
        }
        if (!status) {
          return clonecore::Result<imageformat::TsumugiStagedImageV1>::failure(
              with_abort_failure(status.error(), prepared.value()));
        }
        return prepared;
      },
      [&](SnapshotCopyCallback callback) {
        std::unique_ptr<IWorkflowBackend> backend =
            std::make_unique<WindowsVssBackend>(
                WindowsVssBackendOptions{
                    .async_wait = request.async_wait,
                    .copy_snapshot_data = std::move(callback),
                    .logger = request.logger,
                });
        return clonecore::Result<std::unique_ptr<IWorkflowBackend>>::success(
            std::move(backend));
      });
}

}  // namespace ytec::vssrequester

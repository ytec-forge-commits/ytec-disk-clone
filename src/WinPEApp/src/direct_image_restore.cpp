#include "ytec/winpeapp/direct_image_restore.h"

#include <Windows.h>

#include <span>
#include <utility>

namespace ytec::winpeapp {
namespace {

clonecore::Error restore_error(
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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(restore_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

imageformat::TsumugiPhysicalRestoreRequest make_shared_request(
    const DirectImageRestoreRequest& request) {
  return imageformat::TsumugiPhysicalRestoreRequest{
      .image = request.image,
      .expected_image_global_hash = request.expected_image_global_hash,
      .expected_source_state_hash = request.expected_source_state_hash,
      .expected_target = request.expected_target,
      .expected_target_layout_hash = request.expected_target_layout_hash,
      .individual_partition = request.individual_partition,
      .confirmation = request.confirmation,
      .administrator = request.administrator,
      // The mandatory PE resolver has already positively excluded it.
      .target_is_active_rescue_media = false,
      .callbacks = request.callbacks,
  };
}

}  // namespace

clonecore::Result<imageformat::Sha256Digest>
hash_direct_image_restore_target_layout(
    const diskmodel::DiskInfo& target) {
  return imageformat::hash_tsumugi_physical_restore_target_layout_v1(
      target);
}

clonecore::Result<DirectImageRestoreReport> execute_direct_image_restore(
    const DirectImageRestoreRequest& request,
    const DirectImageRestoreDependencies& dependencies) {
  if (!request.administrator ||
      !request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token != L"OK") {
    return failure<DirectImageRestoreReport>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"PE Tsumugi直接復元確認",
        L"管理者権限、消去内容の確認、または大文字OKが不足しています");
  }
  if (!dependencies.is_active_rescue_media) {
    return failure<DirectImageRestoreReport>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_FUNCTION,
        L"PE起動媒体の安定識別",
        L"起動中レスキュー媒体を厳密に照合する依存がありません");
  }
  auto active = dependencies.is_active_rescue_media(
      request.expected_target, request.confirmation);
  if (!active) {
    return clonecore::Result<DirectImageRestoreReport>::failure(
        active.error());
  }
  if (active.value()) {
    return failure<DirectImageRestoreReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"PE起動媒体の消去防止",
        L"現在このWinPEを起動しているレスキューUSB本体は復元先にできません");
  }

  auto restored = imageformat::execute_tsumugi_physical_restore_v1(
      make_shared_request(request), dependencies.physical);
  if (!restored) {
    return clonecore::Result<DirectImageRestoreReport>::failure(
        restored.error());
  }
  return clonecore::Result<DirectImageRestoreReport>::success({
      .physical = restored.take_value(),
      .active_rescue_media_checked = true,
      .direct_execution_only = true,
  });
}

DirectImageRestoreDependencies
make_direct_image_restore_windows_dependencies(
    ActiveRescueMediaTargetQuery active_rescue_media_query) {
  return DirectImageRestoreDependencies{
      .is_active_rescue_media = std::move(active_rescue_media_query),
      .physical = {
          .verify_image = imageformat::verify_tsumugi_image_v1,
          .reidentify_target =
              [](const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation) {
                auto inventory =
                    diskmodel::make_windows_disk_inventory_provider();
                return diskmodel::reidentify_physical_target(
                    target, confirmation, *inventory);
              },
          .set_target_offline =
              [](const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation,
                 const bool offline) {
                return diskmodel::
                    set_verified_physical_target_offline_with_windows_apis(
                        target, confirmation, offline);
              },
          .open_offline_target =
              [](const clonecore::StableDiskIdentity& target,
                 const clonecore::TargetConfirmation& confirmation) {
                return diskmodel::
                    open_verified_physical_target_with_windows_apis(
                        target, confirmation);
              },
          .collect_mbr_signatures = imageformat::
              collect_tsumugi_mbr_signatures_with_windows_apis,
          .make_connection_token = imageformat::
              make_tsumugi_connection_instance_hash_with_windows_apis,
          .execute_engine =
              [](const imageformat::TsumugiImageVerifyRequest& image_request,
                 const imageformat::TsumugiVerifiedImage& initially_verified,
                 const imageformat::TsumugiRestoreDiskIdentity&
                     target_identity,
                 diskmodel::PhysicalTargetHandle handle,
                 const std::span<const std::uint32_t>
                     disallowed_mbr_signatures,
                 const imageformat::
                     TsumugiPhysicalRestoreLockedTargetRevalidator&
                         revalidate_locked_target,
                 const clonecore::DiskOperationCallbacks& callbacks) {
                return imageformat::
                    execute_tsumugi_physical_whole_disk_restore_engine_v1(
                        image_request,
                        initially_verified,
                        target_identity,
                        std::move(handle.target),
                        disallowed_mbr_signatures,
                        imageformat::TsumugiRestoreHost::winpe,
                        revalidate_locked_target,
                        callbacks);
              },
          .execute_individual_partition_engine =
              [](const imageformat::TsumugiImageVerifyRequest& image_request,
                 const imageformat::TsumugiVerifiedImage& initially_verified,
                 const imageformat::TsumugiRestoreDiskIdentity&
                     target_identity,
                 const imageformat::
                     TsumugiPhysicalIndividualPartitionRestoreSelection&
                         selection,
                 diskmodel::PhysicalTargetHandle handle,
                 const imageformat::
                     TsumugiPhysicalRestoreLockedTargetRevalidator&
                         revalidate_locked_target,
                 const clonecore::DiskOperationCallbacks& callbacks) {
                return imageformat::
                    execute_tsumugi_physical_individual_partition_restore_engine_v1(
                        image_request,
                        initially_verified,
                        target_identity,
                        selection,
                        std::move(handle.target),
                        imageformat::TsumugiRestoreHost::winpe,
                        revalidate_locked_target,
                        callbacks);
              },
      },
      .persistent_exact_resume_capable = false,
  };
}

clonecore::Result<DirectImageRestoreReport>
execute_direct_image_restore_with_windows_apis(
    const DirectImageRestoreRequest& request,
    ActiveRescueMediaTargetQuery active_rescue_media_query) {
  return execute_direct_image_restore(
      request,
      make_direct_image_restore_windows_dependencies(
          std::move(active_rescue_media_query)));
}

}  // namespace ytec::winpeapp

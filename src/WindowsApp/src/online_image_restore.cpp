#include "ytec/windowsapp/online_image_restore.h"

#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <utility>

namespace ytec::windowsapp {
namespace {

imageformat::TsumugiPhysicalRestoreRequest to_shared_request(
    const OnlineImageRestoreRequest& request) {
  return imageformat::TsumugiPhysicalRestoreRequest{
      .image = request.image,
      .expected_image_global_hash = request.expected_image_global_hash,
      .expected_source_state_hash = request.expected_source_state_hash,
      .expected_target = request.expected_target,
      .expected_target_layout_hash = request.expected_target_layout_hash,
      .individual_partition = request.individual_partition,
      .confirmation = request.confirmation,
      .administrator = request.administrator,
      .target_is_active_rescue_media =
          request.target_is_active_rescue_media,
      .callbacks = request.callbacks,
  };
}

imageformat::TsumugiPhysicalRestoreDependencies to_shared_dependencies(
    const OnlineImageRestoreDependencies& dependencies) {
  return imageformat::TsumugiPhysicalRestoreDependencies{
      .verify_image = dependencies.verify_image,
      .reidentify_target = dependencies.reidentify_target,
      .set_target_offline = dependencies.set_target_offline,
      .open_offline_target = dependencies.open_offline_target,
      .collect_mbr_signatures = dependencies.collect_mbr_signatures,
      .make_connection_token = dependencies.make_connection_token,
      .execute_engine = dependencies.execute_engine,
      .execute_individual_partition_engine =
          dependencies.execute_individual_partition_engine,
  };
}

}  // namespace

clonecore::Result<imageformat::Sha256Digest>
hash_online_image_restore_target_layout(
    const diskmodel::DiskInfo& target) {
  return imageformat::hash_tsumugi_physical_restore_target_layout_v1(
      target);
}

clonecore::Result<OnlineImageRestoreReport> execute_online_image_restore(
    const OnlineImageRestoreRequest& request,
    const OnlineImageRestoreDependencies& dependencies) {
  auto restored = imageformat::execute_tsumugi_physical_restore_v1(
      to_shared_request(request), to_shared_dependencies(dependencies));
  if (!restored) {
    return clonecore::Result<OnlineImageRestoreReport>::failure(
        restored.error());
  }
  return clonecore::Result<OnlineImageRestoreReport>::success({
      .restore = std::move(restored.value().restore),
      .initial_image_verification_completed =
          restored.value().initial_image_verification_completed,
      .target_reidentified_before_offline =
          restored.value().target_reidentified_before_offline,
      .target_handle_reidentified =
          restored.value().target_handle_reidentified,
      .target_left_offline = restored.value().target_left_offline,
      .boot_repair_offer_required =
          restored.value().boot_repair_offer_required,
      .partial_loss = restored.value().partial_loss,
  });
}

OnlineImageRestoreDependencies
make_online_image_restore_windows_dependencies() {
  return OnlineImageRestoreDependencies{
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
      .collect_mbr_signatures =
          imageformat::collect_tsumugi_mbr_signatures_with_windows_apis,
      .make_connection_token = imageformat::
          make_tsumugi_connection_instance_hash_with_windows_apis,
      .execute_engine =
          [](const imageformat::TsumugiImageVerifyRequest& image_request,
             const imageformat::TsumugiVerifiedImage& initially_verified,
             const imageformat::TsumugiRestoreDiskIdentity& target_identity,
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
                    imageformat::TsumugiRestoreHost::windows,
                    revalidate_locked_target,
                    callbacks);
          },
      .execute_individual_partition_engine =
          [](const imageformat::TsumugiImageVerifyRequest& image_request,
             const imageformat::TsumugiVerifiedImage& initially_verified,
             const imageformat::TsumugiRestoreDiskIdentity& target_identity,
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
                    imageformat::TsumugiRestoreHost::windows,
                    revalidate_locked_target,
                    callbacks);
          },
  };
}

clonecore::Result<OnlineImageRestoreReport>
execute_online_image_restore_with_windows_apis(
    const OnlineImageRestoreRequest& request) {
  return execute_online_image_restore(
      request, make_online_image_restore_windows_dependencies());
}

}  // namespace ytec::windowsapp

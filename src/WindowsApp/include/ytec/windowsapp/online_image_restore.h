#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace ytec::windowsapp {

struct OnlineImageRestoreRequest final {
  imageformat::TsumugiImageVerifyRequest image;
  // Binds execution to the exact completely verified image reviewed by the
  // user. Replacing the file at the same path cannot silently alter the plan.
  imageformat::Sha256Digest expected_image_global_hash{};
  imageformat::Sha256Digest expected_source_state_hash{};
  clonecore::StableDiskIdentity expected_target;
  // Hash of the read-only inventory layout shown on the final review screen.
  // The controller rejects partition/style/geometry drift both before taking
  // the disk offline and again on the exact opened target handle.
  imageformat::Sha256Digest expected_target_layout_hash{};
  std::optional<imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection>
      individual_partition;
  clonecore::TargetConfirmation confirmation;
  bool administrator{};
  // Windows host always supplies false. A future PE caller must derive this
  // from the boot volume's exact parent disk rather than from a UI checkbox.
  bool target_is_active_rescue_media{};
  clonecore::DiskOperationCallbacks callbacks;
};

[[nodiscard]] clonecore::Result<imageformat::Sha256Digest>
hash_online_image_restore_target_layout(
    const diskmodel::DiskInfo& target);

struct OnlineImageRestoreReport final {
  imageformat::TsumugiRestoreReport restore;
  bool initial_image_verification_completed{};
  bool target_reidentified_before_offline{};
  bool target_handle_reidentified{};
  bool target_left_offline{};
  bool boot_repair_offer_required{};
  bool partial_loss{};
};

using OnlineImageRestoreVerifier = std::function<clonecore::Result<
    imageformat::TsumugiVerifiedImage>(
    const imageformat::TsumugiImageVerifyRequest&,
    const clonecore::DiskOperationCallbacks&)>;

using OnlineImageRestoreTargetReidentifier = std::function<clonecore::Result<
    diskmodel::ReidentifiedPhysicalTarget>(
    const clonecore::StableDiskIdentity&,
    const clonecore::TargetConfirmation&)>;

using OnlineImageRestoreTargetOfflineSetter =
    std::function<clonecore::Status(
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&,
        bool)>;

using OnlineImageRestoreTargetOpener = std::function<clonecore::Result<
    diskmodel::PhysicalTargetHandle>(
    const clonecore::StableDiskIdentity&,
    const clonecore::TargetConfirmation&)>;

using OnlineImageRestoreMbrSignatureCollector =
    std::function<clonecore::Result<std::vector<std::uint32_t>>(
        const clonecore::StableDiskIdentity&)>;

using OnlineImageRestoreConnectionTokenGenerator =
    std::function<clonecore::Result<imageformat::Sha256Digest>()>;

using OnlineImageRestoreEngine = std::function<clonecore::Result<
    imageformat::TsumugiRestoreReport>(
    const imageformat::TsumugiImageVerifyRequest&,
    const imageformat::TsumugiVerifiedImage&,
    const imageformat::TsumugiRestoreDiskIdentity&,
    diskmodel::PhysicalTargetHandle,
    std::span<const std::uint32_t>,
    const imageformat::TsumugiPhysicalRestoreLockedTargetRevalidator&,
    const clonecore::DiskOperationCallbacks&)>;

using OnlineImageRestoreIndividualPartitionEngine =
    imageformat::TsumugiPhysicalIndividualPartitionRestoreEngine;

struct OnlineImageRestoreDependencies final {
  OnlineImageRestoreVerifier verify_image;
  OnlineImageRestoreTargetReidentifier reidentify_target;
  OnlineImageRestoreTargetOfflineSetter set_target_offline;
  OnlineImageRestoreTargetOpener open_offline_target;
  OnlineImageRestoreMbrSignatureCollector collect_mbr_signatures;
  OnlineImageRestoreConnectionTokenGenerator make_connection_token;
  OnlineImageRestoreEngine execute_engine;
  OnlineImageRestoreIndividualPartitionEngine
      execute_individual_partition_engine;
};

// Native dependency factory is public so the shared OperationPlan bridge and
// the direct controller use one identical physical-I/O implementation.
[[nodiscard]] OnlineImageRestoreDependencies
make_online_image_restore_windows_dependencies();

// Exact/rescue whole-disk v1 restore controller. It fully verifies the image
// before the target is taken offline, rejects the original source and every
// unsupported target class, re-identifies again through DiskModel, and leaves
// both successful and failed destructive attempts offline.
[[nodiscard]] clonecore::Result<OnlineImageRestoreReport>
execute_online_image_restore(
    const OnlineImageRestoreRequest& request,
    const OnlineImageRestoreDependencies& dependencies);

// Product Windows adapter. It never brings the target online and does not
// perform UAC elevation. The caller must already run elevated.
[[nodiscard]] clonecore::Result<OnlineImageRestoreReport>
execute_online_image_restore_with_windows_apis(
    const OnlineImageRestoreRequest& request);

}  // namespace ytec::windowsapp

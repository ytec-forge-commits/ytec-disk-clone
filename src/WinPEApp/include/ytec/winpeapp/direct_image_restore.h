#pragma once

#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <functional>

namespace ytec::winpeapp {

// Immutable, directly reviewed selection in the current WinPE session. It
// contains no reservation/job identifier and cannot defer execution to a
// later boot.
struct DirectImageRestoreRequest final {
  imageformat::TsumugiImageVerifyRequest image;
  imageformat::Sha256Digest expected_image_global_hash{};
  imageformat::Sha256Digest expected_source_state_hash{};
  clonecore::StableDiskIdentity expected_target;
  imageformat::Sha256Digest expected_target_layout_hash{};
  std::optional<imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection>
      individual_partition;
  clonecore::TargetConfirmation confirmation;
  bool administrator{};
  clonecore::DiskOperationCallbacks callbacks;
};

struct DirectImageRestoreReport final {
  imageformat::TsumugiPhysicalRestoreReport physical;
  bool active_rescue_media_checked{};
  bool direct_execution_only{};
};

// This dependency must resolve the boot medium by stable identity in the
// current PE session. A false result means the resolver positively proved the
// selected target is not that medium; uncertainty is returned as failure.
using ActiveRescueMediaTargetQuery = std::function<clonecore::Result<bool>(
    const clonecore::StableDiskIdentity&,
    const clonecore::TargetConfirmation&)>;

struct DirectImageRestoreDependencies final {
  ActiveRescueMediaTargetQuery is_active_rescue_media;
  imageformat::TsumugiPhysicalRestoreDependencies physical;
  // False for the current physical factory: it cannot reopen the exact same
  // randomly generated incomplete GPT/MBR transaction after process restart.
  // The Resume controller refuses all transfer I/O while this remains false.
  bool persistent_exact_resume_capable{};
};

[[nodiscard]] clonecore::Result<imageformat::Sha256Digest>
hash_direct_image_restore_target_layout(
    const diskmodel::DiskInfo& target);

// Exact/rescue whole-disk restore, exact existing-partition placement, and a
// new GPT/MBR partition in reviewed unallocated space are supported. Shrink
// placement remains fail-closed. Both a successful and a failed destructive
// attempt leave the target offline.
[[nodiscard]] clonecore::Result<DirectImageRestoreReport>
execute_direct_image_restore(
    const DirectImageRestoreRequest& request,
    const DirectImageRestoreDependencies& dependencies);

// Builds the audited Windows/WinPE physical-I/O dependencies. The active
// rescue-media resolver remains mandatory and is supplied by the PE bootstrap
// once it has bound the currently booted Y-TEC medium to a stable identity.
[[nodiscard]] DirectImageRestoreDependencies
make_direct_image_restore_windows_dependencies(
    ActiveRescueMediaTargetQuery active_rescue_media_query);

[[nodiscard]] clonecore::Result<DirectImageRestoreReport>
execute_direct_image_restore_with_windows_apis(
    const DirectImageRestoreRequest& request,
    ActiveRescueMediaTargetQuery active_rescue_media_query);

}  // namespace ytec::winpeapp

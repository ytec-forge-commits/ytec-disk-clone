#pragma once

#include "ytec/directshrink/target_contract.h"
#include "ytec/diskmodel/physical_disk.h"

#include <memory>

namespace ytec::directshrink {

struct WindowsTargetPlatformRequest final {
  clonecore::TargetConfirmation confirmation;
  clonecore::DiskOperationCallbacks callbacks;
};

// WinPE production adapter for the shared target-only transaction. Factory
// construction performs no destructive disk I/O. The caller must already
// have reidentified the exact two disks and proved that the physical source
// is held OS-level read-only; begin_target_owned_staging() is the first method
// allowed to invalidate or write target metadata.
[[nodiscard]] clonecore::Result<std::unique_ptr<ITargetPlatform>>
make_windows_target_platform_for_winpe(
    const TargetPlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const WindowsTargetPlatformRequest& request);

}  // namespace ytec::directshrink

#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

namespace ytec::winpeapp {

// Product WinPE clone gate shared by the read-only direct-operation review and
// the destructive service. It accepts only an online, fixed, basic GPT/MBR source
// and a fixed target whose layout is empty RAW or known basic GPT/MBR. The
// confirmed destructive service initializes only the reidentified target.
// Unknown buses/layouts, Storage Spaces/LDM metadata, and a target whose
// SMART/NVMe state is caution or failing all fail closed. Unsupported health
// telemetry remains unknown rather than being misreported as a failure.
[[nodiscard]] clonecore::Status validate_clone_execution_observation(
    const diskmodel::DiskInfo& source,
    const diskmodel::DiskInfo& target,
    bool require_target_same_or_larger = true);

}  // namespace ytec::winpeapp

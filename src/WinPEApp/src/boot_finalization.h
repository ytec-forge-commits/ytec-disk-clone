#pragma once

#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <optional>

namespace ytec::winpeapp {

struct ProductBootFinalizationReport final {
  bootrepair::StandaloneBootRepairReport boot_repair;
  bool windows_partition_temporarily_mounted{};
  bool system_partition_temporarily_mounted{};
  bool temporary_mounts_released{};
  bool final_target_verified{};
};

// Finalizes only the already restored/cloned target. The implementation
// re-identifies the physical disk, mounts exact physical partitions through
// Microsoft-signed DiskPart, runs Microsoft-signed BCDBoot, verifies the BCD
// store, releases every temporary letter, and re-identifies the target again.
[[nodiscard]] clonecore::Result<ProductBootFinalizationReport>
finalize_product_target_boot(
    diskmodel::IDiskInventoryProvider& inventory,
    const clonecore::StableDiskIdentity& expected_target,
    diskmodel::PartitionStyle expected_style,
    std::optional<std::uint64_t> expected_windows_partition_offset,
    const clonecore::DiskOperationCallbacks& callbacks);

}  // namespace ytec::winpeapp

#pragma once

#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>

namespace ytec::diskmodel {

enum class CloneTargetLayoutKind : std::uint8_t {
  empty_raw,
  supported_initialized,
  unsupported,
};

// A clone target may already be initialized, because the confirmed WinPE
// operation replaces its entire partition layout. Unknown, dynamic, and
// Storage Spaces layouts still fail closed before the target is opened.
[[nodiscard]] CloneTargetLayoutKind classify_clone_target_layout(
    const DiskInfo& disk) noexcept;

}  // namespace ytec::diskmodel

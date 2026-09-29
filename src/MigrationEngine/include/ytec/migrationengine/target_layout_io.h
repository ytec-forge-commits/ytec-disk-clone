#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/migrationengine/target_layout.h"

#include <cstdint>

namespace ytec::migrationengine {

struct ShrinkTargetMetadataWriteReport final {
  std::uint64_t invalidated_bytes{};
  std::uint64_t metadata_bytes{};
  bool every_write_read_back_verified{};
  bool partition_table_committed{};
};

// Destructive target-only boundary. The caller must already hold a verified
// offline target writer. Old partition metadata is invalidated first; new GPT
// primary header or MBR sector is committed last after read-back verification.
[[nodiscard]] clonecore::Result<ShrinkTargetMetadataWriteReport>
write_shrink_target_metadata(
    const ShrinkTargetLayout& layout,
    clonecore::ITargetDiskWriter& target,
    const clonecore::DiskOperationCallbacks& callbacks = {});

}  // namespace ytec::migrationengine

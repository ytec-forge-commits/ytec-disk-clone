#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/migrationengine/shrink_bundle.h"
#include "ytec/migrationengine/target_layout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ytec::migrationengine {

struct ShrinkVolumeApplyReport final {
  std::uint32_t formatted_volume_count{};
  std::uint32_t applied_wim_count{};
  std::uint64_t applied_payload_bytes{};
  bool every_volume_extent_reidentified{};
  bool every_temporary_mount_released{};
};

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_format_arguments(
    const std::wstring& format_target,
    migrationcore::MigrationFileSystem file_system,
    std::uint64_t cluster_size);

// Destructive target-volume boundary. Requires a freshly reidentified online
// target and a bundle that remains locked. Every new volume is matched by the
// exact target disk number and planned offset before format and WIM apply.
[[nodiscard]] clonecore::Result<ShrinkVolumeApplyReport>
format_and_apply_shrink_volumes(
    const diskmodel::DiskInfo& observed_target,
    const ShrinkTargetLayout& layout,
    const VerifiedShrinkBundle& bundle,
    const std::wstring& scratch_directory,
    const std::wstring& trusted_system_directory,
    bootrepair::IExecutableTrustVerifier& trust_verifier,
    bootrepair::IProcessRunner& process_runner,
    const clonecore::DiskOperationCallbacks& callbacks = {});

}  // namespace ytec::migrationengine

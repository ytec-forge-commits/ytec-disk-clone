#pragma once

#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/bootrepair/system_volume_mount.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::bootrepair {

struct CloneBootFinalizationRequest final {
  clonecore::StableDiskIdentity expected_target;
  diskmodel::PartitionStyle expected_style{
      diskmodel::PartitionStyle::unknown};
  std::optional<std::uint64_t> expected_windows_partition_offset;
};

struct CloneBootFinalizationReport final {
  StandaloneBootRepairReport boot_repair;
  bool windows_partition_temporarily_mounted{};
  bool system_partition_temporarily_mounted{};
  bool temporary_mounts_released{};
  bool final_target_reidentified{};
  bool partition_layout_unchanged{};
};

// Read-only observations that are deliberately kept separate from disk
// mutation.  The Windows implementation uses Volume GUID enumeration and an
// exact offline-Windows probe; tests can provide synthetic observations.
class ICloneBootFinalizationVolumeProvider {
 public:
  virtual ~ICloneBootFinalizationVolumeProvider() = default;

  [[nodiscard]] virtual clonecore::Result<
      std::vector<BootVolumeObservation>>
  observe_volumes_read_only() = 0;

  // Gives the operating system a bounded opportunity to publish newly-online
  // target volumes.  Production waits for a short fixed interval; tests can
  // advance synthetic observations without sleeping.
  [[nodiscard]] virtual clonecore::Status wait_before_volume_retry() = 0;

  [[nodiscard]] virtual clonecore::Result<std::wstring>
  unavailable_drive_letters() = 0;

  // Returns false only when the exact Volume GUID contains no Windows
  // installation.  A present but corrupt, unsupported, or unverifiable
  // installation is an error and must stop finalization.
  [[nodiscard]] virtual clonecore::Result<bool>
  contains_supported_offline_windows(
      const std::wstring& volume_root) = 0;
};

class ICloneBootFinalizationService {
 public:
  virtual ~ICloneBootFinalizationService() = default;

  [[nodiscard]] virtual clonecore::Result<CloneBootFinalizationReport>
  execute(const CloneBootFinalizationRequest& request) = 0;
};

// Common Windows/WinPE orchestration boundary.  It never trusts a remembered
// disk number: the stable target is reidentified before mounting and after all
// temporary roots have been released.  Only exact single-disk Volume GUID
// mappings are mounted.  UEFI finalization first classifies the exact ESP
// Volume GUID read-only and accepts only Microsoft-owned or empty content; the
// injected boot-repair service must independently recheck that immutable
// evidence immediately before executing the transactional BCDBoot boundary.
[[nodiscard]] clonecore::Result<CloneBootFinalizationReport>
finalize_cloned_windows_boot(
    const CloneBootFinalizationRequest& request,
    diskmodel::IDiskInventoryProvider& inventory,
    ICloneBootFinalizationVolumeProvider& volume_provider,
    IEfiBootOwnershipInspector& efi_ownership_inspector,
    ISystemVolumeMountApi& mount_api,
    IStandaloneBootRepairService& boot_repair_service);

// Uses only Windows public APIs and is valid from both the installed Windows
// application and WinPE.  The caller must keep the already cloned target
// online for the duration of this transaction and may offline it afterwards.
[[nodiscard]] std::unique_ptr<ICloneBootFinalizationService>
make_windows_clone_boot_finalization_service(
    diskmodel::IDiskInventoryProvider& inventory);

}  // namespace ytec::bootrepair

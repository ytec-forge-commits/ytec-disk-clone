#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/efi_boot_ownership.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ytec::bootrepair {

enum class BootRepairThirdPartyEfiPolicy : std::uint8_t {
  not_applicable,
  preserve,
  delete_non_microsoft,
};

struct BootRepairTargetRequest final {
  std::uint32_t disk_number{};
  std::wstring windows_root;
  std::wstring system_root;
  BcdBootFirmware firmware{BcdBootFirmware::uefi};
  BcdBootStorePolicy store_policy{
      BcdBootStorePolicy::preserve_existing};
  bool auto_mount_system_partition{};
  // Required for every UEFI execution path. The exact Volume GUID root keeps
  // ownership inspection independent from temporary drive-letter mounting.
  std::wstring system_volume_identity_root;
  bool require_efi_ownership_recheck{};
  EfiBootOwnershipEvidence expected_efi_ownership;
  // Set only by the immutable ReviewedAutomaticBootRepairChoices bridge.
  // "preserve" authorizes BCDBoot /s to rebuild Microsoft's own boot files;
  // it never authorizes deleting or renaming a third-party EFI namespace.
  BootRepairThirdPartyEfiPolicy third_party_efi_policy{
      BootRepairThirdPartyEfiPolicy::not_applicable};
  // Batch members are executed only through execute_multi_windows(). This
  // keeps a later preserve_existing member from being executed by itself.
  bool reviewed_multi_windows_batch{};
  // Reserved for an explicit future same-PC NVRAM flow. 1.0.0's direct
  // existing-ESP transaction rejects true and always uses BCDBoot /s.
  bool update_current_pc_nvram{};
};

struct BootRepairVolumeLocation final {
  std::uint32_t disk_number{};
  std::uint64_t starting_offset{};
  std::uint64_t extent_length{};
  std::wstring file_system;
};

struct BootRepairTargetSelection final {
  diskmodel::DiskInfo disk;
  clonecore::StableDiskIdentity identity;
  diskmodel::PartitionInfo windows_partition;
  diskmodel::PartitionInfo system_partition;
};

struct StandaloneBootRepairExecutionRequest final {
  BootRepairTargetRequest target;
  BootRepairTargetSelection expected;
  clonecore::TargetConfirmation confirmation;
};

struct StandaloneBootRepairReport final {
  BootRepairTargetSelection repaired;
  BcdBootReport bcdboot;
  bool boot_store_verified{};
  bool system_partition_temporarily_mounted{};
  bool temporary_mount_released{};
  bool efi_ownership_revalidated{};
  bool nvram_unchanged{};
};

struct MultiWindowsStandaloneBootRepairExecutionRequest final {
  std::vector<BootRepairTargetRequest> targets_in_boot_priority;
  std::vector<BootRepairTargetSelection> expected_in_boot_priority;
  clonecore::TargetConfirmation confirmation;
};

struct MultiWindowsStandaloneBootRepairReport final {
  std::vector<BootRepairTargetSelection> repaired_in_boot_priority;
  MultiWindowsBcdBootReport bcdboot;
  bool boot_store_verified{};
  bool system_partition_temporarily_mounted{};
  bool temporary_mount_released{};
  bool efi_ownership_revalidated{};
  bool nvram_unchanged{};
};

[[nodiscard]] clonecore::Result<BootRepairTargetSelection>
evaluate_boot_repair_target(
    const BootRepairTargetRequest& request,
    const diskmodel::InventoryReport& inventory,
    const BootRepairVolumeLocation& windows_volume,
    const BootRepairVolumeLocation& system_volume);

[[nodiscard]] std::wstring make_boot_repair_confirmation_token(
    const clonecore::StableDiskIdentity& identity,
    BcdBootFirmware firmware);

[[nodiscard]] clonecore::Status validate_boot_repair_selection(
    const BootRepairTargetSelection& expected,
    const BootRepairTargetSelection& observed,
    BcdBootFirmware firmware,
    const clonecore::TargetConfirmation& confirmation);

[[nodiscard]] clonecore::Status validate_boot_repair_efi_ownership(
    const BootRepairTargetRequest& request,
    const EfiBootOwnershipEvidence& observed);

class IStandaloneBootRepairService {
 public:
  virtual ~IStandaloneBootRepairService() = default;

  [[nodiscard]] virtual clonecore::Result<BootRepairTargetSelection> inspect(
      const BootRepairTargetRequest& request) = 0;

  [[nodiscard]] virtual clonecore::Result<StandaloneBootRepairReport> execute(
      const StandaloneBootRepairExecutionRequest& request) = 0;

  [[nodiscard]] virtual clonecore::Result<
      MultiWindowsStandaloneBootRepairReport>
  execute_multi_windows(
      const MultiWindowsStandaloneBootRepairExecutionRequest& request);
};

[[nodiscard]] std::unique_ptr<IStandaloneBootRepairService>
make_windows_standalone_boot_repair_service(
    diskmodel::IDiskInventoryProvider& inventory);

}  // namespace ytec::bootrepair

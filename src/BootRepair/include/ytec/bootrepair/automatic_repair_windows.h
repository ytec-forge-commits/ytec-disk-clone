#pragma once

#include "ytec/bootrepair/automatic_repair_plan.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ytec::bootrepair {

// Low-level read-only seam around FindFirstVolume/FindNextVolume. The Windows
// implementation delegates to enumerate_windows_boot_volumes_read_only().
class IReadOnlyBootVolumeEnumerator {
 public:
  virtual ~IReadOnlyBootVolumeEnumerator() = default;

  [[nodiscard]] virtual clonecore::Result<
      std::vector<BootVolumeObservation>>
  enumerate_read_only() = 0;
};

// Filters a global observation set to exact partitions of the selected disk.
// Observations from other disks are discarded before validation. Any selected
// observation that is incomplete, duplicated, or does not exactly match one
// selected partition fails closed.
[[nodiscard]] clonecore::Result<std::vector<BootVolumeObservation>>
filter_boot_repair_volumes_for_selected_disk(
    const diskmodel::DiskInfo& selected_disk,
    std::vector<BootVolumeObservation> all_volumes);

[[nodiscard]] std::unique_ptr<IBootRepairVolumeObservationProvider>
make_filtered_boot_repair_volume_observation_provider(
    std::unique_ptr<IReadOnlyBootVolumeEnumerator> enumerator);

[[nodiscard]] std::unique_ptr<IBootRepairVolumeObservationProvider>
make_windows_boot_repair_volume_observation_provider();

enum class OfflineWindowsRootObservation : std::uint8_t {
  absent,
  candidate,
};

// Low-level seam used to classify an exact Volume GUID root. The Windows
// implementation verifies the Windows directory chain and kernel as
// non-reparse objects, reads the offline SOFTWARE hive, and delegates final
// x64/version validation to verify_offline_windows_amd64().
class IOfflineWindowsReadOnlyProbe {
 public:
  virtual ~IOfflineWindowsReadOnlyProbe() = default;

  [[nodiscard]] virtual clonecore::Result<OfflineWindowsRootObservation>
  inspect_root_read_only(const std::wstring& volume_root) = 0;

  [[nodiscard]] virtual clonecore::Result<OfflineWindowsVersion>
  read_version_read_only(const std::wstring& volume_root) = 0;

  [[nodiscard]] virtual clonecore::Status verify_supported_read_only(
      const std::wstring& volume_root) = 0;
};

// Converts platform observations to the pure planner's absent/supported/
// unsupported state. Only a complete, regular candidate with a valid offline
// hive may be reported as unsupported; other failures are propagated.
[[nodiscard]] clonecore::Result<OfflineWindowsCandidateValidation>
classify_offline_windows_candidate(
    const std::wstring& volume_root,
    IOfflineWindowsReadOnlyProbe& probe);

[[nodiscard]] std::unique_ptr<IOfflineWindowsCandidateValidator>
make_offline_windows_candidate_validator(
    std::unique_ptr<IOfflineWindowsReadOnlyProbe> probe);

[[nodiscard]] std::unique_ptr<IOfflineWindowsCandidateValidator>
make_windows_offline_windows_candidate_validator();

class IAutomaticBootRepairPlanService {
 public:
  virtual ~IAutomaticBootRepairPlanService() = default;

  [[nodiscard]] virtual clonecore::Result<AutomaticBootRepairPlan> plan(
      const clonecore::StableDiskIdentity& selected_identity) = 0;

  [[nodiscard]] virtual clonecore::Result<AutomaticBootRepairPlan> plan(
      const diskmodel::DiskInfo& selected_disk) = 0;
};

// Ownership-based composition seam for synthetic tests and future hosts.
// Returns nullptr when any required read-only dependency is missing.
[[nodiscard]] std::unique_ptr<IAutomaticBootRepairPlanService>
make_automatic_boot_repair_plan_service(
    std::unique_ptr<diskmodel::IDiskInventoryProvider> inventory,
    std::unique_ptr<IBootRepairVolumeObservationProvider> volumes,
    std::unique_ptr<IOfflineWindowsCandidateValidator> windows_validator,
    std::unique_ptr<IWinReDiagnosticService> winre_inspector,
    std::unique_ptr<IEfiBootOwnershipInspector> efi_ownership_inspector);

// Common Windows/WinPE factory. Construction performs no enumeration or I/O;
// plan() performs only the read-only calls described above.
[[nodiscard]] std::unique_ptr<IAutomaticBootRepairPlanService>
make_windows_automatic_boot_repair_plan_service();

// One-shot public entry points for callers that only have the selected disk.
[[nodiscard]] clonecore::Result<AutomaticBootRepairPlan>
plan_automatic_boot_repair_with_windows_apis(
    const clonecore::StableDiskIdentity& selected_identity);

[[nodiscard]] clonecore::Result<AutomaticBootRepairPlan>
plan_automatic_boot_repair_with_windows_apis(
    const diskmodel::DiskInfo& selected_disk);

}  // namespace ytec::bootrepair

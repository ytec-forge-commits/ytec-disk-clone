#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/efi_boot_ownership.h"
#include "ytec/bootrepair/offline_windows.h"
#include "ytec/bootrepair/system_volume_mount.h"
#include "ytec/bootrepair/winre_diagnostic.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::bootrepair {

enum class OfflineWindowsCandidateState : std::uint8_t {
  absent,
  present_supported,
  present_unsupported,
};

struct OfflineWindowsCandidateValidation final {
  OfflineWindowsCandidateState state{
      OfflineWindowsCandidateState::absent};
  OfflineWindowsVersion version;
};

// Validates one exact Volume GUID or drive root without changing the volume.
// "absent" means the volume is not a Windows installation. A present but
// unreadable or internally inconsistent installation must be returned as an
// error instead of being treated as absent.
class IOfflineWindowsCandidateValidator {
 public:
  virtual ~IOfflineWindowsCandidateValidator() = default;

  [[nodiscard]] virtual clonecore::Result<
      OfflineWindowsCandidateValidation>
  inspect_volume_read_only(const std::wstring& volume_root) = 0;
};

// Supplies exact single-disk Volume GUID observations. Implementations may
// enumerate globally, but must not mount, unlock, format, or otherwise change
// a volume while producing these observations.
class IBootRepairVolumeObservationProvider {
 public:
  virtual ~IBootRepairVolumeObservationProvider() = default;

  [[nodiscard]] virtual clonecore::Result<
      std::vector<BootVolumeObservation>>
  observe_read_only(const diskmodel::DiskInfo& selected_disk) = 0;
};

enum class BootSystemPartitionRole : std::uint8_t {
  efi_system,
  bios_active,
};

struct AutomaticBootRepairWinReEvidence final {
  WinReSourceState source_state{WinReSourceState::unknown};
  bool registered_location_reported{};
  bool registered_location_matches_selected_disk{};
  std::uint32_t registered_partition_number{};
  bool registered_path_kind_reported{};
  WinReRegisteredPathKind registered_path_kind{
      WinReRegisteredPathKind::recovery_windows_re};
  bool registered_image_present{};
  bool fallback_image_present{};
  std::uint64_t image_size_bytes{};
};

struct DiscoveredWindowsInstallation final {
  diskmodel::PartitionInfo partition;
  BootVolumeObservation volume;
  std::wstring windows_directory;
  OfflineWindowsVersion version;
  bool officially_supported{};
  AutomaticBootRepairWinReEvidence winre;
};

struct DiscoveredSystemPartition final {
  diskmodel::PartitionInfo partition;
  BootVolumeObservation volume;
  BootSystemPartitionRole role{BootSystemPartitionRole::efi_system};
  EfiBootOwnershipEvidence efi_ownership;
};

// This object is a read-only plan. It contains no confirmation token, mount
// request, shrink geometry, or write callback. Consumers must run a separate
// confirmed execution transaction after reidentifying the disk again.
struct AutomaticBootRepairPlan final {
  diskmodel::DiskInfo selected_disk;
  clonecore::StableDiskIdentity selected_identity;
  diskmodel::PartitionStyle partition_style{
      diskmodel::PartitionStyle::unknown};
  BcdBootFirmware firmware{BcdBootFirmware::uefi};
  BootSystemPartitionRole required_system_partition_role{
      BootSystemPartitionRole::efi_system};
  BcdBootStorePolicy planned_bcd_store_policy{
      BcdBootStorePolicy::rebuild_fresh};
  std::vector<DiscoveredWindowsInstallation> windows_installations;
  std::vector<DiscoveredSystemPartition> system_partition_candidates;
  bool windows_not_found{};
  bool windows_selection_policy_needed{};
  bool unsupported_windows_policy_needed{};
  bool system_partition_create_plan_needed{};
  bool system_partition_selection_policy_needed{};
};

// A multi-boot disk is never reduced to an implicit "first Windows" choice.
// The UI must either provide every discovered Windows partition in explicit
// boot priority order, or provide exactly one partition to register.
enum class AutomaticWindowsRegistrationPolicy : std::uint8_t {
  all_with_explicit_priority,
  selected_only,
};

// Only applicable when a read-only ESP inspection found classified
// third-party or untrusted EFI content. Ambiguous content is never made
// actionable by either choice; it remains fail-closed.
enum class AutomaticThirdPartyEfiPolicy : std::uint8_t {
  not_applicable,
  preserve,
  delete_non_microsoft,
};

enum class AutomaticNvramRepairPolicy : std::uint8_t {
  leave_unchanged,
  repair_current_pc_windows_boot_manager,
};

enum class AutomaticWinReRepairDisposition : std::uint8_t {
  verify_existing_registration,
  register_verified_windows_image,
  normal_boot_only_partial,
};

struct AutomaticWinReRepairChoice final {
  std::uint32_t windows_partition_number{};
  AutomaticWinReRepairDisposition disposition{
      AutomaticWinReRepairDisposition::normal_boot_only_partial};
};

struct AutomaticBootRepairChoiceRequest final {
  AutomaticWindowsRegistrationPolicy windows_policy{
      AutomaticWindowsRegistrationPolicy::selected_only};
  std::vector<std::uint32_t> windows_partition_priority;
  std::uint32_t system_partition_number{};
  AutomaticThirdPartyEfiPolicy third_party_efi_policy{
      AutomaticThirdPartyEfiPolicy::not_applicable};
  AutomaticNvramRepairPolicy nvram_policy{
      AutomaticNvramRepairPolicy::leave_unchanged};
};

// Immutable reviewed choices derived from one read-only discovery snapshot.
// The stored snapshot is private so callers cannot silently substitute another
// Windows/ESP after the review. Before confirmation or execution, callers must
// perform discovery again and call revalidate_automatic_boot_repair_choices().
class ReviewedAutomaticBootRepairChoices final {
 public:
  [[nodiscard]] const clonecore::StableDiskIdentity& selected_identity()
      const noexcept;
  [[nodiscard]] diskmodel::PartitionStyle partition_style() const noexcept;
  [[nodiscard]] BcdBootFirmware firmware() const noexcept;
  [[nodiscard]] BcdBootStorePolicy bcd_store_policy() const noexcept;
  [[nodiscard]] AutomaticWindowsRegistrationPolicy windows_policy()
      const noexcept;
  [[nodiscard]] AutomaticThirdPartyEfiPolicy third_party_efi_policy()
      const noexcept;
  [[nodiscard]] AutomaticNvramRepairPolicy nvram_policy() const noexcept;
  [[nodiscard]] std::span<const DiscoveredWindowsInstallation>
  windows_in_boot_priority() const noexcept;
  [[nodiscard]] std::span<const AutomaticWinReRepairChoice>
  winre_choices_in_boot_priority() const noexcept;
  [[nodiscard]] const DiscoveredSystemPartition& system_partition()
      const noexcept;

 private:
  ReviewedAutomaticBootRepairChoices(
      AutomaticBootRepairPlan discovery,
      AutomaticBootRepairChoiceRequest request,
      std::vector<DiscoveredWindowsInstallation> windows,
      std::vector<AutomaticWinReRepairChoice> winre_choices,
      DiscoveredSystemPartition system_partition);

  AutomaticBootRepairPlan discovery_;
  AutomaticBootRepairChoiceRequest request_;
  std::vector<DiscoveredWindowsInstallation> windows_;
  std::vector<AutomaticWinReRepairChoice> winre_choices_;
  DiscoveredSystemPartition system_partition_;

  friend clonecore::Result<ReviewedAutomaticBootRepairChoices>
  review_automatic_boot_repair_choices(
      const AutomaticBootRepairPlan&,
      const AutomaticBootRepairChoiceRequest&);
  friend clonecore::Result<ReviewedAutomaticBootRepairChoices>
  revalidate_automatic_boot_repair_choices(
      const ReviewedAutomaticBootRepairChoices&,
      const AutomaticBootRepairPlan&);
};

// Performs no I/O. Missing Windows/system partitions, unsupported selected
// Windows versions, duplicate priority entries, unreviewed third-party EFI,
// policy choices where no third-party content exists, and ambiguous EFI
// content all fail closed. This binds the BCD-004 preserve/delete choice but
// does not authorize or perform deletion.
[[nodiscard]] clonecore::Result<ReviewedAutomaticBootRepairChoices>
review_automatic_boot_repair_choices(
    const AutomaticBootRepairPlan& discovery,
    const AutomaticBootRepairChoiceRequest& request);

// Binds the user's choices to a fresh read-only discovery. Disk-number churn is
// allowed after stable reidentification, but partition geometry, Volume GUIDs,
// Windows versions/WinRE evidence, ESP ownership, and the complete candidate
// sets must remain equivalent. The returned choices contain the fresh routing
// values and are suitable for the later uppercase-OK execution plan.
[[nodiscard]] clonecore::Result<ReviewedAutomaticBootRepairChoices>
revalidate_automatic_boot_repair_choices(
    const ReviewedAutomaticBootRepairChoices& reviewed,
    const AutomaticBootRepairPlan& fresh_discovery);

// Orchestrates only read-only discovery. The injected services are the seams
// used by the future Windows and WinPE adapters and by fully synthetic tests.
class AutomaticBootRepairPlanner final {
 public:
  AutomaticBootRepairPlanner(
      diskmodel::IDiskInventoryProvider& inventory,
      IBootRepairVolumeObservationProvider& volumes,
      IOfflineWindowsCandidateValidator& windows_validator,
      IWinReDiagnosticService& winre_inspector,
      IEfiBootOwnershipInspector& efi_ownership_inspector) noexcept;

  [[nodiscard]] clonecore::Result<AutomaticBootRepairPlan> plan(
      const clonecore::StableDiskIdentity& selected_identity);

  // The supplied DiskInfo is converted to its stable identity. Its remembered
  // disk number and partition layout are never trusted; inventory is refreshed
  // before any candidate is returned.
  [[nodiscard]] clonecore::Result<AutomaticBootRepairPlan> plan(
      const diskmodel::DiskInfo& selected_disk);

 private:
  diskmodel::IDiskInventoryProvider& inventory_;
  IBootRepairVolumeObservationProvider& volumes_;
  IOfflineWindowsCandidateValidator& windows_validator_;
  IWinReDiagnosticService& winre_inspector_;
  IEfiBootOwnershipInspector& efi_ownership_inspector_;
};

}  // namespace ytec::bootrepair

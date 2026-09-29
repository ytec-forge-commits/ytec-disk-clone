#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/windowsapp/media_preflight.h"
#include "ytec/windowsapp/rescue_media_storage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ytec::windowsapp {

enum class RescueMediaKind : std::uint8_t {
  iso_file,
  usb_drive,
};

enum class RescueMediaBootProfile : std::uint8_t {
  windows_uefi_2011_ca,
  windows_uefi_2023_ca,
};

enum class RescueMediaPlanIssue : std::uint8_t {
  environment_check_required,
  environment_not_ready,
  boot_profile_unavailable,
  iso_destination_missing,
  iso_destination_invalid,
  inventory_loading,
  inventory_unavailable,
  usb_target_not_selected,
  usb_target_is_system,
  usb_target_state_unknown,
  usb_target_not_removable,
  usb_target_not_usb,
  usb_target_read_only,
  usb_target_offline,
  usb_target_style_unknown,
  usb_target_not_single_partition,
  usb_target_too_small,
  usb_inspection_required,
  usb_refresh_ownership_required,
  usb_target_identity_unstable,
  ready_for_confirmation,
};

struct RescueMediaPlanInput final {
  const MediaPreflightView* preflight{};
  RescueMediaKind kind{RescueMediaKind::iso_file};
  RescueMediaBootProfile boot_profile{
      RescueMediaBootProfile::windows_uefi_2011_ca};
  std::wstring iso_destination;
  const diskmodel::InventoryReport* inventory{};
  std::optional<std::size_t> usb_target_index;
  RescueUsbProvisioningMode usb_provisioning_mode{
      RescueUsbProvisioningMode::initialize_all};
  RescueUsbDataFileSystem usb_data_file_system{
      RescueUsbDataFileSystem::ntfs};
  const RescueUsbOwnedMediaInspection* usb_owned_media{};
  const RescueUsbStoragePlan* reviewed_usb_storage_plan{};
  bool inventory_loading{};
};

struct RescueMediaPlanView final {
  RescueMediaPlanIssue issue{
      RescueMediaPlanIssue::environment_check_required};
  std::uint8_t current_step{1U};
  bool ready_for_confirmation{};
  std::optional<clonecore::StableDiskIdentity> usb_target_identity;
  std::optional<RescueUsbStoragePlan> usb_storage_plan;
  std::wstring confirmation_token;
  std::wstring message;
  std::wstring summary;
};

struct RescueUsbAuthorizationRequest final {
  clonecore::StableDiskIdentity expected_target;
  const diskmodel::InventoryReport* fresh_inventory{};
  bool first_step_acknowledged{};
  std::wstring typed_confirmation;
  std::optional<RescueUsbStoragePlan> reviewed_storage_plan;
  const RescueUsbOwnedMediaInspection* fresh_owned_media{};
};

struct RescueUsbTargetAuthorization final {
  clonecore::StableDiskIdentity target;
  std::wstring confirmation_token;
  std::size_t partition_count{};
  std::optional<RescueUsbStoragePlan> storage_plan;
  bool physical_write_started{};
};

[[nodiscard]] bool is_safe_iso_destination_syntax(
    const std::wstring& path) noexcept;

// Builds a read-only UI plan. It never opens an output file or a USB disk and
// never authorizes media creation. The execution service must re-probe every
// path and stable disk identity immediately before a later elevated write.
[[nodiscard]] RescueMediaPlanView evaluate_rescue_media_plan(
    const RescueMediaPlanInput& input);

// Re-evaluates the selected USB against a fresh read-only inventory and the
// two-step confirmation. It never opens the disk and always returns
// physical_write_started=false. A later target-only writer must call this in
// the same invocation immediately before opening any physical target.
[[nodiscard]] clonecore::Result<RescueUsbTargetAuthorization>
authorize_rescue_usb_target(
    const RescueUsbAuthorizationRequest& request);

[[nodiscard]] std::wstring rescue_media_kind_label(
    RescueMediaKind kind);

[[nodiscard]] std::wstring rescue_media_boot_profile_label(
    RescueMediaBootProfile profile);

}  // namespace ytec::windowsapp

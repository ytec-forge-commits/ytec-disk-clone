#pragma once

#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/windowsapp/rescue_media_inspection.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ytec::windowsapp {

enum class RescueMediaUsbUiIssue : std::uint8_t {
  not_usb,
  target_missing,
  inspection_required,
  inspection_scanning,
  inspection_blocked,
  inspection_drift,
  selection_invalid,
  ready,
};

struct RescueMediaUsbUiInput final {
  bool usb_kind_selected{};
  const diskmodel::DiskInfo* selected_target{};
  RescueUsbInspectionState inspection_state{
      RescueUsbInspectionState::not_started};
  const RescueUsbOwnedMediaEvidence* owned_evidence{};
  std::wstring_view inspection_message;
  std::optional<RescueUsbProvisioningMode> selected_mode;
  std::optional<RescueUsbDataFileSystem> selected_file_system;
  bool operation_running{};
};

// Pure presentation model. It carries only a bounded aggregate count/byte
// summary for private data and never exposes its root digest or filenames.
struct RescueMediaUsbUiView final {
  RescueMediaUsbUiIssue issue{RescueMediaUsbUiIssue::not_usb};
  bool selectors_visible{};
  bool mode_selector_enabled{};
  bool file_system_selector_enabled{};
  bool ready_for_review{};
  RescueUsbProvisioningMode recommended_mode{
      RescueUsbProvisioningMode::initialize_all};
  RescueUsbDataFileSystem recommended_file_system{
      RescueUsbDataFileSystem::ntfs};
  std::optional<RescueUsbStoragePlan> storage_plan;
  std::wstring confirmation_token;
  std::wstring status;
  std::wstring review_summary;
};

[[nodiscard]] RescueMediaUsbUiView build_rescue_media_usb_ui_view(
    const RescueMediaUsbUiInput& input);

[[nodiscard]] std::wstring_view rescue_usb_ui_mode_label(
    RescueUsbProvisioningMode mode) noexcept;

[[nodiscard]] std::wstring_view rescue_usb_ui_file_system_label(
    RescueUsbDataFileSystem file_system) noexcept;

}  // namespace ytec::windowsapp

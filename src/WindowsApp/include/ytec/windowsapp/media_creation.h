#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/windowsapp/media_preflight.h"
#include "ytec/windowsapp/media_wizard.h"
#include "ytec/windowsapp/usb_volume_mapping.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ytec::windowsapp {

enum class RescueMediaCreationStage : std::uint8_t {
  validating_request,
  verifying_environment,
  verifying_product_payload,
  preparing_work_area,
  staging_adk_media,
  servicing_wim,
  generating_iso,
  verifying_iso,
  publishing_iso,
  writing_usb,
  verifying_usb,
  completed,
};

struct RescueMediaCreationProgress final {
  RescueMediaCreationStage stage{
      RescueMediaCreationStage::validating_request};
  std::uint8_t percent{};
  std::wstring message;
  bool cancellation_allowed{};
};

struct RescueMediaCreationCallbacks final {
  std::function<void(const RescueMediaCreationProgress&)> progress;
  std::function<bool()> cancellation_requested;
};

struct ProductMediaPayloadPaths final {
  std::wstring builder_script;
  std::wstring environment_diagnostic;
  std::wstring winpe_cli;
  std::wstring winpe_gui;
  std::wstring powershell;
  std::wstring package_root;
};

struct RescueMediaCreationRequest final {
  RescueMediaKind kind{RescueMediaKind::iso_file};
  RescueMediaBootProfile boot_profile{
      RescueMediaBootProfile::windows_uefi_2011_ca};
  std::wstring final_iso_path;
  bool administrator{};
  std::optional<RescueUsbTargetAuthorization> usb_authorization;
  std::optional<RescueUsbDriveLetterResolution> usb_mapping;
  RescueMediaCreationCallbacks callbacks;
};

struct RescueMediaIsoExecutionRequest final {
  RescueMediaBootProfile boot_profile{
      RescueMediaBootProfile::windows_uefi_2011_ca};
  std::wstring final_iso_path;
  std::wstring work_root;
  ProductMediaPayloadPaths payload;
  RescueMediaCreationCallbacks callbacks;
};

struct RescueMediaUsbExecutionRequest final {
  RescueMediaBootProfile boot_profile{
      RescueMediaBootProfile::windows_uefi_2011_ca};
  std::wstring work_root;
  ProductMediaPayloadPaths payload;
  RescueUsbTargetAuthorization authorization;
  RescueUsbDriveLetterResolution mapping;
  RescueMediaCreationCallbacks callbacks;
};

struct RescueMediaCreationReport final {
  std::wstring final_iso_path;
  std::wstring manifest_path;
  std::wstring retained_work_root;
  std::uint64_t iso_length{};
  std::string iso_sha256;
  bool complete_iso_verified{};
  bool published_without_overwrite{};
  std::wstring usb_root_path;
  std::string usb_boot_wim_sha256;
  bool usb_layout_verified{};
  bool usb_boot_staged_without_overwrite{};
  bool usb_data_preserved{};
  bool complete_usb_verified{};
};

class IRescueMediaIsoExecutor {
 public:
  virtual ~IRescueMediaIsoExecutor() = default;

  [[nodiscard]] virtual clonecore::Result<RescueMediaCreationReport>
  execute(const RescueMediaIsoExecutionRequest& request) = 0;
};

class IRescueMediaUsbExecutor {
 public:
  virtual ~IRescueMediaUsbExecutor() = default;

  [[nodiscard]] virtual clonecore::Result<RescueMediaCreationReport>
  execute(const RescueMediaUsbExecutionRequest& request) = 0;
};

// The product EXE pins the audited MediaBuilder script identity at build time.
// A process-scoped PowerShell execution-policy override is permitted only after
// both the exact byte length and SHA-256 match this embedded identity.
[[nodiscard]] bool matches_embedded_media_builder_identity(
    std::uint64_t length,
    std::string_view sha256) noexcept;

// Produces a bounded, display-safe diagnostic from captured PowerShell output.
// Standard error is preferred so a failed media build does not collapse into
// an unactionable numeric exit code.
[[nodiscard]] std::wstring format_media_builder_failure_message(
    std::string_view standard_output,
    std::string_view standard_error) noexcept;

// Reads the single bounded drive-letter marker emitted only after the audited
// MediaBuilder has completed and verified a USB. This allows Windows to choose
// a different unused letter if the originally proposed access path becomes
// occupied while the old USB volume is being removed.
[[nodiscard]] clonecore::Result<wchar_t>
parse_media_builder_usb_drive_marker(std::string_view standard_output);

using MediaEnvironmentInspector =
    std::function<MediaPreflightView()>;
using ProductMediaPayloadResolver = std::function<
    clonecore::Result<ProductMediaPayloadPaths>()>;
using MediaWorkRootFactory = std::function<
    clonecore::Result<std::wstring>(const std::wstring&)>;
using IsoDestinationVerifier =
    std::function<clonecore::Status(const std::wstring&)>;
using UsbDestinationVerifier = std::function<clonecore::Result<
    RescueUsbDriveLetterResolution>(
    const RescueUsbStoragePlan&,
    RescueUsbDestinationVerificationPoint,
    wchar_t)>;
using UsbMediaWorkRootFactory =
    std::function<clonecore::Result<std::wstring>()>;

struct RescueMediaCreationDependencies final {
  MediaEnvironmentInspector inspect_environment;
  ProductMediaPayloadResolver resolve_payload;
  MediaWorkRootFactory create_work_root_name;
  UsbMediaWorkRootFactory create_usb_work_root_name;
  IsoDestinationVerifier verify_new_iso_destination;
  UsbDestinationVerifier verify_usb_destination;
  IRescueMediaIsoExecutor* iso_executor{};
  IRescueMediaUsbExecutor* usb_executor{};
};

// Coordinates ISO and target-bound USB creation. USB must carry a fresh,
// two-step authorization and either an existing unique drive-letter mapping
// or an unused letter proposed read-only for an unpartitioned target. The
// selected stable identity is rechecked immediately before the local ADK
// writer starts and again against the actual post-initialization drive letter.
// This function never requests UAC.
[[nodiscard]] clonecore::Result<RescueMediaCreationReport>
execute_rescue_media_creation(
    const RescueMediaCreationRequest& request,
    const RescueMediaCreationDependencies& dependencies);

// Resolves only self-authored payload files from the portable package:
//   tools\New-WinPEAppValidationMedia.ps1
//   tools\ytec-winpe-environment.exe
//   winpe\ytec-winpe-app.exe
//   winpe\ytec-winpe-gui.exe
// Microsoft payloads are never resolved from or copied into the package.
[[nodiscard]] clonecore::Result<ProductMediaPayloadPaths>
resolve_product_media_payload_paths_with_windows_apis();

[[nodiscard]] clonecore::Status
verify_new_iso_destination_with_windows_apis(
    const std::wstring& final_iso_path);

[[nodiscard]] clonecore::Result<std::wstring>
make_media_work_root_name_with_windows_apis(
    const std::wstring& final_iso_path);

[[nodiscard]] clonecore::Result<std::wstring>
make_usb_media_work_root_name_with_windows_apis();

// Re-enumerates disks and volume extents with read-only handles. It requires
// one exact stable identity and disk number, then resolves one existing volume
// or reserves one unused drive letter for a zero-partition target. It never
// locks, dismounts, formats or opens a target for write access.
[[nodiscard]] clonecore::Result<RescueUsbDriveLetterResolution>
verify_usb_destination_with_windows_apis(
    const RescueUsbStoragePlan& reviewed_plan,
    RescueUsbDestinationVerificationPoint verification_point,
    wchar_t expected_drive_letter);

[[nodiscard]] std::unique_ptr<IRescueMediaIsoExecutor>
make_windows_rescue_media_iso_executor();

[[nodiscard]] std::unique_ptr<IRescueMediaUsbExecutor>
make_windows_rescue_media_usb_executor();

[[nodiscard]] clonecore::Result<RescueMediaCreationReport>
execute_rescue_media_creation_with_windows_apis(
    const RescueMediaCreationRequest& request);

[[nodiscard]] std::wstring_view rescue_media_creation_stage_label(
    RescueMediaCreationStage stage) noexcept;

}  // namespace ytec::windowsapp

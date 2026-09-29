#pragma once

#include "ytec/windowsapp/adk_acquisition.h"
#include "ytec/windowsapp/windows_adk_eula_extractor.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct WindowsAdkDownloadPolicy final {
  std::vector<std::wstring> ordered_exact_urls;
};

// Performs the adapter-side network policy check without network access.
// The returned order is the only request/redirect sequence the WinHTTP
// implementation may follow.
[[nodiscard]] clonecore::Result<WindowsAdkDownloadPolicy>
validate_windows_adk_download_policy(
    const AdkDownloadRequest& request);

// Checks the path-only portion of an offline stage request. Handle based
// reparse, identity, link-count and size checks are performed by the adapter.
[[nodiscard]] clonecore::Status
validate_windows_adk_offline_stage_scope(
    const AdkOfflineStageRequest& request,
    const std::filesystem::path& owned_staging_root);

struct WindowsAdkInstalledObservation final {
  bool deployment_tools_present{};
  bool winpe_addon_present{};
  bool servicing_update_present{};
  bool microsoft_binaries_trusted{};
  std::wstring deployment_tools_version;
  std::wstring winpe_addon_version;
  std::wstring serviced_dism_version;
};

// Maps only observed facts to product state. The servicing identifier is
// asserted only when the required update was independently observed.
[[nodiscard]] AdkInstalledState evaluate_windows_adk_installed_observation(
    const AdkReleaseManifest& manifest,
    const WindowsAdkInstalledObservation& observation);

struct WindowsAdkPrelaunchObservation final {
  bool owned_staged_identity_matches{};
  bool payload_regular_non_reparse{};
  bool payload_single_link{};
  std::uint64_t payload_byte_count{};
  std::string payload_sha256;
  bool payload_signature_trusted{};
  std::wstring payload_signer_subject;
  std::wstring payload_version;
  std::wstring msp_revision_guid;
  bool system_handler_regular_non_reparse{};
  bool system_handler_signature_trusted{};
};

struct WindowsAdkLaunchPlan final {
  std::filesystem::path executable_path;
  std::vector<std::wstring> arguments;
  std::filesystem::path working_directory;
  bool uses_shell{};
};

// Revalidates the complete prelaunch observation and constructs a shell-free
// CreateProcess plan. The real adapter gathers the observation while holding
// the exact staged file open without write/delete sharing.
[[nodiscard]] clonecore::Result<WindowsAdkLaunchPlan>
validate_and_build_windows_adk_launch_plan(
    const AdkSilentInstallRequest& request,
    const std::filesystem::path& absolute_system_directory,
    const WindowsAdkPrelaunchObservation& observation);

class IWindowsAdkProcessLauncher {
 public:
  virtual ~IWindowsAdkProcessLauncher() = default;

  [[nodiscard]] virtual clonecore::Result<std::uint32_t> launch_and_wait(
      const WindowsAdkLaunchPlan& plan) = 0;
};

[[nodiscard]] std::unique_ptr<IWindowsAdkProcessLauncher>
make_windows_adk_process_launcher();

class WindowsAdkAcquisitionPlatform final
    : public IAdkAcquisitionPlatform {
 public:
  explicit WindowsAdkAcquisitionPlatform(
      std::unique_ptr<IWindowsAdkProcessLauncher> launcher = {});
  ~WindowsAdkAcquisitionPlatform() override;

  WindowsAdkAcquisitionPlatform(
      const WindowsAdkAcquisitionPlatform&) = delete;
  WindowsAdkAcquisitionPlatform& operator=(
      const WindowsAdkAcquisitionPlatform&) = delete;

  [[nodiscard]] clonecore::Result<AdkInstalledState>
  inspect_installed_state(const AdkReleaseManifest& manifest) override;

  [[nodiscard]] clonecore::Result<AdkStagingArea>
  create_new_staging_area(std::uint64_t maximum_total_bytes) override;

  [[nodiscard]] clonecore::Result<AdkStagedPayloadReceipt>
  download_to_new_file(const AdkDownloadRequest& request) override;

  [[nodiscard]] clonecore::Result<AdkStagedPayloadReceipt>
  stage_offline_payload(const AdkOfflineStageRequest& request) override;

  [[nodiscard]] clonecore::Result<std::string> sha256_file(
      const std::filesystem::path& path,
      std::uint64_t maximum_bytes) override;

  [[nodiscard]] clonecore::Status verify_authenticode(
      const std::filesystem::path& path,
      std::wstring_view expected_signer_subject) override;

  [[nodiscard]] clonecore::Result<std::wstring>
  query_payload_version(const std::filesystem::path& path) override;

  [[nodiscard]] clonecore::Result<std::vector<AdkVerifiedPayload>>
  expand_and_verify_patch_archive(
      const AdkPatchArchiveExpandRequest& request) override;

  // Reopens the exact CREATE_NEW bootstrap tracked by this adapter and keeps
  // its owned stage-root and file handles fixed while the bounded CAB/EULA
  // extractor performs before/after whole-file identity verification.
  [[nodiscard]] clonecore::Result<WindowsAdkEulaExtractionResult>
  extract_verified_embedded_eula(
      const AdkPinnedPayload& pinned_bootstrap,
      const AdkVerifiedPayload& verified_bootstrap,
      const AdkEmbeddedEulaPin& pin);

  [[nodiscard]] clonecore::Result<std::uint32_t>
  run_verified_silent_installer(
      const AdkSilentInstallRequest& request) override;

  [[nodiscard]] clonecore::Status remove_staging_area(
      const AdkStagingArea& staging) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::unique_ptr<IAdkAcquisitionPlatform>
make_windows_adk_acquisition_platform();

}  // namespace ytec::windowsapp

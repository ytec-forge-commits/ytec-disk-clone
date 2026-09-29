#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

enum class AdkPayloadKind : std::uint8_t {
  deployment_tools,
  winpe_addon,
  servicing_update,
};

enum class AdkInstallerKind : std::uint8_t {
  microsoft_bootstrap_exe,
  windows_update_msu,
  windows_installer_patch_msp,
  windows_installer_patch_archive_zip,
};

enum class AdkAcquisitionSource : std::uint8_t {
  official_download,
  official_offline_layout,
};

struct AdkPatchMemberPin final {
  // archive_member_name is an ASCII basename. The archive must contain one
  // safe root directory and exactly these members immediately below it.
  std::wstring archive_member_name;
  std::wstring staging_file_name;
  std::uint64_t expected_byte_count{};
  std::string expected_sha256;
  std::wstring expected_signer_subject;
  std::wstring expected_revision_guid;
};

struct AdkPinnedPayload final {
  AdkPayloadKind kind{AdkPayloadKind::deployment_tools};
  AdkInstallerKind installer_kind{
      AdkInstallerKind::microsoft_bootstrap_exe};
  std::wstring display_name;
  std::wstring staging_file_name;
  std::filesystem::path offline_relative_path;
  std::wstring exact_source_url;
  std::vector<std::wstring> allowed_redirect_urls;
  std::string expected_sha256;
  std::wstring expected_signer_subject;
  std::wstring expected_payload_version;
  std::vector<std::wstring> acquired_components;
  std::wstring uninstall_registration_id;
  std::uint64_t expected_byte_count{};
  std::uint64_t maximum_bytes{};
  std::vector<AdkPatchMemberPin> patch_members;
};

// ADK 10.1.26100.2454 carries its product-specific EULA as a UX member in the
// attached Burn CAB of the Microsoft-signed bootstrap. The whole bootstrap is
// verified first; only then may an owned, bounded extractor read this exact
// container range and member. The SHA-256 below, not Burn's internal SHA-1, is
// the product trust anchor for the EULA body.
struct AdkEmbeddedEulaPin final {
  AdkPayloadKind source_payload_kind{AdkPayloadKind::deployment_tools};
  std::wstring official_bootstrap_url;
  std::uint64_t container_offset{};
  std::uint64_t container_length{};
  std::wstring container_member_name;
  std::filesystem::path display_file_name;
  std::uint64_t expected_byte_count{};
  std::string expected_sha256;
  std::wstring expected_document_title;
  bool primary_source_confirmed{};

  friend bool operator==(
      const AdkEmbeddedEulaPin&,
      const AdkEmbeddedEulaPin&) = default;
};

struct AdkReleaseManifest final {
  std::string manifest_id;
  std::wstring product_release_version;
  std::wstring tested_adk_version;
  std::wstring information_url;
  AdkEmbeddedEulaPin embedded_eula;

  // Quiet setup is never allowed merely because /norestart was requested.
  // This release gate is set only after current Microsoft primary material or
  // controlled clean-VM evidence proves that no unexpected restart can occur.
  bool unattended_install_no_unexpected_restart_confirmed{};
  std::wstring expected_deployment_tools_version;
  std::wstring expected_winpe_addon_version;
  std::wstring expected_serviced_dism_version;
  std::wstring required_servicing_update_id;
  std::vector<AdkPinnedPayload> payloads;

  // A release owner sets this only after every URL, redirect, signer, version,
  // and SHA-256 was checked against Microsoft primary material and the exact
  // acquired files passed the release validation matrix. False is a permanent
  // execution gate, not a warning that a caller can bypass.
  bool primary_source_pins_confirmed{};
};

struct AdkAcquisitionConsent final {
  bool accepted{};
  std::string presented_manifest_id;
  AdkEmbeddedEulaPin presented_embedded_eula;
  std::vector<AdkPayloadKind> presented_payloads;
};

struct AdkAcquisitionRequest final {
  bool administrator{};
  AdkAcquisitionSource source{
      AdkAcquisitionSource::official_download};
  std::filesystem::path offline_layout_root;
  AdkAcquisitionConsent consent;
};

struct AdkInstalledState final {
  bool deployment_tools_present{};
  bool winpe_addon_present{};
  bool servicing_update_present{};
  bool microsoft_binaries_trusted{};
  std::wstring deployment_tools_version;
  std::wstring winpe_addon_version;
  std::wstring serviced_dism_version;
  std::wstring servicing_update_id;
};

struct AdkStagingArea final {
  std::filesystem::path root;
  bool created_new{};
  bool reparse_point{};
};

struct AdkDownloadRequest final {
  std::wstring exact_source_url;
  std::vector<std::wstring> exact_allowed_urls;
  std::filesystem::path create_new_destination;
  std::uint64_t maximum_bytes{};
};

struct AdkOfflineStageRequest final {
  std::filesystem::path layout_root;
  std::filesystem::path exact_relative_path;
  std::filesystem::path create_new_destination;
  std::uint64_t maximum_bytes{};
};

struct AdkStagedPayloadReceipt final {
  std::filesystem::path staged_path;
  std::filesystem::path offline_source_path;
  std::uint64_t byte_count{};
  bool created_new{};
  bool source_regular_file{};
  bool source_reparse_point{};

  // For a download, visited_urls contains the requested URL first and the
  // effective URL last. Every redirect must have been pinned in the manifest.
  // For an offline layout both values remain empty.
  std::vector<std::wstring> visited_urls;
  std::wstring effective_url;
};

struct AdkVerifiedPayload final {
  AdkPayloadKind kind{AdkPayloadKind::deployment_tools};
  AdkInstallerKind installer_kind{
      AdkInstallerKind::microsoft_bootstrap_exe};
  std::filesystem::path staged_path;
  std::uint64_t byte_count{};
  std::string sha256;
  std::wstring signer_subject;
  std::wstring payload_version;
  std::wstring msp_revision_guid;
};

struct AdkPatchArchiveExpandRequest final {
  AdkVerifiedPayload archive;
  std::vector<AdkPatchMemberPin> members;
};

struct AdkSilentInstallRequest final {
  AdkVerifiedPayload payload;
  std::vector<std::wstring> fixed_arguments;
};

class IAdkAcquisitionPlatform {
 public:
  virtual ~IAdkAcquisitionPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<AdkInstalledState>
  inspect_installed_state(const AdkReleaseManifest& manifest) = 0;

  [[nodiscard]] virtual clonecore::Result<AdkStagingArea>
  create_new_staging_area(std::uint64_t maximum_total_bytes) = 0;

  // The implementation must disable credential forwarding and ambient proxy
  // credentials, reject TLS downgrade, enforce exact_allowed_urls before each
  // request/redirect, use CREATE_NEW, and stop at maximum_bytes.
  [[nodiscard]] virtual clonecore::Result<AdkStagedPayloadReceipt>
  download_to_new_file(const AdkDownloadRequest& request) = 0;

  // The implementation must reject UNC roots, reparse points, hard-link or
  // identity changes, non-regular sources, overwrite, and over-limit files.
  [[nodiscard]] virtual clonecore::Result<AdkStagedPayloadReceipt>
  stage_offline_payload(const AdkOfflineStageRequest& request) = 0;

  [[nodiscard]] virtual clonecore::Result<std::string> sha256_file(
      const std::filesystem::path& path,
      std::uint64_t maximum_bytes) = 0;

  [[nodiscard]] virtual clonecore::Status verify_authenticode(
      const std::filesystem::path& path,
      std::wstring_view expected_signer_subject) = 0;

  [[nodiscard]] virtual clonecore::Result<std::wstring>
  query_payload_version(const std::filesystem::path& path) = 0;

  // Reopens the exact owned ZIP without write/delete sharing, validates its
  // complete bounded ZIP structure, extracts only the pinned MSP members with
  // CREATE_NEW, and verifies every member's length, SHA-256, exact Microsoft
  // Windows signer and MSI Revision GUID before returning any installable
  // payload.
  [[nodiscard]] virtual clonecore::Result<std::vector<AdkVerifiedPayload>>
  expand_and_verify_patch_archive(
      const AdkPatchArchiveExpandRequest& request) = 0;

  // The Windows adapter must reopen the exact non-reparse staged file and
  // atomically recheck the supplied length/hash/signature/version immediately
  // before CreateProcess. It must call only the verified payload itself or the
  // absolute System32 handler selected by installer_kind; never a shell.
  [[nodiscard]] virtual clonecore::Result<std::uint32_t>
  run_verified_silent_installer(
      const AdkSilentInstallRequest& request) = 0;

  [[nodiscard]] virtual clonecore::Status remove_staging_area(
      const AdkStagingArea& staging) = 0;
};

struct AdkAcquisitionReport final {
  std::string manifest_id;
  bool used_existing_installation{};
  bool installed_by_this_operation{};
  bool preexisting_adk_preserved{};
  bool offline_layout_used{};
  bool reboot_required{};
  bool post_install_preflight_verified{};
  bool staging_removed{};
  std::vector<AdkVerifiedPayload> verified_archives;
  std::vector<AdkVerifiedPayload> verified_payloads;
  std::vector<std::uint32_t> installer_exit_codes;
  std::vector<std::wstring> managed_installation_registration_ids;
};

// Returns the 1.0.0 release manifest shell. The Microsoft installer/update
// URLs, byte counts, versions, signatures, SHA-256 values and the embedded ADK
// EULA member are pinned. The bounded production extractor/presentation UI,
// no-unexpected-restart proof and clean-VM gates are incomplete, so the
// manifest deliberately keeps primary_source_pins_confirmed=false and cannot
// execute.
[[nodiscard]] AdkReleaseManifest tsumugi_1_0_0_adk_manifest();

// Validates every bounded field, component order, URL host/path, redirect,
// SHA-256 and source-layout rule without performing I/O.
// This structural form deliberately does not open either release execution
// gate.  It exists so the product can render the complete pinned manifest and
// explain the exact blocked gate without treating a pending release as valid
// for communication or installer launch.
[[nodiscard]] clonecore::Status validate_adk_release_manifest_structure(
    const AdkReleaseManifest& manifest);

// Validates the structural manifest and both release execution gates.  Only
// this form may authorize acquisition, installer launch, offline publication,
// or managed removal.
[[nodiscard]] clonecore::Status validate_adk_release_manifest(
    const AdkReleaseManifest& manifest);

[[nodiscard]] bool installed_state_matches_manifest(
    const AdkInstalledState& state,
    const AdkReleaseManifest& manifest) noexcept;

[[nodiscard]] std::vector<std::wstring> fixed_silent_install_arguments(
    AdkPayloadKind payload_kind,
    AdkInstallerKind installer_kind);

// Windows Installer returns 1642 when a signed MSP targets an ADK feature that
// is not installed. The fixed KB archive contains patches for more features
// than Tsumugi installs, so that code is non-fatal only for an MSP. Required
// Deployment Tools/WinPE servicing is still proved by the final installed-state
// inspection before the acquisition can succeed.
[[nodiscard]] bool adk_installer_exit_code_permitted(
    AdkPayloadKind payload_kind,
    AdkInstallerKind installer_kind,
    std::uint32_t exit_code) noexcept;

// Order is fixed: validate manifest -> validate explicit consent -> inspect
// existing state -> create staging -> acquire every payload -> SHA-256 ->
// Authenticode -> version -> launch all installers in manifest order -> final
// installed-state preflight -> remove staging. All payloads are completely
// verified before the first installer is launched.
[[nodiscard]] clonecore::Result<AdkAcquisitionReport>
execute_adk_acquisition(
    const AdkReleaseManifest& manifest,
    const AdkAcquisitionRequest& request,
    IAdkAcquisitionPlatform& platform);

struct AdkManagedInstallationRecord final {
  std::string manifest_id;
  bool installed_by_tsumugi{};
  std::vector<std::wstring> installed_registration_ids;
};

struct AdkUninstallStep final {
  AdkPayloadKind kind{AdkPayloadKind::deployment_tools};
  std::wstring display_name;
  std::wstring registration_id;
};

struct AdkUninstallPlan final {
  std::string manifest_id;
  bool requires_explicit_confirmation{true};
  bool preserves_unmanaged_adk{true};
  std::vector<AdkUninstallStep> steps;
};

// Produces a plan only for an exact record written after this application
// installed the pinned manifest. It never searches for or removes an existing
// unmanaged ADK and does not execute an uninstaller.
[[nodiscard]] clonecore::Result<AdkUninstallPlan>
build_managed_adk_uninstall_plan(
    const AdkReleaseManifest& manifest,
    const AdkManagedInstallationRecord& record);

[[nodiscard]] std::wstring_view adk_payload_kind_label(
    AdkPayloadKind kind) noexcept;

}  // namespace ytec::windowsapp

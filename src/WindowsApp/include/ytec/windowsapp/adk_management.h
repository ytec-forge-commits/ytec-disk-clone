#pragma once

#include "ytec/windowsapp/adk_acquisition.h"
#include "ytec/windowsapp/adk_consent_review.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

enum class AdkManagementAction : std::uint8_t {
  official_download_install,
  offline_layout_install,
  create_offline_layout,
  uninstall_managed,
};

struct AdkManagementUiContract final {
  std::wstring_view title;
  std::wstring_view official_download_label;
  std::wstring_view offline_install_label;
  std::wstring_view create_layout_label;
  std::wstring_view uninstall_label;
  bool explicit_start_required{};
  bool background_execution{};
  bool enter_activates_focused_command{};
  bool escape_cancels_review{};
  bool previous_focus_restored{};
  bool official_page_never_opens_automatically{};
};

[[nodiscard]] constexpr AdkManagementUiContract
adk_management_ui_contract() noexcept {
  return AdkManagementUiContract{
      .title = L"ADK取得・管理",
      .official_download_label = L"固定Microsoft公式URLから取得・導入",
      .offline_install_label = L"検証済みオフラインレイアウトから導入",
      .create_layout_label = L"公式オフラインレイアウトを新規作成",
      .uninstall_label = L"Tsumugiが導入したADKだけを削除",
      .explicit_start_required = true,
      .background_execution = true,
      .enter_activates_focused_command = true,
      .escape_cancels_review = true,
      .previous_focus_restored = true,
      .official_page_never_opens_automatically = true,
  };
}

struct AdkManagementView final {
  bool manifest_structure_valid{};
  bool primary_source_pins_confirmed{};
  bool unattended_no_restart_confirmed{};
  bool execution_gate_open{};
  bool platform_creation_permitted{};
  bool path_selection_permitted{};
  std::wstring title;
  std::wstring status;
  std::wstring summary;
  std::vector<std::wstring> payload_rows;
};

// This is a pure view builder. The closed release gate is deliberately
// displayed as a product state; it never probes a path, creates an adapter,
// starts a download, requests elevation, or launches an installer.
[[nodiscard]] AdkManagementView build_adk_management_view(
    const AdkReleaseManifest& manifest);

enum class AdkManagementPathSelection : std::uint8_t {
  none,
  existing_offline_layout,
  new_offline_layout_parent,
};

struct AdkManagementActionReview final {
  AdkManagementAction action{
      AdkManagementAction::official_download_install};
  AdkManagementPathSelection path_selection{
      AdkManagementPathSelection::none};
  bool manifest_structure_valid{};
  bool execution_gate_open{};
  bool requires_eula_review{};
  bool requires_network_retrieval{};
  bool requires_explicit_uninstall_confirmation{};
  bool path_picker_permitted{};
  std::wstring title;
  std::wstring summary;
};

// Pure per-command product contract. A closed execution gate always keeps the
// path picker disabled, including for requests containing a hostile path.
[[nodiscard]] AdkManagementActionReview build_adk_management_action_review(
    const AdkReleaseManifest& manifest,
    AdkManagementAction action);

// Converts a user-selected existing local parent into a fixed new child for
// official /layout export. No directory is read or created by this helper.
[[nodiscard]] clonecore::Result<std::filesystem::path>
make_adk_offline_layout_destination(
    const AdkReleaseManifest& manifest,
    const std::filesystem::path& selected_parent);

enum class AdkEvidenceStage : std::uint8_t {
  review_opened,
  gate_blocked,
  path_selected,
  eula_retrieval_started,
  eula_verified,
  consent_accepted,
  action_started,
  action_succeeded,
  action_failed,
};

struct AdkEvidenceFacts final {
  AdkManagementAction action{
      AdkManagementAction::official_download_install};
  AdkEvidenceStage stage{AdkEvidenceStage::review_opened};
  bool path_selected{};
  bool complete_eula_presented{};
  bool explicit_consent{};
  std::uint32_t native_code{};
};

// Produces a bounded log line containing manifest and observed booleans only.
// It intentionally has no path, URL, command-line, EULA-body, or credential
// field, so product evidence cannot accidentally disclose those values.
[[nodiscard]] clonecore::Result<std::wstring> format_adk_evidence_event(
    const AdkReleaseManifest& manifest,
    const AdkEvidenceFacts& facts);

struct AdkManagementLayout final {
  int client_width{};
  int client_height{};
  int summary_left{};
  int summary_top{};
  int summary_width{};
  int summary_height{};
  int command_left{};
  int command_top{};
  int command_width{};
  int command_height{};
  int command_gap{};
  int cancel_left{};
  int cancel_top{};
  int cancel_width{};
  int cancel_height{};
  bool bounded{};
};

// The smallest supported logical work area is 960x516, corresponding to a
// 1024x600 display at the product's 200 percent compact layout boundary.
[[nodiscard]] AdkManagementLayout calculate_adk_management_layout(
    int available_width,
    int available_height) noexcept;

inline constexpr std::size_t kMaximumManagedAdkRecordBytes = 8U * 1024U;
inline constexpr std::wstring_view kManagedAdkRecordFileName =
    L"adk-managed-installation.v1";
inline constexpr std::wstring_view kAdkOfflineLayoutManifestFileName =
    L"tsumugi-adk-offline-layout.v1";

// The record contains identifiers only. It never stores a URL, path,
// credential, command line, EULA body, or installer payload.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
serialize_managed_adk_record(
    const AdkManagedInstallationRecord& record);

[[nodiscard]] clonecore::Result<AdkManagedInstallationRecord>
parse_managed_adk_record(std::span<const std::byte> bytes);

struct AdkOfflineLayoutPublishedFile final {
  AdkPayloadKind kind{AdkPayloadKind::deployment_tools};
  std::wstring file_name;
  std::uint64_t byte_count{};
  std::string sha256;
};

struct AdkOfflineLayoutReport final {
  std::string manifest_id;
  std::filesystem::path layout_root;
  std::uint64_t total_bytes{};
  bool complete_manifest_written{};
  std::vector<AdkOfflineLayoutPublishedFile> files;
};

class IAdkManagementPlatform {
 public:
  virtual ~IAdkManagementPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<
      std::optional<AdkManagedInstallationRecord>>
  load_managed_installation_record() = 0;

  [[nodiscard]] virtual clonecore::Status
  save_managed_installation_record_create_new(
      const AdkManagedInstallationRecord& record) = 0;

  [[nodiscard]] virtual clonecore::Status
  remove_managed_installation_record_if_exact(
      const AdkManagedInstallationRecord& record) = 0;

  // The Windows adapter creates a previously absent local directory and
  // holds its non-reparse identity until finalize/abandon.
  [[nodiscard]] virtual clonecore::Status begin_new_offline_layout(
      const std::filesystem::path& layout_root,
      std::string_view manifest_id) = 0;

  // The adapter independently reopens the verified staging source without
  // write/delete sharing, rechecks length and SHA-256, then copies to a
  // CREATE_NEW direct child and verifies the destination readback.
  [[nodiscard]] virtual clonecore::Status publish_offline_layout_payload(
      const AdkPinnedPayload& payload,
      const AdkVerifiedPayload& verified_payload) = 0;

  [[nodiscard]] virtual clonecore::Result<AdkOfflineLayoutReport>
  finalize_offline_layout(const AdkReleaseManifest& manifest) = 0;

  [[nodiscard]] virtual clonecore::Status abandon_offline_layout() = 0;

  // Only exact MSI/MSP registration identities from the Tsumugi-owned record
  // are accepted. No registry search, display-name matching, shell, or
  // arbitrary uninstall string is allowed.
  [[nodiscard]] virtual clonecore::Result<std::vector<std::uint32_t>>
  execute_managed_uninstall(const AdkUninstallPlan& plan) = 0;
};

using AdkAcquisitionPlatformFactory =
    std::function<std::unique_ptr<IAdkAcquisitionPlatform>()>;
using AdkManagementPlatformFactory =
    std::function<std::unique_ptr<IAdkManagementPlatform>()>;

struct AdkManagementDependencies final {
  AdkAcquisitionPlatformFactory make_acquisition_platform;
  AdkManagementPlatformFactory make_management_platform;
};

struct AdkManagementRequest final {
  AdkManagementAction action{
      AdkManagementAction::official_download_install};
  bool administrator{};
  bool explicit_start_confirmed{};
  std::filesystem::path offline_layout_root;
  std::optional<AdkEulaDocumentReceipt> eula_receipt;
  std::optional<AdkConsentReviewAcknowledgement> consent_acknowledgement;
};

struct AdkManagementReport final {
  AdkManagementAction action{
      AdkManagementAction::official_download_install};
  std::string manifest_id;
  bool used_existing_installation{};
  bool installed_by_this_operation{};
  bool managed_record_persisted{};
  bool managed_record_removed{};
  bool offline_layout_completed{};
  bool preexisting_adk_preserved{};
  std::uint64_t offline_layout_bytes{};
  std::size_t offline_layout_file_count{};
  std::vector<std::uint32_t> uninstall_exit_codes;
};

// Release-gate validation is the first operation. When the production
// manifest retains primary_source_pins_confirmed=false (or the no-restart
// proof is absent), this returns before validating user paths or invoking
// either factory. That keeps communication, UAC, installers, file dialogs and
// directory reads at zero under the closed gate.
[[nodiscard]] clonecore::Result<AdkManagementReport>
execute_adk_management_action(
    const AdkReleaseManifest& manifest,
    const AdkManagementRequest& request,
    const AdkManagementDependencies& dependencies);

}  // namespace ytec::windowsapp

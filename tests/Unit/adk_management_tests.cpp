#include "ytec/windowsapp/adk_management.h"
#include "ytec/windowsapp/windows_adk_management_platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace ytec::windowsapp;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

std::string repeated_hash(const char character) {
  return std::string(64U, character);
}

AdkReleaseManifest make_manifest() {
  return AdkReleaseManifest{
      .manifest_id = "tsumugi-drive-1.0.0-adk-management-test",
      .product_release_version = L"1.0.0",
      .tested_adk_version = L"10.1.26100.2454",
      .information_url =
          L"https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install",
      .embedded_eula = AdkEmbeddedEulaPin{
          .source_payload_kind = AdkPayloadKind::deployment_tools,
          .official_bootstrap_url =
              L"https://download.microsoft.com/download/a/adksetup.exe",
          .container_offset = 10U,
          .container_length = 80U,
          .container_member_name = L"u6",
          .display_file_name = L"ja\\eula.rtf",
          .expected_byte_count = 50U,
          .expected_sha256 = repeated_hash('D'),
          .expected_document_title =
              L"WINDOWS ASSESSMENT AND DEPLOYMENT KIT (ADK)",
          .primary_source_confirmed = true,
      },
      .unattended_install_no_unexpected_restart_confirmed = true,
      .expected_deployment_tools_version = L"10.1.26100.2454",
      .expected_winpe_addon_version = L"10.1.26100.2454",
      .expected_serviced_dism_version = L"10.0.26100.8972",
      .required_servicing_update_id = L"KB5101684",
      .payloads = {
          AdkPinnedPayload{
              .kind = AdkPayloadKind::deployment_tools,
              .installer_kind =
                  AdkInstallerKind::microsoft_bootstrap_exe,
              .display_name = L"Windows ADK Deployment Tools",
              .staging_file_name = L"adksetup.exe",
              .offline_relative_path = L"adksetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/a/adksetup.exe",
              .expected_sha256 = repeated_hash('A'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.1.26100.2454",
              .acquired_components = {L"Deployment Tools"},
              .uninstall_registration_id =
                  L"MSI|{11111111-1111-1111-1111-111111111111}",
              .expected_byte_count = 100U,
              .maximum_bytes = 1'024U,
          },
          AdkPinnedPayload{
              .kind = AdkPayloadKind::winpe_addon,
              .installer_kind =
                  AdkInstallerKind::microsoft_bootstrap_exe,
              .display_name = L"Windows PE Add-on",
              .staging_file_name = L"adkwinpesetup.exe",
              .offline_relative_path = L"adkwinpesetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/b/adkwinpesetup.exe",
              .expected_sha256 = repeated_hash('B'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.1.26100.2454",
              .acquired_components = {
                  L"Windows Preinstallation Environment"},
              .uninstall_registration_id =
                  L"MSI|{22222222-2222-2222-2222-222222222222}",
              .expected_byte_count = 101U,
              .maximum_bytes = 2'048U,
          },
          AdkPinnedPayload{
              .kind = AdkPayloadKind::servicing_update,
              .installer_kind = AdkInstallerKind::windows_update_msu,
              .display_name = L"Windows ADK Servicing Update",
              .staging_file_name = L"adk-update.msu",
              .offline_relative_path = L"adk-update.msu",
              .exact_source_url =
                  L"https://download.microsoft.com/download/c/adk-update.msu",
              .expected_sha256 = repeated_hash('C'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.0.26100.8972",
              .acquired_components = {L"DISM", L"Oscdimg"},
              .uninstall_registration_id =
                  L"MSP|{33333333-3333-3333-3333-333333333333}|"
                  L"{11111111-1111-1111-1111-111111111111}",
              .expected_byte_count = 102U,
              .maximum_bytes = 4'096U,
          },
      },
      .primary_source_pins_confirmed = true,
  };
}

AdkInstalledState matching_state(const AdkReleaseManifest& manifest) {
  return AdkInstalledState{
      .deployment_tools_present = true,
      .winpe_addon_present = true,
      .servicing_update_present = true,
      .microsoft_binaries_trusted = true,
      .deployment_tools_version = manifest.expected_deployment_tools_version,
      .winpe_addon_version = manifest.expected_winpe_addon_version,
      .serviced_dism_version = manifest.expected_serviced_dism_version,
      .servicing_update_id = manifest.required_servicing_update_id,
  };
}

AdkEulaDocumentReceipt verified_eula_receipt(
    const AdkReleaseManifest& manifest) {
  return AdkEulaDocumentReceipt{
      .extracted_identity = manifest.embedded_eula,
      .bootstrap_full_identity_verified = true,
      .attached_container_bounds_verified = true,
      .burn_ux_mapping_verified = true,
      .member_copy_created_new = true,
      .member_copy_regular_file = true,
      .member_copy_single_link = true,
      .member_copy_reparse_point = false,
      .bounded_read_complete = true,
      .temporary_files_removed = true,
  };
}

AdkConsentReviewAcknowledgement accepted_review(
    const AdkReleaseManifest& manifest) {
  return AdkConsentReviewAcknowledgement{
      .reviewed_manifest_id = manifest.manifest_id,
      .reviewed_embedded_eula = manifest.embedded_eula,
      .official_sources_presented = true,
      .acquired_components_presented = true,
      .eula_body_opened = true,
      .eula_body_fully_presented = true,
      .explicit_acceptance = true,
  };
}

struct AcquisitionState final {
  const AdkReleaseManifest* manifest{};
  std::size_t inspect_count{};
  std::size_t download_count{};
  std::size_t offline_count{};
  std::size_t installer_count{};
  std::size_t cleanup_count{};
};

std::size_t payload_index(
    const AdkReleaseManifest& manifest,
    const std::filesystem::path& path) {
  for (std::size_t index = 0U; index < manifest.payloads.size(); ++index) {
    if (path.filename() == manifest.payloads[index].staging_file_name) {
      return index;
    }
  }
  throw TestFailure{"unknown synthetic staging path"};
}

class FakeAcquisitionPlatform final : public IAdkAcquisitionPlatform {
 public:
  explicit FakeAcquisitionPlatform(
      std::shared_ptr<AcquisitionState> state)
      : state_(std::move(state)) {}

  ytec::clonecore::Result<AdkInstalledState> inspect_installed_state(
      const AdkReleaseManifest& manifest) override {
    ++state_->inspect_count;
    return ytec::clonecore::Result<AdkInstalledState>::success(
        state_->inspect_count == 1U ? AdkInstalledState{}
                                    : matching_state(manifest));
  }

  ytec::clonecore::Result<AdkStagingArea> create_new_staging_area(
      std::uint64_t) override {
    return ytec::clonecore::Result<AdkStagingArea>::success(
        AdkStagingArea{
            .root = L"C:\\Synthetic\\adk-stage",
            .created_new = true,
            .reparse_point = false,
        });
  }

  ytec::clonecore::Result<AdkStagedPayloadReceipt> download_to_new_file(
      const AdkDownloadRequest& request) override {
    ++state_->download_count;
    const auto index = payload_index(
        *state_->manifest, request.create_new_destination);
    return ytec::clonecore::Result<AdkStagedPayloadReceipt>::success(
        AdkStagedPayloadReceipt{
            .staged_path = request.create_new_destination,
            .byte_count = state_->manifest->payloads[index]
                              .expected_byte_count,
            .created_new = true,
            .source_regular_file = true,
            .source_reparse_point = false,
            .visited_urls = {request.exact_source_url},
            .effective_url = request.exact_source_url,
        });
  }

  ytec::clonecore::Result<AdkStagedPayloadReceipt> stage_offline_payload(
      const AdkOfflineStageRequest& request) override {
    ++state_->offline_count;
    const auto index = payload_index(
        *state_->manifest, request.create_new_destination);
    return ytec::clonecore::Result<AdkStagedPayloadReceipt>::success(
        AdkStagedPayloadReceipt{
            .staged_path = request.create_new_destination,
            .offline_source_path =
                request.layout_root / request.exact_relative_path,
            .byte_count = state_->manifest->payloads[index]
                              .expected_byte_count,
            .created_new = true,
            .source_regular_file = true,
            .source_reparse_point = false,
        });
  }

  ytec::clonecore::Result<std::string> sha256_file(
      const std::filesystem::path& path,
      std::uint64_t) override {
    return ytec::clonecore::Result<std::string>::success(
        state_->manifest->payloads[payload_index(*state_->manifest, path)]
            .expected_sha256);
  }

  ytec::clonecore::Status verify_authenticode(
      const std::filesystem::path&,
      std::wstring_view) override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::wstring> query_payload_version(
      const std::filesystem::path& path) override {
    return ytec::clonecore::Result<std::wstring>::success(
        state_->manifest->payloads[payload_index(*state_->manifest, path)]
            .expected_payload_version);
  }

  ytec::clonecore::Result<std::vector<AdkVerifiedPayload>>
  expand_and_verify_patch_archive(
      const AdkPatchArchiveExpandRequest&) override {
    return ytec::clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::invalid_argument,
            .native_code = ERROR_INVALID_FUNCTION,
            .operation = L"unexpected synthetic archive",
            .message = L"unexpected synthetic archive",
        });
  }

  ytec::clonecore::Result<std::uint32_t> run_verified_silent_installer(
      const AdkSilentInstallRequest&) override {
    ++state_->installer_count;
    return ytec::clonecore::Result<std::uint32_t>::success(ERROR_SUCCESS);
  }

  ytec::clonecore::Status remove_staging_area(
      const AdkStagingArea&) override {
    ++state_->cleanup_count;
    return ytec::clonecore::success_status();
  }

 private:
  std::shared_ptr<AcquisitionState> state_;
};

struct ManagementState final {
  std::optional<AdkManagedInstallationRecord> record;
  std::size_t load_count{};
  std::size_t save_count{};
  std::size_t remove_count{};
  std::size_t begin_layout_count{};
  std::size_t publish_count{};
  std::size_t finalize_count{};
  std::size_t abandon_count{};
  std::size_t uninstall_count{};
  std::filesystem::path layout_root;
  std::vector<AdkOfflineLayoutPublishedFile> files;
};

class FakeManagementPlatform final : public IAdkManagementPlatform {
 public:
  explicit FakeManagementPlatform(std::shared_ptr<ManagementState> state)
      : state_(std::move(state)) {}

  ytec::clonecore::Result<std::optional<AdkManagedInstallationRecord>>
  load_managed_installation_record() override {
    ++state_->load_count;
    return ytec::clonecore::Result<
        std::optional<AdkManagedInstallationRecord>>::success(state_->record);
  }

  ytec::clonecore::Status save_managed_installation_record_create_new(
      const AdkManagedInstallationRecord& record) override {
    ++state_->save_count;
    if (state_->record.has_value()) {
      return ytec::clonecore::Status::failure(ytec::clonecore::Error{
          .code = ytec::clonecore::ErrorCode::access_denied,
          .native_code = ERROR_FILE_EXISTS,
          .operation = L"synthetic save",
          .message = L"synthetic record already exists",
      });
    }
    state_->record = record;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status remove_managed_installation_record_if_exact(
      const AdkManagedInstallationRecord& record) override {
    ++state_->remove_count;
    if (!state_->record.has_value() ||
        state_->record->manifest_id != record.manifest_id ||
        state_->record->installed_registration_ids !=
            record.installed_registration_ids) {
      return ytec::clonecore::Status::failure(ytec::clonecore::Error{
          .code = ytec::clonecore::ErrorCode::identity_mismatch,
          .native_code = ERROR_INVALID_DATA,
          .operation = L"synthetic remove",
          .message = L"synthetic record mismatch",
      });
    }
    state_->record.reset();
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status begin_new_offline_layout(
      const std::filesystem::path& layout_root,
      std::string_view) override {
    ++state_->begin_layout_count;
    state_->layout_root = layout_root;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status publish_offline_layout_payload(
      const AdkPinnedPayload& payload,
      const AdkVerifiedPayload& verified) override {
    ++state_->publish_count;
    state_->files.push_back(AdkOfflineLayoutPublishedFile{
        .kind = payload.kind,
        .file_name = payload.offline_relative_path.filename().native(),
        .byte_count = verified.byte_count,
        .sha256 = verified.sha256,
    });
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<AdkOfflineLayoutReport> finalize_offline_layout(
      const AdkReleaseManifest& manifest) override {
    ++state_->finalize_count;
    std::uint64_t total{};
    for (const auto& file : state_->files) {
      total += file.byte_count;
    }
    return ytec::clonecore::Result<AdkOfflineLayoutReport>::success(
        AdkOfflineLayoutReport{
            .manifest_id = manifest.manifest_id,
            .layout_root = state_->layout_root,
            .total_bytes = total,
            .complete_manifest_written = true,
            .files = state_->files,
        });
  }

  ytec::clonecore::Status abandon_offline_layout() override {
    ++state_->abandon_count;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::uint32_t>>
  execute_managed_uninstall(const AdkUninstallPlan& plan) override {
    ++state_->uninstall_count;
    return ytec::clonecore::Result<std::vector<std::uint32_t>>::success(
        std::vector<std::uint32_t>(plan.steps.size(), ERROR_SUCCESS));
  }

 private:
  std::shared_ptr<ManagementState> state_;
};

struct Factories final {
  std::shared_ptr<AcquisitionState> acquisition;
  std::shared_ptr<ManagementState> management;
  std::size_t acquisition_factory_count{};
  std::size_t management_factory_count{};

  AdkManagementDependencies dependencies() {
    return AdkManagementDependencies{
        .make_acquisition_platform = [this]() {
          ++acquisition_factory_count;
          return std::make_unique<FakeAcquisitionPlatform>(acquisition);
        },
        .make_management_platform = [this]() {
          ++management_factory_count;
          return std::make_unique<FakeManagementPlatform>(management);
        },
    };
  }
};

AdkManagementRequest confirmed_request(
    const AdkReleaseManifest& manifest,
    const AdkManagementAction action) {
  return AdkManagementRequest{
      .action = action,
      .administrator = true,
      .explicit_start_confirmed = true,
      .offline_layout_root = L"C:\\Synthetic\\OfficialAdkLayout",
      .eula_receipt = verified_eula_receipt(manifest),
      .consent_acknowledgement = accepted_review(manifest),
  };
}

void test_product_gate_stops_every_factory_and_path_use() {
  const auto manifest = tsumugi_1_0_0_adk_manifest();
  const std::array actions{
      AdkManagementAction::official_download_install,
      AdkManagementAction::offline_layout_install,
      AdkManagementAction::create_offline_layout,
      AdkManagementAction::uninstall_managed,
  };
  for (const auto action : actions) {
    Factories factories{
        .acquisition = std::make_shared<AcquisitionState>(),
        .management = std::make_shared<ManagementState>(),
    };
    auto request = AdkManagementRequest{
        .action = action,
        .administrator = true,
        .explicit_start_confirmed = true,
        .offline_layout_root = L"\\\\server\\must-not-be-read",
    };
    const auto result = execute_adk_management_action(
        manifest, request, factories.dependencies());
    check(!result, "closed product manifest must fail");
    check(
        factories.acquisition_factory_count == 0U &&
            factories.management_factory_count == 0U,
        "closed gate must create no platform for every action");
    check(
        factories.acquisition->inspect_count == 0U &&
            factories.acquisition->download_count == 0U &&
            factories.acquisition->offline_count == 0U &&
            factories.acquisition->installer_count == 0U &&
            factories.management->load_count == 0U,
        "closed gate must perform no communication, installer, or folder read");
  }
}

void test_unreviewed_registration_id_stops_before_every_factory() {
  auto manifest = make_manifest();
  manifest.payloads.front().uninstall_registration_id =
      L"cmd.exe /c must-never-run";
  Factories factories{
      .acquisition = std::make_shared<AcquisitionState>(
          AcquisitionState{.manifest = &manifest}),
      .management = std::make_shared<ManagementState>(),
  };
  const auto result = execute_adk_management_action(
      manifest,
      confirmed_request(
          manifest, AdkManagementAction::official_download_install),
      factories.dependencies());
  check(!result, "unreviewed registration id must fail closed");
  check(
      factories.acquisition_factory_count == 0U &&
          factories.management_factory_count == 0U &&
          factories.acquisition->inspect_count == 0U &&
          factories.acquisition->download_count == 0U &&
          factories.acquisition->installer_count == 0U,
      "registration contract must stop before platform or acquisition use");
}

void test_ui_contract_and_compact_layout() {
  const auto contract = adk_management_ui_contract();
  check(
      contract.title == L"ADK取得・管理" &&
          contract.official_download_label.find(L"Microsoft公式") !=
              std::wstring_view::npos &&
          contract.offline_install_label.find(L"オフライン") !=
              std::wstring_view::npos &&
          contract.create_layout_label.find(L"新規作成") !=
              std::wstring_view::npos &&
          contract.uninstall_label.find(L"Tsumugi") !=
              std::wstring_view::npos,
      "all fixed product actions must be explicit");
  check(
      contract.explicit_start_required && contract.background_execution &&
          contract.enter_activates_focused_command &&
          contract.escape_cancels_review &&
          contract.previous_focus_restored &&
          contract.official_page_never_opens_automatically,
      "keyboard/focus/background/no-auto-open contract must be fixed");
  for (const auto [width, height] :
       std::array<std::pair<int, int>, 3>{
           std::pair{960, 516},
           std::pair{1024, 600},
           std::pair{1280, 720},
       }) {
    const auto layout = calculate_adk_management_layout(width, height);
    check(layout.bounded, "supported ADK management layout must be bounded");
    check(
        layout.summary_left >= 0 && layout.summary_top >= 0 &&
            layout.command_left >= 0 && layout.command_top >= 0 &&
            layout.cancel_left + layout.cancel_width <=
                layout.client_width &&
            layout.cancel_top + layout.cancel_height <=
                layout.client_height,
        "ADK management controls must remain inside the logical client");
  }
}

void test_pending_view_explains_zero_io_gate() {
  const auto view = build_adk_management_view(
      tsumugi_1_0_0_adk_manifest());
  check(
      view.manifest_structure_valid &&
          !view.primary_source_pins_confirmed &&
          !view.unattended_no_restart_confirmed &&
          !view.execution_gate_open && !view.platform_creation_permitted &&
          !view.path_selection_permitted,
      "pending product pins must block platform and path selection");
  check(
      view.status.find(L"通信") != std::wstring::npos &&
          view.status.find(L"UAC") != std::wstring::npos &&
          view.summary.find(L"停止理由") != std::wstring::npos &&
          view.summary.find(L"予期しない再起動なし") !=
              std::wstring::npos &&
          view.payload_rows.size() == 3U,
      "blocked view must explain the gate and fixed payload summary");
}

void test_action_review_path_contract_and_evidence_are_pure() {
  const auto pending = tsumugi_1_0_0_adk_manifest();
  const auto pending_review = build_adk_management_action_review(
      pending, AdkManagementAction::offline_layout_install);
  check(
      pending_review.manifest_structure_valid &&
          !pending_review.execution_gate_open &&
          pending_review.path_selection ==
              AdkManagementPathSelection::existing_offline_layout &&
          !pending_review.path_picker_permitted &&
          pending_review.requires_eula_review,
      "closed release gate must preserve action explanation but forbid the picker");
  check(
      !make_adk_offline_layout_destination(
          pending, L"\\\\server\\must-not-be-observed"),
      "closed release gate must stop before a hostile parent path is accepted");

  const auto manifest = make_manifest();
  const auto export_review = build_adk_management_action_review(
      manifest, AdkManagementAction::create_offline_layout);
  check(
      export_review.execution_gate_open &&
          export_review.path_picker_permitted &&
          export_review.path_selection ==
              AdkManagementPathSelection::new_offline_layout_parent &&
          export_review.requires_network_retrieval &&
          export_review.requires_eula_review,
      "confirmed export must require EULA, network, and a new-layout parent picker");
  const auto destination = make_adk_offline_layout_destination(
      manifest, L"C:\\Synthetic\\Exports");
  check(
      destination.has_value() &&
          destination.value() ==
              std::filesystem::path(
                  L"C:\\Synthetic\\Exports\\Tsumugi-ADK-Offline-10.1.26100.2454"),
      "offline export must derive one fixed direct child without I/O");

  const auto uninstall_review = build_adk_management_action_review(
      manifest, AdkManagementAction::uninstall_managed);
  check(
      uninstall_review.requires_explicit_uninstall_confirmation &&
          !uninstall_review.requires_eula_review &&
          uninstall_review.path_selection ==
              AdkManagementPathSelection::none,
      "managed uninstall must have its own explicit confirmation and no picker");

  const auto evidence = format_adk_evidence_event(
      manifest,
      AdkEvidenceFacts{
          .action = AdkManagementAction::create_offline_layout,
          .stage = AdkEvidenceStage::consent_accepted,
          .path_selected = true,
          .complete_eula_presented = true,
          .explicit_consent = true,
      });
  check(
      evidence.has_value() && evidence.value().size() <= 1'024U &&
          evidence.value().find(L"manifest=") != std::wstring::npos &&
          evidence.value().find(L"full_eula=1") != std::wstring::npos &&
          evidence.value().find(L"C:\\") == std::wstring::npos &&
          evidence.value().find(L"https://") == std::wstring::npos,
      "evidence must retain bounded facts without paths, URLs, or EULA body");
}

void test_managed_record_fixed_schema_roundtrip_and_rejects_unknown() {
  const AdkManagedInstallationRecord record{
      .manifest_id = "tsumugi-drive-1.0.0-adk-management-test",
      .installed_by_tsumugi = true,
      .installed_registration_ids = {
          L"MSI|{11111111-1111-1111-1111-111111111111}",
          L"MSI|{22222222-2222-2222-2222-222222222222}",
          L"MSP|{33333333-3333-3333-3333-333333333333}|"
          L"{11111111-1111-1111-1111-111111111111}",
      },
  };
  const auto serialized = serialize_managed_adk_record(record);
  check(serialized.has_value(), "managed record must serialize");
  check(
      serialized.value().size() <= kMaximumManagedAdkRecordBytes,
      "managed record must remain bounded");
  const auto parsed = parse_managed_adk_record(serialized.value());
  check(parsed.has_value(), "managed record must parse");
  check(
      parsed.value().manifest_id == record.manifest_id &&
          parsed.value().installed_by_tsumugi &&
          parsed.value().installed_registration_ids ==
              record.installed_registration_ids,
      "managed record roundtrip must preserve only exact identities");

  auto tampered = serialized.value();
  const std::string unknown = "unknown=1\n";
  tampered.insert(
      tampered.end() - 6,
      reinterpret_cast<const std::byte*>(unknown.data()),
      reinterpret_cast<const std::byte*>(unknown.data() + unknown.size()));
  check(
      !parse_managed_adk_record(tampered),
      "unknown managed-record field must fail closed");
}

Factories make_factories(const AdkReleaseManifest& manifest) {
  return Factories{
      .acquisition = std::make_shared<AcquisitionState>(
          AcquisitionState{.manifest = &manifest}),
      .management = std::make_shared<ManagementState>(),
  };
}

void test_mock_official_and_offline_install_routes() {
  const auto manifest = make_manifest();
  for (const auto action : {
           AdkManagementAction::official_download_install,
           AdkManagementAction::offline_layout_install,
       }) {
    auto factories = make_factories(manifest);
    const auto result = execute_adk_management_action(
        manifest,
        confirmed_request(manifest, action),
        factories.dependencies());
    check(result.has_value(), "confirmed synthetic install route must succeed");
    check(
        result.value().installed_by_this_operation &&
            result.value().managed_record_persisted &&
            factories.management->record.has_value(),
        "new installation must persist an exact managed record");
    check(
        factories.acquisition->installer_count == 3U &&
            factories.acquisition->cleanup_count == 1U,
        "all verified payloads must install before cleanup");
    if (action == AdkManagementAction::official_download_install) {
      check(
          factories.acquisition->download_count == 3U &&
              factories.acquisition->offline_count == 0U,
          "official route must use only fixed downloads");
    } else {
      check(
          factories.acquisition->offline_count == 3U &&
              factories.acquisition->download_count == 0U,
          "offline route must use only the selected local layout");
    }
  }
}

void test_foreign_managed_record_blocks_install_before_acquisition() {
  const auto manifest = make_manifest();
  auto factories = make_factories(manifest);
  factories.management->record = AdkManagedInstallationRecord{
      .manifest_id = "foreign-manifest",
      .installed_by_tsumugi = true,
      .installed_registration_ids = {
          L"MSI|{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}",
      },
  };
  const auto result = execute_adk_management_action(
      manifest,
      confirmed_request(
          manifest, AdkManagementAction::official_download_install),
      factories.dependencies());
  check(!result, "foreign managed record must fail closed");
  check(
      factories.management->load_count == 1U &&
          factories.management->save_count == 0U,
      "install must inspect but never replace a foreign managed record");
  check(
      factories.acquisition_factory_count == 0U &&
          factories.acquisition->inspect_count == 0U &&
          factories.acquisition->download_count == 0U &&
          factories.acquisition->offline_count == 0U &&
          factories.acquisition->installer_count == 0U,
      "foreign managed record must stop before acquisition or installer use");
}

void test_mock_offline_layout_generation_route() {
  const auto manifest = make_manifest();
  auto factories = make_factories(manifest);
  const auto result = execute_adk_management_action(
      manifest,
      confirmed_request(
          manifest, AdkManagementAction::create_offline_layout),
      factories.dependencies());
  check(result.has_value(), "synthetic layout generation must succeed");
  check(
      result.value().offline_layout_completed &&
          result.value().offline_layout_file_count == 3U &&
          result.value().offline_layout_bytes == 303U,
      "layout report must aggregate only fixed payload facts");
  check(
      factories.acquisition->download_count == 3U &&
          factories.acquisition->installer_count == 0U &&
          factories.management->begin_layout_count == 1U &&
          factories.management->publish_count == 3U &&
          factories.management->finalize_count == 1U,
      "layout generation must verify/download/publish without installer use");
}

void test_mock_managed_uninstall_route() {
  const auto manifest = make_manifest();
  auto factories = make_factories(manifest);
  AdkManagedInstallationRecord record{
      .manifest_id = manifest.manifest_id,
      .installed_by_tsumugi = true,
  };
  for (const auto& payload : manifest.payloads) {
    record.installed_registration_ids.push_back(
        payload.uninstall_registration_id);
  }
  factories.management->record = record;
  auto request = confirmed_request(
      manifest, AdkManagementAction::uninstall_managed);
  request.eula_receipt.reset();
  request.consent_acknowledgement.reset();
  const auto result = execute_adk_management_action(
      manifest, request, factories.dependencies());
  check(result.has_value(), "exact synthetic managed uninstall must succeed");
  check(
      result.value().managed_record_removed &&
          result.value().uninstall_exit_codes.size() == 3U &&
          !factories.management->record.has_value(),
      "managed record must be removed only after all exact uninstall steps");
  check(
      factories.acquisition_factory_count == 0U &&
          factories.management->load_count == 1U &&
          factories.management->uninstall_count == 1U &&
          factories.management->remove_count == 1U,
      "uninstall must not create a download platform");
}

void test_production_factory_is_constructible_without_io() {
  auto platform = make_windows_adk_management_platform();
  check(
      platform != nullptr,
      "production management adapter factory must be linked and constructible");
  const auto arbitrary = platform->execute_managed_uninstall(
      AdkUninstallPlan{
          .manifest_id = "synthetic-invalid",
          .requires_explicit_confirmation = true,
          .preserves_unmanaged_adk = true,
          .steps = {
              AdkUninstallStep{
                  .kind = AdkPayloadKind::deployment_tools,
                  .display_name = L"synthetic invalid command",
                  .registration_id = L"cmd.exe /c must-never-run",
              },
          },
      });
  check(
      !arbitrary && arbitrary.error().native_code == ERROR_NOT_SUPPORTED,
      "production adapter must reject arbitrary uninstall strings before any native mutation");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"product_gate_stops_every_factory_and_path_use",
       test_product_gate_stops_every_factory_and_path_use},
      {"unreviewed_registration_id_stops_before_every_factory",
       test_unreviewed_registration_id_stops_before_every_factory},
      {"ui_contract_and_compact_layout",
       test_ui_contract_and_compact_layout},
      {"pending_view_explains_zero_io_gate",
       test_pending_view_explains_zero_io_gate},
      {"action_review_path_contract_and_evidence_are_pure",
       test_action_review_path_contract_and_evidence_are_pure},
      {"managed_record_fixed_schema_roundtrip_and_rejects_unknown",
       test_managed_record_fixed_schema_roundtrip_and_rejects_unknown},
      {"mock_official_and_offline_install_routes",
       test_mock_official_and_offline_install_routes},
      {"foreign_managed_record_blocks_install_before_acquisition",
       test_foreign_managed_record_blocks_install_before_acquisition},
      {"mock_offline_layout_generation_route",
       test_mock_offline_layout_generation_route},
      {"mock_managed_uninstall_route",
       test_mock_managed_uninstall_route},
      {"production_factory_is_constructible_without_io",
       test_production_factory_is_constructible_without_io},
  };
  try {
    for (const auto& [name, test] : tests) {
      test();
      std::cout << "PASS " << name << '\n';
    }
  } catch (const TestFailure& failure) {
    std::cerr << "FAIL " << failure.message << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "FAIL exception: " << error.what() << '\n';
    return 1;
  }
  return 0;
}

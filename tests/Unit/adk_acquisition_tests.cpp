#include "ytec/windowsapp/adk_acquisition.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::windowsapp::AdkAcquisitionConsent;
using ytec::windowsapp::AdkAcquisitionRequest;
using ytec::windowsapp::AdkAcquisitionSource;
using ytec::windowsapp::AdkDownloadRequest;
using ytec::windowsapp::AdkInstalledState;
using ytec::windowsapp::AdkInstallerKind;
using ytec::windowsapp::AdkOfflineStageRequest;
using ytec::windowsapp::AdkPayloadKind;
using ytec::windowsapp::AdkPinnedPayload;
using ytec::windowsapp::AdkReleaseManifest;
using ytec::windowsapp::AdkSilentInstallRequest;
using ytec::windowsapp::AdkStagedPayloadReceipt;
using ytec::windowsapp::AdkStagingArea;
using ytec::windowsapp::IAdkAcquisitionPlatform;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

ytec::clonecore::Error mock_error(
    std::wstring operation,
    const DWORD native_code = ERROR_INVALID_DATA) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::verification_failed,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = L"mock failure",
  };
}

template <typename T>
ytec::clonecore::Result<T> mock_failure(std::wstring operation) {
  return ytec::clonecore::Result<T>::failure(
      mock_error(std::move(operation)));
}

std::string repeated_hash(const char character) {
  return std::string(64U, character);
}

AdkReleaseManifest make_manifest() {
  return AdkReleaseManifest{
      .manifest_id = "tsumugi-drive-1.0.0-adk-test-pins",
      .product_release_version = L"1.0.0",
      .tested_adk_version = L"10.1.26100.2454",
      .information_url =
          L"https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install",
      .embedded_eula = ytec::windowsapp::AdkEmbeddedEulaPin{
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
              .offline_relative_path = L"installers/adksetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/a/adksetup.exe",
              .expected_sha256 = repeated_hash('A'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.1.26100.2454",
              .acquired_components = {L"Deployment Tools"},
              .uninstall_registration_id = L"ADK-DEPLOYMENT-TEST",
              .expected_byte_count = 100U,
              .maximum_bytes = 1'024U,
          },
          AdkPinnedPayload{
              .kind = AdkPayloadKind::winpe_addon,
              .installer_kind =
                  AdkInstallerKind::microsoft_bootstrap_exe,
              .display_name = L"Windows PE Add-on",
              .staging_file_name = L"adkwinpesetup.exe",
              .offline_relative_path =
                  L"installers/adkwinpesetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/b/adkwinpesetup.exe",
              .expected_sha256 = repeated_hash('B'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.1.26100.2454",
              .acquired_components = {
                  L"Windows Preinstallation Environment"},
              .uninstall_registration_id = L"ADK-WINPE-TEST",
              .expected_byte_count = 101U,
              .maximum_bytes = 2'048U,
          },
          AdkPinnedPayload{
              .kind = AdkPayloadKind::servicing_update,
              .installer_kind =
                  AdkInstallerKind::windows_update_msu,
              .display_name = L"Windows ADK Servicing Update",
              .staging_file_name = L"adk-update.msu",
              .offline_relative_path = L"updates/adk-update.msu",
              .exact_source_url =
                  L"https://download.microsoft.com/download/c/adk-update.msu",
              .expected_sha256 = repeated_hash('C'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.0.26100.8972",
              .acquired_components = {L"DISM", L"Oscdimg"},
              .uninstall_registration_id = L"ADK-UPDATE-TEST",
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
      .deployment_tools_version =
          manifest.expected_deployment_tools_version,
      .winpe_addon_version = manifest.expected_winpe_addon_version,
      .serviced_dism_version = manifest.expected_serviced_dism_version,
      .servicing_update_id = manifest.required_servicing_update_id,
  };
}

AdkAcquisitionConsent matching_consent(
    const AdkReleaseManifest& manifest) {
  AdkAcquisitionConsent consent{
      .accepted = true,
      .presented_manifest_id = manifest.manifest_id,
      .presented_embedded_eula = manifest.embedded_eula,
  };
  for (const auto& payload : manifest.payloads) {
    consent.presented_payloads.push_back(payload.kind);
  }
  return consent;
}

AdkAcquisitionRequest online_request(
    const AdkReleaseManifest& manifest) {
  return AdkAcquisitionRequest{
      .administrator = true,
      .source = AdkAcquisitionSource::official_download,
      .consent = matching_consent(manifest),
  };
}

std::string event_name(
    const std::string_view prefix,
    const std::size_t index) {
  return std::string(prefix) + std::to_string(index);
}

class MockPlatform final : public IAdkAcquisitionPlatform {
 public:
  explicit MockPlatform(const AdkReleaseManifest& manifest)
      : manifest_(&manifest), after_state_(matching_state(manifest)) {}

  ytec::clonecore::Result<AdkInstalledState> inspect_installed_state(
      const AdkReleaseManifest&) override {
    events.push_back("inspect");
    ++inspect_count_;
    if (inspect_failure_) {
      return mock_failure<AdkInstalledState>(L"inspect");
    }
    if (inspect_count_ == 1U) {
      return ytec::clonecore::Result<AdkInstalledState>::success(
          initial_state_);
    }
    return ytec::clonecore::Result<AdkInstalledState>::success(
        after_state_);
  }

  ytec::clonecore::Result<AdkStagingArea> create_new_staging_area(
      const std::uint64_t maximum_total_bytes) override {
    events.push_back("stage");
    observed_maximum_total_bytes = maximum_total_bytes;
    return ytec::clonecore::Result<AdkStagingArea>::success(
        AdkStagingArea{
            .root = L"C:\\YtecStage\\fixed-new-stage",
            .created_new = stage_created_new_,
            .reparse_point = stage_reparse_,
        });
  }

  ytec::clonecore::Result<AdkStagedPayloadReceipt>
  download_to_new_file(const AdkDownloadRequest& request) override {
    const std::size_t index = index_for_path(request.create_new_destination);
    events.push_back(event_name("download", index));
    download_requests.push_back(request);
    if (download_failure_index_ == index) {
      return mock_failure<AdkStagedPayloadReceipt>(L"download");
    }
    std::vector<std::wstring> visited{request.exact_source_url};
    std::wstring effective = request.exact_source_url;
    if (bad_redirect_index_ == index) {
      effective =
          L"https://download.microsoft.com/download/unlisted/payload.exe";
      visited.push_back(effective);
    }
    return ytec::clonecore::Result<AdkStagedPayloadReceipt>::success(
        AdkStagedPayloadReceipt{
            .staged_path = request.create_new_destination,
            .byte_count = 100U + index,
            .created_new = true,
            .source_regular_file = true,
            .source_reparse_point = false,
            .visited_urls = std::move(visited),
            .effective_url = std::move(effective),
        });
  }

  ytec::clonecore::Result<AdkStagedPayloadReceipt>
  stage_offline_payload(const AdkOfflineStageRequest& request) override {
    const std::size_t index = index_for_path(request.create_new_destination);
    events.push_back(event_name("offline", index));
    offline_requests.push_back(request);
    return ytec::clonecore::Result<AdkStagedPayloadReceipt>::success(
        AdkStagedPayloadReceipt{
            .staged_path = request.create_new_destination,
            .offline_source_path =
                request.layout_root / request.exact_relative_path,
            .byte_count = 100U + index,
            .created_new = true,
            .source_regular_file = true,
            .source_reparse_point = offline_source_reparse_,
        });
  }

  ytec::clonecore::Result<std::string> sha256_file(
      const std::filesystem::path& path,
      const std::uint64_t) override {
    const std::size_t index = index_for_path(path);
    events.push_back(event_name("sha", index));
    if (hash_failure_index_ == index) {
      return mock_failure<std::string>(L"sha");
    }
    if (bad_hash_index_ == index) {
      return ytec::clonecore::Result<std::string>::success(
          repeated_hash('0'));
    }
    return ytec::clonecore::Result<std::string>::success(
        manifest_->payloads[index].expected_sha256);
  }

  ytec::clonecore::Status verify_authenticode(
      const std::filesystem::path& path,
      const std::wstring_view expected_signer_subject) override {
    const std::size_t index = index_for_path(path);
    events.push_back(event_name("signature", index));
    observed_signers.emplace_back(expected_signer_subject);
    if (bad_signature_index_ == index) {
      return ytec::clonecore::Status::failure(
          mock_error(
              L"signature",
              static_cast<DWORD>(TRUST_E_NOSIGNATURE)));
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::wstring> query_payload_version(
      const std::filesystem::path& path) override {
    const std::size_t index = index_for_path(path);
    events.push_back(event_name("version", index));
    if (bad_version_index_ == index) {
      return ytec::clonecore::Result<std::wstring>::success(L"0.0.0.0");
    }
    return ytec::clonecore::Result<std::wstring>::success(
        manifest_->payloads[index].expected_payload_version);
  }

  ytec::clonecore::Result<
      std::vector<ytec::windowsapp::AdkVerifiedPayload>>
  expand_and_verify_patch_archive(
      const ytec::windowsapp::AdkPatchArchiveExpandRequest&) override {
    return mock_failure<std::vector<ytec::windowsapp::AdkVerifiedPayload>>(
        L"unexpected archive expansion");
  }

  ytec::clonecore::Result<std::uint32_t>
  run_verified_silent_installer(
      const AdkSilentInstallRequest& request) override {
    const std::size_t index = index_for_path(request.payload.staged_path);
    events.push_back(event_name("run", index));
    install_requests.push_back(request);
    if (installer_api_failure_index_ == index) {
      return mock_failure<std::uint32_t>(L"run");
    }
    if (index < installer_exit_codes_.size()) {
      return ytec::clonecore::Result<std::uint32_t>::success(
          installer_exit_codes_[index]);
    }
    return ytec::clonecore::Result<std::uint32_t>::success(ERROR_SUCCESS);
  }

  ytec::clonecore::Status remove_staging_area(
      const AdkStagingArea&) override {
    events.push_back("cleanup");
    if (cleanup_failure_) {
      return ytec::clonecore::Status::failure(mock_error(L"cleanup"));
    }
    return ytec::clonecore::success_status();
  }

  void use_matching_existing_state() {
    initial_state_ = matching_state(*manifest_);
  }

  void use_partial_existing_state() {
    initial_state_.deployment_tools_present = true;
    initial_state_.deployment_tools_version =
        manifest_->expected_deployment_tools_version;
  }

  void set_post_state_mismatch() {
    after_state_.servicing_update_present = false;
  }

  void set_bad_hash(const std::size_t index) { bad_hash_index_ = index; }
  void set_bad_signature(const std::size_t index) {
    bad_signature_index_ = index;
  }
  void set_bad_version(const std::size_t index) {
    bad_version_index_ = index;
  }
  void set_bad_redirect(const std::size_t index) {
    bad_redirect_index_ = index;
  }
  void set_installer_exit_codes(std::vector<std::uint32_t> values) {
    installer_exit_codes_ = std::move(values);
  }
  void set_offline_source_reparse() { offline_source_reparse_ = true; }

  std::vector<std::string> events;
  std::vector<AdkDownloadRequest> download_requests;
  std::vector<AdkOfflineStageRequest> offline_requests;
  std::vector<AdkSilentInstallRequest> install_requests;
  std::vector<std::wstring> observed_signers;
  std::uint64_t observed_maximum_total_bytes{};

 private:
  std::size_t index_for_path(const std::filesystem::path& path) const {
    for (std::size_t index = 0; index < manifest_->payloads.size(); ++index) {
      if (path.filename() == manifest_->payloads[index].staging_file_name) {
        return index;
      }
    }
    throw TestFailure{"Mock received an unknown staged path"};
  }

  static constexpr std::size_t kNever =
      (std::numeric_limits<std::size_t>::max)();
  const AdkReleaseManifest* manifest_{};
  AdkInstalledState initial_state_;
  AdkInstalledState after_state_;
  std::size_t inspect_count_{};
  std::size_t download_failure_index_{kNever};
  std::size_t hash_failure_index_{kNever};
  std::size_t bad_hash_index_{kNever};
  std::size_t bad_signature_index_{kNever};
  std::size_t bad_version_index_{kNever};
  std::size_t bad_redirect_index_{kNever};
  std::size_t installer_api_failure_index_{kNever};
  std::vector<std::uint32_t> installer_exit_codes_;
  bool inspect_failure_{};
  bool stage_created_new_{true};
  bool stage_reparse_{};
  bool offline_source_reparse_{};
  bool cleanup_failure_{};
};

std::size_t event_position(
    const std::vector<std::string>& events,
    const std::string_view expected) {
  const auto iterator = std::find(events.begin(), events.end(), expected);
  check(iterator != events.end(), "Expected event was not recorded");
  return static_cast<std::size_t>(std::distance(events.begin(), iterator));
}

void test_pending_product_manifest_fails_before_platform_use() {
  const auto manifest =
      ytec::windowsapp::tsumugi_1_0_0_adk_manifest();
  MockPlatform platform(manifest);
  auto request = online_request(manifest);
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, platform);
  check(!result, "Pending product manifest must fail closed");
  check(
      platform.events.empty(),
      "Pending manifest must perform no platform I/O");
  check(
      result.error().message.find(L"未完了") != std::wstring::npos,
      "Pending manifest error should explain the missing primary pins");
}

void test_missing_or_stale_consent_performs_no_platform_call() {
  const auto manifest = make_manifest();
  MockPlatform platform(manifest);
  auto request = online_request(manifest);
  request.consent.accepted = false;
  auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, platform);
  check(!result, "Missing consent must fail");
  check(platform.events.empty(), "Missing consent must cause zero calls");

  request.consent.accepted = true;
  request.consent.presented_manifest_id = "stale-manifest";
  result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, platform);
  check(!result, "Stale consent must fail");
  check(platform.events.empty(), "Stale consent must cause zero calls");

  request = online_request(manifest);
  request.consent.presented_embedded_eula.expected_sha256 =
      repeated_hash('0');
  result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, platform);
  check(!result, "Consent for a different EULA body must fail");
  check(
      platform.events.empty(),
      "Stale EULA digest must cause zero platform calls");

  request = online_request(manifest);
  request.administrator = false;
  result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, platform);
  check(!result, "A non-administrator request must fail");
  check(
      platform.events.empty(),
      "A non-administrator request must cause zero platform calls");
}

void test_generic_microsoft_terms_cannot_stand_in_for_adk_eula() {
  auto manifest = make_manifest();
  manifest.embedded_eula.official_bootstrap_url =
      L"https://www.microsoft.com/en-us/licensing/terms";
  MockPlatform platform(manifest);
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, online_request(manifest), platform);
  check(!result, "A generic Microsoft terms page must not be an ADK EULA");
  check(
      platform.events.empty(),
      "A generic terms page must fail before any platform or network call");
}

void test_unconfirmed_bootstrap_restart_safety_blocks_before_platform() {
  auto manifest = make_manifest();
  manifest.unattended_install_no_unexpected_restart_confirmed = false;
  MockPlatform platform(manifest);
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, online_request(manifest), platform);
  check(!result, "Unproved bootstrap restart behavior must fail");
  check(
      platform.events.empty(),
      "Restart-safety gate must fail before platform or network calls");
  check(
      result.error().message.find(L"自動再起動") != std::wstring::npos,
      "Restart-safety failure should name the exact blocker");
}

void test_wrong_host_and_redirect_manifest_fail_before_network() {
  auto manifest = make_manifest();
  manifest.payloads[0].exact_source_url =
      L"https://example.invalid/download/adksetup.exe";
  MockPlatform wrong_host_platform(manifest);
  auto request = online_request(manifest);
  auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, wrong_host_platform);
  check(!result, "A non-Microsoft package host must fail");
  check(
      wrong_host_platform.events.empty(),
      "Bad host must be rejected before any network/platform call");

  manifest = make_manifest();
  manifest.payloads[1].allowed_redirect_urls = {
      L"https://cdn.example.invalid/download/winpe.exe"};
  MockPlatform wrong_redirect_platform(manifest);
  request = online_request(manifest);
  result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, wrong_redirect_platform);
  check(!result, "A non-Microsoft redirect must fail");
  check(
      wrong_redirect_platform.events.empty(),
      "Bad redirect must be rejected before any network/platform call");
}

void test_existing_verified_adk_is_preserved_without_staging() {
  const auto manifest = make_manifest();
  MockPlatform platform(manifest);
  platform.use_matching_existing_state();
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, online_request(manifest), platform);
  check(result.has_value(), "Matching existing ADK should be reused");
  check(
      result.value().used_existing_installation,
      "Report should identify existing installation reuse");
  check(
      !result.value().installed_by_this_operation,
      "Existing ADK must not be claimed as app-installed");
  check(
      platform.events == std::vector<std::string>{"inspect"},
      "Existing ADK must not create staging, download, or launch");
}

void test_online_success_verifies_all_before_first_launch() {
  const auto manifest = make_manifest();
  MockPlatform platform(manifest);
  platform.use_partial_existing_state();
  platform.set_installer_exit_codes({ERROR_SUCCESS, 3010U, ERROR_SUCCESS});
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, online_request(manifest), platform);
  check(result.has_value(), "Pinned online acquisition should succeed");
  check(
      result.value().reboot_required,
      "Exit code 3010 should be retained as reboot required");
  check(
      result.value().preexisting_adk_preserved,
      "Existing mismatched ADK components must be preserved");
  check(
      result.value().verified_payloads.size() == 3U &&
          result.value().installer_exit_codes.size() == 3U &&
          result.value().managed_installation_registration_ids.size() == 3U,
      "Report should include verified payloads, exits, and managed IDs");
  check(
      platform.observed_maximum_total_bytes == 7'168U &&
          platform.download_requests.size() == 3U &&
          platform.download_requests[2].maximum_bytes == 4'096U &&
          platform.download_requests[0].create_new_destination.filename() ==
              L"adksetup.exe",
      "Stage and each CREATE_NEW download must retain fixed byte bounds");
  check(
      event_position(platform.events, "version2") <
          event_position(platform.events, "run0"),
      "Every payload must pass version verification before first launch");
  for (std::size_t index = 0; index < 3U; ++index) {
    check(
        event_position(platform.events, event_name("sha", index)) <
            event_position(platform.events, event_name("signature", index)) &&
            event_position(platform.events, event_name("signature", index)) <
                event_position(platform.events, event_name("version", index)),
        "Each payload must be checked SHA-256, signature, then version");
  }
  check(
      platform.events.back() == "cleanup",
      "Successful final preflight must be followed by staging cleanup");
  check(
      platform.install_requests[0].fixed_arguments ==
          std::vector<std::wstring>{
              L"/quiet",
              L"/norestart",
              L"/features",
              L"OptionId.DeploymentTools"},
      "Deployment Tools must use the exact fixed feature arguments");
  check(
      platform.install_requests[1].fixed_arguments.back() ==
          L"OptionId.WindowsPreinstallationEnvironment",
      "WinPE Add-on must use only its fixed feature identifier");
  check(
      platform.install_requests[2].fixed_arguments ==
          std::vector<std::wstring>{L"/quiet", L"/norestart"},
      "Servicing update must use bounded fixed quiet arguments");
}

void test_tamper_signature_and_version_fail_without_launch() {
  for (std::size_t mode = 0; mode < 3U; ++mode) {
    const auto manifest = make_manifest();
    MockPlatform platform(manifest);
    if (mode == 0U) {
      platform.set_bad_hash(1U);
    } else if (mode == 1U) {
      platform.set_bad_signature(1U);
    } else {
      platform.set_bad_version(1U);
    }
    const auto result = ytec::windowsapp::execute_adk_acquisition(
        manifest, online_request(manifest), platform);
    check(!result, "Tampered payload identity must fail");
    check(
        std::none_of(
            platform.events.begin(),
            platform.events.end(),
            [](const std::string& event) { return event.starts_with("run"); }),
        "Hash/signature/version failure must launch no installer");
    check(
        platform.events.back() == "cleanup",
        "Failed staged acquisition should clean its private staging area");
  }
}

void test_unreported_redirect_is_rejected_without_launch() {
  const auto manifest = make_manifest();
  MockPlatform platform(manifest);
  platform.set_bad_redirect(0U);
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, online_request(manifest), platform);
  check(!result, "Unpinned effective URL must fail");
  check(
      std::none_of(
          platform.events.begin(),
          platform.events.end(),
          [](const std::string& event) { return event.starts_with("run"); }),
      "Unpinned redirect must launch no installer");
}

void test_offline_layout_uses_same_identity_checks() {
  const auto manifest = make_manifest();
  MockPlatform platform(manifest);
  auto request = online_request(manifest);
  request.source = AdkAcquisitionSource::official_offline_layout;
  request.offline_layout_root = L"D:\\OfficialAdkLayout";
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, platform);
  check(result.has_value(), "Verified offline layout should succeed");
  check(result.value().offline_layout_used, "Report should name offline use");
  check(
      platform.download_requests.empty() &&
          platform.offline_requests.size() == 3U,
      "Offline layout must perform no downloader call");
  check(
      event_position(platform.events, "sha0") <
              event_position(platform.events, "signature0") &&
          event_position(platform.events, "signature0") <
              event_position(platform.events, "version0"),
      "Offline payload must receive the same verification sequence");
}

void test_offline_reparse_and_unc_root_fail_closed() {
  const auto manifest = make_manifest();
  auto request = online_request(manifest);
  request.source = AdkAcquisitionSource::official_offline_layout;
  request.offline_layout_root = L"\\\\server\\layout";
  MockPlatform unc_platform(manifest);
  auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, unc_platform);
  check(!result, "UNC offline layout should be rejected");
  check(
      unc_platform.events.empty(),
      "UNC root should fail before platform access");

  request.offline_layout_root = L"D:\\OfficialAdkLayout";
  MockPlatform reparse_platform(manifest);
  reparse_platform.set_offline_source_reparse();
  result = ytec::windowsapp::execute_adk_acquisition(
      manifest, request, reparse_platform);
  check(!result, "Offline reparse source should fail");
  check(
      std::none_of(
          reparse_platform.events.begin(),
          reparse_platform.events.end(),
          [](const std::string& event) { return event.starts_with("run"); }),
      "Offline reparse source must launch no installer");
}

void test_installer_failure_stops_following_components() {
  const auto manifest = make_manifest();
  MockPlatform platform(manifest);
  platform.set_installer_exit_codes({ERROR_SUCCESS, 1603U, ERROR_SUCCESS});
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, online_request(manifest), platform);
  check(!result, "Unexpected installer exit code must fail");
  check(
      event_position(platform.events, "run1") <
          event_position(platform.events, "cleanup"),
      "Failed installer should be followed by staging cleanup");
  check(
      std::find(platform.events.begin(), platform.events.end(), "run2") ==
          platform.events.end(),
      "Installer failure must stop later components");
}

void test_patch_target_not_found_is_only_permitted_for_msp() {
  using ytec::windowsapp::adk_installer_exit_code_permitted;
  constexpr std::uint32_t kPatchTargetNotFound = 1642U;

  check(
      adk_installer_exit_code_permitted(
          AdkPayloadKind::servicing_update,
          AdkInstallerKind::windows_installer_patch_msp,
          kPatchTargetNotFound),
      "An MSP for an uninstalled optional ADK feature may report 1642");
  check(
      !adk_installer_exit_code_permitted(
          AdkPayloadKind::servicing_update,
          AdkInstallerKind::windows_update_msu,
          kPatchTargetNotFound) &&
          !adk_installer_exit_code_permitted(
              AdkPayloadKind::deployment_tools,
              AdkInstallerKind::microsoft_bootstrap_exe,
              kPatchTargetNotFound),
      "1642 must remain fatal for non-MSP installers");
  check(
      adk_installer_exit_code_permitted(
          AdkPayloadKind::deployment_tools,
          AdkInstallerKind::microsoft_bootstrap_exe,
          ERROR_SUCCESS) &&
          adk_installer_exit_code_permitted(
              AdkPayloadKind::winpe_addon,
              AdkInstallerKind::microsoft_bootstrap_exe,
              3010U),
      "Success and reboot-required must remain permitted");
}

void test_post_install_preflight_is_mandatory() {
  const auto manifest = make_manifest();
  MockPlatform platform(manifest);
  platform.set_post_state_mismatch();
  const auto result = ytec::windowsapp::execute_adk_acquisition(
      manifest, online_request(manifest), platform);
  check(!result, "Mismatched post-install state must fail");
  check(
      std::count(platform.events.begin(), platform.events.end(), "inspect") ==
          2,
      "Installed state must be inspected before and after installation");
  check(
      platform.events.back() == "cleanup",
      "Post-install preflight failure should clean staging");
}

void test_uninstall_plan_only_accepts_exact_managed_record() {
  const auto manifest = make_manifest();
  ytec::windowsapp::AdkManagedInstallationRecord record{
      .manifest_id = manifest.manifest_id,
      .installed_by_tsumugi = true,
      .installed_registration_ids = {
          L"ADK-DEPLOYMENT-TEST",
          L"ADK-WINPE-TEST",
          L"ADK-UPDATE-TEST",
      },
  };
  const auto plan =
      ytec::windowsapp::build_managed_adk_uninstall_plan(manifest, record);
  check(plan.has_value(), "Exact app-managed record should produce a plan");
  check(
      plan.value().requires_explicit_confirmation &&
          plan.value().preserves_unmanaged_adk,
      "Uninstall plan must require confirmation and preserve unmanaged ADK");
  check(
      plan.value().steps.size() == 3U &&
          plan.value().steps.front().kind ==
              AdkPayloadKind::servicing_update &&
          plan.value().steps.back().kind ==
              AdkPayloadKind::deployment_tools,
      "Removal plan should use reverse installation order");

  record.installed_by_tsumugi = false;
  const auto unmanaged =
      ytec::windowsapp::build_managed_adk_uninstall_plan(manifest, record);
  check(!unmanaged, "Unmanaged existing ADK must not produce a removal plan");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"pending_product_manifest_fails_before_platform_use",
       test_pending_product_manifest_fails_before_platform_use},
      {"missing_or_stale_consent_performs_no_platform_call",
       test_missing_or_stale_consent_performs_no_platform_call},
      {"generic_microsoft_terms_cannot_stand_in_for_adk_eula",
       test_generic_microsoft_terms_cannot_stand_in_for_adk_eula},
      {"unconfirmed_bootstrap_restart_safety_blocks_before_platform",
       test_unconfirmed_bootstrap_restart_safety_blocks_before_platform},
      {"wrong_host_and_redirect_manifest_fail_before_network",
       test_wrong_host_and_redirect_manifest_fail_before_network},
      {"existing_verified_adk_is_preserved_without_staging",
       test_existing_verified_adk_is_preserved_without_staging},
      {"online_success_verifies_all_before_first_launch",
       test_online_success_verifies_all_before_first_launch},
      {"tamper_signature_and_version_fail_without_launch",
       test_tamper_signature_and_version_fail_without_launch},
      {"unreported_redirect_is_rejected_without_launch",
       test_unreported_redirect_is_rejected_without_launch},
      {"offline_layout_uses_same_identity_checks",
       test_offline_layout_uses_same_identity_checks},
      {"offline_reparse_and_unc_root_fail_closed",
       test_offline_reparse_and_unc_root_fail_closed},
      {"installer_failure_stops_following_components",
       test_installer_failure_stops_following_components},
      {"patch_target_not_found_is_only_permitted_for_msp",
       test_patch_target_not_found_is_only_permitted_for_msp},
      {"post_install_preflight_is_mandatory",
       test_post_install_preflight_is_mandatory},
      {"uninstall_plan_only_accepts_exact_managed_record",
       test_uninstall_plan_only_accepts_exact_managed_record},
  };

  int failures{};
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name
                << ": unexpected exception: " << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

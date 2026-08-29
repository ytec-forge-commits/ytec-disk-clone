#include "ytec/windowsapp/adk_consent_review.h"

#include "ytec/imageformat/sha256.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

std::string hash_of(const char value) {
  return std::string(64U, value);
}

std::vector<std::byte> synthetic_eula() {
  constexpr std::string_view body =
      "{\\rtf1\\ansi Synthetic verified ADK EULA for product review.}";
  std::vector<std::byte> bytes;
  bytes.reserve(body.size());
  for (const char character : body) {
    bytes.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(character)));
  }
  return bytes;
}

std::string hex_digest(const ytec::imageformat::Sha256Digest& digest) {
  constexpr std::array<char, 16U> hex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string text;
  text.reserve(digest.size() * 2U);
  for (const std::byte value : digest) {
    const auto byte = std::to_integer<unsigned int>(value);
    text.push_back(hex[(byte >> 4U) & 0x0FU]);
    text.push_back(hex[byte & 0x0FU]);
  }
  return text;
}

void bind_synthetic_eula(
    ytec::windowsapp::AdkReleaseManifest& manifest,
    const std::span<const std::byte> body) {
  const auto digest = ytec::imageformat::sha256(body);
  check(digest.has_value(), "synthetic EULA hash must be available");
  manifest.embedded_eula.expected_byte_count = body.size();
  manifest.embedded_eula.expected_sha256 = hex_digest(digest.value());
}

ytec::windowsapp::AdkReleaseManifest confirmed_manifest() {
  using ytec::windowsapp::AdkInstallerKind;
  using ytec::windowsapp::AdkPayloadKind;
  using ytec::windowsapp::AdkPinnedPayload;
  return ytec::windowsapp::AdkReleaseManifest{
      .manifest_id = "tsumugi-adk-consent-test",
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
          .expected_sha256 = hash_of('D'),
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
              .expected_sha256 = hash_of('A'),
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
              .offline_relative_path = L"adkwinpesetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/b/adkwinpesetup.exe",
              .expected_sha256 = hash_of('B'),
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
              .installer_kind = AdkInstallerKind::windows_update_msu,
              .display_name = L"Windows ADK Servicing Update",
              .staging_file_name = L"adk-update.msu",
              .offline_relative_path = L"adk-update.msu",
              .exact_source_url =
                  L"https://download.microsoft.com/download/c/adk-update.msu",
              .expected_sha256 = hash_of('C'),
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.0.26100.8972",
              .acquired_components = {L"DISM", L"Oscdimg"},
              .uninstall_registration_id = L"KB5101684",
              .expected_byte_count = 102U,
              .maximum_bytes = 4'096U,
          },
      },
      .primary_source_pins_confirmed = true,
  };
}

ytec::windowsapp::AdkEulaDocumentReceipt receipt_for(
    const ytec::windowsapp::AdkReleaseManifest& manifest) {
  return ytec::windowsapp::AdkEulaDocumentReceipt{
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

ytec::windowsapp::AdkConsentReviewAcknowledgement acknowledgement_for(
    const ytec::windowsapp::AdkReleaseManifest& manifest) {
  return ytec::windowsapp::AdkConsentReviewAcknowledgement{
      .reviewed_manifest_id = manifest.manifest_id,
      .reviewed_embedded_eula = manifest.embedded_eula,
      .official_sources_presented = true,
      .acquired_components_presented = true,
      .eula_body_opened = true,
      .eula_body_fully_presented = true,
      .explicit_acceptance = true,
  };
}

void test_product_manifest_names_exact_eula_ui_blocker() {
  const auto manifest =
      ytec::windowsapp::tsumugi_1_0_0_adk_manifest();
  check(
      manifest.embedded_eula.official_bootstrap_url ==
              manifest.payloads[0].exact_source_url &&
          manifest.embedded_eula.container_offset == 0xB3000U &&
          manifest.embedded_eula.container_length == 0x16C2CDU &&
          manifest.embedded_eula.container_member_name == L"u6" &&
          manifest.embedded_eula.display_file_name ==
              std::filesystem::path(L"ja\\eula.rtf") &&
          manifest.embedded_eula.expected_byte_count == 293'766U &&
          manifest.embedded_eula.expected_sha256 ==
              "32B66AE90683DE9C91EDE927A45E8E44845CD36E43821BFA4EB2CA5C36A9CF54" &&
          manifest.embedded_eula.primary_source_confirmed &&
          !manifest.unattended_install_no_unexpected_restart_confirmed &&
          !manifest.primary_source_pins_confirmed,
      "The product must retain the audited embedded EULA pin and closed execution gates");
  const auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, nullptr);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_unavailable &&
          !view.ready_to_present && !view.consent_permitted,
      "The product must remain blocked until the verified receipt reaches the UI");
  check(
      view.message.find(L"EULA全文レビュー") !=
          std::wstring::npos,
      "The blocker must name the complete product review requirement");

  const auto fabricated_receipt = receipt_for(manifest);
  const auto blocked_consent =
      ytec::windowsapp::complete_adk_consent_review(
          manifest,
          fabricated_receipt,
          acknowledgement_for(manifest));
  check(
      !blocked_consent,
      "Even a matching receipt cannot bypass the closed product release gate");
}

void test_generic_terms_and_missing_receipt_fail_closed() {
  auto manifest = confirmed_manifest();
  auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, nullptr);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_unavailable,
      "A confirmed pin still requires a verified document receipt");

  manifest.embedded_eula.official_bootstrap_url =
      L"https://www.microsoft.com/en-us/licensing/terms";
  auto generic_receipt = receipt_for(manifest);
  view = ytec::windowsapp::build_adk_consent_review(
      &manifest, &generic_receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_manifest_invalid,
      "A generic Microsoft terms page must fail manifest validation");
}

void test_unverified_or_changed_receipt_cannot_reach_review() {
  const auto manifest = confirmed_manifest();
  auto receipt = receipt_for(manifest);
  receipt.bootstrap_full_identity_verified = false;
  auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_unverified,
      "Unverified Microsoft provenance must fail");

  receipt = receipt_for(manifest);
  receipt.member_copy_reparse_point = true;
  view = ytec::windowsapp::build_adk_consent_review(&manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_unverified,
      "A reparse staged document must fail");

  receipt = receipt_for(manifest);
  receipt.extracted_identity.expected_sha256 = hash_of('0');
  view = ytec::windowsapp::build_adk_consent_review(&manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_mismatch,
      "A different EULA body must fail exact identity matching");
}

void test_ready_review_contains_every_exact_source_and_component() {
  const auto manifest = confirmed_manifest();
  const auto receipt = receipt_for(manifest);
  const auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, &receipt);
  check(
      view.issue ==
              ytec::windowsapp::AdkConsentReviewIssue::review_required &&
          view.ready_to_present && !view.consent_permitted,
      "Verified inputs should create a review, not implicit consent");
  check(
      view.payloads.size() == manifest.payloads.size() &&
          view.payloads[0].exact_source_url ==
              manifest.payloads[0].exact_source_url &&
          view.payloads[1].acquired_components ==
              manifest.payloads[1].acquired_components &&
          view.embedded_eula == manifest.embedded_eula,
      "Review rows must preserve exact sources, components, and EULA pin");
  check(
      view.summary.find(manifest.payloads[2].exact_source_url) !=
              std::wstring::npos &&
          view.summary.find(L"Windows Preinstallation Environment") !=
              std::wstring::npos,
      "Display summary must include every acquired source and component");
}

void test_consent_requires_full_same_review_and_explicit_acceptance() {
  const auto manifest = confirmed_manifest();
  const auto receipt = receipt_for(manifest);
  auto acknowledgement = acknowledgement_for(manifest);

  acknowledgement.eula_body_fully_presented = false;
  auto result = ytec::windowsapp::complete_adk_consent_review(
      manifest, receipt, acknowledgement);
  check(!result, "A partially displayed EULA must not issue consent");

  acknowledgement = acknowledgement_for(manifest);
  acknowledgement.explicit_acceptance = false;
  result = ytec::windowsapp::complete_adk_consent_review(
      manifest, receipt, acknowledgement);
  check(!result, "Review without explicit acceptance must not issue consent");

  acknowledgement = acknowledgement_for(manifest);
  acknowledgement.reviewed_embedded_eula.expected_sha256 = hash_of('0');
  result = ytec::windowsapp::complete_adk_consent_review(
      manifest, receipt, acknowledgement);
  check(!result, "Consent cannot move to a different EULA revision");

  acknowledgement = acknowledgement_for(manifest);
  result = ytec::windowsapp::complete_adk_consent_review(
      manifest, receipt, acknowledgement);
  check(result.has_value(), "A complete exact review should issue consent");
  check(
      result.value().accepted &&
          result.value().presented_manifest_id == manifest.manifest_id &&
          result.value().presented_embedded_eula ==
              manifest.embedded_eula &&
          result.value().presented_payloads.size() == 3U,
      "Issued consent must bind the exact manifest, EULA, and payload order");
}

void test_receipt_requires_matching_container_range_and_member() {
  const auto manifest = confirmed_manifest();
  auto receipt = receipt_for(manifest);
  receipt.extracted_identity.container_offset += 1U;
  auto view = ytec::windowsapp::build_adk_consent_review(
      &manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_mismatch,
      "A different attached container range must fail");

  receipt = receipt_for(manifest);
  receipt.extracted_identity.container_member_name = L"u7";
  view = ytec::windowsapp::build_adk_consent_review(&manifest, &receipt);
  check(
      view.issue == ytec::windowsapp::AdkConsentReviewIssue::
                        blocked_eula_receipt_mismatch,
      "A different Burn UX member must fail");
}

void test_verified_document_requires_full_scroll_and_explicit_acceptance() {
  auto manifest = confirmed_manifest();
  auto body = synthetic_eula();
  bind_synthetic_eula(manifest, body);
  ytec::windowsapp::AdkVerifiedEulaDocument document{
      .receipt = receipt_for(manifest),
      .rtf_document = body,
      .staging_removed = true,
  };
  check(
      static_cast<bool>(
          ytec::windowsapp::validate_adk_eula_document_for_presentation(
              manifest, document)),
      "exact verified in-memory RTF should reach the presentation boundary");

  for (const auto [width, height] :
       std::array<std::pair<int, int>, 3U>{
           std::pair{640, 480},
           std::pair{960, 516},
           std::pair{1280, 720},
       }) {
    const auto layout =
        ytec::windowsapp::calculate_adk_consent_dialog_layout(
            width, height);
    check(layout.bounded, "supported EULA dialog layout must be bounded");
  }

  auto result = ytec::windowsapp::complete_adk_consent_presentation(
      manifest,
      document,
      ytec::windowsapp::AdkConsentPresentationFacts{
          .official_sources_presented = true,
          .acquired_components_presented = true,
          .eula_body_opened = true,
          .eula_body_end_reached = false,
          .explicit_acceptance = true,
      });
  check(!result, "checkbox acceptance before the document end must fail");
  result = ytec::windowsapp::complete_adk_consent_presentation(
      manifest,
      document,
      ytec::windowsapp::AdkConsentPresentationFacts{
          .official_sources_presented = true,
          .acquired_components_presented = true,
          .eula_body_opened = true,
          .eula_body_end_reached = true,
          .explicit_acceptance = false,
      });
  check(!result, "full scroll without explicit acceptance must fail");
  result = ytec::windowsapp::complete_adk_consent_presentation(
      manifest,
      document,
      ytec::windowsapp::AdkConsentPresentationFacts{
          .official_sources_presented = true,
          .acquired_components_presented = true,
          .eula_body_opened = true,
          .eula_body_end_reached = true,
          .explicit_acceptance = true,
      });
  check(result.has_value(), "full exact review plus acceptance should bind acknowledgement");

  document.rtf_document.back() ^= std::byte{0x01};
  check(
      !ytec::windowsapp::validate_adk_eula_document_for_presentation(
          manifest, document),
      "tampered in-memory RTF must fail before presentation");
}

struct PreparationState final {
  const ytec::windowsapp::AdkReleaseManifest* manifest{};
  std::size_t create_count{};
  std::size_t download_count{};
  std::size_t offline_count{};
  std::size_t installer_count{};
  std::size_t cleanup_count{};
};

class PreparationPlatform final
    : public ytec::windowsapp::IAdkAcquisitionPlatform {
 public:
  explicit PreparationPlatform(std::shared_ptr<PreparationState> state)
      : state_(std::move(state)) {}

  ytec::clonecore::Result<ytec::windowsapp::AdkInstalledState>
  inspect_installed_state(
      const ytec::windowsapp::AdkReleaseManifest&) override {
    return unexpected<ytec::windowsapp::AdkInstalledState>();
  }

  ytec::clonecore::Result<ytec::windowsapp::AdkStagingArea>
  create_new_staging_area(std::uint64_t) override {
    ++state_->create_count;
    return ytec::clonecore::Result<
        ytec::windowsapp::AdkStagingArea>::success(
        ytec::windowsapp::AdkStagingArea{
            .root = L"C:\\Synthetic\\adk-eula-stage",
            .created_new = true,
            .reparse_point = false,
        });
  }

  ytec::clonecore::Result<ytec::windowsapp::AdkStagedPayloadReceipt>
  download_to_new_file(
      const ytec::windowsapp::AdkDownloadRequest& request) override {
    ++state_->download_count;
    const auto& pin = state_->manifest->payloads.front();
    return ytec::clonecore::Result<
        ytec::windowsapp::AdkStagedPayloadReceipt>::success(
        ytec::windowsapp::AdkStagedPayloadReceipt{
            .staged_path = request.create_new_destination,
            .byte_count = pin.expected_byte_count,
            .created_new = true,
            .source_regular_file = true,
            .source_reparse_point = false,
            .visited_urls = {request.exact_source_url},
            .effective_url = request.exact_source_url,
        });
  }

  ytec::clonecore::Result<ytec::windowsapp::AdkStagedPayloadReceipt>
  stage_offline_payload(
      const ytec::windowsapp::AdkOfflineStageRequest& request) override {
    ++state_->offline_count;
    const auto& pin = state_->manifest->payloads.front();
    return ytec::clonecore::Result<
        ytec::windowsapp::AdkStagedPayloadReceipt>::success(
        ytec::windowsapp::AdkStagedPayloadReceipt{
            .staged_path = request.create_new_destination,
            .offline_source_path =
                request.layout_root / request.exact_relative_path,
            .byte_count = pin.expected_byte_count,
            .created_new = true,
            .source_regular_file = true,
            .source_reparse_point = false,
        });
  }

  ytec::clonecore::Result<std::string> sha256_file(
      const std::filesystem::path&,
      std::uint64_t) override {
    return ytec::clonecore::Result<std::string>::success(
        state_->manifest->payloads.front().expected_sha256);
  }

  ytec::clonecore::Status verify_authenticode(
      const std::filesystem::path&,
      std::wstring_view) override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::wstring> query_payload_version(
      const std::filesystem::path&) override {
    return ytec::clonecore::Result<std::wstring>::success(
        state_->manifest->payloads.front().expected_payload_version);
  }

  ytec::clonecore::Result<std::vector<ytec::windowsapp::AdkVerifiedPayload>>
  expand_and_verify_patch_archive(
      const ytec::windowsapp::AdkPatchArchiveExpandRequest&) override {
    return unexpected<std::vector<ytec::windowsapp::AdkVerifiedPayload>>();
  }

  ytec::clonecore::Result<std::uint32_t> run_verified_silent_installer(
      const ytec::windowsapp::AdkSilentInstallRequest&) override {
    ++state_->installer_count;
    return unexpected<std::uint32_t>();
  }

  ytec::clonecore::Status remove_staging_area(
      const ytec::windowsapp::AdkStagingArea&) override {
    ++state_->cleanup_count;
    return ytec::clonecore::success_status();
  }

 private:
  template <typename T>
  ytec::clonecore::Result<T> unexpected() const {
    return ytec::clonecore::Result<T>::failure(ytec::clonecore::Error{
        .code = ytec::clonecore::ErrorCode::internal_error,
        .native_code = 1U,
        .operation = L"synthetic unexpected call",
        .message = L"synthetic unexpected call",
    });
  }

  std::shared_ptr<PreparationState> state_;
};

void test_document_preparation_is_single_bootstrap_and_no_installer() {
  auto manifest = confirmed_manifest();
  auto body = synthetic_eula();
  bind_synthetic_eula(manifest, body);
  auto state = std::make_shared<PreparationState>();
  state->manifest = &manifest;
  PreparationPlatform platform(state);
  const auto extractor = [&body, &manifest](
                             const ytec::windowsapp::AdkPinnedPayload&,
                             const ytec::windowsapp::AdkVerifiedPayload&,
                             const ytec::windowsapp::AdkEmbeddedEulaPin&) {
    return ytec::clonecore::Result<
        ytec::windowsapp::AdkVerifiedEulaDocument>::success(
        ytec::windowsapp::AdkVerifiedEulaDocument{
            .receipt = receipt_for(manifest),
            .rtf_document = body,
        });
  };
  auto prepared = ytec::windowsapp::prepare_adk_consent_document(
      manifest,
      ytec::windowsapp::AdkConsentDocumentPreparationRequest{
          .source = ytec::windowsapp::AdkAcquisitionSource::official_download,
          .explicit_eula_retrieval_confirmed = true,
      },
      platform,
      extractor);
  check(
      prepared.has_value() && prepared.value().staging_removed &&
          state->create_count == 1U && state->download_count == 1U &&
          state->offline_count == 0U && state->installer_count == 0U &&
          state->cleanup_count == 1U,
      "official preparation must verify one bootstrap, remove staging, and never install");

  state->create_count = 0U;
  state->download_count = 0U;
  state->offline_count = 0U;
  state->cleanup_count = 0U;
  prepared = ytec::windowsapp::prepare_adk_consent_document(
      manifest,
      ytec::windowsapp::AdkConsentDocumentPreparationRequest{
          .source =
              ytec::windowsapp::AdkAcquisitionSource::official_offline_layout,
          .offline_layout_root = L"C:\\Synthetic\\OfficialAdkLayout",
          .explicit_eula_retrieval_confirmed = true,
      },
      platform,
      extractor);
  check(
      prepared.has_value() && state->create_count == 1U &&
          state->download_count == 0U && state->offline_count == 1U &&
          state->installer_count == 0U && state->cleanup_count == 1U,
      "offline preparation must read one fixed local bootstrap and never install");

  state->create_count = 0U;
  state->offline_count = 0U;
  state->cleanup_count = 0U;
  auto pending_manifest =
      ytec::windowsapp::tsumugi_1_0_0_adk_manifest();
  state->manifest = &pending_manifest;
  prepared = ytec::windowsapp::prepare_adk_consent_document(
      pending_manifest,
      ytec::windowsapp::AdkConsentDocumentPreparationRequest{
          .source = ytec::windowsapp::AdkAcquisitionSource::official_download,
          .explicit_eula_retrieval_confirmed = true,
      },
      platform,
      extractor);
  check(
      !prepared && state->create_count == 0U &&
          state->download_count == 0U && state->offline_count == 0U &&
          state->cleanup_count == 0U,
      "closed product gate must stop before every platform method");
}

}  // namespace

int main() {
  try {
    test_product_manifest_names_exact_eula_ui_blocker();
    test_generic_terms_and_missing_receipt_fail_closed();
    test_unverified_or_changed_receipt_cannot_reach_review();
    test_ready_review_contains_every_exact_source_and_component();
    test_consent_requires_full_same_review_and_explicit_acceptance();
    test_receipt_requires_matching_container_range_and_member();
    test_verified_document_requires_full_scroll_and_explicit_acceptance();
    test_document_preparation_is_single_bootstrap_and_no_installer();
    std::cout << "adk consent review tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "adk consent review tests: FAIL: "
              << failure.message << '\n';
    return 1;
  }
}

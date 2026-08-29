#pragma once

#include "ytec/windowsapp/adk_acquisition.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct AdkEulaDocumentReceipt final {
  AdkEmbeddedEulaPin extracted_identity;

  // The adapter first verifies the whole bootstrap using its release payload
  // URL, SHA-256, Authenticode signer and file version. It then reopens that
  // same owned file, copies only the pinned attached-CAB range, validates the
  // Burn UX mapping to the pinned member, and extracts that one member with
  // CREATE_NEW and bounded reads.
  bool bootstrap_full_identity_verified{};
  bool attached_container_bounds_verified{};
  bool burn_ux_mapping_verified{};
  bool member_copy_created_new{};
  bool member_copy_regular_file{};
  bool member_copy_single_link{};
  bool member_copy_reparse_point{};
  bool bounded_read_complete{};
  bool temporary_files_removed{};
};

enum class AdkConsentReviewIssue : std::uint8_t {
  blocked_manifest_missing,
  blocked_eula_source_unconfirmed,
  blocked_manifest_invalid,
  blocked_eula_receipt_unavailable,
  blocked_eula_receipt_unverified,
  blocked_eula_receipt_mismatch,
  review_required,
};

struct AdkConsentPayloadReview final {
  AdkPayloadKind kind{AdkPayloadKind::deployment_tools};
  std::wstring display_name;
  std::wstring exact_source_url;
  std::uint64_t expected_byte_count{};
  std::string expected_sha256;
  std::vector<std::wstring> acquired_components;
};

struct AdkConsentReviewView final {
  AdkConsentReviewIssue issue{
      AdkConsentReviewIssue::blocked_manifest_missing};
  bool ready_to_present{};
  bool consent_permitted{};
  std::string manifest_id;
  std::wstring product_release_version;
  std::wstring tested_adk_version;
  std::wstring information_url;
  AdkEmbeddedEulaPin embedded_eula;
  std::vector<AdkConsentPayloadReview> payloads;
  std::wstring message;
  std::wstring summary;
};

struct AdkConsentReviewAcknowledgement final {
  std::string reviewed_manifest_id;
  AdkEmbeddedEulaPin reviewed_embedded_eula;
  bool official_sources_presented{};
  bool acquired_components_presented{};
  bool eula_body_opened{};
  bool eula_body_fully_presented{};
  bool explicit_acceptance{};
};

// In-memory only result of the pre-consent bootstrap verification.  The
// Microsoft document is never added to the repository or product package and
// the owned staging area must already have been removed before this value can
// be presented.
struct AdkVerifiedEulaDocument final {
  AdkEulaDocumentReceipt receipt;
  std::vector<std::byte> rtf_document;
  bool staging_removed{};
};

struct AdkConsentDocumentPreparationRequest final {
  AdkAcquisitionSource source{AdkAcquisitionSource::official_download};
  std::filesystem::path offline_layout_root;

  // This is consent to retrieve or locally stage the exact pinned bootstrap
  // solely to display its ADK-specific EULA.  It is not EULA acceptance and
  // does not authorize an installer.
  bool explicit_eula_retrieval_confirmed{};
};

using AdkVerifiedEulaExtractor = std::function<
    clonecore::Result<AdkVerifiedEulaDocument>(
        const AdkPinnedPayload&,
        const AdkVerifiedPayload&,
        const AdkEmbeddedEulaPin&)>;

// Acquires only the pinned Deployment Tools bootstrap, verifies its complete
// identity, extracts the bounded EULA member through the supplied audited
// extractor, validates the in-memory document, and removes staging.  It never
// inspects or launches an installer.  The complete release gate is checked
// before a path or platform method is observed.
[[nodiscard]] clonecore::Result<AdkVerifiedEulaDocument>
prepare_adk_consent_document(
    const AdkReleaseManifest& manifest,
    const AdkConsentDocumentPreparationRequest& request,
    IAdkAcquisitionPlatform& platform,
    const AdkVerifiedEulaExtractor& extractor);

struct AdkConsentPresentationFacts final {
  bool official_sources_presented{};
  bool acquired_components_presented{};
  bool eula_body_opened{};
  bool eula_body_end_reached{};
  bool explicit_acceptance{};
};

struct AdkConsentDialogLayout final {
  int client_width{};
  int client_height{};
  int summary_left{};
  int summary_top{};
  int summary_width{};
  int summary_height{};
  int eula_left{};
  int eula_top{};
  int eula_width{};
  int eula_height{};
  int acceptance_left{};
  int acceptance_top{};
  int acceptance_width{};
  int acceptance_height{};
  int accept_left{};
  int accept_top{};
  int accept_width{};
  int accept_height{};
  int cancel_left{};
  int cancel_top{};
  int cancel_width{};
  int cancel_height{};
  bool bounded{};
};

// Validates that the exact verified RTF bytes, receipt, and removed staging
// still bind to the current manifest before the RichEdit product UI sees them.
[[nodiscard]] clonecore::Status validate_adk_eula_document_for_presentation(
    const AdkReleaseManifest& manifest,
    const AdkVerifiedEulaDocument& document);

[[nodiscard]] AdkConsentDialogLayout calculate_adk_consent_dialog_layout(
    int available_width,
    int available_height) noexcept;

// Converts only observed product-UI facts into the acknowledgement consumed
// by the existing consent contract.  Merely opening the dialog or checking a
// box before the end of the verified document never grants consent.
[[nodiscard]] clonecore::Result<AdkConsentReviewAcknowledgement>
complete_adk_consent_presentation(
    const AdkReleaseManifest& manifest,
    const AdkVerifiedEulaDocument& document,
    const AdkConsentPresentationFacts& facts);

// Produces display-ready, exact source/component rows only when both the
// release manifest and a bounded ADK-specific EULA receipt are verified.
// The bounded Windows extractor and product RichEdit review can produce and
// present the complete returned RTF. A missing receipt remains a hard stop.
[[nodiscard]] AdkConsentReviewView build_adk_consent_review(
    const AdkReleaseManifest* manifest,
    const AdkEulaDocumentReceipt* eula_receipt);

// Issues the exact AdkAcquisitionConsent consumed by execute_adk_acquisition.
// The caller must have displayed every source/component row and the complete
// verified EULA body, then received an explicit user acceptance in this same
// review. No I/O or installer launch occurs here.
[[nodiscard]] clonecore::Result<AdkAcquisitionConsent>
complete_adk_consent_review(
    const AdkReleaseManifest& manifest,
    const AdkEulaDocumentReceipt& eula_receipt,
    const AdkConsentReviewAcknowledgement& acknowledgement);

}  // namespace ytec::windowsapp

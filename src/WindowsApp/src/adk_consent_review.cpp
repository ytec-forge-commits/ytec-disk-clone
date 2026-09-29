#include "ytec/windowsapp/adk_consent_review.h"

#include "ytec/imageformat/sha256.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::windowsapp {
namespace {

AdkConsentReviewView issue_view(
    const AdkConsentReviewIssue issue,
    std::wstring message) {
  return AdkConsentReviewView{
      .issue = issue,
      .message = std::move(message),
  };
}

clonecore::Error consent_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = L"ADK利用条件レビュー",
      .message = std::move(message),
  };
}

std::wstring widen_ascii(const std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

bool eula_pin_is_declared(const AdkReleaseManifest& manifest) {
  const auto& eula = manifest.embedded_eula;
  return eula.primary_source_confirmed &&
         !eula.official_bootstrap_url.empty() &&
         eula.container_offset != 0U && eula.container_length != 0U &&
         !eula.container_member_name.empty() &&
         !eula.display_file_name.empty() &&
         !eula.expected_sha256.empty() &&
         eula.expected_byte_count != 0U &&
         !eula.expected_document_title.empty();
}

bool receipt_has_verified_provenance(
    const AdkEulaDocumentReceipt& receipt) noexcept {
  return receipt.bootstrap_full_identity_verified &&
         receipt.attached_container_bounds_verified &&
         receipt.burn_ux_mapping_verified &&
         receipt.member_copy_created_new &&
         receipt.member_copy_regular_file &&
         receipt.member_copy_single_link &&
         !receipt.member_copy_reparse_point &&
         receipt.bounded_read_complete &&
         receipt.temporary_files_removed;
}

bool receipt_matches_manifest(
    const AdkReleaseManifest& manifest,
    const AdkEulaDocumentReceipt& receipt) {
  return receipt.extracted_identity == manifest.embedded_eula;
}

bool local_absolute_path(const std::filesystem::path& path) {
  const std::wstring value = path.native();
  return value.size() >= 3U && value.size() < 32U * 1024U &&
         ((value[0] >= L'A' && value[0] <= L'Z') ||
          (value[0] >= L'a' && value[0] <= L'z')) &&
         value[1] == L':' &&
         (value[2] == L'\\' || value[2] == L'/') &&
         !value.starts_with(L"\\\\") && !value.starts_with(L"\\?") &&
         !value.starts_with(L"\\.");
}

bool paths_equal(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
  const std::wstring lhs = left.lexically_normal().native();
  const std::wstring rhs = right.lexically_normal().native();
  if (lhs.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
      rhs.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             lhs.data(),
             static_cast<int>(lhs.size()),
             rhs.data(),
             static_cast<int>(rhs.size()),
             TRUE) == CSTR_EQUAL;
}

std::string digest_hex(const imageformat::Sha256Digest& digest) {
  constexpr std::array<char, 16U> kHex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string result;
  result.reserve(digest.size() * 2U);
  for (const std::byte value : digest) {
    const auto byte = std::to_integer<unsigned int>(value);
    result.push_back(kHex[(byte >> 4U) & 0x0FU]);
    result.push_back(kHex[byte & 0x0FU]);
  }
  return result;
}

bool rtf_marker_present(const std::span<const std::byte> bytes) noexcept {
  constexpr std::array<unsigned char, 5U> kMarker{
      static_cast<unsigned char>('{'),
      static_cast<unsigned char>('\\'),
      static_cast<unsigned char>('r'),
      static_cast<unsigned char>('t'),
      static_cast<unsigned char>('f')};
  if (bytes.size() < kMarker.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < kMarker.size(); ++index) {
    if (std::to_integer<unsigned char>(bytes[index]) != kMarker[index]) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_document_identity(
    const AdkReleaseManifest& manifest,
    const AdkVerifiedEulaDocument& document) {
  const auto review = build_adk_consent_review(
      &manifest, &document.receipt);
  if (!review.ready_to_present ||
      review.issue != AdkConsentReviewIssue::review_required) {
    return clonecore::Status::failure(consent_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_REVISION_MISMATCH,
        review.message.empty()
            ? L"ADK固有EULAを製品画面へ安全に渡せません"
            : review.message));
  }
  if (document.rtf_document.size() !=
          manifest.embedded_eula.expected_byte_count ||
      !rtf_marker_present(document.rtf_document)) {
    return clonecore::Status::failure(consent_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK固有EULA本文の長さまたはRTF形式が固定値と一致しません"));
  }
  const auto digest = imageformat::sha256(document.rtf_document);
  if (!digest ||
      digest_hex(digest.value()) != manifest.embedded_eula.expected_sha256) {
    return digest
               ? clonecore::Status::failure(consent_error(
                     clonecore::ErrorCode::verification_failed,
                     ERROR_CRC,
                     L"ADK固有EULA本文のSHA-256が固定値と一致しません"))
               : clonecore::Status::failure(digest.error());
  }
  return clonecore::success_status();
}

class ConsentStagingCleanup final {
 public:
  ConsentStagingCleanup(
      IAdkAcquisitionPlatform& platform,
      const AdkStagingArea& staging) noexcept
      : platform_(&platform), staging_(&staging) {}

  ConsentStagingCleanup(const ConsentStagingCleanup&) = delete;
  ConsentStagingCleanup& operator=(const ConsentStagingCleanup&) = delete;

  ~ConsentStagingCleanup() {
    if (active_) {
      static_cast<void>(platform_->remove_staging_area(*staging_));
    }
  }

  void release() noexcept { active_ = false; }

 private:
  IAdkAcquisitionPlatform* platform_{};
  const AdkStagingArea* staging_{};
  bool active_{true};
};

clonecore::Status validate_prepared_receipt(
    const AdkPinnedPayload& payload,
    const AdkConsentDocumentPreparationRequest& request,
    const AdkStagingArea& staging,
    const AdkStagedPayloadReceipt& receipt) {
  const auto staged_path = staging.root / payload.staging_file_name;
  if (!receipt.created_new || !receipt.source_regular_file ||
      receipt.source_reparse_point ||
      receipt.byte_count != payload.expected_byte_count ||
      receipt.byte_count > payload.maximum_bytes ||
      !paths_equal(receipt.staged_path, staged_path)) {
    return clonecore::Status::failure(consent_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK固有EULA取得用bootstrapの新規保存、通常ファイル、長さ、または所有先を再確認できません"));
  }
  if (request.source == AdkAcquisitionSource::official_download) {
    if (!receipt.offline_source_path.empty() ||
        receipt.visited_urls.empty() ||
        receipt.visited_urls.front() != payload.exact_source_url ||
        receipt.effective_url != receipt.visited_urls.back() ||
        receipt.visited_urls.size() >
            payload.allowed_redirect_urls.size() + 1U) {
      return clonecore::Status::failure(consent_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_NAME,
          L"ADK固有EULA取得用bootstrapの取得URLが固定Microsoft URL列と一致しません"));
    }
    for (std::size_t index = 1U; index < receipt.visited_urls.size(); ++index) {
      if (receipt.visited_urls[index] !=
          payload.allowed_redirect_urls[index - 1U]) {
        return clonecore::Status::failure(consent_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_NAME,
            L"ADK固有EULA取得用bootstrapのリダイレクト順序が固定値と一致しません"));
      }
    }
  } else {
    const auto expected_source =
        request.offline_layout_root / payload.offline_relative_path;
    if (!receipt.visited_urls.empty() || !receipt.effective_url.empty() ||
        !paths_equal(receipt.offline_source_path, expected_source)) {
      return clonecore::Status::failure(consent_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK固有EULA取得用bootstrapが選択したオフラインレイアウトの固定位置と一致しません"));
    }
  }
  return clonecore::success_status();
}

std::wstring build_review_summary(
    const AdkReleaseManifest& manifest) {
  std::wostringstream summary;
  const auto& eula = manifest.embedded_eula;
  summary << L"Microsoft公式案内: " << manifest.information_url
          << L"\nADK: " << manifest.tested_adk_version
          << L"\nADK固有EULAを含むMicrosoft bootstrap: "
          << eula.official_bootstrap_url
          << L"\nEULA container offset: " << eula.container_offset
          << L" bytes\nEULA container length: "
          << eula.container_length << L" bytes"
          << L"\nEULA表示名: " << eula.display_file_name.native()
          << L"\nEULA container member: "
          << eula.container_member_name
          << L"\nEULA長さ: " << eula.expected_byte_count
          << L" bytes\nEULA SHA-256: "
          << widen_ascii(eula.expected_sha256)
          << L"\nEULA文書: " << eula.expected_document_title
          << L"\n\n取得内容:";
  for (const auto& payload : manifest.payloads) {
    summary << L"\n- " << payload.display_name
            << L"\n  公式取得元: " << payload.exact_source_url
            << L"\n  長さ: " << payload.expected_byte_count
            << L" bytes\n  SHA-256: "
            << widen_ascii(payload.expected_sha256);
    for (const auto& component : payload.acquired_components) {
      summary << L"\n  ・" << component;
    }
  }
  summary << L"\n\n利用条件を最後まで表示し、内容を確認した利用者が"
             L"明示的に同意した場合だけ導入へ進みます。";
  return summary.str();
}

}  // namespace

AdkConsentReviewView build_adk_consent_review(
    const AdkReleaseManifest* manifest,
    const AdkEulaDocumentReceipt* eula_receipt) {
  if (manifest == nullptr) {
    return issue_view(
        AdkConsentReviewIssue::blocked_manifest_missing,
        L"ADK固定マニフェストがないため、取得や導入へ進めません。");
  }
  if (!eula_pin_is_declared(*manifest)) {
    return issue_view(
        AdkConsentReviewIssue::blocked_eula_source_unconfirmed,
        L"Microsoft署名済みADK bootstrap内のADK固有EULAについて、"
        L"有界container範囲、member、本文の長さとSHA-256が未確認です。"
        L"汎用Microsoft規約や別製品の利用条件では代用しません。");
  }
  if (eula_receipt == nullptr) {
    return issue_view(
        AdkConsentReviewIssue::
            blocked_eula_receipt_unavailable,
        L"Microsoft署名・版・bootstrap全体SHA-256の検証後に、"
        L"固定CAB範囲とmemberだけを有界抽出したreceiptが未指定です。"
        L"抽出結果を製品のEULA全文レビューへ渡せないため、"
        L"取得・導入は停止中です。");
  }
  const auto manifest_status = validate_adk_release_manifest(*manifest);
  if (!manifest_status) {
    return issue_view(
        AdkConsentReviewIssue::blocked_manifest_invalid,
        L"ADK固定マニフェストが安全検証に合格しないため、"
        L"取得や導入へ進めません: " + manifest_status.error().message);
  }
  if (!receipt_has_verified_provenance(*eula_receipt)) {
    return issue_view(
        AdkConsentReviewIssue::blocked_eula_receipt_unverified,
        L"ADK bootstrap全体の署名・版・Hash、固定CAB範囲、"
        L"Burn UX mapping、非reparse新規member、または"
        L"有界完全読取りを証明できません。");
  }
  if (!receipt_matches_manifest(*manifest, *eula_receipt)) {
    return issue_view(
        AdkConsentReviewIssue::blocked_eula_receipt_mismatch,
        L"抽出したADK固有EULAのbootstrap、container範囲、member、"
        L"表示名、長さ、文書名、またはSHA-256が固定値と一致しません。");
  }

  AdkConsentReviewView view{
      .issue = AdkConsentReviewIssue::review_required,
      .ready_to_present = true,
      .consent_permitted = false,
      .manifest_id = manifest->manifest_id,
      .product_release_version = manifest->product_release_version,
      .tested_adk_version = manifest->tested_adk_version,
      .information_url = manifest->information_url,
      .embedded_eula = manifest->embedded_eula,
      .message =
          L"Microsoft公式取得元、取得内容、検証済みADK固有EULAを"
          L"確認してください。まだ取得・導入処理は開始していません。",
      .summary = build_review_summary(*manifest),
  };
  view.payloads.reserve(manifest->payloads.size());
  for (const auto& payload : manifest->payloads) {
    view.payloads.push_back(AdkConsentPayloadReview{
        .kind = payload.kind,
        .display_name = payload.display_name,
        .exact_source_url = payload.exact_source_url,
        .expected_byte_count = payload.expected_byte_count,
        .expected_sha256 = payload.expected_sha256,
        .acquired_components = payload.acquired_components,
    });
  }
  return view;
}

clonecore::Result<AdkVerifiedEulaDocument>
prepare_adk_consent_document(
    const AdkReleaseManifest& manifest,
    const AdkConsentDocumentPreparationRequest& request,
    IAdkAcquisitionPlatform& platform,
    const AdkVerifiedEulaExtractor& extractor) {
  // This must remain the first observable operation.  A pending product
  // manifest therefore cannot trigger even a folder read or staging call.
  const auto release_gate = validate_adk_release_manifest(manifest);
  if (!release_gate) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(
        release_gate.error());
  }
  if (!request.explicit_eula_retrieval_confirmed) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(consent_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"ADK固有EULAを表示するための固定bootstrap取得またはローカル読取りを、利用者が明示的に開始していません"));
  }
  if (!extractor) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(consent_error(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_FUNCTION,
        L"ADK固有EULAの有界抽出器が設定されていません"));
  }
  if (request.source == AdkAcquisitionSource::official_offline_layout) {
    if (!local_absolute_path(request.offline_layout_root)) {
      return clonecore::Result<AdkVerifiedEulaDocument>::failure(consent_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_NAME,
          L"ローカル絶対パスの検証済みADKオフラインレイアウトだけを選択できます"));
    }
  } else if (!request.offline_layout_root.empty()) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(consent_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Microsoft公式ダウンロードとオフラインレイアウトを同時指定できません"));
  }

  const auto payload = std::find_if(
      manifest.payloads.begin(),
      manifest.payloads.end(),
      [&](const AdkPinnedPayload& candidate) {
        return candidate.kind == manifest.embedded_eula.source_payload_kind;
      });
  if (payload == manifest.payloads.end() ||
      payload->installer_kind != AdkInstallerKind::microsoft_bootstrap_exe ||
      payload->exact_source_url != manifest.embedded_eula.official_bootstrap_url) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(consent_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_REVISION_MISMATCH,
        L"ADK固有EULAの取得元bootstrapが固定マニフェストと一意に対応しません"));
  }

  const auto staging = platform.create_new_staging_area(payload->maximum_bytes);
  if (!staging) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(staging.error());
  }
  ConsentStagingCleanup cleanup(platform, staging.value());
  if (!staging.value().created_new || staging.value().reparse_point ||
      !local_absolute_path(staging.value().root)) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(consent_error(
        clonecore::ErrorCode::verification_failed,
        staging.value().reparse_point ? ERROR_REPARSE_TAG_INVALID
                                      : ERROR_INVALID_DATA,
        L"ADK固有EULA取得用の新規ローカル非reparse一時領域を確認できません"));
  }

  const auto staged_path = staging.value().root / payload->staging_file_name;
  clonecore::Result<AdkStagedPayloadReceipt> acquired =
      clonecore::Result<AdkStagedPayloadReceipt>::failure(consent_error(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_FUNCTION,
          L"ADK固有EULA取得元が初期化されていません"));
  if (request.source == AdkAcquisitionSource::official_download) {
    std::vector<std::wstring> exact_urls{payload->exact_source_url};
    exact_urls.insert(
        exact_urls.end(),
        payload->allowed_redirect_urls.begin(),
        payload->allowed_redirect_urls.end());
    acquired = platform.download_to_new_file(AdkDownloadRequest{
        .exact_source_url = payload->exact_source_url,
        .exact_allowed_urls = std::move(exact_urls),
        .create_new_destination = staged_path,
        .maximum_bytes = payload->maximum_bytes,
    });
  } else {
    acquired = platform.stage_offline_payload(AdkOfflineStageRequest{
        .layout_root = request.offline_layout_root,
        .exact_relative_path = payload->offline_relative_path,
        .create_new_destination = staged_path,
        .maximum_bytes = payload->maximum_bytes,
    });
  }
  if (!acquired) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(
        acquired.error());
  }
  const auto receipt_status = validate_prepared_receipt(
      *payload, request, staging.value(), acquired.value());
  if (!receipt_status) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(
        receipt_status.error());
  }
  const auto hash = platform.sha256_file(staged_path, payload->maximum_bytes);
  if (!hash) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(hash.error());
  }
  if (hash.value() != payload->expected_sha256) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(consent_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"ADK固有EULA取得用bootstrapのSHA-256が固定値と一致しません"));
  }
  const auto signature = platform.verify_authenticode(
      staged_path, payload->expected_signer_subject);
  if (!signature) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(
        signature.error());
  }
  const auto version = platform.query_payload_version(staged_path);
  if (!version) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(
        version.error());
  }
  if (version.value() != payload->expected_payload_version) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(consent_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_REVISION_MISMATCH,
        L"ADK固有EULA取得用bootstrapの版が固定値と一致しません"));
  }
  const AdkVerifiedPayload verified{
      .kind = payload->kind,
      .installer_kind = payload->installer_kind,
      .staged_path = staged_path,
      .byte_count = acquired.value().byte_count,
      .sha256 = hash.value(),
      .signer_subject = payload->expected_signer_subject,
      .payload_version = version.value(),
  };
  auto document = extractor(*payload, verified, manifest.embedded_eula);
  if (!document) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(
        document.error());
  }
  const auto document_status = validate_document_identity(
      manifest, document.value());
  if (!document_status) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(
        document_status.error());
  }
  const auto removed = platform.remove_staging_area(staging.value());
  if (!removed) {
    return clonecore::Result<AdkVerifiedEulaDocument>::failure(
        removed.error());
  }
  cleanup.release();
  document.value().staging_removed = true;
  return document;
}

clonecore::Status validate_adk_eula_document_for_presentation(
    const AdkReleaseManifest& manifest,
    const AdkVerifiedEulaDocument& document) {
  const auto identity = validate_document_identity(manifest, document);
  if (!identity) {
    return identity;
  }
  if (!document.staging_removed) {
    return clonecore::Status::failure(consent_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_BUSY,
        L"ADK固有EULAを抽出した所有一時領域の削除を確認できないため、利用条件レビューを開始しません"));
  }
  return clonecore::success_status();
}

AdkConsentDialogLayout calculate_adk_consent_dialog_layout(
    const int available_width,
    const int available_height) noexcept {
  constexpr int kMinimumWidth = 640;
  constexpr int kMinimumHeight = 480;
  const int width = std::clamp(available_width, kMinimumWidth, 960);
  const int height = std::clamp(available_height, kMinimumHeight, 720);
  constexpr int margin = 16;
  constexpr int summary_height = 118;
  constexpr int gap = 10;
  constexpr int acceptance_height = 42;
  constexpr int button_width = 132;
  constexpr int button_height = 34;
  const int button_top = height - margin - button_height;
  const int acceptance_top = button_top - gap - acceptance_height;
  const int eula_top = margin + summary_height + gap;
  const int eula_height = acceptance_top - gap - eula_top;
  AdkConsentDialogLayout layout{
      .client_width = width,
      .client_height = height,
      .summary_left = margin,
      .summary_top = margin,
      .summary_width = width - margin * 2,
      .summary_height = summary_height,
      .eula_left = margin,
      .eula_top = eula_top,
      .eula_width = width - margin * 2,
      .eula_height = eula_height,
      .acceptance_left = margin,
      .acceptance_top = acceptance_top,
      .acceptance_width = width - margin * 2,
      .acceptance_height = acceptance_height,
      .accept_left = width - margin - button_width * 2 - gap,
      .accept_top = button_top,
      .accept_width = button_width,
      .accept_height = button_height,
      .cancel_left = width - margin - button_width,
      .cancel_top = button_top,
      .cancel_width = button_width,
      .cancel_height = button_height,
  };
  layout.bounded = available_width >= kMinimumWidth &&
                   available_height >= kMinimumHeight &&
                   layout.summary_left >= 0 && layout.summary_top >= 0 &&
                   layout.summary_width > 0 && layout.summary_height > 0 &&
                   layout.eula_top >
                       layout.summary_top + layout.summary_height &&
                   layout.eula_width > 0 && layout.eula_height >= 180 &&
                   layout.acceptance_top >
                       layout.eula_top + layout.eula_height &&
                   layout.accept_left >= margin &&
                   layout.cancel_left + layout.cancel_width <= width &&
                   layout.cancel_top + layout.cancel_height <= height;
  return layout;
}

clonecore::Result<AdkConsentReviewAcknowledgement>
complete_adk_consent_presentation(
    const AdkReleaseManifest& manifest,
    const AdkVerifiedEulaDocument& document,
    const AdkConsentPresentationFacts& facts) {
  const auto document_status =
      validate_adk_eula_document_for_presentation(manifest, document);
  if (!document_status) {
    return clonecore::Result<AdkConsentReviewAcknowledgement>::failure(
        document_status.error());
  }
  AdkConsentReviewAcknowledgement acknowledgement{
      .reviewed_manifest_id = manifest.manifest_id,
      .reviewed_embedded_eula = manifest.embedded_eula,
      .official_sources_presented = facts.official_sources_presented,
      .acquired_components_presented =
          facts.acquired_components_presented,
      .eula_body_opened = facts.eula_body_opened,
      .eula_body_fully_presented = facts.eula_body_end_reached,
      .explicit_acceptance = facts.explicit_acceptance,
  };
  const auto consent = complete_adk_consent_review(
      manifest, document.receipt, acknowledgement);
  if (!consent) {
    return clonecore::Result<AdkConsentReviewAcknowledgement>::failure(
        consent.error());
  }
  return clonecore::Result<AdkConsentReviewAcknowledgement>::success(
      std::move(acknowledgement));
}

clonecore::Result<AdkAcquisitionConsent>
complete_adk_consent_review(
    const AdkReleaseManifest& manifest,
    const AdkEulaDocumentReceipt& eula_receipt,
    const AdkConsentReviewAcknowledgement& acknowledgement) {
  const auto review = build_adk_consent_review(
      &manifest, &eula_receipt);
  if (!review.ready_to_present ||
      review.issue != AdkConsentReviewIssue::review_required) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_REVISION_MISMATCH,
            review.message.empty()
                ? L"ADK利用条件レビューを安全に開始できません"
                : review.message));
  }
  if (acknowledgement.reviewed_manifest_id != manifest.manifest_id ||
      acknowledgement.reviewed_embedded_eula !=
          manifest.embedded_eula) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_REVISION_MISMATCH,
            L"画面で確認したマニフェストまたはADK固有EULAが現在の固定値と一致しません"));
  }
  if (!acknowledgement.official_sources_presented ||
      !acknowledgement.acquired_components_presented) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_CANCELLED,
            L"Microsoft公式取得元と取得内容をすべて表示してから確認してください"));
  }
  if (!acknowledgement.eula_body_opened ||
      !acknowledgement.eula_body_fully_presented) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_CANCELLED,
            L"検証済みADK固有EULA本文を最後まで表示してから確認してください"));
  }
  if (!acknowledgement.explicit_acceptance) {
    return clonecore::Result<AdkAcquisitionConsent>::failure(
        consent_error(
            clonecore::ErrorCode::confirmation_required,
            ERROR_CANCELLED,
            L"利用者による明示的な同意がないため、ADK導入へ進みません"));
  }

  AdkAcquisitionConsent consent{
      .accepted = true,
      .presented_manifest_id = manifest.manifest_id,
      .presented_embedded_eula = manifest.embedded_eula,
  };
  consent.presented_payloads.reserve(manifest.payloads.size());
  for (const auto& payload : manifest.payloads) {
    consent.presented_payloads.push_back(payload.kind);
  }
  return clonecore::Result<AdkAcquisitionConsent>::success(
      std::move(consent));
}

}  // namespace ytec::windowsapp

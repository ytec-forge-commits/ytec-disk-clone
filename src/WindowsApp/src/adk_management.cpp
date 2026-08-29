#include "ytec/windowsapp/adk_management.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace ytec::windowsapp {
namespace {

constexpr std::string_view kManagedRecordHeader =
    "TSUMUGI_ADK_MANAGED_V1\n";
constexpr std::string_view kManagedRecordInstalled = "installed=1\n";
constexpr std::string_view kManagedRecordEnd = "end=1\n";
constexpr std::size_t kMaximumManagedRegistrationCount = 16U;
constexpr std::uint64_t kMaximumOfflineLayoutBytes =
    16ULL * 1024ULL * 1024ULL * 1024ULL;

clonecore::Error management_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(management_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(management_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

std::wstring widen_ascii(const std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

bool safe_record_ascii(const std::string_view value) noexcept {
  return !value.empty() && value.size() <= 128U &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') ||
                  character == '-' || character == '_' ||
                  character == '.';
         });
}

bool ascii_hex_digit(const wchar_t character) noexcept {
  return (character >= L'0' && character <= L'9') ||
         (character >= L'a' && character <= L'f') ||
         (character >= L'A' && character <= L'F');
}

bool strict_braced_guid(const std::wstring_view value) noexcept {
  if (value.size() != 38U || value.front() != L'{' ||
      value.back() != L'}') {
    return false;
  }
  for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
    const bool separator =
        index == 9U || index == 14U || index == 19U || index == 24U;
    if (separator ? value[index] != L'-'
                  : !ascii_hex_digit(value[index])) {
      return false;
    }
  }
  return true;
}

bool safe_registration_id(const std::wstring_view value) noexcept {
  constexpr std::wstring_view kMsiPrefix = L"MSI|";
  constexpr std::wstring_view kMspPrefix = L"MSP|";
  if (value.starts_with(kMsiPrefix)) {
    return strict_braced_guid(value.substr(kMsiPrefix.size()));
  }
  if (!value.starts_with(kMspPrefix)) {
    return false;
  }
  const std::wstring_view identities = value.substr(kMspPrefix.size());
  const std::size_t separator = identities.find(L'|');
  return separator != std::wstring_view::npos &&
         identities.find(L'|', separator + 1U) == std::wstring_view::npos &&
         strict_braced_guid(identities.substr(0U, separator)) &&
         strict_braced_guid(identities.substr(separator + 1U));
}

std::optional<std::string> narrow_registration_id(
    const std::wstring_view value) {
  if (!safe_registration_id(value)) {
    return std::nullopt;
  }
  std::string result;
  result.reserve(value.size());
  for (const wchar_t character : value) {
    if (character > 0x7FU) {
      return std::nullopt;
    }
    result.push_back(static_cast<char>(character));
  }
  return result;
}

bool is_local_absolute_path(const std::filesystem::path& path) {
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

bool url_matches_payload(
    const AdkPinnedPayload& payload,
    const std::wstring_view candidate) {
  if (candidate == payload.exact_source_url) {
    return true;
  }
  return std::find(
             payload.allowed_redirect_urls.begin(),
             payload.allowed_redirect_urls.end(),
             candidate) != payload.allowed_redirect_urls.end();
}

clonecore::Status validate_download_receipt(
    const AdkPinnedPayload& payload,
    const AdkStagingArea& staging,
    const AdkStagedPayloadReceipt& receipt) {
  const auto expected_path = staging.root / payload.staging_file_name;
  if (!receipt.created_new || !receipt.source_regular_file ||
      receipt.source_reparse_point || receipt.byte_count == 0U ||
      receipt.byte_count != payload.expected_byte_count ||
      receipt.byte_count > payload.maximum_bytes ||
      !paths_equal(receipt.staged_path, expected_path) ||
      !receipt.offline_source_path.empty()) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADKオフラインレイアウト取得物検証",
        L"CREATE_NEW、通常ファイル、長さ、または所有一時領域の証跡が一致しません");
  }
  if (receipt.visited_urls.empty() ||
      receipt.visited_urls.front() != payload.exact_source_url ||
      receipt.effective_url != receipt.visited_urls.back() ||
      !std::all_of(
          receipt.visited_urls.begin(),
          receipt.visited_urls.end(),
          [&](const std::wstring& url) {
            return url_matches_payload(payload, url);
          })) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        L"ADKオフラインレイアウト公式URL検証",
        L"取得元またはリダイレクトが固定Microsoft公式URLと一致しません");
  }
  return clonecore::success_status();
}

clonecore::Result<AdkVerifiedPayload> verify_layout_payload(
    const AdkPinnedPayload& payload,
    const AdkStagedPayloadReceipt& receipt,
    IAdkAcquisitionPlatform& acquisition) {
  const auto hash = acquisition.sha256_file(
      receipt.staged_path, payload.maximum_bytes);
  if (!hash) {
    return clonecore::Result<AdkVerifiedPayload>::failure(hash.error());
  }
  if (hash.value() != payload.expected_sha256) {
    return failure<AdkVerifiedPayload>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"ADKオフラインレイアウトSHA-256検証",
        L"取得物のSHA-256が固定値と一致しません");
  }

  AdkVerifiedPayload verified{
      .kind = payload.kind,
      .installer_kind = payload.installer_kind,
      .staged_path = receipt.staged_path,
      .byte_count = receipt.byte_count,
      .sha256 = hash.value(),
  };
  if (payload.installer_kind ==
      AdkInstallerKind::windows_installer_patch_archive_zip) {
    const auto expanded = acquisition.expand_and_verify_patch_archive(
        AdkPatchArchiveExpandRequest{
            .archive = verified,
            .members = payload.patch_members,
        });
    if (!expanded) {
      return clonecore::Result<AdkVerifiedPayload>::failure(
          expanded.error());
    }
    if (expanded.value().size() != payload.patch_members.size()) {
      return failure<AdkVerifiedPayload>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADKオフラインレイアウト更新archive検証",
          L"固定MSP memberをすべて完全検証できません");
    }
    return clonecore::Result<AdkVerifiedPayload>::success(
        std::move(verified));
  }

  const auto trust = acquisition.verify_authenticode(
      receipt.staged_path, payload.expected_signer_subject);
  if (!trust) {
    return clonecore::Result<AdkVerifiedPayload>::failure(trust.error());
  }
  const auto version = acquisition.query_payload_version(receipt.staged_path);
  if (!version) {
    return clonecore::Result<AdkVerifiedPayload>::failure(version.error());
  }
  if (version.value() != payload.expected_payload_version) {
    return failure<AdkVerifiedPayload>(
        clonecore::ErrorCode::verification_failed,
        ERROR_REVISION_MISMATCH,
        L"ADKオフラインレイアウト版検証",
        L"取得物の版が固定値と一致しません");
  }
  verified.signer_subject = payload.expected_signer_subject;
  verified.payload_version = version.value();
  return clonecore::Result<AdkVerifiedPayload>::success(
      std::move(verified));
}

class StagingCleanup final {
 public:
  StagingCleanup(
      IAdkAcquisitionPlatform& acquisition,
      const AdkStagingArea& staging)
      : acquisition_(&acquisition), staging_(&staging) {}

  StagingCleanup(const StagingCleanup&) = delete;
  StagingCleanup& operator=(const StagingCleanup&) = delete;

  ~StagingCleanup() {
    if (active_) {
      static_cast<void>(acquisition_->remove_staging_area(*staging_));
    }
  }

  [[nodiscard]] clonecore::Status remove_now() {
    const auto status = acquisition_->remove_staging_area(*staging_);
    if (status) {
      active_ = false;
    }
    return status;
  }

 private:
  IAdkAcquisitionPlatform* acquisition_{};
  const AdkStagingArea* staging_{};
  bool active_{true};
};

clonecore::Result<AdkOfflineLayoutReport> create_offline_layout(
    const AdkReleaseManifest& manifest,
    const std::filesystem::path& layout_root,
    IAdkAcquisitionPlatform& acquisition,
    IAdkManagementPlatform& management) {
  if (!is_local_absolute_path(layout_root)) {
    return failure<AdkOfflineLayoutReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"ADKオフラインレイアウト新規保存先",
        L"ローカル絶対パスの新しいフォルダーだけを指定できます");
  }
  std::uint64_t maximum_total{};
  for (const auto& payload : manifest.payloads) {
    if (payload.maximum_bytes > kMaximumOfflineLayoutBytes - maximum_total) {
      return failure<AdkOfflineLayoutReport>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"ADKオフラインレイアウト上限計算",
          L"固定取得物の合計上限が製品境界を超えます");
    }
    maximum_total += payload.maximum_bytes;
  }
  const auto staging = acquisition.create_new_staging_area(maximum_total);
  if (!staging) {
    return clonecore::Result<AdkOfflineLayoutReport>::failure(
        staging.error());
  }
  if (!staging.value().created_new || staging.value().reparse_point ||
      !is_local_absolute_path(staging.value().root)) {
    static_cast<void>(
        acquisition.remove_staging_area(staging.value()));
    return failure<AdkOfflineLayoutReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_REPARSE_TAG_INVALID,
        L"ADKオフラインレイアウト一時領域",
        L"新規作成したローカル非reparse一時領域ではありません");
  }
  StagingCleanup cleanup(acquisition, staging.value());
  std::vector<AdkVerifiedPayload> verified_payloads;
  verified_payloads.reserve(manifest.payloads.size());
  for (const auto& payload : manifest.payloads) {
    const auto receipt = acquisition.download_to_new_file(
        AdkDownloadRequest{
            .exact_source_url = payload.exact_source_url,
            .exact_allowed_urls = payload.allowed_redirect_urls,
            .create_new_destination =
                staging.value().root / payload.staging_file_name,
            .maximum_bytes = payload.maximum_bytes,
        });
    if (!receipt) {
      return clonecore::Result<AdkOfflineLayoutReport>::failure(
          receipt.error());
    }
    const auto receipt_status = validate_download_receipt(
        payload, staging.value(), receipt.value());
    if (!receipt_status) {
      return clonecore::Result<AdkOfflineLayoutReport>::failure(
          receipt_status.error());
    }
    auto verified = verify_layout_payload(
        payload, receipt.value(), acquisition);
    if (!verified) {
      return clonecore::Result<AdkOfflineLayoutReport>::failure(
          verified.error());
    }
    verified_payloads.push_back(verified.take_value());
  }

  const auto begun = management.begin_new_offline_layout(
      layout_root, manifest.manifest_id);
  if (!begun) {
    return clonecore::Result<AdkOfflineLayoutReport>::failure(
        begun.error());
  }
  bool layout_active = true;
  for (std::size_t index = 0U;
       index < manifest.payloads.size();
       ++index) {
    const auto published = management.publish_offline_layout_payload(
        manifest.payloads[index], verified_payloads[index]);
    if (!published) {
      static_cast<void>(management.abandon_offline_layout());
      layout_active = false;
      return clonecore::Result<AdkOfflineLayoutReport>::failure(
          published.error());
    }
  }
  auto finalized = management.finalize_offline_layout(manifest);
  if (!finalized) {
    if (layout_active) {
      static_cast<void>(management.abandon_offline_layout());
    }
    return clonecore::Result<AdkOfflineLayoutReport>::failure(
        finalized.error());
  }
  layout_active = false;
  const auto removed = cleanup.remove_now();
  if (!removed) {
    return clonecore::Result<AdkOfflineLayoutReport>::failure(
        removed.error());
  }
  return finalized;
}

clonecore::Result<AdkAcquisitionConsent> require_consent(
    const AdkReleaseManifest& manifest,
    const AdkManagementRequest& request) {
  if (!request.eula_receipt.has_value() ||
      !request.consent_acknowledgement.has_value()) {
    return failure<AdkAcquisitionConsent>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"ADK取得・導入 利用条件レビュー",
        L"検証済みADK固有EULA全文、Microsoft公式取得元、取得内容を同じ画面で確認し、明示同意する必要があります");
  }
  return complete_adk_consent_review(
      manifest,
      request.eula_receipt.value(),
      request.consent_acknowledgement.value());
}

clonecore::Status validate_management_request_after_release_gate(
    const AdkManagementRequest& request) {
  if (!request.explicit_start_confirmed) {
    return status_failure(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"ADK取得・管理 明示開始",
        L"事前要約を確認した利用者による明示開始が必要です");
  }
  if (!request.administrator) {
    return status_failure(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"ADK取得・管理 管理者確認",
        L"ADKの導入・削除・管理記録は管理者として起動した製品からだけ実行できます");
  }
  return clonecore::success_status();
}

}  // namespace

AdkManagementView build_adk_management_view(
    const AdkReleaseManifest& manifest) {
  AdkManagementView view{
      .title = L"ADK取得・管理",
  };
  const auto structure =
      validate_adk_release_manifest_structure(manifest);
  const auto valid = validate_adk_release_manifest(manifest);
  view.manifest_structure_valid = static_cast<bool>(structure);
  view.primary_source_pins_confirmed =
      manifest.primary_source_pins_confirmed;
  view.unattended_no_restart_confirmed =
      manifest.unattended_install_no_unexpected_restart_confirmed;
  view.execution_gate_open = static_cast<bool>(valid);
  view.platform_creation_permitted = view.execution_gate_open;
  view.path_selection_permitted = view.execution_gate_open;
  if (valid) {
    view.status =
        L"固定Microsoft公式取得物と無予期再起動なしのリリース証跡を確認済みです。";
  } else {
    view.status =
        L"安全ゲート停止中 — 通信・フォルダー参照・UAC・installerは開始しません。";
  }
  std::wostringstream summary;
  summary << L"対象ADK: " << manifest.tested_adk_version
          << L"\n固定取得物: " << manifest.payloads.size()
          << L"件\n取得元: Microsoft固定HTTPS URL または検証済みローカルlayout"
          << L"\n一次資料pin: "
          << (manifest.primary_source_pins_confirmed ? L"確認済み"
                                                    : L"未確認（停止）")
          << L"\nquiet導入の予期しない再起動なし: "
          << (manifest.unattended_install_no_unexpected_restart_confirmed
                  ? L"確認済み"
                  : L"未確認（停止）")
          << L"\n既存ADK: 自動削除しない"
          << L"\n削除対象: Tsumugiが同じmanifestで導入し、固定記録が残るものだけ";
  if (!valid) {
    summary << L"\n\n停止理由: " << valid.error().message;
  }
  view.summary = summary.str();
  view.payload_rows.reserve(manifest.payloads.size());
  if (structure) {
    for (const auto& payload : manifest.payloads) {
      view.payload_rows.push_back(
          payload.display_name + L"\n  URL: " + payload.exact_source_url +
          L"\n  offline: " + payload.offline_relative_path.native() +
          L"\n  signer: " + payload.expected_signer_subject +
          L"\n  version: " + payload.expected_payload_version +
          L"\n  length: " +
          std::to_wstring(payload.expected_byte_count) +
          L" bytes\n  SHA-256: " +
          widen_ascii(payload.expected_sha256));
    }
  }
  return view;
}

AdkManagementActionReview build_adk_management_action_review(
    const AdkReleaseManifest& manifest,
    const AdkManagementAction action) {
  const auto structure =
      validate_adk_release_manifest_structure(manifest);
  const auto release = validate_adk_release_manifest(manifest);
  AdkManagementActionReview review{
      .action = action,
      .manifest_structure_valid = static_cast<bool>(structure),
      .execution_gate_open = static_cast<bool>(release),
  };
  switch (action) {
    case AdkManagementAction::official_download_install:
      review.requires_eula_review = true;
      review.requires_network_retrieval = true;
      review.title = L"Microsoft公式取得物からADKを導入";
      review.summary =
          L"固定bootstrapからADK固有EULA全文を検証表示し、明示同意後だけ取得・導入します。";
      break;
    case AdkManagementAction::offline_layout_install:
      review.path_selection =
          AdkManagementPathSelection::existing_offline_layout;
      review.requires_eula_review = true;
      review.title = L"検証済みオフラインレイアウトからADKを導入";
      review.summary =
          L"利用者が選んだ既存ローカルlayoutの固定位置だけを読み、ADK固有EULA全文への明示同意後に導入します。";
      break;
    case AdkManagementAction::create_offline_layout:
      review.path_selection =
          AdkManagementPathSelection::new_offline_layout_parent;
      review.requires_eula_review = true;
      review.requires_network_retrieval = true;
      review.title = L"公式ADKオフラインレイアウトを新規作成";
      review.summary =
          L"利用者が選んだ既存ローカル親フォルダー直下へ固定名の新規layoutを作成し、既存先は上書きしません。";
      break;
    case AdkManagementAction::uninstall_managed:
      review.requires_explicit_uninstall_confirmation = true;
      review.title = L"Tsumugi管理対象ADKを削除";
      review.summary =
          L"同じmanifestの固定管理記録にあるMSI/MSP登録だけを対象とし、既存・管理外ADKは削除しません。";
      break;
    default:
      review.title = L"未対応のADK管理操作";
      review.summary = L"固定された製品操作ではありません。";
      review.execution_gate_open = false;
      break;
  }
  review.path_picker_permitted =
      review.execution_gate_open &&
      review.path_selection != AdkManagementPathSelection::none;
  if (!release) {
    review.summary += L"\n\n安全ゲート停止理由: " +
                      release.error().message;
  }
  return review;
}

clonecore::Result<std::filesystem::path>
make_adk_offline_layout_destination(
    const AdkReleaseManifest& manifest,
    const std::filesystem::path& selected_parent) {
  const auto release = validate_adk_release_manifest(manifest);
  if (!release) {
    return clonecore::Result<std::filesystem::path>::failure(
        release.error());
  }
  if (!is_local_absolute_path(selected_parent)) {
    return failure<std::filesystem::path>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"ADKオフラインレイアウト 親フォルダー",
        L"既存のローカル絶対親フォルダーだけを選択できます");
  }
  const std::filesystem::path child =
      selected_parent /
      (L"Tsumugi-ADK-Offline-" + manifest.tested_adk_version);
  if (!is_local_absolute_path(child) ||
      child.parent_path().lexically_normal() !=
          selected_parent.lexically_normal()) {
    return failure<std::filesystem::path>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_NAME,
        L"ADKオフラインレイアウト 固定保存先",
        L"選択した親直下の固定新規保存先を構成できません");
  }
  return clonecore::Result<std::filesystem::path>::success(child);
}

clonecore::Result<std::wstring> format_adk_evidence_event(
    const AdkReleaseManifest& manifest,
    const AdkEvidenceFacts& facts) {
  const auto structure =
      validate_adk_release_manifest_structure(manifest);
  if (!structure) {
    return clonecore::Result<std::wstring>::failure(structure.error());
  }
  constexpr std::array<std::wstring_view, 4U> kActionNames{
      L"official-install", L"offline-install", L"offline-export",
      L"managed-uninstall"};
  constexpr std::array<std::wstring_view, 9U> kStageNames{
      L"review-opened", L"gate-blocked", L"path-selected",
      L"eula-retrieval-started", L"eula-verified",
      L"consent-accepted", L"action-started", L"action-succeeded",
      L"action-failed"};
  const auto action_index = static_cast<std::size_t>(facts.action);
  const auto stage_index = static_cast<std::size_t>(facts.stage);
  if (action_index >= kActionNames.size() ||
      stage_index >= kStageNames.size()) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"ADK管理 証拠ログ",
        L"固定されていないactionまたはstageです");
  }
  std::wostringstream text;
  text << L"ADK_EVIDENCE manifest=" << widen_ascii(manifest.manifest_id)
       << L" action=" << kActionNames[action_index]
       << L" stage=" << kStageNames[stage_index]
       << L" payloads=" << manifest.payloads.size()
       << L" pins_confirmed="
       << (manifest.primary_source_pins_confirmed ? 1 : 0)
       << L" no_restart_confirmed="
       << (manifest.unattended_install_no_unexpected_restart_confirmed ? 1
                                                                        : 0)
       << L" path_selected=" << (facts.path_selected ? 1 : 0)
       << L" full_eula=" << (facts.complete_eula_presented ? 1 : 0)
       << L" explicit_consent=" << (facts.explicit_consent ? 1 : 0)
       << L" native=" << facts.native_code;
  std::wstring result = text.str();
  if (result.size() > 1'024U) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"ADK管理 証拠ログ",
        L"証拠ログが固定長上限を超えます");
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

AdkManagementLayout calculate_adk_management_layout(
    const int available_width,
    const int available_height) noexcept {
  constexpr int kMinimumWidth = 640;
  constexpr int kMinimumHeight = 420;
  const int width = std::clamp(available_width, kMinimumWidth, 960);
  const int height = std::clamp(available_height, kMinimumHeight, 516);
  constexpr int margin = 18;
  constexpr int command_height = 36;
  constexpr int command_gap = 8;
  constexpr int cancel_width = 132;
  constexpr int cancel_height = 34;
  const int commands_total = command_height * 4 + command_gap * 3;
  const int cancel_top = height - margin - cancel_height;
  const int command_top = cancel_top - 14 - commands_total;
  const int summary_top = margin;
  const int summary_height = std::max(96, command_top - summary_top - 14);
  AdkManagementLayout layout{
      .client_width = width,
      .client_height = height,
      .summary_left = margin,
      .summary_top = summary_top,
      .summary_width = width - margin * 2,
      .summary_height = summary_height,
      .command_left = margin,
      .command_top = command_top,
      .command_width = width - margin * 2,
      .command_height = command_height,
      .command_gap = command_gap,
      .cancel_left = width - margin - cancel_width,
      .cancel_top = cancel_top,
      .cancel_width = cancel_width,
      .cancel_height = cancel_height,
  };
  const int last_command_bottom =
      layout.command_top + layout.command_height * 4 +
      layout.command_gap * 3;
  layout.bounded = available_width >= kMinimumWidth &&
                   available_height >= kMinimumHeight &&
                   layout.summary_left >= 0 && layout.summary_top >= 0 &&
                   layout.summary_width > 0 && layout.summary_height > 0 &&
                   layout.command_top >
                       layout.summary_top + layout.summary_height &&
                   last_command_bottom < layout.cancel_top &&
                   layout.cancel_left >= margin &&
                   layout.cancel_top + layout.cancel_height <= height;
  return layout;
}

clonecore::Result<std::vector<std::byte>> serialize_managed_adk_record(
    const AdkManagedInstallationRecord& record) {
  if (!record.installed_by_tsumugi ||
      !safe_record_ascii(record.manifest_id) ||
      record.installed_registration_ids.empty() ||
      record.installed_registration_ids.size() >
          kMaximumManagedRegistrationCount) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"ADK管理記録 固定schema生成",
        L"Tsumugi導入証跡、manifest識別子、または登録識別子件数が不正です");
  }
  std::set<std::wstring> unique;
  std::string text(kManagedRecordHeader);
  text.append("manifest=").append(record.manifest_id).push_back('\n');
  text.append(kManagedRecordInstalled);
  for (const auto& registration : record.installed_registration_ids) {
    const auto ascii = narrow_registration_id(registration);
    if (!ascii.has_value() || !unique.insert(registration).second) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"ADK管理記録 登録識別子",
          L"登録識別子は重複のない有界ASCII固定識別子である必要があります");
    }
    text.append("registration=").append(ascii.value()).push_back('\n');
  }
  text.append(kManagedRecordEnd);
  if (text.size() > kMaximumManagedAdkRecordBytes) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_TOO_LARGE,
        L"ADK管理記録 長さ上限",
        L"固定schemaの管理記録が上限を超えます");
  }
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char character : text) {
    bytes.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(character)));
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

clonecore::Result<AdkManagedInstallationRecord> parse_managed_adk_record(
    const std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > kMaximumManagedAdkRecordBytes) {
    return failure<AdkManagedInstallationRecord>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ADK管理記録 固定schema読取り",
        L"管理記録の長さが不正です");
  }
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte value : bytes) {
    const auto character = std::to_integer<unsigned char>(value);
    if (character < 0x20U && character != '\n') {
      return failure<AdkManagedInstallationRecord>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK管理記録 ASCII検証",
          L"固定schema外の制御文字を含みます");
    }
    if (character > 0x7EU) {
      return failure<AdkManagedInstallationRecord>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK管理記録 ASCII検証",
          L"固定ASCII schemaではありません");
    }
    text.push_back(static_cast<char>(character));
  }
  if (!text.starts_with(kManagedRecordHeader) ||
      !text.ends_with(kManagedRecordEnd)) {
    return failure<AdkManagedInstallationRecord>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ADK管理記録 schema識別",
        L"version headerまたは終端が一致しません");
  }
  std::vector<std::string> lines;
  std::size_t cursor{};
  while (cursor < text.size()) {
    const auto end = text.find('\n', cursor);
    if (end == std::string::npos) {
      return failure<AdkManagedInstallationRecord>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK管理記録 行終端",
          L"各fieldはLFで終端する必要があります");
    }
    lines.push_back(text.substr(cursor, end - cursor));
    cursor = end + 1U;
  }
  if (lines.size() < 5U ||
      lines.front() != "TSUMUGI_ADK_MANAGED_V1" ||
      !lines[1].starts_with("manifest=") ||
      lines[2] != "installed=1" || lines.back() != "end=1") {
    return failure<AdkManagedInstallationRecord>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ADK管理記録 field順序",
        L"固定fieldの欠落、重複、または順序違反があります");
  }
  AdkManagedInstallationRecord record{
      .manifest_id = lines[1].substr(std::string("manifest=").size()),
      .installed_by_tsumugi = true,
  };
  if (!safe_record_ascii(record.manifest_id)) {
    return failure<AdkManagedInstallationRecord>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ADK管理記録 manifest識別子",
        L"manifest識別子が固定ASCII境界外です");
  }
  std::set<std::wstring> unique;
  for (std::size_t index = 3U; index + 1U < lines.size(); ++index) {
    constexpr std::string_view prefix = "registration=";
    if (!lines[index].starts_with(prefix)) {
      return failure<AdkManagedInstallationRecord>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK管理記録 未知field",
          L"固定schemaにないfieldがあります");
    }
    const std::string value = lines[index].substr(prefix.size());
    const std::wstring registration(value.begin(), value.end());
    if (!safe_registration_id(registration) ||
        !unique.insert(registration).second) {
      return failure<AdkManagedInstallationRecord>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"ADK管理記録 登録識別子",
          L"登録識別子が不正または重複しています");
    }
    record.installed_registration_ids.push_back(registration);
  }
  if (record.installed_registration_ids.empty() ||
      record.installed_registration_ids.size() >
          kMaximumManagedRegistrationCount) {
    return failure<AdkManagedInstallationRecord>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"ADK管理記録 登録件数",
        L"登録識別子の件数が固定境界外です");
  }
  return clonecore::Result<AdkManagedInstallationRecord>::success(
      std::move(record));
}

clonecore::Result<AdkManagementReport> execute_adk_management_action(
    const AdkReleaseManifest& manifest,
    const AdkManagementRequest& request,
    const AdkManagementDependencies& dependencies) {
  // Keep this as the first observable operation. Tests deliberately pass an
  // invalid/UNC path and counting factories with the production closed gate.
  const auto release_gate = validate_adk_release_manifest(manifest);
  if (!release_gate) {
    return clonecore::Result<AdkManagementReport>::failure(
        release_gate.error());
  }
  if (!std::all_of(
          manifest.payloads.begin(),
          manifest.payloads.end(),
          [](const AdkPinnedPayload& payload) {
            return safe_registration_id(payload.uninstall_registration_id);
          })) {
    return failure<AdkManagementReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_NOT_SUPPORTED,
        L"ADK管理対象 登録識別子固定",
        L"監査済みMSI|{GUID}またはMSP|{PATCH}|{PRODUCT}形式ではないため、取得・導入を開始しません");
  }
  const auto request_status =
      validate_management_request_after_release_gate(request);
  if (!request_status) {
    return clonecore::Result<AdkManagementReport>::failure(
        request_status.error());
  }
  if (!dependencies.make_management_platform) {
    return failure<AdkManagementReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_FUNCTION,
        L"ADK取得・管理 platform factory",
        L"管理platform factoryが設定されていません");
  }

  if (request.action == AdkManagementAction::uninstall_managed) {
    auto management = dependencies.make_management_platform();
    if (!management) {
      return failure<AdkManagementReport>(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"ADK管理対象削除 platform生成",
          L"Windows管理platformを生成できません");
    }
    const auto stored = management->load_managed_installation_record();
    if (!stored) {
      return clonecore::Result<AdkManagementReport>::failure(
          stored.error());
    }
    if (!stored.value().has_value()) {
      return failure<AdkManagementReport>(
          clonecore::ErrorCode::access_denied,
          ERROR_NOT_FOUND,
          L"ADK管理対象削除 記録確認",
          L"Tsumugiが導入した同じmanifestの固定管理記録がないため、既存ADKは削除しません");
    }
    const auto plan = build_managed_adk_uninstall_plan(
        manifest, stored.value().value());
    if (!plan) {
      return clonecore::Result<AdkManagementReport>::failure(plan.error());
    }
    auto exit_codes = management->execute_managed_uninstall(plan.value());
    if (!exit_codes) {
      return clonecore::Result<AdkManagementReport>::failure(
          exit_codes.error());
    }
    const auto removed =
        management->remove_managed_installation_record_if_exact(
            stored.value().value());
    if (!removed) {
      return clonecore::Result<AdkManagementReport>::failure(
          removed.error());
    }
    return clonecore::Result<AdkManagementReport>::success(
        AdkManagementReport{
            .action = request.action,
            .manifest_id = manifest.manifest_id,
            .managed_record_removed = true,
            .preexisting_adk_preserved = true,
            .uninstall_exit_codes = exit_codes.take_value(),
        });
  }

  const auto consent = require_consent(manifest, request);
  if (!consent) {
    return clonecore::Result<AdkManagementReport>::failure(consent.error());
  }

  if (request.action == AdkManagementAction::create_offline_layout) {
    if (!dependencies.make_acquisition_platform) {
      return failure<AdkManagementReport>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_FUNCTION,
          L"ADK取得 platform factory",
          L"取得platform factoryが設定されていません");
    }
    auto acquisition = dependencies.make_acquisition_platform();
    if (!acquisition) {
      return failure<AdkManagementReport>(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"ADK取得 platform生成",
          L"Windows取得platformを生成できません");
    }
    auto management = dependencies.make_management_platform();
    if (!management) {
      return failure<AdkManagementReport>(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"ADKオフラインレイアウト platform生成",
          L"Windows管理platformを生成できません");
    }
    auto report = create_offline_layout(
        manifest,
        request.offline_layout_root,
        *acquisition,
        *management);
    if (!report) {
      return clonecore::Result<AdkManagementReport>::failure(
          report.error());
    }
    return clonecore::Result<AdkManagementReport>::success(
        AdkManagementReport{
            .action = request.action,
            .manifest_id = manifest.manifest_id,
            .offline_layout_completed =
                report.value().complete_manifest_written,
            .preexisting_adk_preserved = true,
            .offline_layout_bytes = report.value().total_bytes,
            .offline_layout_file_count = report.value().files.size(),
        });
  }

  if (request.action != AdkManagementAction::official_download_install &&
      request.action != AdkManagementAction::offline_layout_install) {
    return failure<AdkManagementReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"ADK取得・管理 action",
        L"固定済みではない管理actionです");
  }
  auto management = dependencies.make_management_platform();
  if (!management) {
    return failure<AdkManagementReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"ADK管理記録 platform生成",
        L"導入前に固定管理記録を確認するplatformを生成できません");
  }
  const auto existing_record =
      management->load_managed_installation_record();
  if (!existing_record) {
    return clonecore::Result<AdkManagementReport>::failure(
        existing_record.error());
  }
  bool current_managed_record{};
  if (existing_record.value().has_value()) {
    const auto existing_plan = build_managed_adk_uninstall_plan(
        manifest, existing_record.value().value());
    if (!existing_plan) {
      return failure<AdkManagementReport>(
          clonecore::ErrorCode::access_denied,
          ERROR_ALREADY_EXISTS,
          L"ADK導入前 管理記録保護",
          L"別manifestまたは改変された管理記録があるため、既存ADKを変更しません");
    }
    current_managed_record = true;
  }
  if (!dependencies.make_acquisition_platform) {
    return failure<AdkManagementReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_FUNCTION,
        L"ADK取得 platform factory",
        L"取得platform factoryが設定されていません");
  }
  auto acquisition = dependencies.make_acquisition_platform();
  if (!acquisition) {
    return failure<AdkManagementReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"ADK取得 platform生成",
        L"Windows取得platformを生成できません");
  }
  const AdkAcquisitionSource source =
      request.action == AdkManagementAction::official_download_install
          ? AdkAcquisitionSource::official_download
          : AdkAcquisitionSource::official_offline_layout;
  auto acquisition_report = execute_adk_acquisition(
      manifest,
      AdkAcquisitionRequest{
          .administrator = request.administrator,
          .source = source,
          .offline_layout_root =
              source == AdkAcquisitionSource::official_offline_layout
                  ? request.offline_layout_root
                  : std::filesystem::path{},
          .consent = consent.value(),
      },
      *acquisition);
  if (!acquisition_report) {
    return clonecore::Result<AdkManagementReport>::failure(
        acquisition_report.error());
  }
  bool record_persisted{};
  if (acquisition_report.value().installed_by_this_operation) {
    if (!current_managed_record) {
      const AdkManagedInstallationRecord record{
          .manifest_id = acquisition_report.value().manifest_id,
          .installed_by_tsumugi = true,
          .installed_registration_ids =
              acquisition_report.value()
                  .managed_installation_registration_ids,
      };
      const auto saved =
          management->save_managed_installation_record_create_new(record);
      if (!saved) {
        return clonecore::Result<AdkManagementReport>::failure(
            saved.error());
      }
    }
    record_persisted = true;
  }
  return clonecore::Result<AdkManagementReport>::success(
      AdkManagementReport{
          .action = request.action,
          .manifest_id = acquisition_report.value().manifest_id,
          .used_existing_installation =
              acquisition_report.value().used_existing_installation,
          .installed_by_this_operation =
              acquisition_report.value().installed_by_this_operation,
          .managed_record_persisted = record_persisted,
          .preexisting_adk_preserved =
              acquisition_report.value().preexisting_adk_preserved,
      });
}

}  // namespace ytec::windowsapp

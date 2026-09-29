#include "ytec/windowsapp/adk_acquisition.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <limits>
#include <set>
#include <utility>

namespace ytec::windowsapp {
namespace {

constexpr std::size_t kMaximumManifestIdLength = 128U;
constexpr std::size_t kMaximumTextLength = 512U;
constexpr std::size_t kMaximumUrlLength = 2'048U;
constexpr std::size_t kMaximumRedirects = 3U;
constexpr std::size_t kMaximumAcquiredComponents = 16U;
constexpr std::uint64_t kMaximumPayloadBytes =
    8ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumTotalBytes =
    16ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumEulaBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kInstallerRebootRequired = 3010U;
constexpr std::wstring_view kRequiredSigner = L"Microsoft Corporation";
constexpr std::wstring_view kRequiredPatchSigner =
    L"CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, "
    L"S=Washington, C=US";
constexpr std::size_t kRequiredPatchMemberCount = 9U;

clonecore::Error acquisition_error(
    clonecore::ErrorCode code,
    DWORD native_code,
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
    clonecore::ErrorCode code,
    DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(acquisition_error(
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status invalid_manifest(std::wstring message) {
  return clonecore::Status::failure(acquisition_error(
      clonecore::ErrorCode::invalid_data,
      ERROR_INVALID_DATA,
      L"ADK固定マニフェスト検証",
      std::move(message)));
}

bool is_bounded_text(
    const std::wstring_view value,
    const std::size_t maximum_length) noexcept {
  if (value.empty() || value.size() > maximum_length) {
    return false;
  }
  return std::none_of(value.begin(), value.end(), [](const wchar_t value) {
    return value == L'\0' || value == L'\r' || value == L'\n' ||
           value < 0x20;
  });
}

bool is_safe_manifest_id(const std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaximumManifestIdLength) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const char character) {
    const auto byte = static_cast<unsigned char>(character);
    return std::isalnum(byte) != 0 || character == '-' || character == '_' ||
           character == '.';
  });
}

bool is_canonical_sha256(const std::string_view value) noexcept {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'A' && character <= 'F');
         });
}

bool is_canonical_guid(const std::wstring_view value) noexcept {
  if (value.size() != 38U || value.front() != L'{' || value.back() != L'}') {
    return false;
  }
  constexpr std::array<std::size_t, 4U> hyphens{9U, 14U, 19U, 24U};
  for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
    if (std::find(hyphens.begin(), hyphens.end(), index) != hyphens.end()) {
      if (value[index] != L'-') {
        return false;
      }
    } else if (!((value[index] >= L'0' && value[index] <= L'9') ||
                 (value[index] >= L'A' && value[index] <= L'F'))) {
      return false;
    }
  }
  return true;
}

bool is_ascii_url_character(const wchar_t value) noexcept {
  return value >= 0x21 && value <= 0x7E && value != L'\\';
}

std::wstring ascii_lower(std::wstring value) {
  std::transform(
      value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        if (character >= L'A' && character <= L'Z') {
          return static_cast<wchar_t>(character - L'A' + L'a');
        }
        return character;
      });
  return value;
}

struct ParsedHttpsUrl final {
  std::wstring host;
  std::wstring path_and_query;
};

bool parse_strict_https_url(
    const std::wstring_view url,
    ParsedHttpsUrl& parsed) {
  constexpr std::wstring_view prefix = L"https://";
  if (url.size() <= prefix.size() || url.size() > kMaximumUrlLength ||
      !url.starts_with(prefix) ||
      !std::all_of(url.begin(), url.end(), is_ascii_url_character) ||
      url.find(L'#') != std::wstring_view::npos) {
    return false;
  }
  const std::size_t path_offset = url.find(L'/', prefix.size());
  if (path_offset == std::wstring_view::npos ||
      path_offset == prefix.size()) {
    return false;
  }
  const std::wstring_view authority =
      url.substr(prefix.size(), path_offset - prefix.size());
  if (authority.find(L'@') != std::wstring_view::npos ||
      authority.find(L':') != std::wstring_view::npos) {
    return false;
  }
  parsed.host = ascii_lower(std::wstring(authority));
  parsed.path_and_query = std::wstring(url.substr(path_offset));
  return !parsed.path_and_query.empty();
}

bool is_allowed_download_url(const std::wstring_view url) {
  ParsedHttpsUrl parsed;
  if (!parse_strict_https_url(url, parsed)) {
    return false;
  }
  const std::wstring path = ascii_lower(parsed.path_and_query);
  if (parsed.host == L"download.microsoft.com") {
    return path.starts_with(L"/download/");
  }
  if (parsed.host == L"go.microsoft.com") {
    return path.starts_with(L"/fwlink/");
  }
  return false;
}

bool is_allowed_microsoft_information_url(const std::wstring_view url) {
  ParsedHttpsUrl parsed;
  if (!parse_strict_https_url(url, parsed)) {
    return false;
  }
  return parsed.host == L"learn.microsoft.com" ||
         parsed.host == L"www.microsoft.com" ||
         parsed.host == L"microsoft.com";
}

bool is_safe_staging_file_name(const std::wstring_view value) noexcept {
  if (!is_bounded_text(value, 128U) || value == L"." || value == L"..") {
    return false;
  }
  return value.find_first_of(L"/\\:") == std::wstring_view::npos;
}

bool is_safe_relative_path(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() ||
      path.has_root_directory() || path.native().size() > 512U) {
    return false;
  }
  for (const auto& component : path) {
    if (component == L"." || component == L".." || component.empty() ||
        component.native().find(L':') != std::wstring::npos) {
      return false;
    }
  }
  return true;
}

bool is_local_absolute_path(const std::filesystem::path& path) {
  if (!path.is_absolute() || path.empty()) {
    return false;
  }
  const auto native = path.native();
  return !native.starts_with(L"\\\\") &&
         !native.starts_with(L"\\?") &&
         !native.starts_with(L"\\.");
}

std::array<AdkPayloadKind, 3U> required_payload_order() noexcept {
  return {
      AdkPayloadKind::deployment_tools,
      AdkPayloadKind::winpe_addon,
      AdkPayloadKind::servicing_update,
  };
}

bool installer_kind_permitted(const AdkPinnedPayload& payload) noexcept {
  if (payload.kind == AdkPayloadKind::deployment_tools ||
      payload.kind == AdkPayloadKind::winpe_addon) {
    return payload.installer_kind ==
           AdkInstallerKind::microsoft_bootstrap_exe;
  }
  return payload.installer_kind == AdkInstallerKind::windows_update_msu ||
         payload.installer_kind ==
             AdkInstallerKind::windows_installer_patch_msp ||
         payload.installer_kind ==
             AdkInstallerKind::windows_installer_patch_archive_zip;
}

bool manifest_payloads_match_consent(
    const AdkReleaseManifest& manifest,
    const AdkAcquisitionConsent& consent) {
  if (consent.presented_payloads.size() != manifest.payloads.size()) {
    return false;
  }
  for (std::size_t index = 0; index < manifest.payloads.size(); ++index) {
    if (consent.presented_payloads[index] != manifest.payloads[index].kind) {
      return false;
    }
  }
  return true;
}

bool manifest_eula_matches_consent(
    const AdkReleaseManifest& manifest,
    const AdkAcquisitionConsent& consent) {
  return consent.presented_embedded_eula == manifest.embedded_eula;
}

bool is_structurally_valid_embedded_eula_pin(
    const AdkReleaseManifest& manifest) {
  const auto& eula = manifest.embedded_eula;
  if (!eula.primary_source_confirmed ||
      !is_allowed_download_url(eula.official_bootstrap_url) ||
      eula.container_offset == 0U || eula.container_length == 0U ||
      eula.container_offset >
          (std::numeric_limits<std::uint64_t>::max)() -
              eula.container_length ||
      !is_safe_staging_file_name(eula.container_member_name) ||
      !is_safe_relative_path(eula.display_file_name) ||
      eula.expected_byte_count == 0U ||
      eula.expected_byte_count > kMaximumEulaBytes ||
      eula.expected_byte_count > eula.container_length ||
      !is_canonical_sha256(eula.expected_sha256) ||
      !is_bounded_text(eula.expected_document_title, 256U)) {
    return false;
  }
  return eula.source_payload_kind == AdkPayloadKind::deployment_tools ||
         eula.source_payload_kind == AdkPayloadKind::winpe_addon;
}

bool embedded_eula_source_matches_payload(
    const AdkReleaseManifest& manifest) {
  const auto& eula = manifest.embedded_eula;
  const auto source = std::find_if(
      manifest.payloads.begin(),
      manifest.payloads.end(),
      [&](const AdkPinnedPayload& payload) {
        return payload.kind == eula.source_payload_kind;
      });
  if (source == manifest.payloads.end() ||
      source->installer_kind !=
          AdkInstallerKind::microsoft_bootstrap_exe ||
      source->exact_source_url != eula.official_bootstrap_url ||
      eula.container_offset > source->expected_byte_count ||
      eula.container_length >
          source->expected_byte_count - eula.container_offset) {
    return false;
  }
  return true;
}

bool paths_equal(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
  return left.lexically_normal() == right.lexically_normal();
}

bool urls_are_pinned(
    const AdkPinnedPayload& payload,
    const AdkStagedPayloadReceipt& receipt) {
  if (receipt.visited_urls.empty() ||
      receipt.visited_urls.front() != payload.exact_source_url ||
      receipt.effective_url != receipt.visited_urls.back() ||
      receipt.visited_urls.size() > payload.allowed_redirect_urls.size() + 1U ||
      receipt.visited_urls.size() > kMaximumRedirects + 1U) {
    return false;
  }
  std::set<std::wstring> visited;
  for (std::size_t index = 0; index < receipt.visited_urls.size(); ++index) {
    const auto& url = receipt.visited_urls[index];
    const bool expected = index == 0U
                              ? url == payload.exact_source_url
                              : index - 1U <
                                        payload.allowed_redirect_urls.size() &&
                                    url == payload.allowed_redirect_urls[
                                               index - 1U];
    if (!expected || !is_allowed_download_url(url) ||
        !visited.insert(url).second) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_staged_receipt(
    const AdkPinnedPayload& payload,
    const AdkAcquisitionSource source,
    const std::filesystem::path& expected_path,
    const std::filesystem::path& expected_offline_source,
    const AdkStagedPayloadReceipt& receipt) {
  if (!receipt.created_new || !receipt.source_regular_file ||
      receipt.source_reparse_point || receipt.byte_count == 0U ||
      receipt.byte_count != payload.expected_byte_count ||
      receipt.byte_count > payload.maximum_bytes ||
      !paths_equal(receipt.staged_path, expected_path)) {
    return clonecore::Status::failure(acquisition_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADK取得物ステージング検証",
        L"新規作成、通常ファイル、reparse、長さ、または保存先の検証に失敗しました"));
  }
  if (source == AdkAcquisitionSource::official_download) {
    if (!receipt.offline_source_path.empty() ||
        !urls_are_pinned(payload, receipt)) {
      return clonecore::Status::failure(acquisition_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_NAME,
          L"ADK公式URL再検証",
          L"取得元またはリダイレクトが固定Microsoft URLと一致しません"));
    }
  } else if (!receipt.visited_urls.empty() ||
             !receipt.effective_url.empty() ||
             !paths_equal(
                 receipt.offline_source_path, expected_offline_source)) {
    return clonecore::Status::failure(acquisition_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"ADKオフラインレイアウト再検証",
        L"固定オフラインレイアウト内の取得元と一致しません"));
  }
  return clonecore::success_status();
}

class StagingCleanup final {
 public:
  StagingCleanup(
      IAdkAcquisitionPlatform& platform,
      const AdkStagingArea& staging)
      : platform_(&platform), staging_(&staging) {}

  StagingCleanup(const StagingCleanup&) = delete;
  StagingCleanup& operator=(const StagingCleanup&) = delete;

  ~StagingCleanup() {
    if (active_) {
      (void)platform_->remove_staging_area(*staging_);
    }
  }

  void release() noexcept { active_ = false; }

 private:
  IAdkAcquisitionPlatform* platform_{};
  const AdkStagingArea* staging_{};
  bool active_{true};
};

}  // namespace

AdkReleaseManifest tsumugi_1_0_0_adk_manifest() {
  // 10.1.26100.2454 and the serviced DISM version below are the currently
  // exercised local configuration. The primary artifacts and embedded
  // Japanese ADK EULA are pinned, but the bounded production extractor,
  // presentation UI, no-unexpected-restart proof, and clean-VM gates remain
  // incomplete, so execution stays disabled.
  return AdkReleaseManifest{
      .manifest_id = "tsumugi-drive-1.0.0-adk-pending-primary-pins",
      .product_release_version = L"1.0.0",
      .tested_adk_version = L"10.1.26100.2454",
      .information_url =
          L"https://learn.microsoft.com/en-us/windows-hardware/get-started/adk-install",
      .embedded_eula = AdkEmbeddedEulaPin{
          .source_payload_kind = AdkPayloadKind::deployment_tools,
          .official_bootstrap_url =
              L"https://download.microsoft.com/download/2/d/9/2d9c8902-3fcd-48a6-a22a-432b08bed61e/ADK/adksetup.exe",
          .container_offset = 0xB3000U,
          .container_length = 0x16C2CDU,
          .container_member_name = L"u6",
          .display_file_name = L"ja\\eula.rtf",
          .expected_byte_count = 293'766U,
          .expected_sha256 =
              "32B66AE90683DE9C91EDE927A45E8E44845CD36E43821BFA4EB2CA5C36A9CF54",
          .expected_document_title =
              L"WINDOWS ASSESSMENT AND DEPLOYMENT KIT (ADK)",
          .primary_source_confirmed = true,
      },
      .unattended_install_no_unexpected_restart_confirmed = false,
      .expected_deployment_tools_version = L"10.1.26100.2454",
      .expected_winpe_addon_version = L"10.1.26100.2454",
      .expected_serviced_dism_version = L"10.0.26100.8972",
      .required_servicing_update_id = L"KB5101684",
      .payloads = {
          AdkPinnedPayload{
              .kind = AdkPayloadKind::deployment_tools,
              .installer_kind = AdkInstallerKind::microsoft_bootstrap_exe,
              .display_name = L"Windows ADK Deployment Tools",
              .staging_file_name = L"adksetup.exe",
              .offline_relative_path = L"adksetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/2/d/9/2d9c8902-3fcd-48a6-a22a-432b08bed61e/ADK/adksetup.exe",
              .expected_sha256 =
                  "7F61E29F2314BCDD7E0ABF67A8367D83A05AA4A7B9223F85C5FD2582A35CC6F4",
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.1.26100.2454",
              .acquired_components = {L"Deployment Tools"},
              .uninstall_registration_id = L"ADK-10.1.26100.2454-DeploymentTools",
              .expected_byte_count = 2'234'632U,
              .maximum_bytes = 2'234'632U,
          },
          AdkPinnedPayload{
              .kind = AdkPayloadKind::winpe_addon,
              .installer_kind = AdkInstallerKind::microsoft_bootstrap_exe,
              .display_name = L"Windows PE Add-on",
              .staging_file_name = L"adkwinpesetup.exe",
              .offline_relative_path = L"adkwinpesetup.exe",
              .exact_source_url =
                  L"https://download.microsoft.com/download/5/5/6/556e01ec-9d78-417d-b1e1-d83a2eff20bc/ADKWinPEAddons/adkwinpesetup.exe",
              .expected_sha256 =
                  "ADF53CA21CAE36821E0A8F3C31546752B9CE066944DE1D4F1673E491831255E2",
              .expected_signer_subject = L"Microsoft Corporation",
              .expected_payload_version = L"10.1.26100.2454",
              .acquired_components = {L"Windows Preinstallation Environment"},
              .uninstall_registration_id = L"ADK-10.1.26100.2454-WinPE",
              .expected_byte_count = 1'945'400U,
              .maximum_bytes = 1'945'400U,
          },
          AdkPinnedPayload{
              .kind = AdkPayloadKind::servicing_update,
              .installer_kind =
                  AdkInstallerKind::windows_installer_patch_archive_zip,
              .display_name = L"Windows ADK Servicing Update KB5101684",
              .staging_file_name = L"Windows_ADK_KB5101684.zip",
              .offline_relative_path =
                  L"Windows_ADK_10.1.26100.2454_Update_KB5101684.zip",
              .exact_source_url =
                  L"https://download.microsoft.com/download/a087a851-4056-4f7f-9791-02a20509b706/Windows_ADK_10.1.26100.2454_Update_KB5101684.zip",
              .expected_sha256 =
                  "DC19725A2FB0CCE44C32AC14059A85A25257B9534BA21C93B479F4F09FB5AF38",
              .acquired_components = {L"DISM", L"Oscdimg"},
              .uninstall_registration_id = L"KB5101684",
              .expected_byte_count = 411'048'362U,
              .maximum_bytes = 411'048'362U,
              .patch_members = {
                  AdkPatchMemberPin{
                      .archive_member_name = L"Appman Sequencer on amd64-x64_en-us.msp",
                      .staging_file_name = L"kb5101684-01.msp",
                      .expected_byte_count = 91'561'984U,
                      .expected_sha256 = "027A1FC0C20CFD2A35B9D51225419C682C8F6CF3B68BE90E4B5FBF9A8DFF3BB5",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{F8E9F1ED-2F45-4C33-8C3D-FA9C657511C8}",
                  },
                  AdkPatchMemberPin{
                      .archive_member_name = L"Appman Sequencer on x86-x86_en-us.msp",
                      .staging_file_name = L"kb5101684-02.msp",
                      .expected_byte_count = 84'307'968U,
                      .expected_sha256 = "309B4C907F85778D57CCCE94A7E9EE2BEA077F4894656FE1D68BAB4CF12C265F",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{C4256C64-EEC0-4D8D-9666-3A09317B69FD}",
                  },
                  AdkPatchMemberPin{
                      .archive_member_name = L"OA3Tool-x86_en-us.msp",
                      .staging_file_name = L"kb5101684-03.msp",
                      .expected_byte_count = 1'634'304U,
                      .expected_sha256 = "F1BF0D357C32E3767D3215D8DA3AEEADD2E40CF613BB0C1326AD2EC2A62A98D3",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{F5FA22BB-8025-481D-AEBF-8D54AC801DA7}",
                  },
                  AdkPatchMemberPin{
                      .archive_member_name = L"OACheck-x86_en-us.msp",
                      .staging_file_name = L"kb5101684-04.msp",
                      .expected_byte_count = 1'462'272U,
                      .expected_sha256 = "9D5D50E16D77ABD32A7348371291B6FD3A7EF3BC4472644BAB20ED82BF1A4C28",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{9D99F1AD-52C1-48C5-B0A2-8C203EBC054C}",
                  },
                  AdkPatchMemberPin{
                      .archive_member_name = L"OATool-x86_en-us.msp",
                      .staging_file_name = L"kb5101684-05.msp",
                      .expected_byte_count = 258'048U,
                      .expected_sha256 = "307B2923025340E789D1156E782DADF2B124273EBE675BDB83D2FAF2208C25E3",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{EE1D618C-B0FF-4572-9B0B-F66A25559385}",
                  },
                  AdkPatchMemberPin{
                      .archive_member_name = L"Oscdimg (OnecoreUAP)-x86_en-us.msp",
                      .staging_file_name = L"kb5101684-06.msp",
                      .expected_byte_count = 18'132'992U,
                      .expected_sha256 = "A93F24C3275967E4F0DEC5792BE2102A068A60EE481143545589B1D073327C6A",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{83449E02-24CA-44C1-A0A3-80A9AA2E85F0}",
                  },
                  AdkPatchMemberPin{
                      .archive_member_name = L"Volume Activation Management Tool-x86_en-us.msp",
                      .staging_file_name = L"kb5101684-07.msp",
                      .expected_byte_count = 864'256U,
                      .expected_sha256 = "8D278EB333C601A28E26FA2EA2097864C596AFAA9C20440DBAD67A034A6777D5",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{4FEA9E08-B1CF-4C0C-8E90-31BAEE5AF0E4}",
                  },
                  AdkPatchMemberPin{
                      .archive_member_name = L"Windows Deployment Image Servicing and Management Tools (OnecoreUAP)-x86_en-us.msp",
                      .staging_file_name = L"kb5101684-08.msp",
                      .expected_byte_count = 181'039'104U,
                      .expected_sha256 = "77242FEFF4CF0221C84249A21675215DB6A12D26E633F4E4BD622E942B368B04",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{BF926991-C615-45A6-BF73-AFC83E790865}",
                  },
                  AdkPatchMemberPin{
                      .archive_member_name = L"Windows System Image Manager-x86_en-us.msp",
                      .staging_file_name = L"kb5101684-09.msp",
                      .expected_byte_count = 33'853'440U,
                      .expected_sha256 = "2570FA4C28A3D939BA60CC3290823998D3506C803F152E4D95B28F310EC2B416",
                      .expected_signer_subject = std::wstring(kRequiredPatchSigner),
                      .expected_revision_guid = L"{4F944331-4B13-4554-9627-2B606D4B4EEE}",
                  },
              },
          },
      },
      .primary_source_pins_confirmed = false,
  };
}

clonecore::Status validate_adk_release_manifest(
    const AdkReleaseManifest& manifest) {
  if (!manifest.primary_source_pins_confirmed) {
    return clonecore::Status::failure(acquisition_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_REVISION_MISMATCH,
        L"ADK固定マニフェスト確定確認",
        L"Microsoft一次資料と実取得物によるURL、EULA、版、署名、SHA-256の固定が未完了です"));
  }
  const auto structure = validate_adk_release_manifest_structure(manifest);
  if (!structure) {
    return structure;
  }
  if (!manifest.unattended_install_no_unexpected_restart_confirmed) {
    return invalid_manifest(
        L"ADK quiet導入が予期しない自動再起動を行わないことを、現在のMicrosoft一次資料または制御VM実挙動で確認できていません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_adk_release_manifest_structure(
    const AdkReleaseManifest& manifest) {
  if (!is_safe_manifest_id(manifest.manifest_id) ||
      !is_bounded_text(manifest.product_release_version, 64U) ||
      !is_bounded_text(manifest.tested_adk_version, 64U) ||
      !is_bounded_text(manifest.expected_deployment_tools_version, 64U) ||
      !is_bounded_text(manifest.expected_winpe_addon_version, 64U) ||
      !is_bounded_text(manifest.expected_serviced_dism_version, 64U) ||
      !is_bounded_text(manifest.required_servicing_update_id, 128U) ||
      !is_allowed_microsoft_information_url(manifest.information_url) ||
      !is_structurally_valid_embedded_eula_pin(manifest)) {
    return invalid_manifest(
        L"版、識別子、Microsoft公式案内、またはADK bootstrap内EULAの範囲・member・Hash固定値が不正です");
  }
  const auto order = required_payload_order();
  if (manifest.payloads.size() != order.size()) {
    return invalid_manifest(
        L"Deployment Tools、WinPE Add-on、必須更新の3取得物が必要です");
  }

  std::uint64_t total_bytes{};
  std::set<std::wstring> staging_names;
  std::set<std::wstring> registration_ids;
  std::set<std::wstring> source_urls;
  for (std::size_t index = 0; index < manifest.payloads.size(); ++index) {
    const auto& payload = manifest.payloads[index];
    if (payload.kind != order[index] || !installer_kind_permitted(payload) ||
        !is_bounded_text(payload.display_name, kMaximumTextLength) ||
        !is_safe_staging_file_name(payload.staging_file_name) ||
        !is_safe_relative_path(payload.offline_relative_path) ||
        !is_allowed_download_url(payload.exact_source_url) ||
        payload.allowed_redirect_urls.size() > kMaximumRedirects ||
        !is_canonical_sha256(payload.expected_sha256) ||
        payload.acquired_components.empty() ||
        payload.acquired_components.size() > kMaximumAcquiredComponents ||
        !is_bounded_text(payload.uninstall_registration_id, 128U) ||
        payload.expected_byte_count == 0U ||
        payload.expected_byte_count > payload.maximum_bytes ||
        payload.maximum_bytes == 0U ||
        payload.maximum_bytes > kMaximumPayloadBytes) {
      return invalid_manifest(
          L"取得物の順序、固定値、上限、署名者、または保存名が不正です");
    }
    const bool patch_archive =
        payload.installer_kind ==
        AdkInstallerKind::windows_installer_patch_archive_zip;
    if (patch_archive) {
      if (!payload.expected_signer_subject.empty() ||
          !payload.expected_payload_version.empty() ||
          payload.patch_members.size() != kRequiredPatchMemberCount) {
        return invalid_manifest(
            L"更新ZIPは外側署名/版を持たず、固定MSP 9件を必要とします");
      }
      std::set<std::wstring> archive_names;
      std::set<std::wstring> member_staging_names;
      std::set<std::wstring> revision_guids;
      std::uint64_t member_total{};
      for (const auto& member : payload.patch_members) {
        if (!is_safe_staging_file_name(member.archive_member_name) ||
            !is_safe_staging_file_name(member.staging_file_name) ||
            !member.archive_member_name.ends_with(L".msp") ||
            !member.staging_file_name.ends_with(L".msp") ||
            member.expected_byte_count == 0U ||
            member.expected_byte_count > kMaximumPayloadBytes ||
            !is_canonical_sha256(member.expected_sha256) ||
            member.expected_signer_subject != kRequiredPatchSigner ||
            !is_canonical_guid(member.expected_revision_guid) ||
            !archive_names.insert(member.archive_member_name).second ||
            !member_staging_names.insert(member.staging_file_name).second ||
            !revision_guids.insert(member.expected_revision_guid).second ||
            member_total > kMaximumTotalBytes -
                               member.expected_byte_count) {
          return invalid_manifest(
              L"更新ZIP内MSPの名前、長さ、Hash、署名者、Revision GUID、または重複が不正です");
        }
        member_total += member.expected_byte_count;
      }
      if (member_total == 0U ||
          total_bytes > kMaximumTotalBytes - member_total) {
        return invalid_manifest(L"更新ZIP展開後の合計上限が製品上限を超えています");
      }
      total_bytes += member_total;
    } else if (payload.expected_signer_subject != kRequiredSigner ||
               !is_bounded_text(payload.expected_payload_version, 128U) ||
               !payload.patch_members.empty()) {
      return invalid_manifest(
          L"通常取得物の署名者、版、または更新ZIP member指定が不正です");
    }
    if (!staging_names.insert(payload.staging_file_name).second ||
        !registration_ids.insert(payload.uninstall_registration_id).second ||
        !source_urls.insert(payload.exact_source_url).second) {
      return invalid_manifest(
          L"取得物のURL、保存名、または登録識別子が重複しています");
    }
    std::set<std::wstring> redirect_urls;
    for (const auto& redirect : payload.allowed_redirect_urls) {
      if (!is_allowed_download_url(redirect) ||
          redirect == payload.exact_source_url ||
          !redirect_urls.insert(redirect).second) {
        return invalid_manifest(
            L"リダイレクト先が固定Microsoft配布URLではありません");
      }
    }
    for (const auto& component : payload.acquired_components) {
      if (!is_bounded_text(component, 128U)) {
        return invalid_manifest(L"表示する取得内容が不正です");
      }
    }
    if (total_bytes > kMaximumTotalBytes - payload.maximum_bytes) {
      return invalid_manifest(L"取得物の合計上限が製品上限を超えています");
    }
    total_bytes += payload.maximum_bytes;
  }
  if (total_bytes == 0U || total_bytes > kMaximumTotalBytes) {
    return invalid_manifest(L"取得物の合計上限が不正です");
  }
  if (!embedded_eula_source_matches_payload(manifest)) {
    return invalid_manifest(
        L"ADK固有EULAの参照先が固定Microsoft bootstrapのURLまたは有界ファイル範囲と一致しません");
  }
  return clonecore::success_status();
}

bool installed_state_matches_manifest(
    const AdkInstalledState& state,
    const AdkReleaseManifest& manifest) noexcept {
  return state.deployment_tools_present && state.winpe_addon_present &&
         state.servicing_update_present && state.microsoft_binaries_trusted &&
         state.deployment_tools_version ==
             manifest.expected_deployment_tools_version &&
         state.winpe_addon_version == manifest.expected_winpe_addon_version &&
         state.serviced_dism_version ==
             manifest.expected_serviced_dism_version &&
         state.servicing_update_id ==
             manifest.required_servicing_update_id;
}

std::vector<std::wstring> fixed_silent_install_arguments(
    const AdkPayloadKind payload_kind,
    const AdkInstallerKind installer_kind) {
  // Supplying /norestart is not itself proof that the current ADK bootstrap
  // will honor it under /quiet. Manifest validation separately requires
  // unattended_install_no_unexpected_restart_confirmed. The documented
  // msiexec /norestart behavior for MSPs does not satisfy that bootstrap gate.
  if (payload_kind == AdkPayloadKind::deployment_tools &&
      installer_kind == AdkInstallerKind::microsoft_bootstrap_exe) {
    return {
        L"/quiet",
        L"/norestart",
        L"/features",
        L"OptionId.DeploymentTools",
    };
  }
  if (payload_kind == AdkPayloadKind::winpe_addon &&
      installer_kind == AdkInstallerKind::microsoft_bootstrap_exe) {
    return {
        L"/quiet",
        L"/norestart",
        L"/features",
        L"OptionId.WindowsPreinstallationEnvironment",
    };
  }
  if (payload_kind == AdkPayloadKind::servicing_update) {
    return installer_kind ==
                   AdkInstallerKind::windows_installer_patch_msp
               ? std::vector<std::wstring>{L"/qn", L"/norestart"}
               : std::vector<std::wstring>{L"/quiet", L"/norestart"};
  }
  return {};
}

bool adk_installer_exit_code_permitted(
    const AdkPayloadKind payload_kind,
    const AdkInstallerKind installer_kind,
    const std::uint32_t exit_code) noexcept {
  constexpr std::uint32_t kPatchTargetNotFound = 1642U;
  if (exit_code == ERROR_SUCCESS ||
      exit_code == kInstallerRebootRequired) {
    return true;
  }
  return payload_kind == AdkPayloadKind::servicing_update &&
         installer_kind ==
             AdkInstallerKind::windows_installer_patch_msp &&
         exit_code == kPatchTargetNotFound;
}

clonecore::Result<AdkAcquisitionReport> execute_adk_acquisition(
    const AdkReleaseManifest& manifest,
    const AdkAcquisitionRequest& request,
    IAdkAcquisitionPlatform& platform) {
  const auto manifest_status = validate_adk_release_manifest(manifest);
  if (!manifest_status) {
    return clonecore::Result<AdkAcquisitionReport>::failure(
        manifest_status.error());
  }
  if (!request.administrator) {
    return failure<AdkAcquisitionReport>(
        clonecore::ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"ADK導入 管理者確認",
        L"ADK導入は管理者として起動した製品からだけ実行できます");
  }
  if (!request.consent.accepted ||
      request.consent.presented_manifest_id != manifest.manifest_id ||
      !manifest_eula_matches_consent(manifest, request.consent) ||
      !manifest_payloads_match_consent(manifest, request.consent)) {
    return failure<AdkAcquisitionReport>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"ADK導入 明示同意",
        L"固定取得物、Microsoft公式URL、利用条件を表示した同じマニフェストへの明示同意が必要です");
  }
  if (request.source == AdkAcquisitionSource::official_offline_layout &&
      !is_local_absolute_path(request.offline_layout_root)) {
    return failure<AdkAcquisitionReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"ADKオフラインレイアウト選択",
        L"ローカルの絶対パスにある公式オフラインレイアウトだけを使用できます");
  }
  if (request.source == AdkAcquisitionSource::official_download &&
      !request.offline_layout_root.empty()) {
    return failure<AdkAcquisitionReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"ADK取得元選択",
        L"公式ダウンロード時にオフラインレイアウトを同時指定できません");
  }

  const auto before = platform.inspect_installed_state(manifest);
  if (!before) {
    return clonecore::Result<AdkAcquisitionReport>::failure(before.error());
  }
  if (installed_state_matches_manifest(before.value(), manifest)) {
    return clonecore::Result<AdkAcquisitionReport>::success(
        AdkAcquisitionReport{
            .manifest_id = manifest.manifest_id,
            .used_existing_installation = true,
            .installed_by_this_operation = false,
            .preexisting_adk_preserved = true,
            .offline_layout_used =
                request.source ==
                AdkAcquisitionSource::official_offline_layout,
            .post_install_preflight_verified = true,
            .staging_removed = true,
        });
  }
  const bool existing_components_present =
      before.value().deployment_tools_present ||
      before.value().winpe_addon_present ||
      before.value().servicing_update_present;

  std::uint64_t maximum_total_bytes{};
  for (const auto& payload : manifest.payloads) {
    maximum_total_bytes += payload.maximum_bytes;
    for (const auto& member : payload.patch_members) {
      maximum_total_bytes += member.expected_byte_count;
    }
  }
  const auto staging =
      platform.create_new_staging_area(maximum_total_bytes);
  if (!staging) {
    return clonecore::Result<AdkAcquisitionReport>::failure(staging.error());
  }
  if (!staging.value().created_new || staging.value().reparse_point ||
      !is_local_absolute_path(staging.value().root)) {
    return failure<AdkAcquisitionReport>(
        clonecore::ErrorCode::verification_failed,
        staging.value().reparse_point ? ERROR_REPARSE_TAG_INVALID
                                      : ERROR_INVALID_DATA,
        L"ADK一時領域再検証",
        L"新規作成したローカルの非reparse一時領域を確認できません");
  }
  StagingCleanup cleanup(platform, staging.value());

  std::vector<AdkVerifiedPayload> verified_payloads;
  std::vector<AdkVerifiedPayload> verified_archives;
  verified_payloads.reserve(manifest.payloads.size() + kRequiredPatchMemberCount);
  for (const auto& payload : manifest.payloads) {
    const std::filesystem::path expected_staged_path =
        staging.value().root / payload.staging_file_name;
    clonecore::Result<AdkStagedPayloadReceipt> acquired =
        failure<AdkStagedPayloadReceipt>(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_FUNCTION,
            L"ADK取得物ステージング",
            L"取得元が初期化されていません");
    std::filesystem::path expected_offline_source;
    if (request.source == AdkAcquisitionSource::official_download) {
      std::vector<std::wstring> exact_allowed{
          payload.exact_source_url};
      exact_allowed.insert(
          exact_allowed.end(),
          payload.allowed_redirect_urls.begin(),
          payload.allowed_redirect_urls.end());
      acquired = platform.download_to_new_file(AdkDownloadRequest{
          .exact_source_url = payload.exact_source_url,
          .exact_allowed_urls = std::move(exact_allowed),
          .create_new_destination = expected_staged_path,
          .maximum_bytes = payload.maximum_bytes,
      });
    } else {
      expected_offline_source =
          request.offline_layout_root / payload.offline_relative_path;
      acquired = platform.stage_offline_payload(AdkOfflineStageRequest{
          .layout_root = request.offline_layout_root,
          .exact_relative_path = payload.offline_relative_path,
          .create_new_destination = expected_staged_path,
          .maximum_bytes = payload.maximum_bytes,
      });
    }
    if (!acquired) {
      return clonecore::Result<AdkAcquisitionReport>::failure(
          acquired.error());
    }
    const auto receipt_status = validate_staged_receipt(
        payload,
        request.source,
        expected_staged_path,
        expected_offline_source,
        acquired.value());
    if (!receipt_status) {
      return clonecore::Result<AdkAcquisitionReport>::failure(
          receipt_status.error());
    }

    const auto hash = platform.sha256_file(
        expected_staged_path, payload.maximum_bytes);
    if (!hash) {
      return clonecore::Result<AdkAcquisitionReport>::failure(hash.error());
    }
    if (hash.value() != payload.expected_sha256) {
      return failure<AdkAcquisitionReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"ADK取得物 SHA-256検証",
          L"取得物のSHA-256が固定値と一致しないため、インストーラーは起動しません");
    }
    if (payload.installer_kind ==
        AdkInstallerKind::windows_installer_patch_archive_zip) {
      AdkVerifiedPayload archive{
          .kind = payload.kind,
          .installer_kind = payload.installer_kind,
          .staged_path = expected_staged_path,
          .byte_count = acquired.value().byte_count,
          .sha256 = hash.value(),
      };
      const auto expanded = platform.expand_and_verify_patch_archive(
          AdkPatchArchiveExpandRequest{
              .archive = archive,
              .members = payload.patch_members,
          });
      if (!expanded) {
        return clonecore::Result<AdkAcquisitionReport>::failure(
            expanded.error());
      }
      if (expanded.value().size() != payload.patch_members.size()) {
        return failure<AdkAcquisitionReport>(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_DATA,
            L"ADK更新ZIP展開結果",
            L"固定MSP 9件すべての検証結果を確認できません");
      }
      for (std::size_t member_index = 0;
           member_index < payload.patch_members.size(); ++member_index) {
        const auto& pin = payload.patch_members[member_index];
        const auto& verified = expanded.value()[member_index];
        if (verified.kind != AdkPayloadKind::servicing_update ||
            verified.installer_kind !=
                AdkInstallerKind::windows_installer_patch_msp ||
            !paths_equal(
                verified.staged_path,
                staging.value().root / pin.staging_file_name) ||
            verified.byte_count != pin.expected_byte_count ||
            verified.sha256 != pin.expected_sha256 ||
            verified.signer_subject != pin.expected_signer_subject ||
            !verified.payload_version.empty() ||
            verified.msp_revision_guid != pin.expected_revision_guid) {
          return failure<AdkAcquisitionReport>(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"ADK更新MSP固定値再照合",
              L"展開したMSPの保存先、長さ、Hash、署名者、またはRevision GUIDが固定値と一致しません");
        }
      }
      verified_archives.push_back(std::move(archive));
      verified_payloads.insert(
          verified_payloads.end(),
          expanded.value().begin(),
          expanded.value().end());
      continue;
    }
    const auto signature = platform.verify_authenticode(
        expected_staged_path, payload.expected_signer_subject);
    if (!signature) {
      return clonecore::Result<AdkAcquisitionReport>::failure(
          signature.error());
    }
    const auto version =
        platform.query_payload_version(expected_staged_path);
    if (!version) {
      return clonecore::Result<AdkAcquisitionReport>::failure(
          version.error());
    }
    if (version.value() != payload.expected_payload_version) {
      return failure<AdkAcquisitionReport>(
          clonecore::ErrorCode::verification_failed,
          ERROR_REVISION_MISMATCH,
          L"ADK取得物 バージョン検証",
          L"Microsoft署名済み取得物の版が固定値と一致しないため、インストーラーは起動しません");
    }
    verified_payloads.push_back(AdkVerifiedPayload{
        .kind = payload.kind,
        .installer_kind = payload.installer_kind,
        .staged_path = expected_staged_path,
        .byte_count = acquired.value().byte_count,
        .sha256 = hash.value(),
          .signer_subject = payload.expected_signer_subject,
          .payload_version = version.value(),
    });
  }

  std::vector<std::uint32_t> exit_codes;
  exit_codes.reserve(verified_payloads.size());
  bool reboot_required{};
  for (const auto& payload : verified_payloads) {
    const auto arguments = fixed_silent_install_arguments(
        payload.kind, payload.installer_kind);
    if (arguments.empty()) {
      return failure<AdkAcquisitionReport>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"ADK固定サイレント引数",
          L"固定済みの導入方式に対応する引数がありません");
    }
    const auto launched = platform.run_verified_silent_installer(
        AdkSilentInstallRequest{
            .payload = payload,
            .fixed_arguments = arguments,
        });
    if (!launched) {
      return clonecore::Result<AdkAcquisitionReport>::failure(
          launched.error());
    }
    exit_codes.push_back(launched.value());
    if (!adk_installer_exit_code_permitted(
            payload.kind,
            payload.installer_kind,
            launched.value())) {
      return failure<AdkAcquisitionReport>(
          clonecore::ErrorCode::io_failed,
          ERROR_INSTALL_FAILURE,
          L"ADKサイレント導入 終了コード",
          L"Microsoftセットアップが許可していない終了コードを返したため、後続導入を停止しました");
    }
    reboot_required = reboot_required ||
                      launched.value() == kInstallerRebootRequired;
  }

  const auto after = platform.inspect_installed_state(manifest);
  if (!after) {
    return clonecore::Result<AdkAcquisitionReport>::failure(after.error());
  }
  if (!installed_state_matches_manifest(after.value(), manifest)) {
    return failure<AdkAcquisitionReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_REVISION_MISMATCH,
        L"ADK導入後プリフライト",
        L"Deployment Tools、WinPE Add-on、必須更新、署名、または版の最終検査に合格しませんでした");
  }

  const auto removed = platform.remove_staging_area(staging.value());
  cleanup.release();
  if (!removed) {
    return clonecore::Result<AdkAcquisitionReport>::failure(removed.error());
  }
  std::vector<std::wstring> managed_registration_ids;
  managed_registration_ids.reserve(manifest.payloads.size());
  for (const auto& payload : manifest.payloads) {
    managed_registration_ids.push_back(payload.uninstall_registration_id);
  }
  return clonecore::Result<AdkAcquisitionReport>::success(
      AdkAcquisitionReport{
          .manifest_id = manifest.manifest_id,
          .used_existing_installation = false,
          .installed_by_this_operation = true,
          .preexisting_adk_preserved = existing_components_present,
          .offline_layout_used =
              request.source ==
              AdkAcquisitionSource::official_offline_layout,
          .reboot_required = reboot_required,
          .post_install_preflight_verified = true,
          .staging_removed = true,
          .verified_archives = std::move(verified_archives),
          .verified_payloads = std::move(verified_payloads),
          .installer_exit_codes = std::move(exit_codes),
          .managed_installation_registration_ids =
              std::move(managed_registration_ids),
      });
}

clonecore::Result<AdkUninstallPlan> build_managed_adk_uninstall_plan(
    const AdkReleaseManifest& manifest,
    const AdkManagedInstallationRecord& record) {
  const auto valid = validate_adk_release_manifest(manifest);
  if (!valid) {
    return clonecore::Result<AdkUninstallPlan>::failure(valid.error());
  }
  if (!record.installed_by_tsumugi ||
      record.manifest_id != manifest.manifest_id ||
      record.installed_registration_ids.size() != manifest.payloads.size()) {
    return failure<AdkUninstallPlan>(
        clonecore::ErrorCode::access_denied,
        ERROR_NOT_FOUND,
        L"ADK管理対象アンインストール計画",
        L"この機能で導入した同じ固定マニフェストの記録を確認できないため、既存ADKは削除しません");
  }
  for (std::size_t index = 0; index < manifest.payloads.size(); ++index) {
    if (record.installed_registration_ids[index] !=
        manifest.payloads[index].uninstall_registration_id) {
      return failure<AdkUninstallPlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADK管理対象アンインストール識別",
          L"導入時記録と固定登録識別子が一致しないため、既存ADKは削除しません");
    }
  }
  AdkUninstallPlan plan{
      .manifest_id = manifest.manifest_id,
  };
  plan.steps.reserve(manifest.payloads.size());
  for (auto iterator = manifest.payloads.rbegin();
       iterator != manifest.payloads.rend();
       ++iterator) {
    plan.steps.push_back(AdkUninstallStep{
        .kind = iterator->kind,
        .display_name = iterator->display_name,
        .registration_id = iterator->uninstall_registration_id,
    });
  }
  return clonecore::Result<AdkUninstallPlan>::success(std::move(plan));
}

std::wstring_view adk_payload_kind_label(
    const AdkPayloadKind kind) noexcept {
  switch (kind) {
    case AdkPayloadKind::deployment_tools:
      return L"ADK Deployment Tools";
    case AdkPayloadKind::winpe_addon:
      return L"Windows PE Add-on";
    case AdkPayloadKind::servicing_update:
      return L"必須更新";
  }
  return L"不明";
}

}  // namespace ytec::windowsapp

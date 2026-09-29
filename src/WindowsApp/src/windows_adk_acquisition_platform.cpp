#include "ytec/windowsapp/windows_adk_acquisition_platform.h"

#include "ytec/windowsapp/adk_patch_archive.h"
#include "windows_adk_eula_extractor_internal.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/error.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/mediabuilder/adk_detection.h"

#include <Windows.h>
#include <Objbase.h>
#include <Msi.h>
#include <MsiDefs.h>
#include <MsiQuery.h>
#include <Softpub.h>
#include <Wincrypt.h>
#include <Wintrust.h>
#include <winhttp.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::uint64_t kMaximumPayloadBytes =
    8ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaximumTotalBytes =
    16ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumUrlCharacters = 2'048U;
constexpr std::size_t kMaximumRedirects = 3U;
constexpr std::size_t kIoBlockBytes = 1024U * 1024U;
constexpr DWORD kMaximumVersionResourceBytes = 1024U * 1024U;
constexpr std::wstring_view kMicrosoftSigner = L"Microsoft Corporation";
constexpr std::wstring_view kMicrosoftPatchSigner =
    L"CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, "
    L"S=Washington, C=US";

clonecore::Error platform_error(
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
  return clonecore::Result<T>::failure(platform_error(
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(platform_error(
      code, native_code, std::move(operation), std::move(message)));
}

bool is_ascii_drive_absolute(const std::filesystem::path& path) {
  const std::wstring value = path.native();
  return value.size() >= 3U &&
         ((value[0] >= L'A' && value[0] <= L'Z') ||
          (value[0] >= L'a' && value[0] <= L'z')) &&
         value[1] == L':' &&
         (value[2] == L'\\' || value[2] == L'/') &&
         !value.starts_with(L"\\\\") &&
         !value.starts_with(L"\\?") &&
         !value.starts_with(L"\\.");
}

bool path_equal(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
  const std::wstring left_value = left.lexically_normal().native();
  const std::wstring right_value = right.lexically_normal().native();
  return CompareStringOrdinal(
             left_value.data(),
             static_cast<int>(left_value.size()),
             right_value.data(),
             static_cast<int>(right_value.size()),
             TRUE) == CSTR_EQUAL;
}

bool text_equal(
    const std::wstring_view left,
    const std::wstring_view right) {
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

bool is_safe_relative_path(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() ||
      path.has_root_directory() || path.native().size() > 512U) {
    return false;
  }
  for (const auto& component : path) {
    if (component.empty() || component == L"." || component == L".." ||
        component.native().find(L':') != std::wstring::npos) {
      return false;
    }
  }
  return true;
}

bool is_safe_windows_leaf_name(const std::filesystem::path& leaf) {
  const std::wstring value = leaf.native();
  if (value.empty() || value.size() > 128U || value == L"." ||
      value == L".." || value.back() == L'.' || value.back() == L' ' ||
      value.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
      std::any_of(value.begin(), value.end(), [](const wchar_t character) {
        return character < 0x20;
      })) {
    return false;
  }
  std::wstring stem = value.substr(0, value.find(L'.'));
  std::transform(
      stem.begin(), stem.end(), stem.begin(), [](const wchar_t character) {
        if (character >= L'A' && character <= L'Z') {
          return static_cast<wchar_t>(character - L'A' + L'a');
        }
        return character;
      });
  if (stem == L"con" || stem == L"prn" || stem == L"aux" ||
      stem == L"nul") {
    return false;
  }
  if (stem.size() == 4U &&
      (stem.starts_with(L"com") || stem.starts_with(L"lpt")) &&
      stem[3] >= L'1' && stem[3] <= L'9') {
    return false;
  }
  return true;
}

bool is_direct_child(
    const std::filesystem::path& parent,
    const std::filesystem::path& child) {
  const auto normalized_parent = parent.lexically_normal();
  const auto normalized_child = child.lexically_normal();
  return is_ascii_drive_absolute(normalized_parent) &&
         is_ascii_drive_absolute(normalized_child) &&
         path_equal(normalized_child.parent_path(), normalized_parent) &&
         is_safe_windows_leaf_name(normalized_child.filename());
}

struct StrictHttpsUrl final {
  std::wstring host;
  std::wstring path_and_query;
};

bool is_ascii_url_character(const wchar_t character) noexcept {
  return character >= 0x21 && character <= 0x7E && character != L'\\';
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

std::optional<StrictHttpsUrl> parse_pinned_microsoft_url(
    const std::wstring_view url) {
  constexpr std::wstring_view prefix = L"https://";
  if (url.size() <= prefix.size() || url.size() > kMaximumUrlCharacters ||
      !url.starts_with(prefix) ||
      !std::all_of(url.begin(), url.end(), is_ascii_url_character) ||
      url.find(L'#') != std::wstring_view::npos) {
    return std::nullopt;
  }
  const std::size_t path_offset = url.find(L'/', prefix.size());
  if (path_offset == std::wstring_view::npos ||
      path_offset == prefix.size()) {
    return std::nullopt;
  }
  const std::wstring_view authority =
      url.substr(prefix.size(), path_offset - prefix.size());
  if (authority.find(L'@') != std::wstring_view::npos ||
      authority.find(L':') != std::wstring_view::npos) {
    return std::nullopt;
  }
  StrictHttpsUrl parsed{
      .host = ascii_lower(std::wstring(authority)),
      .path_and_query = std::wstring(url.substr(path_offset)),
  };
  const std::wstring lower_path = ascii_lower(parsed.path_and_query);
  const bool allowed =
      (parsed.host == L"download.microsoft.com" &&
       lower_path.starts_with(L"/download/")) ||
      (parsed.host == L"go.microsoft.com" &&
       lower_path.starts_with(L"/fwlink/"));
  if (!allowed) {
    return std::nullopt;
  }
  return parsed;
}

bool is_upper_sha256(const std::string_view value) {
  return value.size() == 64U &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'A' && character <= 'F');
         });
}

bool has_expected_extension(
    const std::filesystem::path& path,
    const std::wstring_view expected) {
  return text_equal(path.extension().native(), expected);
}

clonecore::Result<std::filesystem::path> full_local_path(
    const std::filesystem::path& path,
    const std::wstring_view operation) {
  if (!is_ascii_drive_absolute(path)) {
    return failure<std::filesystem::path>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"ローカルドライブの絶対パスではありません");
  }
  const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0U || required > 32'768U) {
    return clonecore::Result<std::filesystem::path>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  std::vector<wchar_t> buffer(required, L'\0');
  const DWORD written = GetFullPathNameW(
      path.c_str(), required, buffer.data(), nullptr);
  if (written == 0U || written >= required) {
    return clonecore::Result<std::filesystem::path>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  std::filesystem::path result(std::wstring(buffer.data(), written));
  if (!is_ascii_drive_absolute(result)) {
    return failure<std::filesystem::path>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"ローカルドライブの正規化済み絶対パスを確認できません");
  }
  return clonecore::Result<std::filesystem::path>::success(
      result.lexically_normal());
}

struct FileIdentity final {
  std::uint64_t volume_serial{};
  std::array<std::byte, 16U> file_id{};

  [[nodiscard]] bool operator==(const FileIdentity&) const noexcept = default;
};

struct FileObservation final {
  FileIdentity identity;
  std::uint64_t byte_count{};
  std::uint32_t link_count{};
  bool directory{};
  bool reparse_point{};
  bool device{};
};

clonecore::Result<FileObservation> observe_handle(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!GetFileInformationByHandleEx(
          handle,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes))) {
    return clonecore::Result<FileObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(
          handle,
          FileStandardInfo,
          &standard,
          sizeof(standard))) {
    return clonecore::Result<FileObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  FILE_ID_INFO id{};
  if (!GetFileInformationByHandleEx(
          handle, FileIdInfo, &id, sizeof(id))) {
    return clonecore::Result<FileObservation>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            operation,
            GetLastError()));
  }
  if (standard.EndOfFile.QuadPart < 0 || standard.NumberOfLinks == 0U) {
    return failure<FileObservation>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"ファイル長またはリンク数が不正です");
  }
  FileObservation result{};
  result.identity.volume_serial = id.VolumeSerialNumber;
  std::memcpy(
      result.identity.file_id.data(), id.FileId.Identifier, result.identity.file_id.size());
  result.byte_count = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
  result.link_count = standard.NumberOfLinks;
  result.directory =
      (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U;
  result.reparse_point =
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
  result.device =
      (attributes.FileAttributes & FILE_ATTRIBUTE_DEVICE) != 0U;
  return clonecore::Result<FileObservation>::success(result);
}

bool regular_non_reparse(const FileObservation& observation) noexcept {
  return !observation.directory && !observation.reparse_point &&
         !observation.device;
}

clonecore::Status mark_handle_for_delete(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (!SetFileInformationByHandle(
          handle,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        operation,
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Status require_existing_chain_without_reparse(
    const std::filesystem::path& path,
    const bool include_leaf,
    const std::wstring_view operation) {
  auto current = path.root_path();
  const auto relative = path.relative_path();
  std::size_t index{};
  const std::size_t count = static_cast<std::size_t>(
      std::distance(relative.begin(), relative.end()));
  for (const auto& component : relative) {
    ++index;
    if (!include_leaf && index == count) {
      break;
    }
    current /= component;
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          operation,
          GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          std::wstring(operation),
          L"reparse pointを経由するパスは使用できません");
    }
  }
  return clonecore::success_status();
}

clonecore::Result<std::filesystem::path> module_directory() {
  std::vector<wchar_t> buffer(1024U, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U) {
      return clonecore::Result<std::filesystem::path>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"ADK一時領域 EXE配置先取得",
              GetLastError()));
    }
    if (length < buffer.size() - 1U) {
      return full_local_path(
          std::filesystem::path(std::wstring(buffer.data(), length))
              .parent_path(),
          L"ADK一時領域 EXE配置先正規化");
    }
    if (buffer.size() >= 32'768U) {
      return failure<std::filesystem::path>(
          clonecore::ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"ADK一時領域 EXE配置先取得",
          L"EXE配置先パスがWindows上限を超えています");
    }
    buffer.resize(std::min<std::size_t>(buffer.size() * 2U, 32'768U));
  }
}

clonecore::Status create_or_verify_directory(
    const std::filesystem::path& path,
    const std::wstring_view operation) {
  DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD native_code = GetLastError();
    if (native_code != ERROR_FILE_NOT_FOUND &&
        native_code != ERROR_PATH_NOT_FOUND) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          operation,
          native_code));
    }
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          operation,
          GetLastError()));
    }
    attributes = GetFileAttributesW(path.c_str());
  }
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(operation),
        L"通常の非reparseフォルダーを確認できません");
  }
  return clonecore::success_status();
}

clonecore::Result<std::filesystem::path> system_directory_path() {
  std::vector<wchar_t> buffer(32'768U, L'\0');
  const UINT length =
      GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::filesystem::path>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ADK導入 System32取得",
            GetLastError()));
  }
  return full_local_path(
      std::filesystem::path(std::wstring(buffer.data(), length)),
      L"ADK導入 System32正規化");
}

clonecore::Result<std::wstring> query_file_version_unlocked(
    const std::filesystem::path& path) {
  DWORD ignored{};
  const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (bytes == 0U || bytes > kMaximumVersionResourceBytes) {
    DWORD native_code = GetLastError();
    if (native_code == ERROR_SUCCESS) {
      native_code = bytes == 0U ? ERROR_RESOURCE_DATA_NOT_FOUND
                                : ERROR_FILE_TOO_LARGE;
    }
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ADK取得物 ファイルバージョン取得",
            native_code));
  }
  std::vector<std::byte> version_data(bytes);
  if (!GetFileVersionInfoW(
          path.c_str(), 0, bytes, version_data.data())) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ADK取得物 ファイルバージョン読取り",
            GetLastError()));
  }
  VS_FIXEDFILEINFO* fixed{};
  UINT fixed_bytes{};
  if (!VerQueryValueW(
          version_data.data(),
          L"\\",
          reinterpret_cast<void**>(&fixed),
          &fixed_bytes) ||
      fixed == nullptr || fixed_bytes < sizeof(VS_FIXEDFILEINFO) ||
      fixed->dwSignature != VS_FFI_SIGNATURE) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_RESOURCE_DATA_NOT_FOUND,
        L"ADK取得物 ファイルバージョン検証",
        L"固定ファイルバージョン情報を確認できません");
  }
  return clonecore::Result<std::wstring>::success(
      std::to_wstring(HIWORD(fixed->dwFileVersionMS)) + L"." +
      std::to_wstring(LOWORD(fixed->dwFileVersionMS)) + L"." +
      std::to_wstring(HIWORD(fixed->dwFileVersionLS)) + L"." +
      std::to_wstring(LOWORD(fixed->dwFileVersionLS)));
}

clonecore::Result<std::string> sha256_locked_handle(
    const HANDLE handle,
    const std::uint64_t byte_count) {
  const auto digest = imageformat::sha256_from_reader(
      byte_count,
      kIoBlockBytes,
      [handle](const std::uint64_t offset, const std::size_t length)
          -> clonecore::Result<std::vector<std::byte>> {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::io_failed,
                  L"ADK取得物 SHA-256位置決め",
                  GetLastError()));
        }
        std::vector<std::byte> bytes(length);
        std::size_t consumed{};
        while (consumed < bytes.size()) {
          const DWORD amount = static_cast<DWORD>(
              std::min<std::size_t>(
                  bytes.size() - consumed,
                  std::numeric_limits<DWORD>::max()));
          DWORD read{};
          if (!ReadFile(
                  handle, bytes.data() + consumed, amount, &read, nullptr)) {
            return clonecore::Result<std::vector<std::byte>>::failure(
                clonecore::make_win32_error(
                    clonecore::ErrorCode::io_failed,
                    L"ADK取得物 SHA-256読取り",
                    GetLastError()));
          }
          if (read == 0U) {
            return failure<std::vector<std::byte>>(
                clonecore::ErrorCode::io_failed,
                ERROR_HANDLE_EOF,
                L"ADK取得物 SHA-256読取り",
                L"検証中に予期しない終端へ到達しました");
          }
          consumed += read;
        }
        return clonecore::Result<std::vector<std::byte>>::success(
            std::move(bytes));
      });
  if (!digest) {
    return clonecore::Result<std::string>::failure(digest.error());
  }
  constexpr char hexadecimal[] = "0123456789ABCDEF";
  std::string text;
  text.reserve(64U);
  for (const std::byte value : digest.value()) {
    const auto byte = std::to_integer<unsigned int>(value);
    text.push_back(hexadecimal[(byte >> 4U) & 0x0FU]);
    text.push_back(hexadecimal[byte & 0x0FU]);
  }
  return clonecore::Result<std::string>::success(std::move(text));
}

void close_wintrust_state(WINTRUST_DATA& trust_data) noexcept {
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
  (void)WinVerifyTrust(nullptr, &policy, &trust_data);
}

bool certificate_attribute_equals(
    const PCCERT_CONTEXT certificate,
    const char* const object_identifier,
    const std::wstring_view expected) {
  const DWORD required = CertGetNameStringW(
      certificate,
      CERT_NAME_ATTR_TYPE,
      0,
      const_cast<char*>(object_identifier),
      nullptr,
      0);
  if (required <= 1U || required > 256U) {
    return false;
  }
  std::vector<wchar_t> value(required, L'\0');
  if (CertGetNameStringW(
          certificate,
          CERT_NAME_ATTR_TYPE,
          0,
          const_cast<char*>(object_identifier),
          value.data(),
          static_cast<DWORD>(value.size())) != required) {
    return false;
  }
  const std::wstring_view observed(value.data());
  return observed.size() == expected.size() &&
         CompareStringOrdinal(
             observed.data(),
             static_cast<int>(observed.size()),
             expected.data(),
             static_cast<int>(expected.size()),
             TRUE) == CSTR_EQUAL;
}

clonecore::Status verify_exact_microsoft_windows_patch_signature(
    const std::filesystem::path& path) {
  WINTRUST_FILE_INFO file_info{};
  file_info.cbStruct = sizeof(file_info);
  file_info.pcwszFilePath = path.c_str();
  WINTRUST_DATA trust_data{};
  trust_data.cbStruct = sizeof(trust_data);
  trust_data.dwUIChoice = WTD_UI_NONE;
  trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust_data.dwUnionChoice = WTD_CHOICE_FILE;
  trust_data.pFile = &file_info;
  trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
  trust_data.dwProvFlags =
      WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG trust_status = WinVerifyTrust(nullptr, &policy, &trust_data);
  if (trust_status != ERROR_SUCCESS) {
    close_wintrust_state(trust_data);
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        static_cast<DWORD>(trust_status),
        L"ADK更新MSP Authenticode検証",
        L"展開したMSPの埋込み署名をWindowsが信頼できません");
  }
  CRYPT_PROVIDER_DATA* const provider =
      WTHelperProvDataFromStateData(trust_data.hWVTStateData);
  CRYPT_PROVIDER_SGNR* const signer = provider == nullptr
      ? nullptr
      : WTHelperGetProvSignerFromChain(provider, 0U, FALSE, 0U);
  const PCCERT_CONTEXT certificate =
      signer == nullptr || signer->csCertChain == 0U ||
              signer->pasCertChain == nullptr
          ? nullptr
          : signer->pasCertChain[0].pCert;
  const bool exact_subject =
      certificate != nullptr &&
      certificate_attribute_equals(
          certificate, szOID_COMMON_NAME, L"Microsoft Windows") &&
      certificate_attribute_equals(
          certificate, szOID_ORGANIZATION_NAME, L"Microsoft Corporation") &&
      certificate_attribute_equals(
          certificate, szOID_LOCALITY_NAME, L"Redmond") &&
      certificate_attribute_equals(
          certificate, szOID_STATE_OR_PROVINCE_NAME, L"Washington") &&
      certificate_attribute_equals(
          certificate, szOID_COUNTRY_NAME, L"US");
  close_wintrust_state(trust_data);
  if (!exact_subject) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        static_cast<DWORD>(TRUST_E_SUBJECT_NOT_TRUSTED),
        L"ADK更新MSP 署名者固定値",
        L"有効な署名ですがMicrosoft Windowsの固定Subjectと一致しません");
  }
  return clonecore::success_status();
}

clonecore::Result<std::wstring> query_msp_revision_guid_unlocked(
    const std::filesystem::path& path) {
  MSIHANDLE summary{};
  const UINT opened = MsiGetSummaryInformationW(
      0U, path.c_str(), 0U, &summary);
  if (opened != ERROR_SUCCESS || summary == 0U) {
    return failure<std::wstring>(
        clonecore::ErrorCode::verification_failed,
        opened,
        L"ADK更新MSP Revision取得",
        L"MSP SummaryInformationを読み取り専用で開けません");
  }
  class SummaryHandle final {
   public:
    explicit SummaryHandle(const MSIHANDLE value) noexcept : value_(value) {}
    ~SummaryHandle() { (void)MsiCloseHandle(value_); }
    SummaryHandle(const SummaryHandle&) = delete;
    SummaryHandle& operator=(const SummaryHandle&) = delete;

   private:
    MSIHANDLE value_{};
  } close_summary(summary);
  UINT data_type{};
  INT integer_value{};
  FILETIME file_time{};
  std::array<wchar_t, 128U> text{};
  DWORD characters = static_cast<DWORD>(text.size());
  const UINT queried = MsiSummaryInfoGetPropertyW(
      summary,
      PID_REVNUMBER,
      &data_type,
      &integer_value,
      &file_time,
      text.data(),
      &characters);
  if (queried != ERROR_SUCCESS || data_type != VT_LPSTR ||
      characters == 0U || characters >= text.size()) {
    return failure<std::wstring>(
        clonecore::ErrorCode::verification_failed,
        queried == ERROR_SUCCESS ? ERROR_INVALID_DATA : queried,
        L"ADK更新MSP Revision検証",
        L"MSP Revision GUIDを有界文字列として取得できません");
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(text.data(), characters));
}

class MappedReadOnlyFile final {
 public:
  MappedReadOnlyFile() = default;
  ~MappedReadOnlyFile() {
    if (view_ != nullptr) {
      UnmapViewOfFile(view_);
    }
  }
  MappedReadOnlyFile(const MappedReadOnlyFile&) = delete;
  MappedReadOnlyFile& operator=(const MappedReadOnlyFile&) = delete;

  clonecore::Status map(const HANDLE file, const std::uint64_t byte_count) {
    if (byte_count == 0U || byte_count > std::numeric_limits<std::size_t>::max()) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_FILE_TOO_LARGE,
          L"ADK更新ZIP 読取り専用Map境界",
          L"ZIP長を現在のx64アドレス空間へ安全にMapできません");
    }
    mapping_.reset(CreateFileMappingW(
        file, nullptr, PAGE_READONLY, 0U, 0U, nullptr));
    if (!mapping_) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"CreateFileMappingW(ADK更新ZIP 読取り専用)",
          GetLastError()));
    }
    view_ = MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0U, 0U, 0U);
    if (view_ == nullptr) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"MapViewOfFile(ADK更新ZIP 読取り専用)",
          GetLastError()));
    }
    bytes_ = std::span<const std::byte>(
        static_cast<const std::byte*>(view_),
        static_cast<std::size_t>(byte_count));
    return clonecore::success_status();
  }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return bytes_;
  }

 private:
  clonecore::UniqueHandle mapping_;
  void* view_{};
  std::span<const std::byte> bytes_;
};

clonecore::Result<std::vector<clonecore::UniqueHandle>>
lock_offline_directory_chain(
    const std::filesystem::path& layout_root,
    const std::filesystem::path& relative_parent) {
  std::vector<clonecore::UniqueHandle> handles;
  std::filesystem::path current = layout_root;
  const auto open_current = [&handles](const std::filesystem::path& path)
      -> clonecore::Status {
    clonecore::UniqueHandle handle(CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"ADKオフライン取得 親フォルダー固定",
          GetLastError()));
    }
    const auto observation = observe_handle(
        handle.get(), L"ADKオフライン取得 親フォルダー識別");
    if (!observation || !observation.value().directory ||
        observation.value().reparse_point) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"ADKオフライン取得 親フォルダー識別",
          L"通常の非reparse親フォルダーではありません");
    }
    handles.push_back(std::move(handle));
    return clonecore::success_status();
  };
  auto status = open_current(current);
  if (!status) {
    return clonecore::Result<std::vector<clonecore::UniqueHandle>>::failure(
        status.error());
  }
  for (const auto& component : relative_parent) {
    current /= component;
    status = open_current(current);
    if (!status) {
      return clonecore::Result<std::vector<clonecore::UniqueHandle>>::failure(
          status.error());
    }
  }
  return clonecore::Result<std::vector<clonecore::UniqueHandle>>::success(
      std::move(handles));
}

class UniqueInternetHandle final {
 public:
  UniqueInternetHandle() noexcept = default;
  explicit UniqueInternetHandle(HINTERNET handle) noexcept : handle_(handle) {}
  ~UniqueInternetHandle() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
  }
  UniqueInternetHandle(const UniqueInternetHandle&) = delete;
  UniqueInternetHandle& operator=(const UniqueInternetHandle&) = delete;
  UniqueInternetHandle(UniqueInternetHandle&& other) noexcept
      : handle_(other.release()) {}
  UniqueInternetHandle& operator=(UniqueInternetHandle&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
  [[nodiscard]] HINTERNET release() noexcept {
    HINTERNET value = handle_;
    handle_ = nullptr;
    return value;
  }
  void reset(HINTERNET value = nullptr) noexcept {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
    handle_ = value;
  }

 private:
  HINTERNET handle_{};
};

clonecore::Result<std::wstring> query_redirect_location(
    const HINTERNET request) {
  DWORD bytes{};
  (void)WinHttpQueryHeaders(
      request,
      WINHTTP_QUERY_LOCATION,
      WINHTTP_HEADER_NAME_BY_INDEX,
      nullptr,
      &bytes,
      WINHTTP_NO_HEADER_INDEX);
  const DWORD native_code = GetLastError();
  if (native_code != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t) ||
      bytes > (kMaximumUrlCharacters + 1U) * sizeof(wchar_t)) {
    return failure<std::wstring>(
        clonecore::ErrorCode::verification_failed,
        native_code,
        L"ADK公式URL リダイレクト取得",
        L"固定対象と比較できるLocationヘッダーを取得できません");
  }
  std::vector<wchar_t> buffer(
      (bytes / sizeof(wchar_t)) + 1U, L'\0');
  DWORD read_bytes = bytes;
  if (!WinHttpQueryHeaders(
          request,
          WINHTTP_QUERY_LOCATION,
          WINHTTP_HEADER_NAME_BY_INDEX,
          buffer.data(),
          &read_bytes,
          WINHTTP_NO_HEADER_INDEX)) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"ADK公式URL リダイレクト読取り",
            GetLastError()));
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(buffer.data()));
}

bool is_redirect_status(const DWORD status) noexcept {
  return status == HTTP_STATUS_MOVED || status == HTTP_STATUS_REDIRECT ||
         status == HTTP_STATUS_REDIRECT_METHOD ||
         status == 307U || status == 308U;
}

std::optional<std::wstring> query_msi_version(
    const std::wstring_view registration_id) {
  if (registration_id.empty() || registration_id.size() > 128U) {
    return std::nullopt;
  }
  const std::wstring product(registration_id);
  DWORD characters{};
  UINT status = MsiGetProductInfoW(
      product.c_str(), INSTALLPROPERTY_VERSIONSTRING, nullptr, &characters);
  if (status != ERROR_MORE_DATA || characters == 0U || characters > 65'535U) {
    return std::nullopt;
  }
  std::vector<wchar_t> value(
      static_cast<std::size_t>(characters) + 1U, L'\0');
  DWORD capacity = static_cast<DWORD>(value.size());
  status = MsiGetProductInfoW(
      product.c_str(),
      INSTALLPROPERTY_VERSIONSTRING,
      value.data(),
      &capacity);
  if (status != ERROR_SUCCESS || capacity == 0U || capacity >= value.size()) {
    return std::nullopt;
  }
  return std::wstring(value.data(), capacity);
}

const AdkPinnedPayload* payload_by_kind(
    const AdkReleaseManifest& manifest,
    const AdkPayloadKind kind) {
  const auto found = std::find_if(
      manifest.payloads.begin(),
      manifest.payloads.end(),
      [kind](const AdkPinnedPayload& payload) { return payload.kind == kind; });
  return found == manifest.payloads.end() ? nullptr : &*found;
}

class WindowsAdkProcessLauncher final : public IWindowsAdkProcessLauncher {
 public:
  [[nodiscard]] clonecore::Result<std::uint32_t> launch_and_wait(
      const WindowsAdkLaunchPlan& plan) override {
    if (plan.uses_shell || !is_ascii_drive_absolute(plan.executable_path) ||
        !is_ascii_drive_absolute(plan.working_directory)) {
      return failure<std::uint32_t>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"ADK検証済みインストーラー起動",
          L"shellなしのローカル絶対パス実行計画ではありません");
    }
    std::wstring command_line =
        bootrepair::quote_windows_argument(plan.executable_path.native());
    for (const auto& argument : plan.arguments) {
      command_line.push_back(L' ');
      command_line.append(bootrepair::quote_windows_argument(argument));
    }
    std::vector<wchar_t> mutable_command(
        command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            plan.executable_path.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_DEFAULT_ERROR_MODE | CREATE_UNICODE_ENVIRONMENT |
                CREATE_NO_WINDOW,
            nullptr,
            plan.working_directory.c_str(),
            &startup,
            &process)) {
      return clonecore::Result<std::uint32_t>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"CreateProcessW(ADK検証済みインストーラー)",
              GetLastError()));
    }
    clonecore::UniqueHandle process_handle(process.hProcess);
    clonecore::UniqueHandle thread_handle(process.hThread);
    const DWORD waited = WaitForSingleObject(process_handle.get(), INFINITE);
    if (waited != WAIT_OBJECT_0) {
      return clonecore::Result<std::uint32_t>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ADK検証済みインストーラー完了待機",
              waited == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE));
    }
    DWORD exit_code{};
    if (!GetExitCodeProcess(process_handle.get(), &exit_code)) {
      return clonecore::Result<std::uint32_t>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"ADK検証済みインストーラー終了コード取得",
              GetLastError()));
    }
    return clonecore::Result<std::uint32_t>::success(exit_code);
  }
};

}  // namespace

clonecore::Result<WindowsAdkDownloadPolicy>
validate_windows_adk_download_policy(const AdkDownloadRequest& request) {
  if (request.maximum_bytes == 0U ||
      request.maximum_bytes > kMaximumPayloadBytes ||
      !is_ascii_drive_absolute(request.create_new_destination) ||
      !is_safe_windows_leaf_name(
          request.create_new_destination.filename()) ||
      request.exact_allowed_urls.empty() ||
      request.exact_allowed_urls.size() > kMaximumRedirects + 1U ||
      request.exact_allowed_urls.front() != request.exact_source_url) {
    return failure<WindowsAdkDownloadPolicy>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"ADK公式取得ポリシー",
        L"保存先、上限、または固定URL列が不正です");
  }
  std::set<std::wstring> unique_urls;
  for (const auto& url : request.exact_allowed_urls) {
    if (!parse_pinned_microsoft_url(url) ||
        !unique_urls.insert(url).second) {
      return failure<WindowsAdkDownloadPolicy>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_NAME,
          L"ADK公式取得ポリシー",
          L"HTTPS固定Microsoft配布URL以外、または重複URLを拒否しました");
    }
  }
  return clonecore::Result<WindowsAdkDownloadPolicy>::success(
      WindowsAdkDownloadPolicy{request.exact_allowed_urls});
}

clonecore::Status validate_windows_adk_offline_stage_scope(
    const AdkOfflineStageRequest& request,
    const std::filesystem::path& owned_staging_root) {
  if (request.maximum_bytes == 0U ||
      request.maximum_bytes > kMaximumPayloadBytes ||
      !is_ascii_drive_absolute(request.layout_root) ||
      !is_safe_relative_path(request.exact_relative_path) ||
      !is_direct_child(
          owned_staging_root, request.create_new_destination)) {
    return status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"ADKオフライン取得範囲",
        L"ローカルレイアウト、固定相対パス、または所有一時領域内の新規保存先ではありません");
  }
  return clonecore::success_status();
}

AdkInstalledState evaluate_windows_adk_installed_observation(
    const AdkReleaseManifest& manifest,
    const WindowsAdkInstalledObservation& observation) {
  return AdkInstalledState{
      .deployment_tools_present = observation.deployment_tools_present,
      .winpe_addon_present = observation.winpe_addon_present,
      .servicing_update_present = observation.servicing_update_present,
      .microsoft_binaries_trusted =
          observation.microsoft_binaries_trusted,
      .deployment_tools_version = observation.deployment_tools_version,
      .winpe_addon_version = observation.winpe_addon_version,
      .serviced_dism_version = observation.serviced_dism_version,
      .servicing_update_id = observation.servicing_update_present
                                 ? manifest.required_servicing_update_id
                                 : L"",
  };
}

clonecore::Result<WindowsAdkLaunchPlan>
validate_and_build_windows_adk_launch_plan(
    const AdkSilentInstallRequest& request,
    const std::filesystem::path& absolute_system_directory,
    const WindowsAdkPrelaunchObservation& observation) {
  const auto expected_arguments = fixed_silent_install_arguments(
      request.payload.kind, request.payload.installer_kind);
  const bool msp = request.payload.installer_kind ==
                   AdkInstallerKind::windows_installer_patch_msp;
  const bool identity_fields_valid =
      msp ? (request.payload.signer_subject == kMicrosoftPatchSigner &&
             request.payload.payload_version.empty() &&
             !request.payload.msp_revision_guid.empty())
          : (request.payload.signer_subject == kMicrosoftSigner &&
             !request.payload.payload_version.empty() &&
             request.payload.msp_revision_guid.empty());
  if (expected_arguments.empty() ||
      request.fixed_arguments != expected_arguments ||
      !is_ascii_drive_absolute(request.payload.staged_path) ||
      request.payload.byte_count == 0U ||
      request.payload.byte_count > kMaximumPayloadBytes ||
      !is_upper_sha256(request.payload.sha256) || !identity_fields_valid) {
    return failure<WindowsAdkLaunchPlan>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"ADK導入直前 固定要求検証",
        L"検証済み取得物または固定サイレント引数が不正です");
  }
  if (!observation.owned_staged_identity_matches ||
      !observation.payload_regular_non_reparse ||
      !observation.payload_single_link ||
      observation.payload_byte_count != request.payload.byte_count ||
      observation.payload_sha256 != request.payload.sha256 ||
      !observation.payload_signature_trusted ||
      observation.payload_signer_subject != request.payload.signer_subject ||
      observation.payload_version != request.payload.payload_version ||
      observation.msp_revision_guid != request.payload.msp_revision_guid) {
    return failure<WindowsAdkLaunchPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"ADK導入直前 取得物再検証",
        L"所有識別、reparse、リンク数、長さ、SHA-256、署名者、または版が取得直後の検証結果と一致しません");
  }

  WindowsAdkLaunchPlan plan{};
  plan.uses_shell = false;
  if (request.payload.installer_kind ==
      AdkInstallerKind::microsoft_bootstrap_exe) {
    if (!has_expected_extension(request.payload.staged_path, L".exe")) {
      return failure<WindowsAdkLaunchPlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_BAD_EXE_FORMAT,
          L"ADK導入直前 実行形式検証",
          L"Microsoft bootstrap取得物が.exeではありません");
    }
    plan.executable_path = request.payload.staged_path;
    plan.arguments = request.fixed_arguments;
    plan.working_directory = request.payload.staged_path.parent_path();
  } else {
    if (!is_ascii_drive_absolute(absolute_system_directory) ||
        !observation.system_handler_regular_non_reparse ||
        !observation.system_handler_signature_trusted) {
      return failure<WindowsAdkLaunchPlan>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_IMAGE_HASH,
          L"ADK導入直前 System32ハンドラー検証",
          L"絶対System32内のMicrosoft署名済みハンドラーを確認できません");
    }
    if (request.payload.installer_kind ==
        AdkInstallerKind::windows_update_msu) {
      if (!has_expected_extension(request.payload.staged_path, L".msu")) {
        return failure<WindowsAdkLaunchPlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"ADK導入直前 MSU形式検証",
            L"Windows Update取得物が.msuではありません");
      }
      plan.executable_path = absolute_system_directory / L"wusa.exe";
      plan.arguments.push_back(request.payload.staged_path.native());
      } else if (request.payload.installer_kind ==
                AdkInstallerKind::windows_installer_patch_msp) {
      if (!has_expected_extension(request.payload.staged_path, L".msp")) {
        return failure<WindowsAdkLaunchPlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"ADK導入直前 MSP形式検証",
            L"Windows Installer更新取得物が.mspではありません");
        }
        plan.executable_path = absolute_system_directory / L"msiexec.exe";
        plan.arguments.push_back(L"/p");
        plan.arguments.push_back(request.payload.staged_path.native());
        plan.arguments.insert(
            plan.arguments.end(),
            request.fixed_arguments.begin(),
            request.fixed_arguments.end());
    } else {
      return failure<WindowsAdkLaunchPlan>(
          clonecore::ErrorCode::unsupported_platform,
          ERROR_NOT_SUPPORTED,
          L"ADK導入直前 実行方式",
          L"固定済みではないインストーラー形式です");
    }
    if (!msp) {
      plan.arguments.insert(
          plan.arguments.end(),
          request.fixed_arguments.begin(),
          request.fixed_arguments.end());
    }
    plan.working_directory = absolute_system_directory;
  }
  return clonecore::Result<WindowsAdkLaunchPlan>::success(std::move(plan));
}

std::unique_ptr<IWindowsAdkProcessLauncher>
make_windows_adk_process_launcher() {
  return std::make_unique<WindowsAdkProcessLauncher>();
}

class WindowsAdkAcquisitionPlatform::Impl final {
 public:
  explicit Impl(std::unique_ptr<IWindowsAdkProcessLauncher> launcher)
      : launcher_(launcher ? std::move(launcher)
                           : make_windows_adk_process_launcher()),
        trust_verifier_(
            bootrepair::make_windows_authenticode_verifier()) {}

  [[nodiscard]] clonecore::Result<AdkInstalledState>
  inspect_installed_state(const AdkReleaseManifest& manifest) {
    auto environment = mediabuilder::make_windows_adk_environment();
    if (!environment) {
      return failure<AdkInstalledState>(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"ADK導入済み状態 読取り専用検査",
          L"Windows ADK検査環境を初期化できません");
    }
    const auto discovery =
        mediabuilder::detect_windows_adk(*environment, L"amd64");
    const mediabuilder::AdkCandidateReport* selected{};
    int selected_score = -1;
    for (const auto& candidate : discovery.candidates) {
      int score{};
      score += candidate.deployment_tools_present ? 4 : 0;
      score += candidate.winpe_addon_present ? 4 : 0;
      score += candidate.microsoft_tools_trusted ? 2 : 0;
      score += candidate.version_and_servicing_verified ? 8 : 0;
      if (score > selected_score) {
        selected = &candidate;
        selected_score = score;
      }
    }
    if (selected == nullptr || selected_score == 0) {
      return clonecore::Result<AdkInstalledState>::success(
          AdkInstalledState{});
    }

    WindowsAdkInstalledObservation observation{};
    observation.deployment_tools_present =
        selected->deployment_tools_present;
    observation.winpe_addon_present = selected->winpe_addon_present;
    observation.microsoft_binaries_trusted =
        selected->microsoft_tools_trusted;
    observation.deployment_tools_version =
        selected->deployment_tools_version;

    if (const auto* winpe =
            payload_by_kind(manifest, AdkPayloadKind::winpe_addon)) {
      const auto version = query_msi_version(winpe->uninstall_registration_id);
      if (version) {
        observation.winpe_addon_version = *version;
      }
    }
    if (selected->dism_file_version != mediabuilder::FileVersion{}) {
      observation.serviced_dism_version =
          mediabuilder::format_file_version(selected->dism_file_version);
    }
    observation.servicing_update_present =
        manifest.required_servicing_update_id == L"KB5101684" &&
        manifest.expected_serviced_dism_version == L"10.0.26100.8972" &&
        selected->oscdimg_servicing_patch_applied &&
        selected->dism_servicing_patch_applied &&
        observation.serviced_dism_version ==
            manifest.expected_serviced_dism_version;
    return clonecore::Result<AdkInstalledState>::success(
        evaluate_windows_adk_installed_observation(
            manifest, observation));
  }

  [[nodiscard]] clonecore::Result<AdkStagingArea>
  create_new_staging_area(const std::uint64_t maximum_total_bytes) {
    if (maximum_total_bytes == 0U ||
        maximum_total_bytes > kMaximumTotalBytes) {
      return failure<AdkStagingArea>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"ADK一時領域 上限検証",
          L"一時領域の合計上限が製品境界外です");
    }
    std::scoped_lock lock(mutex_);
    const auto module = module_directory();
    if (!module) {
      return clonecore::Result<AdkStagingArea>::failure(module.error());
    }
    const auto module_chain = require_existing_chain_without_reparse(
        module.value(), true, L"ADK一時領域 EXE配置先検査");
    if (!module_chain) {
      return clonecore::Result<AdkStagingArea>::failure(
          module_chain.error());
    }

    const auto data_root = module.value() / L"data";
    auto status = create_or_verify_directory(
        data_root, L"ADK一時領域 data作成");
    if (!status) {
      return clonecore::Result<AdkStagingArea>::failure(status.error());
    }
    const auto staging_base = data_root / L"adk-staging";
    status = create_or_verify_directory(
        staging_base, L"ADK一時領域 基点作成");
    if (!status) {
      return clonecore::Result<AdkStagingArea>::failure(status.error());
    }
    status = require_existing_chain_without_reparse(
        staging_base, true, L"ADK一時領域 親経路検査");
    if (!status) {
      return clonecore::Result<AdkStagingArea>::failure(status.error());
    }

    clonecore::UniqueHandle base_handle(CreateFileW(
        staging_base.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!base_handle) {
      return clonecore::Result<AdkStagingArea>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ADK一時領域 基点固定",
              GetLastError()));
    }
    const auto base_observation = observe_handle(
        base_handle.get(), L"ADK一時領域 基点再検証");
    if (!base_observation || !base_observation.value().directory ||
        base_observation.value().reparse_point) {
      return failure<AdkStagingArea>(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"ADK一時領域 基点再検証",
          L"非reparseの一時領域基点を固定できません");
    }
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(
            staging_base.c_str(), &available, nullptr, nullptr)) {
      return clonecore::Result<AdkStagingArea>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"ADK一時領域 空き容量取得",
              GetLastError()));
    }
    if (available.QuadPart < maximum_total_bytes) {
      return failure<AdkStagingArea>(
          clonecore::ErrorCode::io_failed,
          ERROR_DISK_FULL,
          L"ADK一時領域 空き容量検証",
          L"固定取得物の合計上限を安全に保持できる空き容量がありません");
    }

    std::filesystem::path stage_root;
    bool created{};
    for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
      GUID identifier{};
      if (FAILED(CoCreateGuid(&identifier))) {
        return failure<AdkStagingArea>(
            clonecore::ErrorCode::internal_error,
            ERROR_GEN_FAILURE,
            L"ADK一時領域 一意名生成",
            L"暗号学的に一意な一時領域名を生成できません");
      }
      std::array<wchar_t, 40U> text{};
      if (StringFromGUID2(
              identifier,
              text.data(),
              static_cast<int>(text.size())) == 0) {
        return failure<AdkStagingArea>(
            clonecore::ErrorCode::internal_error,
            ERROR_INSUFFICIENT_BUFFER,
            L"ADK一時領域 一意名変換",
            L"一時領域識別子を安全な文字列へ変換できません");
      }
      stage_root = staging_base / std::wstring(text.data());
      if (CreateDirectoryW(stage_root.c_str(), nullptr)) {
        created = true;
        break;
      }
      if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return clonecore::Result<AdkStagingArea>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"ADK一時領域 新規作成",
                GetLastError()));
      }
    }
    if (!created) {
      return failure<AdkStagingArea>(
          clonecore::ErrorCode::io_failed,
          ERROR_ALREADY_EXISTS,
          L"ADK一時領域 新規作成",
          L"衝突しない新規一時領域を確保できません");
    }

    clonecore::UniqueHandle root_handle(CreateFileW(
        stage_root.c_str(),
        FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!root_handle) {
      (void)RemoveDirectoryW(stage_root.c_str());
      return clonecore::Result<AdkStagingArea>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ADK一時領域 所有固定",
              GetLastError()));
    }
    const auto root_observation = observe_handle(
        root_handle.get(), L"ADK一時領域 所有識別");
    if (!root_observation || !root_observation.value().directory ||
        root_observation.value().reparse_point) {
      root_handle.reset();
      (void)RemoveDirectoryW(stage_root.c_str());
      return failure<AdkStagingArea>(
          clonecore::ErrorCode::verification_failed,
          ERROR_REPARSE_TAG_INVALID,
          L"ADK一時領域 所有識別",
          L"新規作成した通常フォルダーを再識別できません");
    }
    clonecore::UniqueHandle base_reopened(CreateFileW(
        staging_base.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    const auto current_base = base_reopened
        ? observe_handle(
              base_reopened.get(), L"ADK一時領域 基点Path再識別")
        : failure<FileObservation>(
              clonecore::ErrorCode::identity_mismatch,
              GetLastError(),
              L"ADK一時領域 基点Path再識別",
              L"一時領域基点を同じPathから再度開けません");
    if (!current_base ||
        current_base.value().identity != base_observation.value().identity ||
        !current_base.value().directory ||
        current_base.value().reparse_point) {
      (void)mark_handle_for_delete(
          root_handle.get(), L"ADK一時領域 基点不一致時削除");
      root_handle.reset();
      return failure<AdkStagingArea>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADK一時領域 基点Path再識別",
          L"新規フォルダー作成中に一時領域基点のPath識別が変化しました");
    }
    const auto canonical = full_local_path(
        stage_root, L"ADK一時領域 所有パス正規化");
    if (!canonical) {
      root_handle.reset();
      (void)RemoveDirectoryW(stage_root.c_str());
      return clonecore::Result<AdkStagingArea>::failure(canonical.error());
    }
    StageRecord record{};
    record.root = canonical.value();
    record.identity = root_observation.value().identity;
    record.root_handle = std::move(root_handle);
    stages_.emplace(record.root.native(), std::move(record));
    return clonecore::Result<AdkStagingArea>::success(AdkStagingArea{
        .root = canonical.value(),
        .created_new = true,
        .reparse_point = false,
    });
  }

  [[nodiscard]] clonecore::Result<AdkStagedPayloadReceipt>
  download_to_new_file(const AdkDownloadRequest& request) {
    const auto policy = validate_windows_adk_download_policy(request);
    if (!policy) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          policy.error());
    }
    std::scoped_lock lock(mutex_);
    const auto destination = full_local_path(
        request.create_new_destination,
        L"ADK公式取得 保存先正規化");
    if (!destination) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          destination.error());
    }
    StageRecord* const stage = find_stage_for_destination(
        destination.value());
    if (stage == nullptr) {
      return failure<AdkStagedPayloadReceipt>(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_OWNER,
          L"ADK公式取得 保存先所有確認",
          L"このアダプターが新規作成した一時領域の直下ではありません");
    }
    auto destination_file = create_new_destination(
        *stage, destination.value());
    if (!destination_file) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          destination_file.error());
    }
    PendingFile pending(destination_file.value().get());

    UniqueInternetHandle session(WinHttpOpen(
        L"Y-TEC Tsumugi Drive/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.valid()) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"WinHttpOpen(ADK公式取得、proxy不使用)",
              GetLastError()));
    }
    if (!WinHttpSetTimeouts(
            session.get(), 30'000, 30'000, 30'000, 60'000)) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"WinHTTP ADK取得 timeout設定",
              GetLastError()));
    }
    DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(
            session.get(),
            WINHTTP_OPTION_SECURE_PROTOCOLS,
            &secure_protocols,
            sizeof(secure_protocols))) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::verification_failed,
              L"WinHTTP ADK取得 TLS下限設定",
              GetLastError()));
    }

    std::vector<std::wstring> visited;
    std::uint64_t total{};
    std::wstring effective_url;
    bool downloaded{};
    for (std::size_t index = 0;
         index < policy.value().ordered_exact_urls.size(); ++index) {
      const auto& current_url =
          policy.value().ordered_exact_urls[index];
      const auto parsed = parse_pinned_microsoft_url(current_url);
      if (!parsed) {
        return failure<AdkStagedPayloadReceipt>(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_NAME,
            L"ADK公式取得 URL再検証",
            L"要求直前のURLが固定Microsoft HTTPS配布先ではありません");
      }
      UniqueInternetHandle connection(WinHttpConnect(
          session.get(),
          parsed->host.c_str(),
          INTERNET_DEFAULT_HTTPS_PORT,
          0));
      if (!connection.valid()) {
        return clonecore::Result<AdkStagedPayloadReceipt>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"WinHttpConnect(ADK公式取得)",
                GetLastError()));
      }
      UniqueInternetHandle http_request(WinHttpOpenRequest(
          connection.get(),
          L"GET",
          parsed->path_and_query.c_str(),
          nullptr,
          WINHTTP_NO_REFERER,
          WINHTTP_DEFAULT_ACCEPT_TYPES,
          WINHTTP_FLAG_SECURE));
      if (!http_request.valid()) {
        return clonecore::Result<AdkStagedPayloadReceipt>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"WinHttpOpenRequest(ADK公式取得)",
                GetLastError()));
      }
      DWORD disabled = WINHTTP_DISABLE_AUTHENTICATION |
                       WINHTTP_DISABLE_COOKIES |
                       WINHTTP_DISABLE_REDIRECTS |
                       WINHTTP_DISABLE_KEEP_ALIVE;
      if (!WinHttpSetOption(
              http_request.get(),
              WINHTTP_OPTION_DISABLE_FEATURE,
              &disabled,
              sizeof(disabled))) {
        return clonecore::Result<AdkStagedPayloadReceipt>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::verification_failed,
                L"WinHTTP ADK取得 credential/redirect無効化",
                GetLastError()));
      }
      DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
      if (!WinHttpSetOption(
              http_request.get(),
              WINHTTP_OPTION_REDIRECT_POLICY,
              &redirect_policy,
              sizeof(redirect_policy)) ||
          !WinHttpSendRequest(
              http_request.get(),
              WINHTTP_NO_ADDITIONAL_HEADERS,
              0,
              WINHTTP_NO_REQUEST_DATA,
              0,
              0,
              0) ||
          !WinHttpReceiveResponse(http_request.get(), nullptr)) {
        return clonecore::Result<AdkStagedPayloadReceipt>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"WinHTTP ADK公式取得",
                GetLastError()));
      }
      visited.push_back(current_url);
      DWORD status_code{};
      DWORD status_bytes = sizeof(status_code);
      if (!WinHttpQueryHeaders(
              http_request.get(),
              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
              WINHTTP_HEADER_NAME_BY_INDEX,
              &status_code,
              &status_bytes,
              WINHTTP_NO_HEADER_INDEX)) {
        return clonecore::Result<AdkStagedPayloadReceipt>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::query_failed,
                L"WinHTTP ADK取得 HTTP状態取得",
                GetLastError()));
      }
      if (is_redirect_status(status_code)) {
        const auto location = query_redirect_location(http_request.get());
        if (!location) {
          return clonecore::Result<AdkStagedPayloadReceipt>::failure(
              location.error());
        }
        if (index + 1U >= policy.value().ordered_exact_urls.size() ||
            location.value() !=
                policy.value().ordered_exact_urls[index + 1U]) {
          return failure<AdkStagedPayloadReceipt>(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_NAME,
              L"ADK公式取得 リダイレクト固定値検証",
              L"応答のLocationが次の固定Microsoft URLと完全一致しません");
        }
        continue;
      }
      if (status_code != HTTP_STATUS_OK) {
        return failure<AdkStagedPayloadReceipt>(
            clonecore::ErrorCode::io_failed,
            ERROR_INVALID_DATA,
            L"ADK公式取得 HTTP状態検証",
            L"固定配布先がHTTP 200を返しませんでした");
      }
      std::vector<std::byte> buffer(kIoBlockBytes);
      for (;;) {
        DWORD read{};
        if (!WinHttpReadData(
                http_request.get(),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read)) {
          return clonecore::Result<AdkStagedPayloadReceipt>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::io_failed,
                  L"WinHTTP ADK取得 body読取り",
                  GetLastError()));
        }
        if (read == 0U) {
          break;
        }
        if (read > request.maximum_bytes ||
            total > request.maximum_bytes - read) {
          return failure<AdkStagedPayloadReceipt>(
              clonecore::ErrorCode::verification_failed,
              ERROR_FILE_TOO_LARGE,
              L"ADK公式取得 容量上限",
              L"応答が固定取得物の最大長を超えました");
        }
        DWORD written{};
        if (!WriteFile(
                destination_file.value().get(),
                buffer.data(),
                read,
                &written,
                nullptr) ||
            written != read) {
          return clonecore::Result<AdkStagedPayloadReceipt>::failure(
              clonecore::make_win32_error(
                  clonecore::ErrorCode::io_failed,
                  L"ADK公式取得 新規ファイル書込み",
                  GetLastError()));
        }
        total += read;
      }
      effective_url = current_url;
      downloaded = true;
      break;
    }
    if (!downloaded || total == 0U) {
      return failure<AdkStagedPayloadReceipt>(
          clonecore::ErrorCode::verification_failed,
          ERROR_HANDLE_EOF,
          L"ADK公式取得 完了検証",
          L"固定URL列の終端までに空でないHTTP 200取得物を確認できません");
    }
    if (!FlushFileBuffers(destination_file.value().get())) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ADK公式取得 書込み確定",
              GetLastError()));
    }
    const auto observed = observe_handle(
        destination_file.value().get(), L"ADK公式取得 保存結果識別");
    if (!observed || !regular_non_reparse(observed.value()) ||
        observed.value().link_count != 1U ||
        observed.value().byte_count != total) {
      return failure<AdkStagedPayloadReceipt>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK公式取得 保存結果識別",
          L"新規通常ファイルの識別、リンク数、または長さが一致しません");
    }
    track_file(*stage, destination.value(), observed.value().identity);
    pending.release();
    return clonecore::Result<AdkStagedPayloadReceipt>::success(
        AdkStagedPayloadReceipt{
            .staged_path = destination.value(),
            .byte_count = total,
            .created_new = true,
            .source_regular_file = true,
            .source_reparse_point = false,
            .visited_urls = std::move(visited),
            .effective_url = std::move(effective_url),
        });
  }

  [[nodiscard]] clonecore::Result<AdkStagedPayloadReceipt>
  stage_offline_payload(const AdkOfflineStageRequest& request) {
    std::scoped_lock lock(mutex_);
    const auto destination = full_local_path(
        request.create_new_destination,
        L"ADKオフライン取得 保存先正規化");
    if (!destination) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          destination.error());
    }
    StageRecord* const stage = find_stage_for_destination(
        destination.value());
    if (stage == nullptr) {
      return failure<AdkStagedPayloadReceipt>(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_OWNER,
          L"ADKオフライン取得 保存先所有確認",
          L"このアダプターが新規作成した一時領域の直下ではありません");
    }
    const auto scope = validate_windows_adk_offline_stage_scope(
        request, stage->root);
    if (!scope) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          scope.error());
    }
    const auto layout_root = full_local_path(
        request.layout_root, L"ADKオフラインレイアウト正規化");
    if (!layout_root) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          layout_root.error());
    }
    const auto source = full_local_path(
        layout_root.value() / request.exact_relative_path,
        L"ADKオフライン取得元正規化");
    if (!source) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          source.error());
    }
    if (!path_equal(
            source.value(),
            layout_root.value() / request.exact_relative_path)) {
      return failure<AdkStagedPayloadReceipt>(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_NAME,
          L"ADKオフライン取得元 範囲検証",
          L"固定相対パスが選択レイアウト外へ解決されました");
    }
    auto chain = require_existing_chain_without_reparse(
        layout_root.value(), true, L"ADKオフラインレイアウト経路検査");
    if (!chain) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          chain.error());
    }
    chain = require_existing_chain_without_reparse(
        source.value(), true, L"ADKオフライン取得元経路検査");
    if (!chain) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          chain.error());
    }
    const auto locked_directories = lock_offline_directory_chain(
        layout_root.value(), request.exact_relative_path.parent_path());
    if (!locked_directories) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          locked_directories.error());
    }
    static_cast<void>(locked_directories);
    clonecore::UniqueHandle source_handle(CreateFileW(
        source.value().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!source_handle) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ADKオフライン取得元 固定読取り",
              GetLastError()));
    }
    const auto source_before = observe_handle(
        source_handle.get(), L"ADKオフライン取得元 識別");
    if (!source_before || !regular_non_reparse(source_before.value()) ||
        source_before.value().link_count != 1U ||
        source_before.value().byte_count == 0U ||
        source_before.value().byte_count > request.maximum_bytes) {
      return failure<AdkStagedPayloadReceipt>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADKオフライン取得元 識別",
          L"通常の単一リンク取得物ではないか、固定長上限外です");
    }
    auto destination_file = create_new_destination(
        *stage, destination.value());
    if (!destination_file) {
      return clonecore::Result<AdkStagedPayloadReceipt>::failure(
          destination_file.error());
    }
    PendingFile pending(destination_file.value().get());
    std::vector<std::byte> buffer(kIoBlockBytes);
    std::uint64_t total{};
    for (;;) {
      DWORD read{};
      if (!ReadFile(
              source_handle.get(),
              buffer.data(),
              static_cast<DWORD>(buffer.size()),
              &read,
              nullptr)) {
        return clonecore::Result<AdkStagedPayloadReceipt>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"ADKオフライン取得元 読取り",
                GetLastError()));
      }
      if (read == 0U) {
        break;
      }
      if (read > request.maximum_bytes ||
          total > request.maximum_bytes - read) {
        return failure<AdkStagedPayloadReceipt>(
            clonecore::ErrorCode::verification_failed,
            ERROR_FILE_TOO_LARGE,
            L"ADKオフライン取得 容量上限",
            L"取得元が固定最大長を超えました");
      }
      DWORD written{};
      if (!WriteFile(
              destination_file.value().get(),
              buffer.data(),
              read,
              &written,
              nullptr) ||
          written != read) {
        return clonecore::Result<AdkStagedPayloadReceipt>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"ADKオフライン取得 新規ファイル書込み",
                GetLastError()));
      }
      total += read;
    }
    if (total != source_before.value().byte_count ||
        !FlushFileBuffers(destination_file.value().get())) {
      return failure<AdkStagedPayloadReceipt>(
          clonecore::ErrorCode::io_failed,
          ERROR_WRITE_FAULT,
          L"ADKオフライン取得 完全コピー検証",
          L"固定取得元の全バイトを新規一時ファイルへ確定できません");
    }
    const auto source_after = observe_handle(
        source_handle.get(), L"ADKオフライン取得元 再識別");
    const auto destination_observation = observe_handle(
        destination_file.value().get(), L"ADKオフライン保存結果 識別");
    if (!source_after ||
        source_after.value().identity != source_before.value().identity ||
        source_after.value().byte_count != source_before.value().byte_count ||
        !destination_observation ||
        !regular_non_reparse(destination_observation.value()) ||
        destination_observation.value().link_count != 1U ||
        destination_observation.value().byte_count != total) {
      return failure<AdkStagedPayloadReceipt>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADKオフライン取得 読戻し識別",
          L"取得元または新規保存先の識別と長さがコピー中に変化しました");
    }
    track_file(
        *stage,
        destination.value(),
        destination_observation.value().identity);
    pending.release();
    return clonecore::Result<AdkStagedPayloadReceipt>::success(
        AdkStagedPayloadReceipt{
            .staged_path = destination.value(),
            .offline_source_path = source.value(),
            .byte_count = total,
            .created_new = true,
            .source_regular_file = true,
            .source_reparse_point = false,
        });
  }

  [[nodiscard]] clonecore::Result<std::string> sha256_file(
      const std::filesystem::path& path,
      const std::uint64_t maximum_bytes) {
    if (maximum_bytes == 0U || maximum_bytes > kMaximumPayloadBytes) {
      return failure<std::string>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"ADK取得物 SHA-256上限",
          L"SHA-256読取り上限が製品境界外です");
    }
    std::scoped_lock lock(mutex_);
    const auto opened = open_owned_file(path);
    if (!opened) {
      return clonecore::Result<std::string>::failure(opened.error());
    }
    if (opened.value().observation.byte_count == 0U ||
        opened.value().observation.byte_count > maximum_bytes) {
      return failure<std::string>(
          clonecore::ErrorCode::verification_failed,
          ERROR_FILE_TOO_LARGE,
          L"ADK取得物 SHA-256上限",
          L"所有ファイル長が固定最大長外です");
    }
    return sha256_locked_handle(
        opened.value().handle.get(),
        opened.value().observation.byte_count);
  }

  [[nodiscard]] clonecore::Status verify_authenticode(
      const std::filesystem::path& path,
      const std::wstring_view expected_signer_subject) {
    if (expected_signer_subject != kMicrosoftSigner) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          static_cast<DWORD>(TRUST_E_SUBJECT_NOT_TRUSTED),
          L"ADK取得物 署名者固定値",
          L"許可署名者はMicrosoft Corporationだけです");
    }
    std::scoped_lock lock(mutex_);
    const auto opened = open_owned_file(path);
    if (!opened) {
      return clonecore::Status::failure(opened.error());
    }
    return trust_verifier_->verify_microsoft_signed(
        opened.value().canonical_path.native());
  }

  [[nodiscard]] clonecore::Result<std::wstring> query_payload_version(
      const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    const auto opened = open_owned_file(path);
    if (!opened) {
      return clonecore::Result<std::wstring>::failure(opened.error());
    }
    return query_file_version_unlocked(opened.value().canonical_path);
  }

  [[nodiscard]] clonecore::Result<std::vector<AdkVerifiedPayload>>
  expand_and_verify_patch_archive(
      const AdkPatchArchiveExpandRequest& request) {
    if (request.archive.kind != AdkPayloadKind::servicing_update ||
        request.archive.installer_kind !=
            AdkInstallerKind::windows_installer_patch_archive_zip ||
        request.archive.byte_count == 0U ||
        request.archive.byte_count > kMaximumPayloadBytes ||
        !is_upper_sha256(request.archive.sha256) ||
        !request.archive.signer_subject.empty() ||
        !request.archive.payload_version.empty() ||
        !request.archive.msp_revision_guid.empty()) {
      return failure<std::vector<AdkVerifiedPayload>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"ADK更新ZIP 展開要求",
          L"外側ZIPの固定長、Hash、形式、または未署名属性が不正です");
    }
    std::scoped_lock lock(mutex_);
    const auto opened_archive = open_owned_file(request.archive.staged_path);
    if (!opened_archive) {
      return clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
          opened_archive.error());
    }
    if (opened_archive.value().observation.byte_count !=
        request.archive.byte_count) {
      return failure<std::vector<AdkVerifiedPayload>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADK更新ZIP 展開直前長さ再検証",
          L"所有するZIPの長さが取得直後から変化しました");
    }
    const auto archive_hash = sha256_locked_handle(
        opened_archive.value().handle.get(),
        opened_archive.value().observation.byte_count);
    if (!archive_hash || archive_hash.value() != request.archive.sha256) {
      return archive_hash
          ? failure<std::vector<AdkVerifiedPayload>>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"ADK更新ZIP 展開直前SHA-256再検証",
                L"所有するZIPの内容が取得直後の固定Hashと一致しません")
          : clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
                archive_hash.error());
    }
    StageRecord* const stage = find_stage_for_destination(
        opened_archive.value().canonical_path);
    if (stage == nullptr) {
      return failure<std::vector<AdkVerifiedPayload>>(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_OWNER,
          L"ADK更新ZIP 展開先所有確認",
          L"ZIPと同じCREATE_NEW所有一時領域を確認できません");
    }
    MappedReadOnlyFile mapping;
    const auto mapped = mapping.map(
        opened_archive.value().handle.get(),
        opened_archive.value().observation.byte_count);
    if (!mapped) {
      return clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
          mapped.error());
    }
    const auto inspection = inspect_adk_patch_archive(
        mapping.bytes(), request.members);
    if (!inspection) {
      return clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
          inspection.error());
    }

    std::vector<std::filesystem::path> extracted_paths;
    extracted_paths.reserve(request.members.size());
    for (std::size_t index = 0; index < request.members.size(); ++index) {
      const auto destination = full_local_path(
          stage->root / request.members[index].staging_file_name,
          L"ADK更新MSP 展開先正規化");
      if (!destination ||
          !is_direct_child(stage->root, destination.value())) {
        return destination
            ? failure<std::vector<AdkVerifiedPayload>>(
                  clonecore::ErrorCode::access_denied,
                  ERROR_INVALID_NAME,
                  L"ADK更新MSP 展開先範囲",
                  L"固定MSP保存名が所有一時領域直下ではありません")
            : clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
                  destination.error());
      }
      auto output = create_new_destination(*stage, destination.value());
      if (!output) {
        return clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
            output.error());
      }
      PendingFile pending(output.value().get());
      const auto extracted = extract_adk_patch_archive_entry(
          mapping.bytes(),
          inspection.value().entries[index],
          [&output](const std::span<const std::byte> chunk) {
            std::size_t consumed{};
            while (consumed < chunk.size()) {
              const DWORD amount = static_cast<DWORD>(
                  std::min<std::size_t>(
                      chunk.size() - consumed,
                      std::numeric_limits<DWORD>::max()));
              DWORD written{};
              if (!WriteFile(
                      output.value().get(),
                      chunk.data() + consumed,
                      amount,
                      &written,
                      nullptr) ||
                  written != amount) {
                return clonecore::Status::failure(
                    clonecore::make_win32_error(
                        clonecore::ErrorCode::io_failed,
                        L"ADK更新MSP CREATE_NEW展開書込み",
                        GetLastError()));
              }
              consumed += written;
            }
            return clonecore::success_status();
          });
      if (!extracted || !FlushFileBuffers(output.value().get())) {
        return extracted
            ? clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
                  clonecore::make_win32_error(
                      clonecore::ErrorCode::io_failed,
                      L"ADK更新MSP 展開書込み確定",
                      GetLastError()))
            : clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
                  extracted.error());
      }
      const auto observed = observe_handle(
          output.value().get(), L"ADK更新MSP 展開結果識別");
      if (!observed || !regular_non_reparse(observed.value()) ||
          observed.value().link_count != 1U ||
          observed.value().byte_count !=
              request.members[index].expected_byte_count) {
        return failure<std::vector<AdkVerifiedPayload>>(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_DATA,
            L"ADK更新MSP 展開結果境界",
            L"CREATE_NEW通常単一リンクMSPの固定長を確認できません");
      }
      track_file(*stage, destination.value(), observed.value().identity);
      pending.release();
      extracted_paths.push_back(destination.value());
    }

    std::vector<AdkVerifiedPayload> verified;
    verified.reserve(request.members.size());
    for (std::size_t index = 0; index < request.members.size(); ++index) {
      const auto opened = open_owned_file(extracted_paths[index]);
      if (!opened) {
        return clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
            opened.error());
      }
      const auto hash = sha256_locked_handle(
          opened.value().handle.get(),
          opened.value().observation.byte_count);
      if (!hash || hash.value() != request.members[index].expected_sha256) {
        return hash
            ? failure<std::vector<AdkVerifiedPayload>>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"ADK更新MSP SHA-256検証",
                  L"展開したMSPの全体Hashが固定値と一致しません")
            : clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
                  hash.error());
      }
      const auto signature =
          verify_exact_microsoft_windows_patch_signature(
              opened.value().canonical_path);
      if (!signature) {
        return clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
            signature.error());
      }
      const auto revision = query_msp_revision_guid_unlocked(
          opened.value().canonical_path);
      if (!revision ||
          revision.value() != request.members[index].expected_revision_guid) {
        return revision
            ? failure<std::vector<AdkVerifiedPayload>>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_REVISION_MISMATCH,
                  L"ADK更新MSP Revision GUID検証",
                  L"MSP SummaryInformationのRevision GUIDが固定値と一致しません")
            : clonecore::Result<std::vector<AdkVerifiedPayload>>::failure(
                  revision.error());
      }
      const auto after = observe_handle(
          opened.value().handle.get(), L"ADK更新MSP 検証後再識別");
      if (!after || after.value().identity != opened.value().observation.identity ||
          after.value().byte_count != opened.value().observation.byte_count ||
          !regular_non_reparse(after.value()) || after.value().link_count != 1U) {
        return failure<std::vector<AdkVerifiedPayload>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"ADK更新MSP 検証後再識別",
            L"署名/Revision検証中にMSPの識別または内容が変化しました");
      }
      verified.push_back(AdkVerifiedPayload{
          .kind = AdkPayloadKind::servicing_update,
          .installer_kind = AdkInstallerKind::windows_installer_patch_msp,
          .staged_path = extracted_paths[index],
          .byte_count = opened.value().observation.byte_count,
          .sha256 = hash.value(),
          .signer_subject = request.members[index].expected_signer_subject,
          .payload_version = L"",
          .msp_revision_guid = revision.value(),
      });
    }
    return clonecore::Result<std::vector<AdkVerifiedPayload>>::success(
        std::move(verified));
  }

  [[nodiscard]] clonecore::Result<WindowsAdkEulaExtractionResult>
  extract_verified_embedded_eula(
      const AdkPinnedPayload& pinned_bootstrap,
      const AdkVerifiedPayload& verified_bootstrap,
      const AdkEmbeddedEulaPin& pin) {
    const auto identity = validate_windows_adk_eula_source_identity(
        pinned_bootstrap, verified_bootstrap, pin);
    if (!identity) {
      return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
          identity.error());
    }
    std::scoped_lock lock(mutex_);
    const auto opened = open_owned_file(verified_bootstrap.staged_path);
    if (!opened) {
      return clonecore::Result<WindowsAdkEulaExtractionResult>::failure(
          opened.error());
    }
    if (opened.value().observation.byte_count !=
        pinned_bootstrap.expected_byte_count) {
      return failure<WindowsAdkEulaExtractionResult>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"ADK EULA bootstrap 所有長再照合",
          L"CREATE_NEW時に追跡したbootstrapの長さが固定値と一致しません");
    }
    return detail::extract_windows_adk_eula_from_verified_owned_handle(
        opened.value().handle.get(),
        opened.value().owned_stage_root,
        pinned_bootstrap,
        verified_bootstrap,
        pin);
  }

  [[nodiscard]] clonecore::Result<std::uint32_t>
  run_verified_silent_installer(const AdkSilentInstallRequest& request) {
    std::scoped_lock lock(mutex_);
    const auto opened = open_owned_file(request.payload.staged_path);
    if (!opened) {
      return clonecore::Result<std::uint32_t>::failure(opened.error());
    }
    WindowsAdkPrelaunchObservation observation{
        .owned_staged_identity_matches = true,
        .payload_regular_non_reparse =
            regular_non_reparse(opened.value().observation),
        .payload_single_link =
            opened.value().observation.link_count == 1U,
        .payload_byte_count = opened.value().observation.byte_count,
    };
    const auto hash = sha256_locked_handle(
        opened.value().handle.get(),
        opened.value().observation.byte_count);
    if (!hash) {
      return clonecore::Result<std::uint32_t>::failure(hash.error());
    }
    observation.payload_sha256 = hash.value();
    const bool msp = request.payload.installer_kind ==
                     AdkInstallerKind::windows_installer_patch_msp;
    const auto signature = msp
        ? verify_exact_microsoft_windows_patch_signature(
              opened.value().canonical_path)
        : trust_verifier_->verify_microsoft_signed(
              opened.value().canonical_path.native());
    observation.payload_signature_trusted =
        static_cast<bool>(signature);
    if (!signature) {
      return clonecore::Result<std::uint32_t>::failure(signature.error());
    }
    observation.payload_signer_subject =
        std::wstring(msp ? kMicrosoftPatchSigner : kMicrosoftSigner);
    if (msp) {
      const auto revision = query_msp_revision_guid_unlocked(
          opened.value().canonical_path);
      if (!revision) {
        return clonecore::Result<std::uint32_t>::failure(revision.error());
      }
      observation.msp_revision_guid = revision.value();
    } else {
      const auto version =
          query_file_version_unlocked(opened.value().canonical_path);
      if (!version) {
        return clonecore::Result<std::uint32_t>::failure(version.error());
      }
      observation.payload_version = version.value();
    }

    const auto system_directory = system_directory_path();
    if (!system_directory) {
      return clonecore::Result<std::uint32_t>::failure(
          system_directory.error());
    }
    clonecore::UniqueHandle handler_handle;
    if (request.payload.installer_kind !=
        AdkInstallerKind::microsoft_bootstrap_exe) {
      const auto handler_path =
          system_directory.value() /
          (request.payload.installer_kind ==
                   AdkInstallerKind::windows_update_msu
               ? L"wusa.exe"
               : L"msiexec.exe");
      const auto handler_chain = require_existing_chain_without_reparse(
          handler_path, true, L"ADK導入 System32ハンドラー経路検査");
      if (!handler_chain) {
        return clonecore::Result<std::uint32_t>::failure(
            handler_chain.error());
      }
      handler_handle.reset(CreateFileW(
          handler_path.c_str(),
          GENERIC_READ,
          FILE_SHARE_READ,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT,
          nullptr));
      if (!handler_handle) {
        return clonecore::Result<std::uint32_t>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"ADK導入 System32ハンドラー固定",
                GetLastError()));
      }
      const auto handler_observation = observe_handle(
          handler_handle.get(), L"ADK導入 System32ハンドラー識別");
      observation.system_handler_regular_non_reparse =
          handler_observation &&
          regular_non_reparse(handler_observation.value());
      if (!observation.system_handler_regular_non_reparse) {
        return failure<std::uint32_t>(
            clonecore::ErrorCode::verification_failed,
            ERROR_REPARSE_TAG_INVALID,
            L"ADK導入 System32ハンドラー識別",
            L"通常の非reparse System32ハンドラーではありません");
      }
      const auto handler_trust =
          trust_verifier_->verify_microsoft_signed(handler_path.native());
      observation.system_handler_signature_trusted =
          static_cast<bool>(handler_trust);
      if (!handler_trust) {
        return clonecore::Result<std::uint32_t>::failure(
            handler_trust.error());
      }
    }

    AdkSilentInstallRequest normalized_request = request;
    normalized_request.payload.staged_path = opened.value().canonical_path;
    const auto plan = validate_and_build_windows_adk_launch_plan(
        normalized_request, system_directory.value(), observation);
    if (!plan) {
      return clonecore::Result<std::uint32_t>::failure(plan.error());
    }
    return launcher_->launch_and_wait(plan.value());
  }

  [[nodiscard]] clonecore::Status remove_staging_area(
      const AdkStagingArea& staging) {
    if (!staging.created_new || staging.reparse_point) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"ADK一時領域 削除対象検証",
          L"この操作で新規作成した非reparse一時領域ではありません");
    }
    std::scoped_lock lock(mutex_);
    const auto canonical = full_local_path(
        staging.root, L"ADK一時領域 削除対象正規化");
    if (!canonical) {
      return clonecore::Status::failure(canonical.error());
    }
    auto found = stages_.find(canonical.value().native());
    if (found == stages_.end()) {
      return status_failure(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_OWNER,
          L"ADK一時領域 削除所有確認",
          L"このアダプターが所有する一時領域ではありません");
    }
    StageRecord& stage = found->second;
    const auto root_observation = observe_handle(
        stage.root_handle.get(), L"ADK一時領域 削除前再識別");
    if (!root_observation ||
        root_observation.value().identity != stage.identity ||
        !root_observation.value().directory ||
        root_observation.value().reparse_point) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADK一時領域 削除前再識別",
          L"所有していた一時領域と一致しないため削除しません");
    }

    auto file = stage.files.begin();
    while (file != stage.files.end()) {
      clonecore::UniqueHandle handle(CreateFileW(
          file->first.c_str(),
          FILE_READ_ATTRIBUTES | DELETE,
          FILE_SHARE_READ,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_OPEN_REPARSE_POINT,
          nullptr));
      if (!handle) {
        const DWORD native_code = GetLastError();
        if (native_code == ERROR_FILE_NOT_FOUND ||
            native_code == ERROR_PATH_NOT_FOUND) {
          file = stage.files.erase(file);
          continue;
        }
        return clonecore::Status::failure(clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"ADK一時取得物 削除固定",
            native_code));
      }
      const auto observed = observe_handle(
          handle.get(), L"ADK一時取得物 削除前再識別");
      if (!observed || !regular_non_reparse(observed.value()) ||
          observed.value().identity != file->second) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"ADK一時取得物 削除前再識別",
            L"所有していた新規ファイルと一致しないため削除しません");
      }
      const auto removed = mark_handle_for_delete(
          handle.get(), L"ADK一時取得物 安全削除");
      if (!removed) {
        return removed;
      }
      handle.reset();
      file = stage.files.erase(file);
    }
    const auto removed = mark_handle_for_delete(
        stage.root_handle.get(), L"ADK一時領域 安全削除");
    if (!removed) {
      return removed;
    }
    stage.root_handle.reset();
    stages_.erase(found);
    return clonecore::success_status();
  }

 private:
  struct CaseInsensitiveLess final {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
      return CompareStringOrdinal(
                 left.data(),
                 static_cast<int>(left.size()),
                 right.data(),
                 static_cast<int>(right.size()),
                 TRUE) == CSTR_LESS_THAN;
    }
  };

  struct StageRecord final {
    std::filesystem::path root;
    FileIdentity identity;
    clonecore::UniqueHandle root_handle;
    std::map<std::filesystem::path, FileIdentity> files;
  };

  struct OwnedFile final {
    std::filesystem::path canonical_path;
    std::filesystem::path owned_stage_root;
    clonecore::UniqueHandle handle;
    FileObservation observation;
  };

  class PendingFile final {
   public:
    explicit PendingFile(const HANDLE handle) noexcept : handle_(handle) {}
    ~PendingFile() {
      if (active_ && handle_ != nullptr &&
          handle_ != INVALID_HANDLE_VALUE) {
        (void)mark_handle_for_delete(
            handle_, L"ADK未完了一時取得物 安全削除");
      }
    }
    PendingFile(const PendingFile&) = delete;
    PendingFile& operator=(const PendingFile&) = delete;
    void release() noexcept { active_ = false; }

   private:
    HANDLE handle_{};
    bool active_{true};
  };

  [[nodiscard]] StageRecord* find_stage_for_destination(
      const std::filesystem::path& destination) {
    for (auto& [unused, stage] : stages_) {
      static_cast<void>(unused);
      if (is_direct_child(stage.root, destination)) {
        if (!stage_path_still_matches(stage)) {
          return nullptr;
        }
        return &stage;
      }
    }
    return nullptr;
  }

  [[nodiscard]] bool stage_path_still_matches(StageRecord& stage) {
    const auto owned = observe_handle(
        stage.root_handle.get(), L"ADK一時領域 所有Handle再識別");
    if (!owned || owned.value().identity != stage.identity ||
        !owned.value().directory || owned.value().reparse_point) {
      return false;
    }
    clonecore::UniqueHandle reopened(CreateFileW(
        stage.root.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!reopened) {
      return false;
    }
    const auto current = observe_handle(
        reopened.get(), L"ADK一時領域 Path再識別");
    return current && current.value().identity == stage.identity &&
           current.value().directory && !current.value().reparse_point;
  }

  [[nodiscard]] clonecore::Result<clonecore::UniqueHandle>
  create_new_destination(
      StageRecord& stage,
      const std::filesystem::path& destination) {
    if (!is_direct_child(stage.root, destination) ||
        stage.files.contains(destination)) {
      return failure<clonecore::UniqueHandle>(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_OWNER,
          L"ADK一時取得物 新規保存先",
          L"所有一時領域直下の未使用ファイル名ではありません");
    }
    clonecore::UniqueHandle handle(CreateFileW(
        destination.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      return clonecore::Result<clonecore::UniqueHandle>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"CreateFileW(ADK一時取得物 CREATE_NEW)",
              GetLastError()));
    }
    const auto observed = observe_handle(
        handle.get(), L"ADK一時取得物 新規識別");
    if (!observed || !regular_non_reparse(observed.value()) ||
        observed.value().link_count != 1U ||
        observed.value().byte_count != 0U) {
      (void)mark_handle_for_delete(
          handle.get(), L"ADK不正一時取得物 安全削除");
      return failure<clonecore::UniqueHandle>(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"ADK一時取得物 新規識別",
          L"空の通常単一リンクファイルをCREATE_NEWで確保できません");
    }
    return clonecore::Result<clonecore::UniqueHandle>::success(
        std::move(handle));
  }

  void track_file(
      StageRecord& stage,
      const std::filesystem::path& path,
      const FileIdentity& identity) {
    stage.files.emplace(path, identity);
  }

  [[nodiscard]] clonecore::Result<OwnedFile> open_owned_file(
      const std::filesystem::path& path) {
    const auto canonical = full_local_path(
        path, L"ADK一時取得物 所有パス正規化");
    if (!canonical) {
      return clonecore::Result<OwnedFile>::failure(canonical.error());
    }
    const FileIdentity* expected_identity{};
    std::filesystem::path owned_stage_root;
    for (auto& [unused, stage] : stages_) {
      static_cast<void>(unused);
      const auto found = stage.files.find(canonical.value());
      if (found != stage.files.end()) {
        if (!stage_path_still_matches(stage)) {
          return failure<OwnedFile>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"ADK一時領域 Path再識別",
              L"所有一時領域の現在Pathが作成時識別と一致しません");
        }
        expected_identity = &found->second;
        owned_stage_root = stage.root;
        break;
      }
    }
    if (expected_identity == nullptr) {
      return failure<OwnedFile>(
          clonecore::ErrorCode::access_denied,
          ERROR_INVALID_OWNER,
          L"ADK一時取得物 所有確認",
          L"このアダプターがCREATE_NEWで作成したファイルではありません");
    }
    clonecore::UniqueHandle handle(CreateFileW(
        canonical.value().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      return clonecore::Result<OwnedFile>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"ADK一時取得物 読取り固定",
              GetLastError()));
    }
    const auto observed = observe_handle(
        handle.get(), L"ADK一時取得物 所有再識別");
    if (!observed || !regular_non_reparse(observed.value()) ||
        observed.value().link_count != 1U ||
        observed.value().identity != *expected_identity) {
      return failure<OwnedFile>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"ADK一時取得物 所有再識別",
          L"CREATE_NEW時の所有識別と一致しない通常ファイルです");
    }
    return clonecore::Result<OwnedFile>::success(OwnedFile{
        .canonical_path = canonical.value(),
        .owned_stage_root = std::move(owned_stage_root),
        .handle = std::move(handle),
        .observation = observed.value(),
    });
  }

  std::mutex mutex_;
  std::map<std::wstring, StageRecord, CaseInsensitiveLess> stages_;
  std::unique_ptr<IWindowsAdkProcessLauncher> launcher_;
  std::unique_ptr<bootrepair::IExecutableTrustVerifier> trust_verifier_;
};

WindowsAdkAcquisitionPlatform::WindowsAdkAcquisitionPlatform(
    std::unique_ptr<IWindowsAdkProcessLauncher> launcher)
    : impl_(std::make_unique<Impl>(std::move(launcher))) {}

WindowsAdkAcquisitionPlatform::~WindowsAdkAcquisitionPlatform() = default;

clonecore::Result<AdkInstalledState>
WindowsAdkAcquisitionPlatform::inspect_installed_state(
    const AdkReleaseManifest& manifest) {
  return impl_->inspect_installed_state(manifest);
}

clonecore::Result<AdkStagingArea>
WindowsAdkAcquisitionPlatform::create_new_staging_area(
    const std::uint64_t maximum_total_bytes) {
  return impl_->create_new_staging_area(maximum_total_bytes);
}

clonecore::Result<AdkStagedPayloadReceipt>
WindowsAdkAcquisitionPlatform::download_to_new_file(
    const AdkDownloadRequest& request) {
  return impl_->download_to_new_file(request);
}

clonecore::Result<AdkStagedPayloadReceipt>
WindowsAdkAcquisitionPlatform::stage_offline_payload(
    const AdkOfflineStageRequest& request) {
  return impl_->stage_offline_payload(request);
}

clonecore::Result<std::string> WindowsAdkAcquisitionPlatform::sha256_file(
    const std::filesystem::path& path,
    const std::uint64_t maximum_bytes) {
  return impl_->sha256_file(path, maximum_bytes);
}

clonecore::Status WindowsAdkAcquisitionPlatform::verify_authenticode(
    const std::filesystem::path& path,
    const std::wstring_view expected_signer_subject) {
  return impl_->verify_authenticode(path, expected_signer_subject);
}

clonecore::Result<std::wstring>
WindowsAdkAcquisitionPlatform::query_payload_version(
    const std::filesystem::path& path) {
  return impl_->query_payload_version(path);
}

clonecore::Result<std::vector<AdkVerifiedPayload>>
WindowsAdkAcquisitionPlatform::expand_and_verify_patch_archive(
    const AdkPatchArchiveExpandRequest& request) {
  return impl_->expand_and_verify_patch_archive(request);
}

clonecore::Result<WindowsAdkEulaExtractionResult>
WindowsAdkAcquisitionPlatform::extract_verified_embedded_eula(
    const AdkPinnedPayload& pinned_bootstrap,
    const AdkVerifiedPayload& verified_bootstrap,
    const AdkEmbeddedEulaPin& pin) {
  return impl_->extract_verified_embedded_eula(
      pinned_bootstrap, verified_bootstrap, pin);
}

clonecore::Result<std::uint32_t>
WindowsAdkAcquisitionPlatform::run_verified_silent_installer(
    const AdkSilentInstallRequest& request) {
  return impl_->run_verified_silent_installer(request);
}

clonecore::Status WindowsAdkAcquisitionPlatform::remove_staging_area(
    const AdkStagingArea& staging) {
  return impl_->remove_staging_area(staging);
}

std::unique_ptr<IAdkAcquisitionPlatform>
make_windows_adk_acquisition_platform() {
  return std::make_unique<WindowsAdkAcquisitionPlatform>();
}

}  // namespace ytec::windowsapp

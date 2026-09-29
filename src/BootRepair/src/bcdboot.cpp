#include "ytec/bootrepair/bcdboot.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <Softpub.h>
#include <Wincrypt.h>
#include <Wintrust.h>
#include <Mscat.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::Status;
using clonecore::UniqueHandle;

constexpr std::size_t kMaximumCapturedBytes = 4U * 1024U * 1024U;

Error boot_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

bool is_drive_absolute_path(const std::wstring& path) {
  return path.size() >= 3 && std::iswalpha(path[0]) != 0 && path[1] == L':' &&
         (path[2] == L'\\' || path[2] == L'/');
}

bool has_forbidden_path_text(const std::wstring& path) {
  if (path.find(L'"') != std::wstring::npos ||
      path.find(L'\r') != std::wstring::npos ||
      path.find(L'\n') != std::wstring::npos ||
      path.find(L'*') != std::wstring::npos ||
      path.find(L'?') != std::wstring::npos) {
    return true;
  }
  std::wstring normalized = path;
  std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
  return normalized.find(L"\\..\\") != std::wstring::npos ||
         normalized.ends_with(L"\\..") || normalized.starts_with(L"..\\");
}

std::wstring normalize_slashes(std::wstring value) {
  std::replace(value.begin(), value.end(), L'/', L'\\');
  while (value.size() > 3 && value.back() == L'\\') {
    value.pop_back();
  }
  return value;
}

bool ends_with_case_insensitive(
    const std::wstring& value,
    const std::wstring_view suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }
  const std::wstring_view ending(
      value.data() + value.size() - suffix.size(), suffix.size());
  return _wcsnicmp(ending.data(), suffix.data(), suffix.size()) == 0;
}

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

Status validate_system_directory(const std::wstring& directory) {
  if (!is_drive_absolute_path(directory) || has_forbidden_path_text(directory) ||
      !ends_with_case_insensitive(
          normalize_slashes(directory), L"\\Windows\\System32")) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"BCDBootシステムディレクトリ",
        L"現在のWindowsまたはWinPEのSystem32絶対パスが必要です"));
  }
  return clonecore::success_status();
}

Status validate_bcdboot_paths(const BcdBootRequest& request) {
  const std::wstring windows_directory =
      normalize_slashes(request.target_windows_directory);
  const std::wstring system_root =
      normalize_slashes(request.target_system_partition_root);
  if (!is_drive_absolute_path(windows_directory) ||
      has_forbidden_path_text(windows_directory) ||
      !ends_with_case_insensitive(windows_directory, L"\\Windows")) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"BCDBoot対象Windowsディレクトリ",
        L"コピー先のドライブ絶対パスで終端がWindowsのディレクトリだけを指定できます"));
  }
  if (system_root.size() != 3 || std::iswalpha(system_root[0]) == 0 ||
      system_root[1] != L':' || system_root[2] != L'\\') {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"BCDBoot対象システムパーティション",
        L"コピー先システムパーティションのドライブ文字ルートが必要です"));
  }
  if (request.firmware != BcdBootFirmware::uefi &&
      request.firmware != BcdBootFirmware::bios) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"BCDBootファームウェア方式",
        L"UEFIまたはBIOSだけを指定できます"));
  }
  if (request.store_policy !=
          BcdBootStorePolicy::preserve_existing &&
      request.store_policy !=
          BcdBootStorePolicy::rebuild_fresh) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"BCDBootストア方針",
        L"既存保持または新規再構築だけを指定できます"));
  }
  if (request.firmware == BcdBootFirmware::uefi &&
      std::towupper(windows_directory[0]) == std::towupper(system_root[0])) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_DRIVE,
        L"BCDBoot対象分離",
        L"WindowsパーティションとEFIシステムパーティションは別ドライブ文字で指定してください"));
  }
  return clonecore::success_status();
}

Result<std::string> read_pipe_to_string(
    const HANDLE pipe,
    const ProcessOutputCallback& callback = {}) {
  std::string output;
  std::array<char, 4096> buffer{};
  for (;;) {
    DWORD bytes_read = 0;
    const BOOL success = ReadFile(
        pipe,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytes_read,
        nullptr);
    if (!success) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_BROKEN_PIPE) {
        break;
      }
      return Result<std::string>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed, L"外部ツール出力の読取り", native_code));
    }
    if (bytes_read == 0) {
      break;
    }
    if (output.size() > kMaximumCapturedBytes - bytes_read) {
      return Result<std::string>::failure(boot_error(
          ErrorCode::invalid_data,
          ERROR_BUFFER_OVERFLOW,
          L"外部ツール出力上限",
          L"Microsoft標準ツールの出力が安全な取得上限を超えました"));
    }
    output.append(buffer.data(), bytes_read);
    if (callback) {
      try {
        callback(std::string_view(buffer.data(), bytes_read));
      } catch (...) {
        // Process capture must remain reliable even if a UI observer fails.
      }
    }
  }
  return Result<std::string>::success(std::move(output));
}

template <typename Function>
Function load_wintrust_function(
    const HMODULE module,
    const char* const function_name) noexcept {
#pragma warning(push)
#pragma warning(disable : 4191)
  const auto function =
      reinterpret_cast<Function>(GetProcAddress(module, function_name));
#pragma warning(pop)
  return function;
}

class WintrustFunctions final {
 public:
  WintrustFunctions() noexcept
      : module_(LoadLibraryExW(
            L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    if (module_ == nullptr) {
      error_ = GetLastError();
      return;
    }
    acquire_context_ = load_wintrust_function<
        decltype(&CryptCATAdminAcquireContext2)>(
        module_, "CryptCATAdminAcquireContext2");
    calculate_hash_ = load_wintrust_function<
        decltype(&CryptCATAdminCalcHashFromFileHandle2)>(
        module_, "CryptCATAdminCalcHashFromFileHandle2");
    enumerate_catalog_ = load_wintrust_function<
        decltype(&CryptCATAdminEnumCatalogFromHash)>(
        module_, "CryptCATAdminEnumCatalogFromHash");
    catalog_information_ = load_wintrust_function<
        decltype(&CryptCATCatalogInfoFromContext)>(
        module_, "CryptCATCatalogInfoFromContext");
    release_catalog_ = load_wintrust_function<
        decltype(&CryptCATAdminReleaseCatalogContext)>(
        module_, "CryptCATAdminReleaseCatalogContext");
    release_context_ = load_wintrust_function<
        decltype(&CryptCATAdminReleaseContext)>(
        module_, "CryptCATAdminReleaseContext");
    provider_data_ = load_wintrust_function<
        decltype(&WTHelperProvDataFromStateData)>(
        module_, "WTHelperProvDataFromStateData");
    provider_signer_ = load_wintrust_function<
        decltype(&WTHelperGetProvSignerFromChain)>(
        module_, "WTHelperGetProvSignerFromChain");
    if (!available()) {
      error_ = ERROR_PROC_NOT_FOUND;
    }
  }

  ~WintrustFunctions() {
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  WintrustFunctions(const WintrustFunctions&) = delete;
  WintrustFunctions& operator=(const WintrustFunctions&) = delete;

  [[nodiscard]] bool available() const noexcept {
    return module_ != nullptr && acquire_context_ != nullptr &&
           calculate_hash_ != nullptr && enumerate_catalog_ != nullptr &&
           catalog_information_ != nullptr && release_catalog_ != nullptr &&
           release_context_ != nullptr && provider_data_ != nullptr &&
           provider_signer_ != nullptr;
  }

  [[nodiscard]] DWORD error() const noexcept { return error_; }

  decltype(&CryptCATAdminAcquireContext2) acquire_context_{};
  decltype(&CryptCATAdminCalcHashFromFileHandle2) calculate_hash_{};
  decltype(&CryptCATAdminEnumCatalogFromHash) enumerate_catalog_{};
  decltype(&CryptCATCatalogInfoFromContext) catalog_information_{};
  decltype(&CryptCATAdminReleaseCatalogContext) release_catalog_{};
  decltype(&CryptCATAdminReleaseContext) release_context_{};
  decltype(&WTHelperProvDataFromStateData) provider_data_{};
  decltype(&WTHelperGetProvSignerFromChain) provider_signer_{};

 private:
  HMODULE module_{};
  DWORD error_{ERROR_SUCCESS};
};

void close_trust_state(WINTRUST_DATA& trust_data) noexcept {
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
  (void)WinVerifyTrust(nullptr, &policy, &trust_data);
}

Status verify_microsoft_provider_signer(
    const WintrustFunctions& functions,
    const WINTRUST_DATA& trust_data) {
  auto* const provider = functions.provider_data_(trust_data.hWVTStateData);
  auto* const signer = provider == nullptr
      ? nullptr
      : functions.provider_signer_(provider, 0, FALSE, 0);
  if (signer == nullptr || signer->csCertChain == 0 ||
      signer->pasCertChain == nullptr ||
      signer->pasCertChain[0].pCert == nullptr) {
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"Microsoft標準ツール署名者情報",
        L"信頼プロバイダーから署名証明書を取得できません"));
  }

  const PCCERT_CONTEXT certificate = signer->pasCertChain[0].pCert;
  const DWORD organization_length = CertGetNameStringW(
      certificate,
      CERT_NAME_ATTR_TYPE,
      0,
      const_cast<char*>(szOID_ORGANIZATION_NAME),
      nullptr,
      0);
  std::vector<wchar_t> organization(organization_length, L'\0');
  if (organization_length <= 1 ||
      CertGetNameStringW(
          certificate,
          CERT_NAME_ATTR_TYPE,
          0,
          const_cast<char*>(szOID_ORGANIZATION_NAME),
          organization.data(),
          static_cast<DWORD>(organization.size())) != organization_length) {
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"Microsoft標準ツール署名者名",
        L"署名証明書の組織名を取得できません"));
  }

  constexpr std::wstring_view kMicrosoftOrganization =
      L"Microsoft Corporation";
  const std::wstring organization_name(organization.data());
  if (organization_name.size() != kMicrosoftOrganization.size() ||
      _wcsicmp(organization_name.c_str(), kMicrosoftOrganization.data()) != 0) {
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        static_cast<DWORD>(TRUST_E_SUBJECT_NOT_TRUSTED),
        L"Microsoft標準ツール署名者",
        L"有効な署名ですが組織名がMicrosoft Corporationではありません"));
  }
  return clonecore::success_status();
}

std::wstring hash_member_tag(const std::span<const BYTE> hash) {
  constexpr wchar_t kHexadecimal[] = L"0123456789ABCDEF";
  std::wstring tag;
  tag.reserve(hash.size() * 2U);
  for (const BYTE value : hash) {
    tag.push_back(kHexadecimal[value >> 4U]);
    tag.push_back(kHexadecimal[value & 0x0FU]);
  }
  return tag;
}

Status verify_catalog_signature(
    const std::wstring& executable_path,
    const WintrustFunctions& functions) {
  UniqueHandle executable(CreateFileW(
      executable_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!executable) {
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::verification_failed,
        L"Microsoft標準ツールカタログ対象ファイル",
        GetLastError()));
  }

  HCATADMIN catalog_admin = nullptr;
  GUID catalog_subsystem = DRIVER_ACTION_VERIFY;
  if (!functions.acquire_context_(
          &catalog_admin, &catalog_subsystem, nullptr, nullptr, 0)) {
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::verification_failed,
        L"Microsoft標準ツールカタログ管理コンテキスト",
        GetLastError()));
  }

  DWORD hash_size = 0;
  if (!functions.calculate_hash_(
          catalog_admin, executable.get(), &hash_size, nullptr, 0) ||
      hash_size == 0 || hash_size > 128) {
    const DWORD native_code = GetLastError();
    functions.release_context_(catalog_admin, 0);
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        native_code == ERROR_SUCCESS ? ERROR_INVALID_DATA : native_code,
        L"Microsoft標準ツールカタログハッシュサイズ",
        L"カタログ照合用ハッシュのサイズを安全に取得できません"));
  }
  std::vector<BYTE> hash(hash_size);
  if (!functions.calculate_hash_(
          catalog_admin,
          executable.get(),
          &hash_size,
          hash.data(),
          0)) {
    const DWORD native_code = GetLastError();
    functions.release_context_(catalog_admin, 0);
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::verification_failed,
        L"Microsoft標準ツールカタログハッシュ",
        native_code));
  }

  HCATINFO catalog = functions.enumerate_catalog_(
      catalog_admin, hash.data(), hash_size, 0, nullptr);
  if (catalog == nullptr) {
    const DWORD native_code = GetLastError();
    functions.release_context_(catalog_admin, 0);
    return Status::failure(boot_error(
        ErrorCode::verification_failed,
        native_code == ERROR_SUCCESS
            ? static_cast<DWORD>(TRUST_E_NOSIGNATURE)
            : native_code,
        L"Microsoft標準ツールカタログ検索",
        L"対象ファイルを含むWindows署名カタログがありません"));
  }

  CATALOG_INFO catalog_info{};
  catalog_info.cbStruct = sizeof(catalog_info);
  if (!functions.catalog_information_(catalog, &catalog_info, 0)) {
    const DWORD native_code = GetLastError();
    functions.release_catalog_(catalog_admin, catalog, 0);
    functions.release_context_(catalog_admin, 0);
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::verification_failed,
        L"Microsoft標準ツールカタログ情報",
        native_code));
  }

  const std::wstring member_tag = hash_member_tag(hash);
  WINTRUST_CATALOG_INFO member{};
  member.cbStruct = sizeof(member);
  member.pcwszCatalogFilePath = catalog_info.wszCatalogFile;
  member.pcwszMemberTag = member_tag.c_str();
  member.pcwszMemberFilePath = executable_path.c_str();
  member.hMemberFile = executable.get();
  member.pbCalculatedFileHash = hash.data();
  member.cbCalculatedFileHash = hash_size;
  member.hCatAdmin = catalog_admin;

  WINTRUST_DATA trust_data{};
  trust_data.cbStruct = sizeof(trust_data);
  trust_data.dwUIChoice = WTD_UI_NONE;
  trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust_data.dwUnionChoice = WTD_CHOICE_CATALOG;
  trust_data.pCatalog = &member;
  trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
  trust_data.dwProvFlags =
      WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG trust_status = WinVerifyTrust(nullptr, &policy, &trust_data);
  const Status signer_status = trust_status == ERROR_SUCCESS
      ? verify_microsoft_provider_signer(functions, trust_data)
      : Status::failure(boot_error(
            ErrorCode::verification_failed,
            static_cast<DWORD>(trust_status),
            L"Microsoft標準ツールカタログ署名検証",
            L"対象ファイルのWindowsカタログ署名を信頼できません"));
  close_trust_state(trust_data);
  functions.release_catalog_(catalog_admin, catalog, 0);
  functions.release_context_(catalog_admin, 0);
  return signer_status;
}

class WindowsAuthenticodeVerifier final : public IExecutableTrustVerifier {
 public:
  Status verify_microsoft_signed(
      const std::wstring& executable_path) override {
    const DWORD attributes = GetFileAttributesW(executable_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return Status::failure(boot_error(
          ErrorCode::invalid_data,
          GetLastError(),
          L"Microsoft標準ツール実行ファイル属性",
          L"通常ファイルとして存在するMicrosoft標準ツールを確認できません"));
    }

    const WintrustFunctions functions;
    if (!functions.available()) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          functions.error(),
          L"Microsoft標準ツール署名検証API",
          L"Windows署名検証APIを安全に読み込めません"));
    }

    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = executable_path.c_str();
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
    if (trust_status == ERROR_SUCCESS) {
      const Status signer_status =
          verify_microsoft_provider_signer(functions, trust_data);
      close_trust_state(trust_data);
      return signer_status;
    }
    close_trust_state(trust_data);
    if (trust_status != TRUST_E_NOSIGNATURE) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          static_cast<DWORD>(trust_status),
          L"Microsoft標準ツールAuthenticode検証",
          L"対象ファイルの埋込みデジタル署名を信頼できません"));
    }
    return verify_catalog_signature(executable_path, functions);
  }
};

class WindowsProcessRunner final : public IProcessRunner {
 public:
  explicit WindowsProcessRunner(const DWORD timeout_milliseconds)
      : timeout_milliseconds_(timeout_milliseconds) {}

  Result<ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) override {
    return run_impl(
        executable_path, arguments, working_directory, {});
  }

  Result<ProcessResult> run_streamed(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory,
      const ProcessOutputCallback& standard_output_callback) override {
    return run_impl(
        executable_path,
        arguments,
        working_directory,
        standard_output_callback);
  }

 private:
  Result<ProcessResult> run_impl(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory,
      const ProcessOutputCallback& standard_output_callback) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE stdout_read_raw = nullptr;
    HANDLE stdout_write_raw = nullptr;
    HANDLE stderr_read_raw = nullptr;
    HANDLE stderr_write_raw = nullptr;
    if (!CreatePipe(&stdout_read_raw, &stdout_write_raw, &security, 0) ||
        !CreatePipe(&stderr_read_raw, &stderr_write_raw, &security, 0)) {
      if (stdout_read_raw != nullptr) {
        CloseHandle(stdout_read_raw);
      }
      if (stdout_write_raw != nullptr) {
        CloseHandle(stdout_write_raw);
      }
      if (stderr_read_raw != nullptr) {
        CloseHandle(stderr_read_raw);
      }
      if (stderr_write_raw != nullptr) {
        CloseHandle(stderr_write_raw);
      }
      return Result<ProcessResult>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"Microsoft標準ツール出力パイプ作成",
          GetLastError()));
    }
    UniqueHandle stdout_read(stdout_read_raw);
    UniqueHandle stdout_write(stdout_write_raw);
    UniqueHandle stderr_read(stderr_read_raw);
    UniqueHandle stderr_write(stderr_write_raw);
    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
      return Result<ProcessResult>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"Microsoft標準ツールパイプ継承設定",
          GetLastError()));
    }

    std::wstring command_line = quote_windows_argument(executable_path);
    for (const auto& argument : arguments) {
      command_line.push_back(L' ');
      command_line.append(quote_windows_argument(argument));
    }
    std::vector<wchar_t> mutable_command(
        command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write.get();
    startup.hStdError = stderr_write.get();
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(
            executable_path.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            working_directory.c_str(),
            &startup,
            &process_info)) {
      return Result<ProcessResult>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"Microsoft標準ツールプロセス起動",
          GetLastError()));
    }
    UniqueHandle process(process_info.hProcess);
    UniqueHandle thread(process_info.hThread);
    stdout_write.reset();
    stderr_write.reset();

    Result<std::string> stdout_result =
        Result<std::string>::success(std::string{});
    Result<std::string> stderr_result =
        Result<std::string>::success(std::string{});
    std::thread stdout_thread([&] {
      stdout_result = read_pipe_to_string(
          stdout_read.get(), standard_output_callback);
    });
    std::thread stderr_thread([&] {
      stderr_result = read_pipe_to_string(stderr_read.get());
    });
    const DWORD wait_result =
        WaitForSingleObject(process.get(), timeout_milliseconds_);
    if (wait_result == WAIT_TIMEOUT) {
      TerminateProcess(process.get(), ERROR_TIMEOUT);
      WaitForSingleObject(process.get(), 30'000);
    }
    stdout_thread.join();
    stderr_thread.join();
    if (wait_result != WAIT_OBJECT_0) {
      return Result<ProcessResult>::failure(boot_error(
          ErrorCode::io_failed,
          wait_result == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError(),
          L"Microsoft標準ツールプロセス待機",
          L"Microsoft標準ツールが制限時間内に正常終了しませんでした"));
    }
    if (!stdout_result) {
      return Result<ProcessResult>::failure(stdout_result.error());
    }
    if (!stderr_result) {
      return Result<ProcessResult>::failure(stderr_result.error());
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code)) {
      return Result<ProcessResult>::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"Microsoft標準ツール終了コード取得",
          GetLastError()));
    }
    return Result<ProcessResult>::success(ProcessResult{
        .exit_code = exit_code,
        .standard_output = stdout_result.take_value(),
        .standard_error = stderr_result.take_value(),
    });
  }

  DWORD timeout_milliseconds_{};
};

Result<BcdStoreFileIdentity> bcd_identity_from_handle(const HANDLE file) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_STANDARD_INFO standard{};
  FILE_ID_INFO id{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(
          file,
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) ||
      !GetFileInformationByHandleEx(
          file,
          FileStandardInfo,
          &standard,
          sizeof(standard)) ||
      !GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) ||
      !GetFileInformationByHandleEx(
          file, FileBasicInfo, &basic, sizeof(basic))) {
    return Result<BcdStoreFileIdentity>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"BCDストアhandle identity取得",
            GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      standard.NumberOfLinks != 1U || standard.DeletePending != FALSE ||
      standard.EndOfFile.QuadPart <= 0 ||
      basic.LastWriteTime.QuadPart < 0 || basic.ChangeTime.QuadPart < 0) {
    return Result<BcdStoreFileIdentity>::failure(boot_error(
        ErrorCode::invalid_data,
        ERROR_REPARSE_TAG_INVALID,
        L"BCDストアhandle identity検証",
        L"通常ファイル、非reparse、単一hard-link、非削除待ちのBCDストアだけを扱えます"));
  }
  BcdStoreFileIdentity identity{
      .volume_serial_number = id.VolumeSerialNumber,
      .length = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .last_write_time =
          static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart),
      .change_time = static_cast<std::uint64_t>(basic.ChangeTime.QuadPart),
  };
  static_assert(
      sizeof(id.FileId.Identifier) ==
      std::tuple_size_v<decltype(identity.file_id)>);
  std::memcpy(
      identity.file_id.data(),
      id.FileId.Identifier,
      identity.file_id.size());
  return Result<BcdStoreFileIdentity>::success(std::move(identity));
}

bool same_bcd_store_file_object(
    const BcdStoreFileIdentity& left,
    const BcdStoreFileIdentity& right) noexcept {
  return left.volume_serial_number == right.volume_serial_number &&
      left.file_id == right.file_id && left.length == right.length;
}

struct BcdStorePathParts final {
  std::wstring parent;
  std::wstring name;
};

Result<BcdStorePathParts> split_bcd_store_path(
    const std::wstring& raw_path) {
  const std::wstring path = normalize_slashes(raw_path);
  const std::size_t separator = path.rfind(L'\\');
  if (!is_drive_absolute_path(path) || has_forbidden_path_text(path) ||
      separator == std::wstring::npos || separator < 2U ||
      separator + 1U >= path.size()) {
    return Result<BcdStorePathParts>::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"BCDストアhandle-bound path分解",
        L"drive absoluteの通常ファイルpathだけをrename対象にできます"));
  }
  std::wstring name = path.substr(separator + 1U);
  if (name == L"." || name == L".." || name.size() > 255U ||
      name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
    return Result<BcdStorePathParts>::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"BCDストアhandle-bound leaf name",
        L"rename先は安全な単一relative nameである必要があります"));
  }
  std::wstring parent = path.substr(0U, separator);
  if (parent.size() == 2U) {
    parent.push_back(L'\\');
  }
  return Result<BcdStorePathParts>::success(BcdStorePathParts{
      .parent = std::move(parent),
      .name = std::move(name),
  });
}

Result<UniqueHandle> open_bcd_parent_directory(
    const std::wstring& path) {
  UniqueHandle directory(CreateFileW(
      path.c_str(),
      FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  if (!directory) {
    return Result<UniqueHandle>::failure(clonecore::make_win32_error(
        ErrorCode::io_failed,
        L"BCDストアrename親directory open",
        GetLastError()));
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_ID_INFO identifier{};
  if (GetFileInformationByHandleEx(
          directory.get(),
          FileAttributeTagInfo,
          &attributes,
          sizeof(attributes)) == FALSE ||
      GetFileInformationByHandleEx(
          directory.get(),
          FileIdInfo,
          &identifier,
          sizeof(identifier)) == FALSE) {
    return Result<UniqueHandle>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"BCDストアrename親directory identity",
        GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      identifier.VolumeSerialNumber == 0U) {
    return Result<UniqueHandle>::failure(boot_error(
        ErrorCode::invalid_data,
        ERROR_REPARSE_TAG_INVALID,
        L"BCDストアrename親directory type",
        L"同一volumeの通常・非reparse directoryだけをrename親にできます"));
  }
  return Result<UniqueHandle>::success(std::move(directory));
}

Result<std::uint64_t> bcd_handle_volume_serial(const HANDLE handle) {
  FILE_ID_INFO identifier{};
  if (GetFileInformationByHandleEx(
          handle,
          FileIdInfo,
          &identifier,
          sizeof(identifier)) == FALSE) {
    return Result<std::uint64_t>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"BCDストアrename volume identity",
            GetLastError()));
  }
  return Result<std::uint64_t>::success(identifier.VolumeSerialNumber);
}

Status rename_bcd_handle_no_replace(
    const HANDLE source,
    const HANDLE destination_parent,
    const std::wstring_view destination_name) {
  if (source == nullptr || source == INVALID_HANDLE_VALUE ||
      destination_parent == nullptr ||
      destination_parent == INVALID_HANDLE_VALUE ||
      destination_name.empty() || destination_name.size() > 255U) {
    return Status::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"BCDストアhandle-bound rename引数",
        L"source/destination handleまたはrelative nameが不正です"));
  }
  const DWORD name_bytes = static_cast<DWORD>(
      destination_name.size() * sizeof(wchar_t));
  std::vector<std::byte> buffer(
      offsetof(FILE_RENAME_INFO, FileName) + name_bytes);
  auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
  rename->ReplaceIfExists = FALSE;
  rename->RootDirectory = destination_parent;
  rename->FileNameLength = name_bytes;
  std::memcpy(rename->FileName, destination_name.data(), name_bytes);
  if (SetFileInformationByHandle(
          source,
          FileRenameInfo,
          rename,
          static_cast<DWORD>(buffer.size())) == FALSE) {
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::io_failed,
        L"BCDストアhandle-bound no-replace rename",
        GetLastError()));
  }
  return clonecore::success_status();
}

class WindowsBcdStoreFileSystem final : public IBcdStoreFileSystem {
 public:
  Result<bool> is_regular_non_reparse_file(
      const std::wstring& path) override {
    const auto identity = observe_regular_file_identity(path);
    if (!identity) {
      return Result<bool>::failure(identity.error());
    }
    return Result<bool>::success(identity.value().has_value());
  }

  Result<std::optional<BcdStoreFileIdentity>>
  observe_regular_file_identity(const std::wstring& path) override {
    UniqueHandle file(CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!file) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND ||
          native_code == ERROR_PATH_NOT_FOUND) {
        return Result<std::optional<BcdStoreFileIdentity>>::success(
            std::nullopt);
      }
      return Result<std::optional<BcdStoreFileIdentity>>::failure(
          clonecore::make_win32_error(
              ErrorCode::query_failed,
              L"BCDストアidentity確認",
              native_code));
    }
    auto identity = bcd_identity_from_handle(file.get());
    if (!identity) {
      return Result<std::optional<BcdStoreFileIdentity>>::failure(
          identity.error());
    }
    return Result<std::optional<BcdStoreFileIdentity>>::success(
        identity.take_value());
  }

  Status verify_bcd_store_read_only(
      const std::wstring& path) override {
    const auto regular = is_regular_non_reparse_file(path);
    if (!regular) {
      return Status::failure(regular.error());
    }
    if (!regular.value()) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          ERROR_FILE_NOT_FOUND,
          L"BCDストア読取り検証",
          L"検証対象のBCDストアがありません"));
    }

    HKEY raw_hive = nullptr;
    const LSTATUS loaded = RegLoadAppKeyW(
        path.c_str(), &raw_hive, KEY_READ, 0U, 0U);
    if (loaded != ERROR_SUCCESS || raw_hive == nullptr) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          static_cast<DWORD>(loaded),
          L"BCDストアhive読取り",
          L"新しいBCDを読取り専用registry hiveとして開けません"));
    }
    struct KeyCloser final {
      void operator()(HKEY__* key) const noexcept {
        if (key != nullptr) {
          (void)RegCloseKey(key);
        }
      }
    };
    std::unique_ptr<HKEY__, KeyCloser> hive(raw_hive);

    HKEY raw_objects = nullptr;
    const LSTATUS opened_objects = RegOpenKeyExW(
        hive.get(), L"Objects", 0U, KEY_READ, &raw_objects);
    if (opened_objects != ERROR_SUCCESS || raw_objects == nullptr) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          static_cast<DWORD>(opened_objects),
          L"BCD Objects読取り",
          L"新しいBCDにObjectsコンテナーを確認できません"));
    }
    std::unique_ptr<HKEY__, KeyCloser> objects(raw_objects);
    DWORD subkey_count = 0U;
    const LSTATUS queried = RegQueryInfoKeyW(
        objects.get(),
        nullptr,
        nullptr,
        nullptr,
        &subkey_count,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr);
    if (queried != ERROR_SUCCESS || subkey_count == 0U) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          static_cast<DWORD>(queried),
          L"BCD Objects列挙",
          L"新しいBCDに起動オブジェクトを確認できません"));
    }

    HKEY raw_boot_manager = nullptr;
    constexpr wchar_t kBootManagerObject[] =
        L"{9dea862c-5cdd-4e70-acc1-f32b344d4795}";
    const LSTATUS opened_boot_manager = RegOpenKeyExW(
        objects.get(),
        kBootManagerObject,
        0U,
        KEY_READ,
        &raw_boot_manager);
    if (opened_boot_manager != ERROR_SUCCESS ||
        raw_boot_manager == nullptr) {
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          static_cast<DWORD>(opened_boot_manager),
          L"Windows Boot Manager読取り",
          L"新しいBCDにWindows Boot Managerオブジェクトを確認できません"));
    }
    std::unique_ptr<HKEY__, KeyCloser> boot_manager(raw_boot_manager);
    return clonecore::success_status();
  }

  Status move_file_no_replace(
      const std::wstring& source,
      const std::wstring& destination,
      const BcdStoreFileIdentity& expected_source) override {
    auto source_parts = split_bcd_store_path(source);
    auto destination_parts = split_bcd_store_path(destination);
    if (!source_parts || !destination_parts) {
      return Status::failure(
          !source_parts ? source_parts.error() : destination_parts.error());
    }
    UniqueHandle source_handle(CreateFileW(
        source.c_str(),
        FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!source_handle) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"BCDストア移動元handle open",
          GetLastError()));
    }
    const auto source_identity =
        bcd_identity_from_handle(source_handle.get());
    if (!source_identity) {
      return Status::failure(source_identity.error());
    }
    if (!equivalent_bcd_store_file_identity(
            expected_source, source_identity.value())) {
      return Status::failure(boot_error(
          ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"BCDストア移動元identity",
          L"確認後にBCD移動元が差し替えまたは変更されました"));
    }
    const auto destination_identity =
        observe_regular_file_identity(destination);
    if (!destination_identity) {
      return Status::failure(destination_identity.error());
    }
    if (destination_identity.value().has_value()) {
      return Status::failure(boot_error(
          ErrorCode::invalid_data,
          ERROR_ALREADY_EXISTS,
          L"BCDストア移動先identity",
          L"BCD移動先が既に存在します"));
    }
    auto source_parent = open_bcd_parent_directory(
        source_parts.value().parent);
    auto destination_parent = open_bcd_parent_directory(
        destination_parts.value().parent);
    if (!source_parent || !destination_parent) {
      return Status::failure(
          !source_parent
              ? source_parent.error()
              : destination_parent.error());
    }
    auto source_parent_serial =
        bcd_handle_volume_serial(source_parent.value().get());
    auto destination_parent_serial =
        bcd_handle_volume_serial(destination_parent.value().get());
    if (!source_parent_serial || !destination_parent_serial ||
        source_parent_serial.value() != expected_source.volume_serial_number ||
        destination_parent_serial.value() !=
            expected_source.volume_serial_number) {
      return Status::failure(
          !source_parent_serial
              ? source_parent_serial.error()
              : !destination_parent_serial
                  ? destination_parent_serial.error()
                  : boot_error(
                        ErrorCode::identity_mismatch,
                        ERROR_NOT_SAME_DEVICE,
                        L"BCDストアrename親volume",
                        L"source fileと両親directoryが同一volumeではありません"));
    }

    const Status renamed = rename_bcd_handle_no_replace(
        source_handle.get(),
        destination_parent.value().get(),
        destination_parts.value().name);
    if (!renamed) {
      const auto source_after_failure =
          observe_regular_file_identity(source);
      const auto destination_after_failure =
          observe_regular_file_identity(destination);
      const bool source_unchanged = source_after_failure &&
          source_after_failure.value().has_value() &&
          equivalent_bcd_store_file_identity(
              expected_source,
              source_after_failure.value().value());
      const bool foreign_destination = destination_after_failure &&
          destination_after_failure.value().has_value() &&
          !same_bcd_store_file_object(
              expected_source,
              destination_after_failure.value().value());
      if (source_unchanged &&
          (foreign_destination ||
           (destination_after_failure &&
            !destination_after_failure.value().has_value()))) {
        return foreign_destination
            ? Status::failure(boot_error(
                  ErrorCode::invalid_data,
                  ERROR_ALREADY_EXISTS,
                  L"BCDストアrename先race",
                  L"no-replace rename直前にforeign destinationが現れたため変更せず停止しました"))
            : renamed;
      }
      return Status::failure(boot_error(
          ErrorCode::verification_failed,
          ERROR_REVISION_MISMATCH,
          L"BCDストアrename失敗後identity",
          L"rename失敗後に元名の完全不変を証明できません。rollback状態は要確認です"));
    }

    const auto moved_handle_identity =
        bcd_identity_from_handle(source_handle.get());
    const auto moved_identity = observe_regular_file_identity(destination);
    const auto source_after_move = observe_regular_file_identity(source);
    if (!moved_handle_identity || !moved_identity ||
        !moved_identity.value().has_value() || !source_after_move ||
        source_after_move.value().has_value() ||
        !same_bcd_store_file_object(
            expected_source, moved_handle_identity.value()) ||
        !equivalent_bcd_store_file_identity(
            moved_handle_identity.value(),
            moved_identity.value().value())) {
      const Status rolled_back = rename_bcd_handle_no_replace(
          source_handle.get(),
          source_parent.value().get(),
          source_parts.value().name);
      const auto restored = observe_regular_file_identity(source);
      const auto destination_after_rollback =
          observe_regular_file_identity(destination);
      const bool rollback_verified = rolled_back && restored &&
          restored.value().has_value() && destination_after_rollback &&
          !destination_after_rollback.value().has_value() &&
          same_bcd_store_file_object(
              expected_source, restored.value().value());
      return Status::failure(
          rollback_verified
              ? boot_error(
                    ErrorCode::identity_mismatch,
                    ERROR_DEVICE_NOT_CONNECTED,
                    L"BCDストア移動後identity",
                    L"移動後readbackが一致せず、同じhandleを元名へ戻して確認しました")
              : boot_error(
                    ErrorCode::verification_failed,
                    ERROR_REVISION_MISMATCH,
                    L"BCDストア移動後rollback",
                    L"移動後readback失敗に加え、同じhandleの元名復帰も完全確認できませんでした"));
    }
    return clonecore::success_status();
  }

  Status remove_file_if_identity_matches(
      const std::wstring& path,
      const BcdStoreFileIdentity& expected) override {
    UniqueHandle file(CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!file) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"BCDストア所有handle削除オープン",
          GetLastError()));
    }
    const auto observed = bcd_identity_from_handle(file.get());
    if (!observed) {
      return Status::failure(observed.error());
    }
    if (!equivalent_bcd_store_file_identity(expected, observed.value())) {
      return Status::failure(boot_error(
          ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"BCDストア所有handle削除identity",
          L"確認後にBCDファイルが差し替えまたは変更されたため削除しません"));
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(
            file.get(),
            FileDispositionInfo,
            &disposition,
            sizeof(disposition))) {
      return Status::failure(clonecore::make_win32_error(
          ErrorCode::io_failed,
          L"BCDストア所有handle削除",
          GetLastError()));
    }
    return clonecore::success_status();
  }
};

}  // namespace

bool equivalent_bcd_store_file_identity(
    const BcdStoreFileIdentity& left,
    const BcdStoreFileIdentity& right) noexcept {
  return left.volume_serial_number == right.volume_serial_number &&
      left.file_id == right.file_id && left.length == right.length &&
      left.last_write_time == right.last_write_time &&
      left.change_time == right.change_time;
}

Result<ProcessResult> IProcessRunner::run_streamed(
    const std::wstring& executable_path,
    const std::vector<std::wstring>& arguments,
    const std::wstring& working_directory,
    const ProcessOutputCallback& standard_output_callback) {
  auto result = run(executable_path, arguments, working_directory);
  if (result && standard_output_callback &&
      !result.value().standard_output.empty()) {
    try {
      standard_output_callback(result.value().standard_output);
    } catch (...) {
      // Observers cannot change a completed process result.
    }
  }
  return result;
}

Result<std::vector<std::wstring>> build_bcdboot_arguments(
    const BcdBootRequest& request) {
  const Status validation = validate_bcdboot_paths(request);
  if (!validation) {
    return Result<std::vector<std::wstring>>::failure(validation.error());
  }
  std::vector<std::wstring> arguments{
      normalize_slashes(request.target_windows_directory),
      L"/s",
      normalize_slashes(request.target_system_partition_root),
      L"/f",
      request.firmware == BcdBootFirmware::uefi ? L"UEFI" : L"BIOS",
      L"/v",
  };
  if (request.store_policy ==
      BcdBootStorePolicy::rebuild_fresh) {
    arguments.emplace_back(L"/c");
  }
  return Result<std::vector<std::wstring>>::success(
      std::move(arguments));
}

Result<std::wstring> bcd_store_path(const BcdBootRequest& request) {
  const Status validation = validate_bcdboot_paths(request);
  if (!validation) {
    return Result<std::wstring>::failure(validation.error());
  }
  const std::wstring root =
      normalize_slashes(request.target_system_partition_root);
  const std::wstring relative =
      request.firmware == BcdBootFirmware::uefi
          ? L"EFI\\Microsoft\\Boot\\BCD"
          : L"Boot\\BCD";
  return Result<std::wstring>::success(root + relative);
}

std::wstring quote_windows_argument(const std::wstring_view argument) {
  if (!argument.empty() &&
      argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }
  std::wstring quoted;
  quoted.push_back(L'"');
  std::size_t backslash_count = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslash_count;
      continue;
    }
    if (character == L'"') {
      quoted.append(backslash_count * 2 + 1, L'\\');
      quoted.push_back(L'"');
      backslash_count = 0;
      continue;
    }
    quoted.append(backslash_count, L'\\');
    backslash_count = 0;
    quoted.push_back(character);
  }
  quoted.append(backslash_count * 2, L'\\');
  quoted.push_back(L'"');
  return quoted;
}

Result<BcdBootReport> execute_bcdboot(
    const BcdBootRequest& request,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner) {
  const Status system_status =
      validate_system_directory(trusted_system_directory);
  if (!system_status) {
    return Result<BcdBootReport>::failure(system_status.error());
  }
  const auto arguments = build_bcdboot_arguments(request);
  if (!arguments) {
    return Result<BcdBootReport>::failure(arguments.error());
  }
  const std::wstring system_directory =
      normalize_slashes(trusted_system_directory);
  const std::wstring executable_path = system_directory + L"\\bcdboot.exe";
  const Status trust_status =
      trust_verifier.verify_microsoft_signed(executable_path);
  if (!trust_status) {
    return Result<BcdBootReport>::failure(trust_status.error());
  }
  const auto process = process_runner.run(
      executable_path, arguments.value(), system_directory);
  if (!process) {
    return Result<BcdBootReport>::failure(process.error());
  }
  if (process.value().exit_code != 0) {
    return Result<BcdBootReport>::failure(boot_error(
        ErrorCode::verification_failed,
        process.value().exit_code,
        L"BCDBoot終了コード",
        L"BCDBootが失敗終了したためコピー先を成功扱いにできません"));
  }
  return Result<BcdBootReport>::success(BcdBootReport{
      .executable_path = executable_path,
      .exit_code = process.value().exit_code,
      .standard_output = process.value().standard_output,
      .standard_error = process.value().standard_error,
      .microsoft_signature_verified = true,
  });
}

Result<BcdBootReport> execute_bcdboot_with_store_transaction(
    const BcdBootRequest& request,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IBcdStoreFileSystem& file_system) {
  if (request.store_policy != BcdBootStorePolicy::rebuild_fresh) {
    return execute_bcdboot(
        request,
        trusted_system_directory,
        trust_verifier,
        process_runner);
  }

  const Status system_status =
      validate_system_directory(trusted_system_directory);
  if (!system_status) {
    return Result<BcdBootReport>::failure(system_status.error());
  }
  const auto arguments = build_bcdboot_arguments(request);
  if (!arguments) {
    return Result<BcdBootReport>::failure(arguments.error());
  }
  const auto store_path_result = bcd_store_path(request);
  if (!store_path_result) {
    return Result<BcdBootReport>::failure(store_path_result.error());
  }
  const std::wstring system_directory =
      normalize_slashes(trusted_system_directory);
  const std::wstring executable_path = system_directory + L"\\bcdboot.exe";

  // The signed executable is verified before the target BCD is changed.
  const Status trust_status =
      trust_verifier.verify_microsoft_signed(executable_path);
  if (!trust_status) {
    return Result<BcdBootReport>::failure(trust_status.error());
  }

  const std::wstring store_path = store_path_result.value();
  const std::wstring backup_path = store_path + L".ytec-rebuild-backup";
  const auto existing = file_system.observe_regular_file_identity(store_path);
  if (!existing) {
    return Result<BcdBootReport>::failure(existing.error());
  }
  const auto backup_exists =
      file_system.observe_regular_file_identity(backup_path);
  if (!backup_exists) {
    return Result<BcdBootReport>::failure(backup_exists.error());
  }
  if (backup_exists.value().has_value()) {
    return Result<BcdBootReport>::failure(boot_error(
        ErrorCode::invalid_data,
        ERROR_ALREADY_EXISTS,
        L"既存BCDストア退避先確認",
        L"前回処理の退避ファイルが残っているため新しいBCDを作成できません"));
  }
  bool prior_store_moved = false;
  std::optional<BcdStoreFileIdentity> prior_store_backup_identity;
  if (existing.value().has_value()) {
    const Status moved =
        file_system.move_file_no_replace(
            store_path, backup_path, existing.value().value());
    if (!moved) {
      return Result<BcdBootReport>::failure(moved.error());
    }
    const auto moved_identity =
        file_system.observe_regular_file_identity(backup_path);
    if (!moved_identity || !moved_identity.value().has_value() ||
        !same_bcd_store_file_object(
            existing.value().value(), moved_identity.value().value())) {
      const Status restored =
          file_system.move_file_no_replace(
              backup_path,
              store_path,
              moved_identity && moved_identity.value().has_value()
                  ? moved_identity.value().value()
                  : existing.value().value());
      if (!restored) {
        return Result<BcdBootReport>::failure(restored.error());
      }
      return Result<BcdBootReport>::failure(
          moved_identity
              ? boot_error(
                    ErrorCode::identity_mismatch,
                    ERROR_DEVICE_NOT_CONNECTED,
                    L"既存BCDストア退避identity",
                    L"退避後のBCDが移動前と同じファイルではありません")
              : moved_identity.error());
    }
    prior_store_backup_identity = moved_identity.value().value();
    prior_store_moved = true;
  }

  const auto rollback_prior_store = [&](const std::optional<
                                      BcdStoreFileIdentity>&
                                          expected_replacement) -> Status {
    std::optional<BcdStoreFileIdentity> replacement =
        expected_replacement;
    if (!replacement.has_value()) {
      const auto observed =
          file_system.observe_regular_file_identity(store_path);
      if (!observed) {
        return Status::failure(observed.error());
      }
      replacement = observed.value();
    }
    if (replacement.has_value()) {
      const Status removed = file_system.remove_file_if_identity_matches(
          store_path, replacement.value());
      if (!removed) {
        return removed;
      }
    }
    if (prior_store_moved) {
      return file_system.move_file_no_replace(
          backup_path,
          store_path,
          prior_store_backup_identity.value());
    }
    return clonecore::success_status();
  };

  // Re-verify immediately before execution as well.  Target mutation cannot
  // weaken the executable trust boundary between preparation and launch.
  auto report = execute_bcdboot(
      request,
      trusted_system_directory,
      trust_verifier,
      process_runner);
  if (!report) {
    const Status rolled_back = rollback_prior_store(std::nullopt);
    if (!rolled_back) {
      return Result<BcdBootReport>::failure(rolled_back.error());
    }
    return report;
  }

  const auto fresh_store =
      file_system.observe_regular_file_identity(store_path);
  if (!fresh_store || !fresh_store.value().has_value()) {
    Error failure = fresh_store
        ? boot_error(
              ErrorCode::verification_failed,
              ERROR_FILE_NOT_FOUND,
              L"新規BCDストア検証",
              L"BCDBoot成功後に新しいBCDストアを確認できません")
        : fresh_store.error();
    const Status rolled_back = rollback_prior_store(std::nullopt);
    if (!rolled_back) {
      return Result<BcdBootReport>::failure(rolled_back.error());
    }
    return Result<BcdBootReport>::failure(std::move(failure));
  }
  const Status verified_store =
      file_system.verify_bcd_store_read_only(store_path);
  if (!verified_store) {
    const Status rolled_back = rollback_prior_store(
        fresh_store.value().value());
    if (!rolled_back) {
      return Result<BcdBootReport>::failure(rolled_back.error());
    }
    return Result<BcdBootReport>::failure(verified_store.error());
  }
  const auto verified_identity =
      file_system.observe_regular_file_identity(store_path);
  if (!verified_identity || !verified_identity.value().has_value() ||
      !equivalent_bcd_store_file_identity(
          fresh_store.value().value(),
          verified_identity.value().value())) {
    const Error failure = verified_identity
        ? boot_error(
              ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"新規BCDストア検証後identity",
              L"BCD hive検証中に対象ファイルが差し替えまたは変更されました")
        : verified_identity.error();
    const Status rolled_back = rollback_prior_store(
        fresh_store.value().value());
    if (!rolled_back) {
      return Result<BcdBootReport>::failure(rolled_back.error());
    }
    return Result<BcdBootReport>::failure(failure);
  }

  if (prior_store_moved) {
    const Status removed = file_system.remove_file_if_identity_matches(
        backup_path, prior_store_backup_identity.value());
    if (!removed) {
      return Result<BcdBootReport>::failure(removed.error());
    }
  }
  report.value().prior_store_replaced = prior_store_moved;
  report.value().fresh_store_verified = true;
  return report;
}

Result<MultiWindowsBcdBootReport>
execute_multi_windows_bcdboot_with_store_transaction(
    const std::vector<BcdBootRequest>& requests_in_boot_priority,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IBcdStoreFileSystem& file_system) {
  constexpr std::size_t kMaximumWindowsInstallations = 32U;
  if (requests_in_boot_priority.empty() ||
      requests_in_boot_priority.size() > kMaximumWindowsInstallations) {
    return Result<MultiWindowsBcdBootReport>::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"複数Windows BCD登録件数",
        L"1件以上32件以下のレビュー済みWindowsが必要です"));
  }
  const Status system_status =
      validate_system_directory(trusted_system_directory);
  if (!system_status) {
    return Result<MultiWindowsBcdBootReport>::failure(
        system_status.error());
  }

  const auto& first = requests_in_boot_priority.front();
  if (first.store_policy != BcdBootStorePolicy::rebuild_fresh) {
    return Result<MultiWindowsBcdBootReport>::failure(boot_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"複数Windows BCD初回方針",
        L"最優先Windowsは新規BCD再構築として指定する必要があります"));
  }
  const std::wstring system_root =
      normalize_slashes(first.target_system_partition_root);
  std::vector<std::wstring> windows_roots;
  windows_roots.reserve(requests_in_boot_priority.size());
  for (std::size_t index = 0U;
       index < requests_in_boot_priority.size(); ++index) {
    const auto& request = requests_in_boot_priority[index];
    const auto arguments = build_bcdboot_arguments(request);
    if (!arguments) {
      return Result<MultiWindowsBcdBootReport>::failure(
          arguments.error());
    }
    if (index != 0U &&
        request.store_policy != BcdBootStorePolicy::preserve_existing) {
      return Result<MultiWindowsBcdBootReport>::failure(boot_error(
          ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"複数Windows BCD追加方針",
          L"2件目以降のWindowsは既存BCD保持として指定する必要があります"));
    }
    if (request.firmware != first.firmware ||
        !equals_case_insensitive(
            normalize_slashes(request.target_system_partition_root),
            system_root)) {
      return Result<MultiWindowsBcdBootReport>::failure(boot_error(
          ErrorCode::identity_mismatch,
          ERROR_INVALID_DRIVE,
          L"複数Windows BCDシステム領域",
          L"すべてのWindowsは同じレビュー済みシステム領域と起動方式を使用する必要があります"));
    }
    const std::wstring windows_root =
        normalize_slashes(request.target_windows_directory);
    const bool duplicate = std::any_of(
        windows_roots.begin(),
        windows_roots.end(),
        [&](const auto& existing) {
          return equals_case_insensitive(existing, windows_root);
        });
    if (duplicate) {
      return Result<MultiWindowsBcdBootReport>::failure(boot_error(
          ErrorCode::invalid_argument,
          ERROR_DUP_NAME,
          L"複数Windows BCD登録対象",
          L"同じWindowsディレクトリを複数回登録できません"));
    }
    windows_roots.push_back(windows_root);
  }

  const auto store_path_result = bcd_store_path(first);
  if (!store_path_result) {
    return Result<MultiWindowsBcdBootReport>::failure(
        store_path_result.error());
  }
  const std::wstring normalized_system_directory =
      normalize_slashes(trusted_system_directory);
  const std::wstring executable_path =
      normalized_system_directory + L"\\bcdboot.exe";
  const Status initial_trust =
      trust_verifier.verify_microsoft_signed(executable_path);
  if (!initial_trust) {
    return Result<MultiWindowsBcdBootReport>::failure(
        initial_trust.error());
  }

  const std::wstring store_path = store_path_result.value();
  const std::wstring backup_path = store_path + L".ytec-rebuild-backup";
  const auto existing = file_system.observe_regular_file_identity(store_path);
  if (!existing) {
    return Result<MultiWindowsBcdBootReport>::failure(existing.error());
  }
  const auto backup_exists =
      file_system.observe_regular_file_identity(backup_path);
  if (!backup_exists) {
    return Result<MultiWindowsBcdBootReport>::failure(
        backup_exists.error());
  }
  if (backup_exists.value().has_value()) {
    return Result<MultiWindowsBcdBootReport>::failure(boot_error(
        ErrorCode::invalid_data,
        ERROR_ALREADY_EXISTS,
        L"複数Windows BCD退避先確認",
        L"前回処理の退避ファイルが残っているため開始できません"));
  }

  bool prior_store_moved = false;
  std::optional<BcdStoreFileIdentity> prior_store_backup_identity;
  if (existing.value().has_value()) {
    const Status moved =
        file_system.move_file_no_replace(
            store_path, backup_path, existing.value().value());
    if (!moved) {
      return Result<MultiWindowsBcdBootReport>::failure(moved.error());
    }
    const auto moved_identity =
        file_system.observe_regular_file_identity(backup_path);
    if (!moved_identity || !moved_identity.value().has_value() ||
        !same_bcd_store_file_object(
            existing.value().value(), moved_identity.value().value())) {
      const Status restored =
          file_system.move_file_no_replace(
              backup_path,
              store_path,
              moved_identity && moved_identity.value().has_value()
                  ? moved_identity.value().value()
                  : existing.value().value());
      if (!restored) {
        return Result<MultiWindowsBcdBootReport>::failure(
            restored.error());
      }
      return Result<MultiWindowsBcdBootReport>::failure(
          moved_identity
              ? boot_error(
                    ErrorCode::identity_mismatch,
                    ERROR_DEVICE_NOT_CONNECTED,
                    L"複数Windows BCD退避identity",
                    L"退避後のBCDが移動前と同じファイルではありません")
              : moved_identity.error());
    }
    prior_store_backup_identity = moved_identity.value().value();
    prior_store_moved = true;
  }

  const auto rollback = [&](const std::optional<BcdStoreFileIdentity>&
                                expected_replacement) -> Status {
    std::optional<BcdStoreFileIdentity> replacement =
        expected_replacement;
    if (!replacement.has_value()) {
      const auto observed =
          file_system.observe_regular_file_identity(store_path);
      if (!observed) {
        return Status::failure(observed.error());
      }
      replacement = observed.value();
    }
    if (replacement.has_value()) {
      const Status removed = file_system.remove_file_if_identity_matches(
          store_path, replacement.value());
      if (!removed) {
        return removed;
      }
    }
    if (prior_store_moved) {
      return file_system.move_file_no_replace(
          backup_path,
          store_path,
          prior_store_backup_identity.value());
    }
    return clonecore::success_status();
  };

  MultiWindowsBcdBootReport combined;
  combined.windows_registrations.reserve(
      requests_in_boot_priority.size());
  for (const auto& request : requests_in_boot_priority) {
    auto report = execute_bcdboot(
        request,
        trusted_system_directory,
        trust_verifier,
        process_runner);
    if (!report) {
      const Status rolled_back = rollback(std::nullopt);
      if (!rolled_back) {
        return Result<MultiWindowsBcdBootReport>::failure(
            rolled_back.error());
      }
      return Result<MultiWindowsBcdBootReport>::failure(report.error());
    }
    const auto store =
        file_system.observe_regular_file_identity(store_path);
    if (!store || !store.value().has_value()) {
      Error failure = store
          ? boot_error(
                ErrorCode::verification_failed,
                ERROR_FILE_NOT_FOUND,
                L"複数Windows BCD追加後検証",
                L"BCDBoot成功後に通常ファイルのBCDストアを確認できません")
          : store.error();
      const Status rolled_back = rollback(std::nullopt);
      if (!rolled_back) {
        return Result<MultiWindowsBcdBootReport>::failure(
            rolled_back.error());
      }
      return Result<MultiWindowsBcdBootReport>::failure(
          std::move(failure));
    }
    const Status verified_store =
        file_system.verify_bcd_store_read_only(store_path);
    if (!verified_store) {
      const Status rolled_back = rollback(store.value().value());
      if (!rolled_back) {
        return Result<MultiWindowsBcdBootReport>::failure(
            rolled_back.error());
      }
      return Result<MultiWindowsBcdBootReport>::failure(
          verified_store.error());
    }
    const auto verified_identity =
        file_system.observe_regular_file_identity(store_path);
    if (!verified_identity || !verified_identity.value().has_value() ||
        !equivalent_bcd_store_file_identity(
            store.value().value(), verified_identity.value().value())) {
      const Error failure = verified_identity
          ? boot_error(
                ErrorCode::identity_mismatch,
                ERROR_DEVICE_NOT_CONNECTED,
                L"複数Windows BCD検証後identity",
                L"BCD hive検証中に対象ファイルが差し替えまたは変更されました")
          : verified_identity.error();
      const Status rolled_back = rollback(store.value().value());
      if (!rolled_back) {
        return Result<MultiWindowsBcdBootReport>::failure(
            rolled_back.error());
      }
      return Result<MultiWindowsBcdBootReport>::failure(failure);
    }
    report.value().fresh_store_verified = true;
    combined.windows_registrations.push_back(report.take_value());
  }

  if (prior_store_moved) {
    const Status removed = file_system.remove_file_if_identity_matches(
        backup_path, prior_store_backup_identity.value());
    if (!removed) {
      return Result<MultiWindowsBcdBootReport>::failure(removed.error());
    }
  }
  combined.prior_store_replaced = prior_store_moved;
  combined.fresh_store_verified = true;
  return Result<MultiWindowsBcdBootReport>::success(
      std::move(combined));
}

Status verify_bcd_store_file_with_windows_apis(
    const std::wstring& path) {
  WindowsBcdStoreFileSystem file_system;
  return file_system.verify_bcd_store_read_only(path);
}

Result<MultiWindowsBcdBootReport>
execute_multi_windows_bcdboot_with_windows_apis(
    const std::vector<BcdBootRequest>& requests_in_boot_priority) {
  std::vector<wchar_t> system_directory(MAX_PATH, L'\0');
  const UINT length = GetSystemDirectoryW(
      system_directory.data(),
      static_cast<UINT>(system_directory.size()));
  if (length == 0U || length >= system_directory.size()) {
    return Result<MultiWindowsBcdBootReport>::failure(
        clonecore::make_win32_error(
            ErrorCode::internal_error,
            L"現在のSystem32パス取得",
            GetLastError()));
  }
  auto verifier = make_windows_authenticode_verifier();
  auto runner = make_windows_process_runner();
  if (verifier == nullptr || runner == nullptr) {
    return Result<MultiWindowsBcdBootReport>::failure(boot_error(
        ErrorCode::internal_error,
        ERROR_INVALID_STATE,
        L"複数Windows BCD実行基盤初期化",
        L"署名検証またはプロセス実行基盤を初期化できません"));
  }
  WindowsBcdStoreFileSystem file_system;
  return execute_multi_windows_bcdboot_with_store_transaction(
      requests_in_boot_priority,
      std::wstring(system_directory.data(), length),
      *verifier,
      *runner,
      file_system);
}

Result<BcdBootReport> execute_bcdboot_with_windows_apis(
    const BcdBootRequest& request) {
  std::vector<wchar_t> system_directory(MAX_PATH, L'\0');
  const UINT length = GetSystemDirectoryW(
      system_directory.data(), static_cast<UINT>(system_directory.size()));
  if (length == 0 || length >= system_directory.size()) {
    return Result<BcdBootReport>::failure(clonecore::make_win32_error(
        ErrorCode::internal_error,
        L"現在のSystem32パス取得",
        GetLastError()));
  }
  auto verifier = make_windows_authenticode_verifier();
  auto runner = make_windows_process_runner();
  WindowsBcdStoreFileSystem file_system;
  return execute_bcdboot_with_store_transaction(
      request,
      std::wstring(system_directory.data(), length),
      *verifier,
      *runner,
      file_system);
}

std::unique_ptr<IExecutableTrustVerifier>
make_windows_authenticode_verifier() {
  return std::make_unique<WindowsAuthenticodeVerifier>();
}

std::unique_ptr<IProcessRunner> make_windows_process_runner(
    const std::uint32_t timeout_milliseconds) {
  const DWORD bounded_timeout =
      (std::max)(1U, (std::min)(
                         timeout_milliseconds,
                         4U * 60U * 60U * 1000U));
  return std::make_unique<WindowsProcessRunner>(bounded_timeout);
}

std::unique_ptr<IBcdStoreFileSystem>
make_windows_bcd_store_file_system() {
  return std::make_unique<WindowsBcdStoreFileSystem>();
}

}  // namespace ytec::bootrepair

#include "ytec/windowsapp/media_creation.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/imageformat/sha256.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#ifndef YTEC_MEDIA_BUILDER_SHA256
#error YTEC_MEDIA_BUILDER_SHA256 must be supplied by CMake
#endif

#ifndef YTEC_MEDIA_BUILDER_SIZE
#error YTEC_MEDIA_BUILDER_SIZE must be supplied by CMake
#endif

namespace ytec::windowsapp {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::Status;
using clonecore::UniqueHandle;

constexpr std::uint64_t kMaximumIsoBytes =
    32ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kHashBlockBytes = 4U * 1024U * 1024U;
constexpr std::uint32_t kMediaBuildTimeoutMilliseconds =
    60U * 60U * 1000U;
constexpr std::uint64_t kExpectedMediaBuilderLength =
    YTEC_MEDIA_BUILDER_SIZE;
constexpr std::string_view kExpectedMediaBuilderSha256 =
    YTEC_MEDIA_BUILDER_SHA256;
constexpr std::size_t kMaximumDisplayedBuilderOutput = 1'600U;

Error media_error(
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

void report_progress(
    const RescueMediaCreationCallbacks& callbacks,
    const RescueMediaCreationStage stage,
    const std::uint8_t percent,
    std::wstring message,
    const bool cancellation_allowed) noexcept {
  if (!callbacks.progress) {
    return;
  }
  try {
    callbacks.progress(RescueMediaCreationProgress{
        .stage = stage,
        .percent = percent,
        .message = std::move(message),
        .cancellation_allowed = cancellation_allowed,
    });
  } catch (...) {
    // UI observation must never unwind through a media operation.
  }
}

bool cancellation_requested(
    const RescueMediaCreationCallbacks& callbacks) noexcept {
  if (!callbacks.cancellation_requested) {
    return false;
  }
  try {
    return callbacks.cancellation_requested();
  } catch (...) {
    return true;
  }
}

Status cancelled_status(const std::wstring_view operation) {
  return Status::failure(media_error(
      ErrorCode::cancelled,
      ERROR_CANCELLED,
      std::wstring(operation),
      L"レスキューメディア作成は書込み開始前に中止されました"));
}

Result<std::wstring> full_path(const std::wstring& path) {
  if (path.empty() || path.size() >= 32'768U) {
    return Result<std::wstring>::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"メディアパス正規化",
        L"パスが空か、安全なWindowsパス上限を超えています"));
  }
  const DWORD required =
      GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0 || required >= 32'768U) {
    return Result<std::wstring>::failure(clonecore::make_win32_error(
        ErrorCode::invalid_argument,
        L"メディアパス正規化",
        required == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE));
  }
  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
  const DWORD written = GetFullPathNameW(
      path.c_str(),
      static_cast<DWORD>(buffer.size()),
      buffer.data(),
      nullptr);
  if (written == 0 || written >= buffer.size()) {
    return Result<std::wstring>::failure(clonecore::make_win32_error(
        ErrorCode::invalid_argument,
        L"メディアパス正規化",
        written == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
  }
  return Result<std::wstring>::success(
      std::wstring(buffer.data(), written));
}

std::wstring parent_path(const std::wstring& path) {
  const std::size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return {};
  }
  if (separator == 2U && path.size() >= 3U && path[1] == L':') {
    return path.substr(0, 3U);
  }
  return path.substr(0, separator);
}

Status require_regular_non_reparse_file(
    const std::wstring& path,
    const std::wstring_view description) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        std::wstring(description),
        GetLastError()));
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return Status::failure(media_error(
        ErrorCode::verification_failed,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(description),
        L"通常ファイルではないかreparse pointのため使用しません"));
  }
  return clonecore::success_status();
}

Status require_new_path(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    return Status::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_FILE_EXISTS,
        L"レスキューISO非上書き確認",
        L"完成ISOまたはmanifestが既に存在するため上書きしません"));
  }
  const DWORD native_code = GetLastError();
  if (native_code != ERROR_FILE_NOT_FOUND) {
    return Status::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"レスキューISO非上書き確認",
        native_code));
  }
  return clonecore::success_status();
}

Status require_safe_existing_ancestors(std::wstring path) {
  for (;;) {
    if (path.empty()) {
      return Status::failure(media_error(
          ErrorCode::invalid_argument,
          ERROR_PATH_NOT_FOUND,
          L"レスキューISO親フォルダー",
          L"完成ISOの親フォルダーがありません"));
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD native_code = GetLastError();
      if (native_code != ERROR_FILE_NOT_FOUND &&
          native_code != ERROR_PATH_NOT_FOUND) {
        return Status::failure(clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"レスキューISO親フォルダー",
            native_code));
      }
    } else {
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return Status::failure(media_error(
            ErrorCode::verification_failed,
            ERROR_REPARSE_TAG_INVALID,
            L"レスキューISO親フォルダー",
            L"親パスが通常フォルダーではないかreparse pointです"));
      }
    }

    const std::wstring parent = parent_path(path);
    if (parent.empty() || parent == path) {
      break;
    }
    path = parent;
  }
  return clonecore::success_status();
}

bool current_process_is_elevated() noexcept {
  HANDLE token_raw = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) {
    return false;
  }
  UniqueHandle token(token_raw);
  TOKEN_ELEVATION elevation{};
  DWORD returned = 0;
  return GetTokenInformation(
             token.get(),
             TokenElevation,
             &elevation,
             static_cast<DWORD>(sizeof(elevation)),
             &returned) != FALSE &&
         returned == sizeof(elevation) &&
         elevation.TokenIsElevated != 0;
}

Result<std::wstring> current_module_path() {
  std::vector<wchar_t> buffer(1024U);
  while (buffer.size() <= 32'768U) {
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return Result<std::wstring>::failure(clonecore::make_win32_error(
          ErrorCode::query_failed,
          L"製品配置 実行ファイルパス",
          GetLastError()));
    }
    if (length < buffer.size() - 1U) {
      return Result<std::wstring>::success(
          std::wstring(buffer.data(), length));
    }
    buffer.resize(buffer.size() * 2U);
  }
  return Result<std::wstring>::failure(media_error(
      ErrorCode::invalid_data,
      ERROR_FILENAME_EXCED_RANGE,
      L"製品配置 実行ファイルパス",
      L"実行ファイルパスが安全上限を超えています"));
}

Result<std::wstring> system_powershell_path() {
  std::array<wchar_t, MAX_PATH + 1U> buffer{};
  const UINT length =
      GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return Result<std::wstring>::failure(clonecore::make_win32_error(
        ErrorCode::query_failed,
        L"Windows PowerShell正規パス",
        length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER));
  }
  return Result<std::wstring>::success(
      std::wstring(buffer.data(), length) +
      L"\\WindowsPowerShell\\v1.0\\powershell.exe");
}

std::string digest_to_hex(const imageformat::Sha256Digest& digest) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(digest.size() * 2U);
  for (const std::byte value : digest) {
    const unsigned byte = std::to_integer<unsigned>(value);
    output.push_back(kHex[(byte >> 4U) & 0x0FU]);
    output.push_back(kHex[byte & 0x0FU]);
  }
  return output;
}

Result<std::pair<std::uint64_t, std::string>> hash_file(
    const std::wstring& path,
    const std::wstring_view description = L"生成ISO") {
  const Status regular =
      require_regular_non_reparse_file(
          path, std::wstring(description) + L"検証");
  if (!regular) {
    return Result<std::pair<std::uint64_t, std::string>>::failure(
        regular.error());
  }
  HANDLE raw = CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return Result<std::pair<std::uint64_t, std::string>>::failure(
        clonecore::make_win32_error(
            ErrorCode::io_failed,
            std::wstring(description) + L"読取り専用オープン",
            GetLastError()));
  }
  UniqueHandle file(raw);
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > kMaximumIsoBytes) {
    return Result<std::pair<std::uint64_t, std::string>>::failure(
        media_error(
            ErrorCode::invalid_data,
            GetLastError(),
            std::wstring(description) + L"サイズ検証",
            std::wstring(description) +
                L"が空か安全な検証上限を超えています"));
  }
  const std::uint64_t length =
      static_cast<std::uint64_t>(size.QuadPart);
  const imageformat::Sha256ReadCallback reader =
      [handle = file.get(), description = std::wstring(description)](
          const std::uint64_t offset,
          const std::size_t read_length) -> Result<std::vector<std::byte>> {
    if (read_length > (std::numeric_limits<DWORD>::max)()) {
      return Result<std::vector<std::byte>>::failure(media_error(
          ErrorCode::invalid_argument,
          ERROR_BUFFER_OVERFLOW,
          description + L"ハッシュ読取り",
          L"1回の読取り長がWindows API上限を超えています"));
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
      return Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              ErrorCode::io_failed,
              std::wstring(description) + L"ハッシュ位置設定",
              GetLastError()));
    }
    std::vector<std::byte> bytes(read_length);
    DWORD read = 0;
    if (!ReadFile(
            handle,
            bytes.data(),
            static_cast<DWORD>(bytes.size()),
            &read,
            nullptr) ||
        read != bytes.size()) {
      return Result<std::vector<std::byte>>::failure(
          clonecore::make_win32_error(
              ErrorCode::io_failed,
              std::wstring(description) + L"ハッシュ読取り",
              GetLastError()));
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
  };
  const auto digest =
      imageformat::sha256_from_reader(length, kHashBlockBytes, reader);
  if (!digest) {
    return Result<std::pair<std::uint64_t, std::string>>::failure(
        digest.error());
  }
  return Result<std::pair<std::uint64_t, std::string>>::success(
      {length, digest_to_hex(digest.value())});
}

Status validate_dependencies(
    const RescueMediaCreationDependencies& dependencies,
    const RescueMediaKind kind) {
  if (!dependencies.inspect_environment ||
      !dependencies.resolve_payload) {
    return Status::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"レスキューメディア実行依存",
        L"環境再検査または製品配置の依存サービスがありません"));
  }
  if (kind == RescueMediaKind::iso_file &&
      (!dependencies.create_work_root_name ||
       !dependencies.verify_new_iso_destination ||
       dependencies.iso_executor == nullptr)) {
    return Status::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"レスキューISO実行依存",
        L"ISO出力検査、作業領域、または実行サービスがありません"));
  }
  if (kind == RescueMediaKind::usb_drive &&
      (!dependencies.create_usb_work_root_name ||
       !dependencies.verify_usb_destination ||
       dependencies.usb_executor == nullptr)) {
    return Status::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"レスキューUSB実行依存",
        L"USB再識別、作業領域、または対象限定実行サービスがありません"));
  }
  return clonecore::success_status();
}

bool is_sha256_hex(const std::string_view value) noexcept {
  return value.size() == 64U &&
         std::all_of(
             value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'A' && character <= 'F') ||
                      (character >= 'a' && character <= 'f');
             });
}

Status validate_report(
    const RescueMediaCreationReport& report,
    const RescueMediaIsoExecutionRequest& execution) {
  if (_wcsicmp(
          report.final_iso_path.c_str(),
          execution.final_iso_path.c_str()) != 0 ||
      _wcsicmp(
          report.manifest_path.c_str(),
          (execution.final_iso_path + L".manifest.json").c_str()) != 0 ||
      _wcsicmp(
          report.retained_work_root.c_str(),
          execution.work_root.c_str()) != 0 ||
      report.iso_length == 0 || !is_sha256_hex(report.iso_sha256) ||
      !report.complete_iso_verified ||
      !report.published_without_overwrite) {
    return Status::failure(media_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"レスキューISO完了報告",
        L"ISO実行サービスの完了報告が要求した出力と一致しません"));
  }
  return clonecore::success_status();
}

Status validate_usb_report(
    const RescueMediaCreationReport& report,
    const RescueMediaUsbExecutionRequest& execution) {
  const bool root_is_drive =
      report.usb_root_path.size() == 3U &&
      report.usb_root_path[0] >= L'A' &&
      report.usb_root_path[0] <= L'Z' &&
      report.usb_root_path[1] == L':' &&
      report.usb_root_path[2] == L'\\';
  if (!execution.authorization.storage_plan.has_value()) {
    return Status::failure(media_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"レスキューUSB完了報告",
        L"実行要求に不変な媒体レイアウト計画がありません"));
  }
  const auto& storage_plan =
      execution.authorization.storage_plan.value();
  const Status binding =
      validate_rescue_usb_storage_plan_binding(storage_plan);
  if (!binding) {
    return binding;
  }
  const bool refresh = storage_plan.mode ==
      RescueUsbProvisioningMode::preserve_data_refresh;
  if (!root_is_drive || _wcsicmp(
          report.retained_work_root.c_str(),
          execution.work_root.c_str()) != 0 ||
      _wcsicmp(
          report.manifest_path.c_str(),
          (execution.work_root +
           L"\\usb-media-manifest.json").c_str()) != 0 ||
      !is_sha256_hex(report.usb_boot_wim_sha256) ||
      !report.usb_layout_verified ||
      !report.usb_boot_staged_without_overwrite ||
      report.usb_data_preserved != refresh ||
      !report.complete_usb_verified ||
      !report.final_iso_path.empty() ||
      report.complete_iso_verified) {
    return Status::failure(media_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"レスキューUSB完了報告",
        L"USB実行サービスの完了報告が要求した対象と一致しません"));
  }
  return clonecore::success_status();
}

struct ScriptProgressMapping final {
  std::string_view stage_text;
  RescueMediaCreationStage stage;
  std::uint8_t product_percent;
  std::wstring_view message;
};

constexpr std::array<ScriptProgressMapping, 13U>
    kScriptProgressMappings{{
        {"preflight",
         RescueMediaCreationStage::verifying_environment,
         24U,
         L"ADK、WinPE、署名、必須更新の再検査に合格しました"},
        {"created-working-area",
         RescueMediaCreationStage::preparing_work_area,
         30U,
         L"専用の新規作業領域を準備しました"},
        {"staged-adk-media",
         RescueMediaCreationStage::staging_adk_media,
         38U,
         L"ローカルADKからWinPE媒体を作業領域へ準備しました"},
        {"mounted-wim",
         RescueMediaCreationStage::servicing_wim,
         48U,
         L"WinPEイメージをマウントしました"},
        {"added-japanese-font",
         RescueMediaCreationStage::servicing_wim,
         58U,
         L"ローカルADKの日本語表示機能を追加しました"},
        {"verified-product-payload",
         RescueMediaCreationStage::servicing_wim,
         68U,
         L"WinPE内のTsumugi DriveをSHA-256で照合しました"},
        {"committed-wim",
         RescueMediaCreationStage::servicing_wim,
         78U,
         L"WinPEイメージを整合性検査付きで確定しました"},
        {"generated-iso",
         RescueMediaCreationStage::generating_iso,
         88U,
         L"BIOS／UEFI対応ISOを生成しました"},
        {"verified-iso",
         RescueMediaCreationStage::verifying_iso,
         92U,
         L"生成直後のISOをSHA-256で検証しました"},
        {"writing-usb",
         RescueMediaCreationStage::writing_usb,
         88U,
         L"対象を再確認したUSBへWinPEを作成しています"},
        {"verified-usb",
         RescueMediaCreationStage::verifying_usb,
         94U,
         L"USB上の起動ファイルを全量照合しました"},
        {"completed-iso",
         RescueMediaCreationStage::publishing_iso,
         96U,
         L"ISOとmanifestを非上書きで完成名へ確定しました"},
        {"completed-usb",
         RescueMediaCreationStage::verifying_usb,
         98U,
         L"作成後も同じUSBであることを再確認しました"},
    }};

void report_script_progress_line(
    std::string_view line,
    const RescueMediaCreationCallbacks& callbacks) noexcept {
  constexpr std::string_view kPrefix = "TSUMUGI_MEDIA_PROGRESS=";
  if (!line.starts_with(kPrefix)) {
    return;
  }
  line.remove_prefix(kPrefix.size());
  const std::size_t separator = line.find('|');
  if (separator == std::string_view::npos || separator == 0U) {
    return;
  }
  unsigned script_percent = 0;
  const auto parsed = std::from_chars(
      line.data(), line.data() + separator, script_percent);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != line.data() + separator ||
      script_percent > 100U) {
    return;
  }
  std::string_view stage_text = line.substr(separator + 1U);
  while (!stage_text.empty() &&
         (stage_text.back() == '\r' || stage_text.back() == '\n')) {
    stage_text.remove_suffix(1U);
  }
  const auto mapping = std::find_if(
      kScriptProgressMappings.begin(),
      kScriptProgressMappings.end(),
      [stage_text](const ScriptProgressMapping& candidate) {
        return candidate.stage_text == stage_text;
      });
  if (mapping == kScriptProgressMappings.end()) {
    return;
  }
  report_progress(
      callbacks,
      mapping->stage,
      mapping->product_percent,
      std::wstring(mapping->message),
      false);
}

void consume_script_progress_output(
    std::string& pending,
    const std::string_view chunk,
    const RescueMediaCreationCallbacks& callbacks) {
  pending.append(chunk.data(), chunk.size());
  for (;;) {
    const std::size_t newline = pending.find('\n');
    if (newline == std::string::npos) {
      break;
    }
    report_script_progress_line(
        std::string_view(pending.data(), newline + 1U), callbacks);
    pending.erase(0, newline + 1U);
  }
}

std::wstring decode_process_output(const std::string_view output) {
  if (output.empty() ||
      output.size() >
          static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return {};
  }
  const auto decode = [output](const UINT code_page, const DWORD flags) {
    const int required = MultiByteToWideChar(
        code_page,
        flags,
        output.data(),
        static_cast<int>(output.size()),
        nullptr,
        0);
    if (required <= 0) {
      return std::wstring{};
    }
    std::wstring decoded(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        code_page,
        flags,
        output.data(),
        static_cast<int>(output.size()),
        decoded.data(),
        required);
    if (written != required) {
      return std::wstring{};
    }
    return decoded;
  };
  std::wstring decoded = decode(CP_UTF8, MB_ERR_INVALID_CHARS);
  if (decoded.empty()) {
    decoded = decode(CP_ACP, 0);
  }
  return decoded;
}

class WindowsRescueMediaIsoExecutor final
    : public IRescueMediaIsoExecutor {
 public:
  Result<RescueMediaCreationReport> execute(
      const RescueMediaIsoExecutionRequest& request) override {
    if (!current_process_is_elevated()) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::access_denied,
          ERROR_ELEVATION_REQUIRED,
          L"レスキューISO 管理者確認",
          L"ISO生成には管理者権限が必要です。このサービスからUACは要求しません"));
    }

    const std::array<std::pair<const std::wstring*, std::wstring_view>, 5U>
        files{{
            {&request.payload.builder_script, L"製品MediaBuilderスクリプト"},
            {&request.payload.environment_diagnostic, L"製品ADK診断CLI"},
            {&request.payload.winpe_cli, L"製品WinPE CLI"},
            {&request.payload.winpe_gui, L"製品WinPE GUI"},
            {&request.payload.powershell, L"Windows PowerShell"},
        }};
    for (const auto& [path, description] : files) {
      const Status regular =
          require_regular_non_reparse_file(*path, description);
      if (!regular) {
        return Result<RescueMediaCreationReport>::failure(
            regular.error());
      }
    }

    const auto builder_identity =
        hash_file(request.payload.builder_script);
    if (!builder_identity) {
      return Result<RescueMediaCreationReport>::failure(
          builder_identity.error());
    }
    if (!matches_embedded_media_builder_identity(
            builder_identity.value().first,
            builder_identity.value().second)) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"製品MediaBuilder完全性検証",
          L"同梱MediaBuilderの長さまたはSHA-256がビルド時の監査値と一致しません"));
    }

    report_progress(
        request.callbacks,
        RescueMediaCreationStage::verifying_product_payload,
        18U,
        L"自作WinPEアプリとWindows PowerShellを検証しています",
        true);
    if (cancellation_requested(request.callbacks)) {
      return Result<RescueMediaCreationReport>::failure(
          cancelled_status(L"レスキューISO製品配置確認").error());
    }

    auto trust = bootrepair::make_windows_authenticode_verifier();
    const Status trusted =
        trust->verify_microsoft_signed(request.payload.powershell);
    if (!trusted) {
      return Result<RescueMediaCreationReport>::failure(trusted.error());
    }

    report_progress(
        request.callbacks,
        RescueMediaCreationStage::preparing_work_area,
        22U,
        L"WinPE作業を開始します。WIM処理中は安全のため中断できません",
        false);
    const std::wstring certificate =
        request.boot_profile ==
                RescueMediaBootProfile::windows_uefi_2023_ca
            ? L"2023CA"
            : L"2011CA";
    const std::vector<std::wstring> arguments{
        L"-NoLogo",
        L"-NoProfile",
        L"-NonInteractive",
        L"-ExecutionPolicy",
        L"Bypass",
        L"-File",
        request.payload.builder_script,
        L"-OutputRoot",
        request.work_root,
        L"-FinalIsoPath",
        request.final_iso_path,
        L"-CertificateGeneration",
        certificate,
        L"-ValidationScenario",
        L"Inventory",
        L"-DiagnosticPath",
        request.payload.environment_diagnostic,
        L"-WinPEAppPath",
        request.payload.winpe_cli,
        L"-WinPEGuiPath",
        request.payload.winpe_gui,
        L"-BuildMedia",
    };
    auto runner = bootrepair::make_windows_process_runner(
        kMediaBuildTimeoutMilliseconds);
    std::string pending_progress;
    const auto process = runner->run_streamed(
        request.payload.powershell,
        arguments,
        request.payload.package_root,
        [&pending_progress, &request](const std::string_view chunk) {
          consume_script_progress_output(
              pending_progress, chunk, request.callbacks);
        });
    if (!pending_progress.empty()) {
      report_script_progress_line(
          pending_progress, request.callbacks);
    }
    if (!process) {
      return Result<RescueMediaCreationReport>::failure(process.error());
    }
    if (process.value().exit_code != 0) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::io_failed,
          process.value().exit_code,
          L"レスキューISO生成",
          format_media_builder_failure_message(
              process.value().standard_output,
              process.value().standard_error)));
    }

    report_progress(
        request.callbacks,
        RescueMediaCreationStage::verifying_iso,
        96U,
        L"完成ISOを全量読み取り、SHA-256を確認しています",
        false);
    const std::wstring manifest_path =
        request.final_iso_path + L".manifest.json";
    const Status manifest = require_regular_non_reparse_file(
        manifest_path, L"レスキューISO manifest");
    if (!manifest) {
      return Result<RescueMediaCreationReport>::failure(
          manifest.error());
    }
    const auto iso = hash_file(request.final_iso_path);
    if (!iso) {
      return Result<RescueMediaCreationReport>::failure(iso.error());
    }

    const std::string success_marker =
        "WINPE_APP_MEDIA_PASS=";
    if (process.value().standard_output.find(success_marker) ==
        std::string::npos) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"レスキューISO成功マーカー",
          L"MediaBuilderの成功マーカーがないため完成扱いにしません"));
    }
    return Result<RescueMediaCreationReport>::success(
        RescueMediaCreationReport{
            .final_iso_path = request.final_iso_path,
            .manifest_path = manifest_path,
            .retained_work_root = request.work_root,
            .iso_length = iso.value().first,
            .iso_sha256 = iso.value().second,
            .complete_iso_verified = true,
            .published_without_overwrite = true,
        });
  }
};

class WindowsRescueMediaUsbExecutor final
    : public IRescueMediaUsbExecutor {
 public:
  Result<RescueMediaCreationReport> execute(
      const RescueMediaUsbExecutionRequest& request) override {
    if (!current_process_is_elevated()) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::access_denied,
          ERROR_ELEVATION_REQUIRED,
          L"レスキューUSB 管理者確認",
          L"USB作成には管理者権限が必要です。このサービスからUACは要求しません"));
    }
    if (request.authorization.physical_write_started ||
        request.mapping.physical_write_started ||
        !request.authorization.storage_plan.has_value() ||
        request.authorization.confirmation_token.empty() ||
        request.mapping.drive_letter < L'A' ||
        request.mapping.drive_letter > L'Z' ||
        request.mapping.root_path !=
            std::wstring{
                request.mapping.drive_letter, L':', L'\\'}) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"レスキューUSB対象限定情報",
          L"書込み前の対象情報、確認語、またはドライブ文字が不正です"));
    }
    const auto& storage_plan =
        request.authorization.storage_plan.value();
    const Status plan_binding =
        validate_rescue_usb_storage_plan_binding(storage_plan);
    if (!plan_binding) {
      return Result<RescueMediaCreationReport>::failure(
          plan_binding.error());
    }
    const Status planned_identity = clonecore::validate_stable_identity(
        storage_plan.expected_target,
        request.authorization.target,
        L"レスキューUSB媒体計画対象");
    if (!planned_identity ||
        storage_plan.reviewed_layout.partitions.size() !=
            request.authorization.partition_count) {
      return Result<RescueMediaCreationReport>::failure(
          planned_identity
              ? media_error(
                    ErrorCode::identity_mismatch,
                    ERROR_INVALID_DATA,
                    L"レスキューUSB媒体計画対象",
                    L"媒体計画と確認済みパーティション数が一致しません")
              : planned_identity.error());
    }
    const bool proposed_initialization_mapping =
        storage_plan.mode == RescueUsbProvisioningMode::initialize_all &&
        request.mapping.partition_number == 0U &&
        request.mapping.extent_start == 0U &&
        request.mapping.extent_length == 0U &&
        request.mapping.drive_letter_was_unassigned;
    const bool owned_refresh_mapping =
        storage_plan.mode ==
            RescueUsbProvisioningMode::preserve_data_refresh &&
        request.authorization.partition_count == 2U &&
        request.mapping.partition_number == 1U &&
        !request.mapping.drive_letter_was_unassigned;
    if (!proposed_initialization_mapping &&
        !owned_refresh_mapping) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"レスキューUSB対象限定情報",
          L"初期化対象または所有済み2領域USBの照合情報が一致しません"));
    }
    const Status identity = clonecore::validate_stable_identity(
        request.authorization.target,
        request.mapping.target_identity,
        L"レスキューUSB実行対象");
    if (!identity) {
      return Result<RescueMediaCreationReport>::failure(
          identity.error());
    }

    const std::array<
        std::pair<const std::wstring*, std::wstring_view>,
        5U>
        files{{
            {&request.payload.builder_script, L"製品MediaBuilderスクリプト"},
            {&request.payload.environment_diagnostic, L"製品ADK診断CLI"},
            {&request.payload.winpe_cli, L"製品WinPE CLI"},
            {&request.payload.winpe_gui, L"製品WinPE GUI"},
            {&request.payload.powershell, L"Windows PowerShell"},
        }};
    for (const auto& [path, description] : files) {
      const Status regular =
          require_regular_non_reparse_file(*path, description);
      if (!regular) {
        return Result<RescueMediaCreationReport>::failure(
            regular.error());
      }
    }

    const auto builder_identity =
        hash_file(request.payload.builder_script, L"製品MediaBuilder");
    if (!builder_identity) {
      return Result<RescueMediaCreationReport>::failure(
          builder_identity.error());
    }
    if (!matches_embedded_media_builder_identity(
            builder_identity.value().first,
            builder_identity.value().second)) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"製品MediaBuilder完全性検証",
          L"同梱MediaBuilderの長さまたはSHA-256がビルド時の監査値と一致しません"));
    }

    report_progress(
        request.callbacks,
        RescueMediaCreationStage::verifying_product_payload,
        18U,
        L"自作WinPEアプリとWindows PowerShellを検証しています",
        true);
    if (cancellation_requested(request.callbacks)) {
      return Result<RescueMediaCreationReport>::failure(
          cancelled_status(L"レスキューUSB製品配置確認").error());
    }

    auto trust = bootrepair::make_windows_authenticode_verifier();
    const Status trusted =
        trust->verify_microsoft_signed(request.payload.powershell);
    if (!trusted) {
      return Result<RescueMediaCreationReport>::failure(
          trusted.error());
    }

    report_progress(
        request.callbacks,
        RescueMediaCreationStage::preparing_work_area,
        22U,
        L"WinPE作業を開始します。WIM処理とUSB書込み中は中断できません",
        false);
    const std::wstring certificate =
        request.boot_profile ==
                RescueMediaBootProfile::windows_uefi_2023_ca
            ? L"2023CA"
            : L"2011CA";
    const std::wstring serial_suffix(
        request.authorization.target.serial_suffix.begin(),
        request.authorization.target.serial_suffix.end());
    const std::wstring usb_drive{
        request.mapping.drive_letter, L':'};
    const std::wstring usb_operation(
        rescue_usb_provisioning_mode_name(storage_plan.mode));
    const std::wstring data_file_system(
        rescue_usb_data_file_system_name(storage_plan.data_file_system));
    const auto canonical_layout_digest =
        make_rescue_usb_canonical_layout_digest(
            storage_plan.reviewed_layout);
    if (!canonical_layout_digest) {
      return Result<RescueMediaCreationReport>::failure(
          canonical_layout_digest.error());
    }
    const std::string canonical_layout_digest_hex =
        digest_to_hex(canonical_layout_digest.value());
    const std::wstring canonical_layout_digest_argument(
        canonical_layout_digest_hex.begin(),
        canonical_layout_digest_hex.end());
    const std::vector<std::wstring> arguments{
        L"-NoLogo",
        L"-NoProfile",
        L"-NonInteractive",
        L"-ExecutionPolicy",
        L"Bypass",
        L"-File",
        request.payload.builder_script,
        L"-OutputRoot",
        request.work_root,
        L"-CertificateGeneration",
        certificate,
        L"-ValidationScenario",
        L"Inventory",
        L"-DiagnosticPath",
        request.payload.environment_diagnostic,
        L"-WinPEAppPath",
        request.payload.winpe_cli,
        L"-WinPEGuiPath",
        request.payload.winpe_gui,
        L"-TargetUsbDrive",
        usb_drive,
        L"-ExpectedUsbDiskNumber",
        std::to_wstring(request.authorization.target.disk_number),
        L"-ExpectedUsbSizeBytes",
        std::to_wstring(request.authorization.target.size_bytes),
        L"-ExpectedUsbSerialSuffix",
        serial_suffix,
        L"-ExpectedUsbDeviceInstanceId",
        request.authorization.target.device_instance_id,
        L"-ExpectedUsbCanonicalLayoutSha256",
        canonical_layout_digest_argument,
        L"-UsbOperation",
        usb_operation,
        L"-UsbDataFileSystem",
        data_file_system,
        L"-BuildUsb",
        L"-BuildMedia",
    };
    auto runner = bootrepair::make_windows_process_runner(
        kMediaBuildTimeoutMilliseconds);
    std::string pending_progress;
    const auto process = runner->run_streamed(
        request.payload.powershell,
        arguments,
        request.payload.package_root,
        [&pending_progress, &request](const std::string_view chunk) {
          consume_script_progress_output(
              pending_progress, chunk, request.callbacks);
        });
    if (!pending_progress.empty()) {
      report_script_progress_line(
          pending_progress, request.callbacks);
    }
    if (!process) {
      return Result<RescueMediaCreationReport>::failure(
          process.error());
    }
    if (process.value().exit_code != 0) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::io_failed,
          process.value().exit_code,
          L"レスキューUSB生成",
          format_media_builder_failure_message(
              process.value().standard_output,
              process.value().standard_error)));
    }

    const std::string success_marker = "WINPE_APP_USB_PASS=";
    if (process.value().standard_output.find(success_marker) ==
        std::string::npos) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"レスキューUSB成功マーカー",
          L"MediaBuilderのUSB成功マーカーがないため完成扱いにしません"));
    }
    const auto actual_drive = parse_media_builder_usb_drive_marker(
        process.value().standard_output);
    if (!actual_drive) {
      return Result<RescueMediaCreationReport>::failure(
          actual_drive.error());
    }
    const std::wstring actual_root{
        actual_drive.value(), L':', L'\\'};

    report_progress(
        request.callbacks,
        RescueMediaCreationStage::verifying_usb,
        96U,
        L"USB上のboot.wimと起動ファイルを読み戻しています",
        false);
    const std::wstring manifest_path =
        request.work_root + L"\\usb-media-manifest.json";
    for (const auto& [path, description] :
         std::array<std::pair<std::wstring, std::wstring_view>, 4U>{{
             {manifest_path, L"レスキューUSB manifest"},
             {actual_root + L"sources\\boot.wim",
              L"USB boot.wim"},
             {actual_root + L"bootmgr",
              L"USB BIOS起動ファイル"},
             {actual_root + L"EFI\\BOOT\\bootx64.efi",
              L"USB UEFI起動ファイル"},
         }}) {
      const Status regular =
          require_regular_non_reparse_file(path, description);
      if (!regular) {
        return Result<RescueMediaCreationReport>::failure(
            regular.error());
      }
    }
    const auto boot_wim = hash_file(
        actual_root + L"sources\\boot.wim",
        L"USB boot.wim");
    if (!boot_wim) {
      return Result<RescueMediaCreationReport>::failure(
          boot_wim.error());
    }
    return Result<RescueMediaCreationReport>::success(
        RescueMediaCreationReport{
            .manifest_path = manifest_path,
            .retained_work_root = request.work_root,
            .usb_root_path = actual_root,
            .usb_boot_wim_sha256 = boot_wim.value().second,
            .usb_layout_verified = true,
            .usb_boot_staged_without_overwrite = true,
            .usb_data_preserved =
                storage_plan.mode ==
                RescueUsbProvisioningMode::preserve_data_refresh,
            .complete_usb_verified = true,
        });
  }
};

}  // namespace

bool matches_embedded_media_builder_identity(
    const std::uint64_t length,
    const std::string_view sha256) noexcept {
  if (length != kExpectedMediaBuilderLength ||
      sha256.size() != kExpectedMediaBuilderSha256.size()) {
    return false;
  }
  return std::equal(
      sha256.begin(),
      sha256.end(),
      kExpectedMediaBuilderSha256.begin(),
      [](const char actual, const char expected) {
        const auto normalize = [](const char value) {
          return value >= 'a' && value <= 'f'
                     ? static_cast<char>(value - ('a' - 'A'))
                     : value;
        };
        return normalize(actual) == normalize(expected);
      });
}

Result<wchar_t> parse_media_builder_usb_drive_marker(
    const std::string_view standard_output) {
  constexpr std::string_view kMarker =
      "WINPE_APP_USB_DRIVE=";
  std::optional<wchar_t> parsed;
  std::size_t search_offset = 0U;
  while (search_offset < standard_output.size()) {
    const std::size_t found =
        standard_output.find(kMarker, search_offset);
    if (found == std::string_view::npos) {
      break;
    }
    search_offset = found + kMarker.size();
    if (found != 0U && standard_output[found - 1U] != '\n') {
      continue;
    }
    if (search_offset + 2U > standard_output.size()) {
      return Result<wchar_t>::failure(media_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"レスキューUSB割当文字",
          L"MediaBuilderの割当ドライブ文字が途中で切れています"));
    }
    const char letter = standard_output[search_offset];
    const char colon = standard_output[search_offset + 1U];
    const std::size_t end = search_offset + 2U;
    if (letter < 'A' || letter > 'Z' || colon != ':' ||
        (end < standard_output.size() &&
         standard_output[end] != '\r' &&
         standard_output[end] != '\n')) {
      return Result<wchar_t>::failure(media_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"レスキューUSB割当文字",
          L"MediaBuilderの割当ドライブ文字が不正です"));
    }
    if (parsed.has_value()) {
      return Result<wchar_t>::failure(media_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"レスキューUSB割当文字",
          L"MediaBuilderの割当ドライブ文字が重複しています"));
    }
    parsed = static_cast<wchar_t>(letter);
  }
  if (!parsed.has_value()) {
    return Result<wchar_t>::failure(media_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"レスキューUSB割当文字",
        L"MediaBuilderの割当ドライブ文字を確認できません"));
  }
  return Result<wchar_t>::success(parsed.value());
}

std::wstring format_media_builder_failure_message(
    const std::string_view standard_output,
    const std::string_view standard_error) noexcept {
  constexpr std::wstring_view kBase =
      L"MediaBuilderが失敗しました。作業フォルダーは診断用に保持しています";
  try {
    const std::string_view selected =
        standard_error.empty() ? standard_output : standard_error;
    std::wstring decoded = decode_process_output(selected);
    for (wchar_t& value : decoded) {
      if (value < L' ' && value != L'\r' && value != L'\n' &&
          value != L'\t') {
        value = L' ';
      }
    }
    while (!decoded.empty() &&
           (decoded.back() == L'\r' || decoded.back() == L'\n' ||
            decoded.back() == L'\t' || decoded.back() == L' ')) {
      decoded.pop_back();
    }
    if (decoded.empty()) {
      return std::wstring(kBase) + L"。ログを確認してください";
    }
    if (decoded.size() > kMaximumDisplayedBuilderOutput) {
      decoded.erase(
          0, decoded.size() - kMaximumDisplayedBuilderOutput);
      decoded.insert(0, L"…");
    }
    return std::wstring(kBase) + L"。\n\n詳細:\n" + decoded;
  } catch (...) {
    return std::wstring(kBase) + L"。ログを確認してください";
  }
}

Result<RescueMediaCreationReport> execute_rescue_media_creation(
    const RescueMediaCreationRequest& request,
    const RescueMediaCreationDependencies& dependencies) {
  const Status valid_dependencies =
      validate_dependencies(dependencies, request.kind);
  if (!valid_dependencies) {
    return Result<RescueMediaCreationReport>::failure(
        valid_dependencies.error());
  }
  report_progress(
      request.callbacks,
      RescueMediaCreationStage::validating_request,
      3U,
      L"作成内容と保存先を確認しています",
      true);
  if (!request.administrator) {
    return Result<RescueMediaCreationReport>::failure(media_error(
        ErrorCode::access_denied,
        ERROR_ELEVATION_REQUIRED,
        L"レスキューメディア 管理者確認",
        L"ISO／USB作成には管理者権限が必要です。この処理からUACは要求しません"));
  }
  if (request.kind == RescueMediaKind::iso_file &&
      !is_safe_iso_destination_syntax(request.final_iso_path)) {
    return Result<RescueMediaCreationReport>::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"レスキューISO保存先",
        L"ローカルドライブ上の新しい絶対.isoパスだけを指定できます"));
  }
  if (cancellation_requested(request.callbacks)) {
    return Result<RescueMediaCreationReport>::failure(
        cancelled_status(L"レスキューISO要求確認").error());
  }

  report_progress(
      request.callbacks,
      RescueMediaCreationStage::verifying_environment,
      9U,
      L"ADK、WinPE Add-on、Microsoft署名、必須更新を再確認しています",
      true);
  MediaPreflightView environment;
  try {
    environment = dependencies.inspect_environment();
  } catch (...) {
    return Result<RescueMediaCreationReport>::failure(media_error(
        ErrorCode::query_failed,
        ERROR_UNHANDLED_EXCEPTION,
        L"レスキューメディア環境再検査",
        L"ADK/WinPE環境の再検査で予期しない失敗が発生しました"));
  }
  if (!environment.media_creation_permitted ||
      !environment.base_layout_ready ||
      (request.boot_profile ==
           RescueMediaBootProfile::windows_uefi_2023_ca &&
       !environment.bootex_layout_ready)) {
    return Result<RescueMediaCreationReport>::failure(media_error(
        ErrorCode::unsupported_platform,
        ERROR_NOT_SUPPORTED,
        L"レスキューメディア環境再検査",
        L"ADK/WinPE/署名/必須更新または選択した起動構成が作成条件を満たしません"));
  }

  if (request.kind == RescueMediaKind::usb_drive) {
    if (!request.usb_authorization.has_value() ||
        !request.usb_mapping.has_value()) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::confirmation_required,
          ERROR_CANCELLED,
          L"レスキューUSB対象限定",
          L"二段階確認済みのUSB対象情報とドライブ文字照合が必要です"));
    }
    const auto& authorization = request.usb_authorization.value();
    const auto& mapping = request.usb_mapping.value();
    if (!authorization.storage_plan.has_value()) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::confirmation_required,
          ERROR_INVALID_DATA,
          L"レスキューUSB媒体計画",
          L"4GiB起動領域＋データ領域のレビュー済み不変計画がありません。製品UI接続後に再選択してください"));
    }
    const auto& storage_plan = authorization.storage_plan.value();
    const Status plan_binding =
        validate_rescue_usb_storage_plan_binding(storage_plan);
    if (!plan_binding) {
      return Result<RescueMediaCreationReport>::failure(
          plan_binding.error());
    }
    const Status planned_identity = clonecore::validate_stable_identity(
        storage_plan.expected_target,
        authorization.target,
        L"レスキューUSB媒体計画対象");
    if (!planned_identity ||
        storage_plan.reviewed_layout.partitions.size() !=
            authorization.partition_count) {
      return Result<RescueMediaCreationReport>::failure(
          planned_identity
              ? media_error(
                    ErrorCode::identity_mismatch,
                    ERROR_INVALID_DATA,
                    L"レスキューUSB媒体計画対象",
                    L"媒体計画と確認済みパーティション数が一致しません")
              : planned_identity.error());
    }
    const bool proposed_initialization_mapping =
        storage_plan.mode == RescueUsbProvisioningMode::initialize_all &&
        mapping.partition_number == 0U &&
        mapping.extent_start == 0U &&
        mapping.extent_length == 0U &&
        mapping.drive_letter_was_unassigned;
    const bool owned_refresh_mapping =
        storage_plan.mode ==
            RescueUsbProvisioningMode::preserve_data_refresh &&
        authorization.partition_count == 2U &&
        mapping.partition_number == 1U &&
        !mapping.drive_letter_was_unassigned;
    if (authorization.physical_write_started ||
        mapping.physical_write_started ||
        (!proposed_initialization_mapping &&
         !owned_refresh_mapping)) {
      return Result<RescueMediaCreationReport>::failure(media_error(
          ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"レスキューUSB対象限定",
          L"書込み前の初期化対象または所有済み2領域USBだけを作成先にできます"));
    }
    const Status selected_identity =
        clonecore::validate_stable_identity(
            authorization.target,
            mapping.target_identity,
            L"レスキューUSB選択対象");
    if (!selected_identity) {
      return Result<RescueMediaCreationReport>::failure(
          selected_identity.error());
    }
    if (cancellation_requested(request.callbacks)) {
      return Result<RescueMediaCreationReport>::failure(
          cancelled_status(L"レスキューUSB実行前確認").error());
    }

    const auto verified_mapping =
        dependencies.verify_usb_destination(
            storage_plan,
            RescueUsbDestinationVerificationPoint::before_write,
            mapping.drive_letter);
    if (!verified_mapping) {
      return Result<RescueMediaCreationReport>::failure(
          verified_mapping.error());
    }
    const Status verified_identity =
        clonecore::validate_stable_identity(
            authorization.target,
            verified_mapping.value().target_identity,
            L"レスキューUSB書込み直前対象");
    const bool exact_drive_mapping =
        verified_mapping.value().drive_letter ==
            mapping.drive_letter &&
        _wcsicmp(
            verified_mapping.value().root_path.c_str(),
            mapping.root_path.c_str()) == 0 &&
        verified_mapping.value().drive_letter_was_unassigned ==
            mapping.drive_letter_was_unassigned;
    const bool refreshed_initialization_mapping =
        proposed_initialization_mapping &&
        verified_mapping.value().partition_number == 0U &&
        verified_mapping.value().extent_start == 0U &&
        verified_mapping.value().extent_length == 0U &&
        verified_mapping.value().drive_letter_was_unassigned &&
        verified_mapping.value().root_path == std::wstring{
            verified_mapping.value().drive_letter, L':', L'\\'};
    if (!verified_identity ||
        (!exact_drive_mapping && !refreshed_initialization_mapping) ||
        verified_mapping.value().physical_write_started) {
      return Result<RescueMediaCreationReport>::failure(
          verified_identity
              ? media_error(
                    ErrorCode::identity_mismatch,
                    ERROR_DEVICE_NOT_CONNECTED,
                    L"レスキューUSB書込み直前対象",
                    L"選択時と書込み直前のドライブ文字または対象情報が一致しません")
              : verified_identity.error());
    }

    const auto payload = dependencies.resolve_payload();
    if (!payload) {
      return Result<RescueMediaCreationReport>::failure(
          payload.error());
    }
    const auto work_root =
        dependencies.create_usb_work_root_name();
    if (!work_root) {
      return Result<RescueMediaCreationReport>::failure(
          work_root.error());
    }
    if (cancellation_requested(request.callbacks)) {
      return Result<RescueMediaCreationReport>::failure(
          cancelled_status(L"レスキューUSB実行前最終確認").error());
    }

    const RescueMediaUsbExecutionRequest execution{
        .boot_profile = request.boot_profile,
        .work_root = work_root.value(),
        .payload = payload.value(),
        .authorization = authorization,
        .mapping = verified_mapping.value(),
        .callbacks = request.callbacks,
    };
    auto report = dependencies.usb_executor->execute(execution);
    if (!report) {
      return report;
    }
    const Status valid_report =
        validate_usb_report(report.value(), execution);
    if (!valid_report) {
      return Result<RescueMediaCreationReport>::failure(
          valid_report.error());
    }

    const wchar_t actual_drive_letter = report.value().usb_root_path[0];
    const auto final_mapping =
        dependencies.verify_usb_destination(
            storage_plan,
            RescueUsbDestinationVerificationPoint::after_write,
            actual_drive_letter);
    if (!final_mapping) {
      return Result<RescueMediaCreationReport>::failure(
          final_mapping.error());
    }
    const Status final_identity =
        clonecore::validate_stable_identity(
            authorization.target,
            final_mapping.value().target_identity,
            L"レスキューUSB作成後対象");
    if (!final_identity ||
        final_mapping.value().drive_letter !=
            actual_drive_letter ||
        _wcsicmp(
            final_mapping.value().root_path.c_str(),
            report.value().usb_root_path.c_str()) != 0 ||
        final_mapping.value().drive_letter_was_unassigned ||
        final_mapping.value().partition_number == 0U ||
        final_mapping.value().physical_write_started) {
      return Result<RescueMediaCreationReport>::failure(
          final_identity
              ? media_error(
                    ErrorCode::identity_mismatch,
                    ERROR_DEVICE_NOT_CONNECTED,
                    L"レスキューUSB作成後対象",
                    L"作成後のUSBを同一対象として再確認できません")
              : final_identity.error());
    }
    report_progress(
        request.callbacks,
        RescueMediaCreationStage::completed,
        100U,
        L"レスキューUSBの作成と全量検証が完了しました",
        false);
    return report;
  }

  const Status destination =
      dependencies.verify_new_iso_destination(request.final_iso_path);
  if (!destination) {
    return Result<RescueMediaCreationReport>::failure(
        destination.error());
  }
  const auto payload = dependencies.resolve_payload();
  if (!payload) {
    return Result<RescueMediaCreationReport>::failure(payload.error());
  }
  const auto work_root =
      dependencies.create_work_root_name(request.final_iso_path);
  if (!work_root) {
    return Result<RescueMediaCreationReport>::failure(
        work_root.error());
  }
  if (cancellation_requested(request.callbacks)) {
    return Result<RescueMediaCreationReport>::failure(
        cancelled_status(L"レスキューISO実行前確認").error());
  }

  const RescueMediaIsoExecutionRequest execution{
      .boot_profile = request.boot_profile,
      .final_iso_path = request.final_iso_path,
      .work_root = work_root.value(),
      .payload = payload.value(),
      .callbacks = request.callbacks,
  };
  auto report = dependencies.iso_executor->execute(execution);
  if (!report) {
    return report;
  }
  const Status valid_report = validate_report(report.value(), execution);
  if (!valid_report) {
    return Result<RescueMediaCreationReport>::failure(
        valid_report.error());
  }
  report_progress(
      request.callbacks,
      RescueMediaCreationStage::completed,
      100U,
      L"レスキューISOの作成と全量検証が完了しました",
      false);
  return report;
}

Result<ProductMediaPayloadPaths>
resolve_product_media_payload_paths_with_windows_apis() {
  const auto module = current_module_path();
  if (!module) {
    return Result<ProductMediaPayloadPaths>::failure(module.error());
  }
  const std::wstring package_root = parent_path(module.value());
  if (package_root.empty()) {
    return Result<ProductMediaPayloadPaths>::failure(media_error(
        ErrorCode::query_failed,
        ERROR_PATH_NOT_FOUND,
        L"製品メディア配置",
        L"製品実行ファイルの親フォルダーを解決できません"));
  }
  const auto powershell = system_powershell_path();
  if (!powershell) {
    return Result<ProductMediaPayloadPaths>::failure(
        powershell.error());
  }
  ProductMediaPayloadPaths paths{
      .builder_script =
          package_root + L"\\tools\\New-WinPEAppValidationMedia.ps1",
      .environment_diagnostic =
          package_root + L"\\tools\\ytec-winpe-environment.exe",
      .winpe_cli = package_root + L"\\winpe\\ytec-winpe-app.exe",
      .winpe_gui = package_root + L"\\winpe\\ytec-winpe-gui.exe",
      .powershell = powershell.value(),
      .package_root = package_root,
  };
  const std::array<std::pair<const std::wstring*, std::wstring_view>, 5U>
      required{{
          {&paths.builder_script, L"製品MediaBuilderスクリプト"},
          {&paths.environment_diagnostic, L"製品ADK診断CLI"},
          {&paths.winpe_cli, L"製品WinPE CLI"},
          {&paths.winpe_gui, L"製品WinPE GUI"},
          {&paths.powershell, L"Windows PowerShell"},
      }};
  for (const auto& [path, description] : required) {
    const Status regular =
        require_regular_non_reparse_file(*path, description);
    if (!regular) {
      return Result<ProductMediaPayloadPaths>::failure(
          regular.error());
    }
  }
  return Result<ProductMediaPayloadPaths>::success(std::move(paths));
}

Status verify_new_iso_destination_with_windows_apis(
    const std::wstring& final_iso_path) {
  if (!is_safe_iso_destination_syntax(final_iso_path)) {
    return Status::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"レスキューISO保存先",
        L"ローカルドライブ上の新しい絶対.isoパスだけを指定できます"));
  }
  const auto normalized = full_path(final_iso_path);
  if (!normalized) {
    return Status::failure(normalized.error());
  }
  const Status iso_new = require_new_path(normalized.value());
  if (!iso_new) {
    return iso_new;
  }
  const Status manifest_new =
      require_new_path(normalized.value() + L".manifest.json");
  if (!manifest_new) {
    return manifest_new;
  }
  const std::wstring parent = parent_path(normalized.value());
  const DWORD parent_attributes = GetFileAttributesW(parent.c_str());
  if (parent_attributes == INVALID_FILE_ATTRIBUTES ||
      (parent_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return Status::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_PATH_NOT_FOUND,
        L"レスキューISO親フォルダー",
        L"完成ISOの親フォルダーは既存のローカルフォルダーでなければなりません"));
  }
  return require_safe_existing_ancestors(parent);
}

Result<std::wstring> make_media_work_root_name_with_windows_apis(
    const std::wstring& final_iso_path) {
  const auto normalized = full_path(final_iso_path);
  if (!normalized) {
    return Result<std::wstring>::failure(normalized.error());
  }
  const std::wstring parent = parent_path(normalized.value());
  if (parent.empty()) {
    return Result<std::wstring>::failure(media_error(
        ErrorCode::invalid_argument,
        ERROR_PATH_NOT_FOUND,
        L"レスキューISO一時領域",
        L"完成ISOの親フォルダーを解決できません"));
  }
  auto generator = clonecore::make_windows_guid_generator();
  const auto guid = generator->next_guid();
  if (!guid) {
    return Result<std::wstring>::failure(guid.error());
  }
  constexpr wchar_t kHex[] = L"0123456789ABCDEF";
  std::wstring suffix;
  suffix.reserve(guid.value().bytes.size() * 2U);
  for (const std::byte value : guid.value().bytes) {
    const unsigned byte = std::to_integer<unsigned>(value);
    suffix.push_back(kHex[(byte >> 4U) & 0x0FU]);
    suffix.push_back(kHex[byte & 0x0FU]);
  }
  const std::wstring work_root =
      parent + L"\\.Y-TEC-Tsumugi-Drive-" + suffix + L".work";
  const Status unused = require_new_path(work_root);
  if (!unused) {
    return Result<std::wstring>::failure(unused.error());
  }
  return Result<std::wstring>::success(work_root);
}

Result<std::wstring>
make_usb_media_work_root_name_with_windows_apis() {
  const DWORD required = GetTempPathW(0U, nullptr);
  if (required == 0U || required >= 32'768U) {
    return Result<std::wstring>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"レスキューUSB一時領域",
            required == 0U
                ? GetLastError()
                : ERROR_FILENAME_EXCED_RANGE));
  }
  std::vector<wchar_t> buffer(
      static_cast<std::size_t>(required) + 1U, L'\0');
  const DWORD written = GetTempPathW(
      static_cast<DWORD>(buffer.size()), buffer.data());
  if (written == 0U || written >= buffer.size()) {
    return Result<std::wstring>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"レスキューUSB一時領域",
            written == 0U
                ? GetLastError()
                : ERROR_INSUFFICIENT_BUFFER));
  }
  std::wstring parent(buffer.data(), written);
  while (!parent.empty() &&
         (parent.back() == L'\\' || parent.back() == L'/')) {
    parent.pop_back();
  }
  if (parent.empty()) {
    return Result<std::wstring>::failure(media_error(
        ErrorCode::invalid_data,
        ERROR_PATH_NOT_FOUND,
        L"レスキューUSB一時領域",
        L"Windows一時フォルダーを安全に解決できません"));
  }
  const Status safe_parent =
      require_safe_existing_ancestors(parent);
  if (!safe_parent) {
    return Result<std::wstring>::failure(
        safe_parent.error());
  }

  auto generator = clonecore::make_windows_guid_generator();
  const auto guid = generator->next_guid();
  if (!guid) {
    return Result<std::wstring>::failure(guid.error());
  }
  constexpr wchar_t kHex[] = L"0123456789ABCDEF";
  std::wstring suffix;
  suffix.reserve(guid.value().bytes.size() * 2U);
  for (const std::byte value : guid.value().bytes) {
    const unsigned byte = std::to_integer<unsigned>(value);
    suffix.push_back(kHex[(byte >> 4U) & 0x0FU]);
    suffix.push_back(kHex[byte & 0x0FU]);
  }
  const std::wstring work_root =
      parent + L"\\Y-TEC-Tsumugi-Drive-USB-" +
      suffix + L".work";
  const Status unused = require_new_path(work_root);
  if (!unused) {
    return Result<std::wstring>::failure(unused.error());
  }
  return Result<std::wstring>::success(work_root);
}

Result<RescueUsbDriveLetterResolution>
verify_usb_destination_with_windows_apis(
    const RescueUsbStoragePlan& reviewed_plan,
    const RescueUsbDestinationVerificationPoint verification_point,
    const wchar_t expected_drive_letter) {
  if (expected_drive_letter < L'A' ||
      expected_drive_letter > L'Z') {
    return Result<RescueUsbDriveLetterResolution>::failure(
        media_error(
            ErrorCode::invalid_argument,
            ERROR_INVALID_DRIVE,
            L"レスキューUSB再識別",
            L"照合するドライブ文字が不正です"));
  }
  auto provider =
      diskmodel::make_windows_disk_inventory_provider();
  auto report = provider->enumerate();
  if (!report) {
    return Result<RescueUsbDriveLetterResolution>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return Result<RescueUsbDriveLetterResolution>::failure(
        media_error(
            ErrorCode::enumeration_failed,
            ERROR_PARTIAL_COPY,
            L"レスキューUSB再識別",
            L"物理ディスク列挙に未解決の診断があるため停止しました"));
  }

  std::vector<const diskmodel::DiskInfo*> matches;
  for (const auto& disk : report.value().disks) {
    const auto identity =
        diskmodel::make_stable_disk_identity(
            disk, disk.is_system_disk);
    if (identity &&
        clonecore::validate_stable_identity(
            reviewed_plan.expected_target,
            identity.value(), L"レスキューUSB再識別")) {
      matches.push_back(&disk);
    }
  }
  if (matches.size() != 1U) {
    return Result<RescueUsbDriveLetterResolution>::failure(
        media_error(
            ErrorCode::identity_mismatch,
            matches.empty()
                ? ERROR_DEVICE_NOT_CONNECTED
                : ERROR_DUP_NAME,
            L"レスキューUSB再識別",
            L"安定識別情報に一致するUSBを一意に特定できません"));
  }
  if (matches.front()->disk_number !=
      reviewed_plan.expected_target.disk_number) {
    return Result<RescueUsbDriveLetterResolution>::failure(
        media_error(
            ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"レスキューUSB再識別",
            L"USBのディスク番号が選択時から変化しました"));
  }

  auto mapping =
      resolve_windows_rescue_usb_drive_letter_for_plan_read_only(
          *matches.front(), reviewed_plan, verification_point);
  if (!mapping) {
    return mapping;
  }
  const bool letter_may_change =
      verification_point ==
          RescueUsbDestinationVerificationPoint::before_write &&
      reviewed_plan.mode == RescueUsbProvisioningMode::initialize_all;
  if ((!letter_may_change &&
       mapping.value().drive_letter != expected_drive_letter) ||
      mapping.value().physical_write_started) {
    return Result<RescueUsbDriveLetterResolution>::failure(
        media_error(
            ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"レスキューUSB再識別",
            L"USBのドライブ文字が選択時から変化しました"));
  }
  return mapping;
}

std::unique_ptr<IRescueMediaIsoExecutor>
make_windows_rescue_media_iso_executor() {
  return std::make_unique<WindowsRescueMediaIsoExecutor>();
}

std::unique_ptr<IRescueMediaUsbExecutor>
make_windows_rescue_media_usb_executor() {
  return std::make_unique<WindowsRescueMediaUsbExecutor>();
}

Result<RescueMediaCreationReport>
execute_rescue_media_creation_with_windows_apis(
    const RescueMediaCreationRequest& request) {
  auto iso_executor =
      make_windows_rescue_media_iso_executor();
  auto usb_executor =
      make_windows_rescue_media_usb_executor();
  return execute_rescue_media_creation(
      request,
      RescueMediaCreationDependencies{
          .inspect_environment =
              [] { return inspect_local_windows_media_environment(); },
          .resolve_payload =
              [] {
                return resolve_product_media_payload_paths_with_windows_apis();
              },
          .create_work_root_name =
              [](const std::wstring& path) {
                return make_media_work_root_name_with_windows_apis(path);
              },
          .create_usb_work_root_name =
              [] {
                return make_usb_media_work_root_name_with_windows_apis();
              },
          .verify_new_iso_destination =
              [](const std::wstring& path) {
                return verify_new_iso_destination_with_windows_apis(path);
              },
          .verify_usb_destination =
              [](const RescueUsbStoragePlan& plan,
                 const RescueUsbDestinationVerificationPoint point,
                 const wchar_t drive_letter) {
                return verify_usb_destination_with_windows_apis(
                    plan, point, drive_letter);
              },
          .iso_executor = iso_executor.get(),
          .usb_executor = usb_executor.get(),
      });
}

std::wstring_view rescue_media_creation_stage_label(
    const RescueMediaCreationStage stage) noexcept {
  switch (stage) {
    case RescueMediaCreationStage::validating_request:
      return L"作成内容を確認";
    case RescueMediaCreationStage::verifying_environment:
      return L"ADK／WinPE環境を再確認";
    case RescueMediaCreationStage::verifying_product_payload:
      return L"製品ファイルを検証";
    case RescueMediaCreationStage::preparing_work_area:
      return L"作業領域を準備";
    case RescueMediaCreationStage::staging_adk_media:
      return L"WinPE媒体を準備";
    case RescueMediaCreationStage::servicing_wim:
      return L"WinPEイメージを構成";
    case RescueMediaCreationStage::generating_iso:
      return L"ISOを生成";
    case RescueMediaCreationStage::verifying_iso:
      return L"ISOを全量検証";
    case RescueMediaCreationStage::publishing_iso:
      return L"完成名へ確定";
    case RescueMediaCreationStage::writing_usb:
      return L"USBへWinPEを作成";
    case RescueMediaCreationStage::verifying_usb:
      return L"USBを全量検証";
    case RescueMediaCreationStage::completed:
      return L"作成完了";
  }
  return L"処理中";
}

}  // namespace ytec::windowsapp

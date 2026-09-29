#include "ytec/bootrepair/winre_registration.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::bootrepair {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::Result;
using clonecore::Status;
using clonecore::UniqueHandle;

constexpr std::uint64_t kMaximumWinReImageBytes =
    8ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumPathCharacters = 32760U;
constexpr DWORD kHashReadBytes = 1024U * 1024U;

Error registration_error(
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

bool is_drive_absolute_path(const std::wstring& path) noexcept {
  return path.size() >= 3U && std::iswalpha(path[0]) != 0 &&
      path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

bool has_forbidden_path_text(const std::wstring& path) {
  if (path.find(L'"') != std::wstring::npos ||
      path.find(L'\r') != std::wstring::npos ||
      path.find(L'\n') != std::wstring::npos ||
      path.find(L'\0') != std::wstring::npos ||
      path.find(L'*') != std::wstring::npos ||
      path.find(L'?') != std::wstring::npos ||
      path.find(L'<') != std::wstring::npos ||
      path.find(L'>') != std::wstring::npos ||
      path.find(L'|') != std::wstring::npos ||
      path.find(L':', 2U) != std::wstring::npos ||
      std::any_of(
          path.begin(), path.end(),
          [](const wchar_t value) {
            return value >= 0 && value < 0x20;
          })) {
    return true;
  }
  std::wstring normalized = path;
  std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
  return normalized.find(L"\\..\\") != std::wstring::npos ||
      normalized.ends_with(L"\\..") ||
      normalized.starts_with(L"..\\") ||
      normalized.find(L"\\.\\") != std::wstring::npos ||
      normalized.ends_with(L"\\.");
}

std::wstring normalize_directory(std::wstring value) {
  std::replace(value.begin(), value.end(), L'/', L'\\');
  while (value.size() > 3U && value.back() == L'\\') {
    value.pop_back();
  }
  return value;
}

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  return left.size() == right.size() &&
      _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

bool ends_with_case_insensitive(
    const std::wstring_view value,
    const std::wstring_view suffix) noexcept {
  return value.size() >= suffix.size() &&
      equals_case_insensitive(value.substr(value.size() - suffix.size()),
                              suffix);
}

Status validate_directory_path(
    const std::wstring& directory,
    const std::wstring_view operation) {
  const std::wstring normalized = normalize_directory(directory);
  if (!is_drive_absolute_path(normalized) ||
      normalized.size() > kMaximumPathCharacters ||
      has_forbidden_path_text(normalized)) {
    return Status::failure(registration_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        std::wstring(operation),
        L"ドライブ絶対パスだけを指定でき、相対要素や特殊文字は使用できません"));
  }
  return clonecore::success_status();
}

Status validate_offline_windows_directory(
    const std::wstring& directory) {
  const Status path = validate_directory_path(
      directory, L"WinRE登録対象Windowsパス");
  if (!path) {
    return path;
  }
  if (!ends_with_case_insensitive(
          normalize_directory(directory), L"\\Windows")) {
    return Status::failure(registration_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"WinRE登録対象Windowsパス",
        L"終端がWindowsのオフラインWindowsディレクトリが必要です"));
  }
  return clonecore::success_status();
}

Status validate_trusted_system_directory(
    const std::wstring& directory) {
  const Status path = validate_directory_path(
      directory, L"WinRE登録System32パス");
  if (!path) {
    return path;
  }
  if (!ends_with_case_insensitive(
          normalize_directory(directory), L"\\Windows\\System32")) {
    return Status::failure(registration_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"WinRE登録System32パス",
        L"現在のWindowsまたはWinPEのSystem32ディレクトリが必要です"));
  }
  return clonecore::success_status();
}

std::wstring make_candidate_path(const std::wstring& directory) {
  return normalize_directory(directory) + L"\\Winre.wim";
}

bool candidate_directory_matches_path_kind(
    const std::wstring& candidate_directory,
    const std::wstring& offline_windows_directory,
    const WinReRegisteredPathKind path_kind) {
  const std::wstring candidate =
      normalize_directory(candidate_directory);
  if (path_kind ==
      WinReRegisteredPathKind::windows_system32_recovery) {
    return equals_case_insensitive(
        candidate,
        normalize_directory(offline_windows_directory) +
            L"\\System32\\Recovery");
  }
  return path_kind == WinReRegisteredPathKind::recovery_windows_re &&
      ends_with_case_insensitive(candidate, L"\\Recovery\\WindowsRE");
}

bool valid_identity_shape(
    const WinReRegistrationImageIdentity& identity) noexcept {
  const bool file_id_present = std::any_of(
      identity.file_id.begin(), identity.file_id.end(),
      [](const std::byte value) { return value != std::byte{}; });
  const bool hash_present = std::any_of(
      identity.sha256.begin(), identity.sha256.end(),
      [](const std::byte value) { return value != std::byte{}; });
  return !identity.requested_path.empty() &&
      !identity.opened_final_path.empty() &&
      identity.volume_serial_number != 0U && file_id_present &&
      identity.length != 0U &&
      identity.length <= kMaximumWinReImageBytes && hash_present;
}

Status validate_prior_diagnostic(
    const WinReRegistrationRequest& request) {
  const WinReDiagnosticReport& prior = request.prior_diagnostic;
  if (!prior.microsoft_signature_verified || !prior.read_only_command ||
      prior.exit_code != 0U ||
      prior.source_state == WinReSourceState::unknown) {
    return Status::failure(registration_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"WinRE登録前診断",
        L"署名済み読取り専用診断で既存WinRE状態を確定できません"));
  }
  if (request.prior_state_origin ==
      WinReRegistrationPriorStateOrigin::cloned_source_stale) {
    if (prior.source_state != WinReSourceState::registered_partition ||
        !prior.registered_location_reported ||
        !prior.registered_path_kind_reported ||
        prior.registered_location_matches_expected_disk ||
        !prior.
            registered_location_mismatch_classified_as_cloned_source_stale ||
        prior.registered_partition_number == 0U ||
        prior.registered_image_present ||
        prior.fallback_image_present ||
        prior.winre_image_size_bytes != 0U ||
        !request.rollback_candidate_directory.empty() ||
        request.reviewed_rollback_image.has_value()) {
      return Status::failure(registration_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"WinREクローン元登録の安全な置換証拠",
          L"クローン元を指す不一致登録と対象側候補を明確に分離できません"));
    }
    return clonecore::success_status();
  }
  if (request.prior_state_origin !=
      WinReRegistrationPriorStateOrigin::existing_target) {
    return Status::failure(registration_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"WinRE変更前状態の由来",
        L"未対応の変更前状態由来です"));
  }
  if (prior.source_state == WinReSourceState::registered_partition) {
    if (!prior.registered_location_reported ||
        !prior.registered_path_kind_reported ||
        !prior.registered_location_matches_expected_disk ||
        !prior.registered_image_present ||
        prior.registered_partition_number == 0U ||
        prior.winre_image_size_bytes == 0U ||
        request.rollback_candidate_directory.empty() ||
        !request.reviewed_rollback_image.has_value()) {
      return Status::failure(registration_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"WinRE既存登録ロールバック証拠",
          L"既存登録先とWinre.wimを完全に固定できないため変更できません"));
    }
  } else if (prior.source_state ==
                 WinReSourceState::image_available_in_windows) {
    if (!prior.fallback_image_present ||
        prior.winre_image_size_bytes == 0U ||
        prior.registered_location_reported) {
      return Status::failure(registration_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"WinRE既存無効状態の証拠",
          L"Windows内のWinre.wimと未登録状態が一致しません"));
    }
  } else if (prior.source_state == WinReSourceState::missing) {
    if (prior.registered_location_reported ||
        prior.registered_image_present || prior.fallback_image_present ||
        prior.winre_image_size_bytes != 0U) {
      return Status::failure(registration_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"WinRE既存欠損状態の証拠",
          L"欠損診断に登録先またはWinre.wimの証拠が混在しています"));
    }
  } else {
    return Status::failure(registration_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"WinRE登録前状態",
        L"未対応のWinRE状態です"));
  }
  return clonecore::success_status();
}

Status validate_registration_request(
    const WinReRegistrationRequest& request) {
  const Status windows =
      validate_offline_windows_directory(
          request.offline_windows_directory);
  if (!windows) {
    return windows;
  }
  const Status system =
      validate_trusted_system_directory(
          request.trusted_system_directory);
  if (!system) {
    return system;
  }
  const Status candidate = validate_directory_path(
      request.candidate_directory, L"WinRE登録候補パス");
  if (!candidate) {
    return candidate;
  }
  if ((request.expected_registered_path_kind !=
           WinReRegisteredPathKind::recovery_windows_re &&
       request.expected_registered_path_kind !=
           WinReRegisteredPathKind::windows_system32_recovery) ||
      request.expected_target_partition_number == 0U ||
      !candidate_directory_matches_path_kind(
          request.candidate_directory,
          request.offline_windows_directory,
          request.expected_registered_path_kind) ||
      !request.reviewed_candidate.has_value() ||
      !valid_identity_shape(*request.reviewed_candidate) ||
      !equals_case_insensitive(
          request.reviewed_candidate->requested_path,
          make_candidate_path(request.candidate_directory))) {
    return Status::failure(registration_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"WinRE登録候補証拠",
        L"確認済みWinre.wim、対象区画、候補ディレクトリが一致しません"));
  }
  const Status prior = validate_prior_diagnostic(request);
  if (!prior) {
    return prior;
  }
  if (request.prior_state_origin ==
          WinReRegistrationPriorStateOrigin::existing_target &&
      request.prior_diagnostic.source_state ==
      WinReSourceState::registered_partition) {
    const Status rollback = validate_directory_path(
        request.rollback_candidate_directory,
        L"WinREロールバック候補パス");
    if (!rollback) {
      return rollback;
    }
    if (!valid_identity_shape(*request.reviewed_rollback_image) ||
        !candidate_directory_matches_path_kind(
            request.rollback_candidate_directory,
            request.offline_windows_directory,
            request.prior_diagnostic.registered_path_kind) ||
        !equals_case_insensitive(
            request.reviewed_rollback_image->requested_path,
            make_candidate_path(
                request.rollback_candidate_directory)) ||
        request.reviewed_rollback_image->length !=
            request.prior_diagnostic.winre_image_size_bytes) {
      return Status::failure(registration_error(
          ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"WinREロールバック候補証拠",
          L"既存登録のWinre.wimと確認済みロールバック候補が一致しません"));
    }
  } else if (!request.rollback_candidate_directory.empty() ||
             request.reviewed_rollback_image.has_value()) {
    return Status::failure(registration_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"WinRE不要なロールバック候補",
        L"既存登録がない場合はロールバック候補を指定できません"));
  }
  return clonecore::success_status();
}

Error process_exit_error(
    const std::wstring_view operation,
    const std::uint32_t exit_code) {
  return registration_error(
      ErrorCode::verification_failed,
      exit_code,
      std::wstring(operation),
      L"Microsoft標準REAgentCが正常終了しませんでした");
}

Status verify_completed_diagnostic(
    const WinReRegistrationRequest& request,
    const WinReDiagnosticReport& diagnostic) {
  if (!diagnostic.microsoft_signature_verified ||
      !diagnostic.read_only_command || diagnostic.exit_code != 0U ||
      diagnostic.source_state !=
          WinReSourceState::registered_partition ||
      !diagnostic.registered_location_reported ||
      !diagnostic.registered_path_kind_reported ||
      !diagnostic.registered_location_matches_expected_disk ||
      diagnostic.registered_path_kind !=
          request.expected_registered_path_kind ||
      !diagnostic.registered_image_present ||
      diagnostic.registered_partition_number !=
          request.expected_target_partition_number ||
      !request.reviewed_candidate.has_value() ||
      diagnostic.winre_image_size_bytes !=
          request.reviewed_candidate->length) {
    return Status::failure(registration_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"WinRE登録完了再診断",
        L"登録先ディスク、区画、Winre.wim、署名済み診断が確認計画と一致しません"));
  }
  return clonecore::success_status();
}

Status verify_rollback_diagnostic(
    const WinReRegistrationRequest& request,
    const WinReDiagnosticReport& diagnostic) {
  const WinReDiagnosticReport& prior = request.prior_diagnostic;
  if (!diagnostic.microsoft_signature_verified ||
      !diagnostic.read_only_command || diagnostic.exit_code != 0U ||
      (request.prior_state_origin ==
               WinReRegistrationPriorStateOrigin::existing_target &&
       diagnostic.source_state != prior.source_state)) {
    return Status::failure(registration_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"WinREロールバック再診断",
        L"ロールバック後のWinRE状態が変更前診断と一致しません"));
  }
  if (request.prior_state_origin ==
      WinReRegistrationPriorStateOrigin::cloned_source_stale) {
    if (diagnostic.registered_location_reported ||
        diagnostic.registered_image_present ||
        diagnostic.registered_partition_number != 0U ||
        diagnostic.source_state == WinReSourceState::unknown ||
        diagnostic.source_state ==
            WinReSourceState::registered_partition) {
      return Status::failure(registration_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"WinREクローン元登録解除の再診断",
          L"クローン元由来の登録が対象側で解除されたことを確認できません"));
    }
  } else if (prior.source_state == WinReSourceState::registered_partition) {
    if (!diagnostic.registered_location_reported ||
        !diagnostic.registered_path_kind_reported ||
        !diagnostic.registered_location_matches_expected_disk ||
        diagnostic.registered_path_kind != prior.registered_path_kind ||
        !diagnostic.registered_image_present ||
        diagnostic.registered_partition_number !=
            prior.registered_partition_number ||
        diagnostic.winre_image_size_bytes !=
            prior.winre_image_size_bytes) {
      return Status::failure(registration_error(
          ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"WinRE既存登録ロールバック再診断",
          L"既存の登録先区画またはWinre.wimを復元確認できません"));
    }
  } else if (diagnostic.registered_location_reported ||
             diagnostic.registered_image_present ||
             diagnostic.registered_partition_number != 0U ||
             diagnostic.fallback_image_present !=
                 prior.fallback_image_present ||
             diagnostic.winre_image_size_bytes !=
                 prior.winre_image_size_bytes) {
    return Status::failure(registration_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"WinRE無効状態ロールバック再診断",
        L"変更前の未登録状態へ戻ったことを確認できません"));
  }
  return clonecore::success_status();
}

void set_primary_failure(
    WinReRegistrationReport& report,
    Error error) {
  if (!report.primary_failure.has_value()) {
    report.primary_failure = std::move(error);
  }
}

void set_rollback_failure(
    WinReRegistrationReport& report,
    Error error) {
  if (!report.rollback_failure.has_value()) {
    report.rollback_failure = std::move(error);
  }
}

Result<ProcessResult> run_checked(
    IProcessRunner& runner,
    const std::wstring& executable,
    const std::vector<std::wstring>& arguments,
    const std::wstring& working_directory,
    const std::wstring_view operation) {
  auto process = runner.run(executable, arguments, working_directory);
  if (!process) {
    return process;
  }
  if (process.value().exit_code != 0U) {
    return Result<ProcessResult>::failure(
        process_exit_error(operation, process.value().exit_code));
  }
  return process;
}

Status revalidate_target(
    IWinReRegistrationTargetGuard& target_guard,
    WinReRegistrationReport& report) {
  const Status status = target_guard.revalidate_target();
  if (status) {
    ++report.target_revalidation_count;
  }
  return status;
}

void mark_incomplete_rollback(
    WinReRegistrationReport& report,
    Error error) {
  report.outcome =
      WinReRegistrationOutcome::failed_rollback_incomplete;
  set_rollback_failure(report, std::move(error));
}

void attempt_rollback(
    const WinReRegistrationRequest& request,
    const std::wstring& executable,
    const std::wstring& working_directory,
    IProcessRunner& process_runner,
    IWinReRegistrationTargetGuard& target_guard,
    IWinReDiagnosticService& diagnostic_service,
    WinReRegistrationReport& report,
    const bool target_identity_was_lost) {
  report.rollback_attempted = true;
  if (target_identity_was_lost) {
    mark_incomplete_rollback(
        report,
        registration_error(
            ErrorCode::identity_mismatch,
            ERROR_DEVICE_NOT_CONNECTED,
            L"WinREロールバック対象再識別",
            L"変更対象の再識別に失敗したため追加書込みを停止しました"));
    return;
  }

  Status target = revalidate_target(target_guard, report);
  if (!target) {
    mark_incomplete_rollback(report, target.error());
    return;
  }

  if (request.prior_state_origin ==
      WinReRegistrationPriorStateOrigin::cloned_source_stale) {
    auto disable_arguments = build_reagentc_disable_arguments(
        request.offline_windows_directory);
    if (!disable_arguments) {
      mark_incomplete_rollback(report, disable_arguments.error());
      return;
    }
    auto disabled = run_checked(
        process_runner,
        executable,
        disable_arguments.value(),
        working_directory,
        L"WinREクローン元登録解除ロールバック");
    if (!disabled) {
      mark_incomplete_rollback(report, disabled.error());
      return;
    }
  } else if (request.prior_diagnostic.source_state ==
      WinReSourceState::registered_partition) {
    auto set_arguments = build_reagentc_setreimage_arguments(
        request.rollback_candidate_directory,
        request.offline_windows_directory);
    if (!set_arguments) {
      mark_incomplete_rollback(report, set_arguments.error());
      return;
    }
    auto restored = run_checked(
        process_runner,
        executable,
        set_arguments.value(),
        working_directory,
        L"WinRE既存登録ロールバック");
    if (!restored) {
      mark_incomplete_rollback(report, restored.error());
      return;
    }
    target = revalidate_target(target_guard, report);
    if (!target) {
      mark_incomplete_rollback(report, target.error());
      return;
    }
    auto enable_arguments = build_reagentc_enable_arguments(
        request.offline_windows_directory);
    if (!enable_arguments) {
      mark_incomplete_rollback(report, enable_arguments.error());
      return;
    }
    auto enabled = run_checked(
        process_runner,
        executable,
        enable_arguments.value(),
        working_directory,
        L"WinRE既存登録再有効化");
    if (!enabled) {
      mark_incomplete_rollback(report, enabled.error());
      return;
    }
  } else {
    auto disable_arguments = build_reagentc_disable_arguments(
        request.offline_windows_directory);
    if (!disable_arguments) {
      mark_incomplete_rollback(report, disable_arguments.error());
      return;
    }
    auto disabled = run_checked(
        process_runner,
        executable,
        disable_arguments.value(),
        working_directory,
        L"WinRE未登録状態ロールバック");
    if (!disabled) {
      mark_incomplete_rollback(report, disabled.error());
      return;
    }
  }
  report.rollback_completed = true;

  target = revalidate_target(target_guard, report);
  if (!target) {
    mark_incomplete_rollback(report, target.error());
    return;
  }
  auto diagnostic = diagnostic_service.inspect(
      request.offline_windows_directory,
      request.expected_target_disk_number);
  if (!diagnostic) {
    mark_incomplete_rollback(report, diagnostic.error());
    return;
  }
  report.final_diagnostic = diagnostic.value();
  const Status verified =
      verify_rollback_diagnostic(request, diagnostic.value());
  if (!verified) {
    mark_incomplete_rollback(report, verified.error());
    return;
  }
  report.rollback_verified = true;
  report.outcome = request.prior_state_origin ==
          WinReRegistrationPriorStateOrigin::cloned_source_stale
      ? WinReRegistrationOutcome::failed_safe_unregistered
      : WinReRegistrationOutcome::failed_rolled_back;
}

std::uint64_t file_time_value(const LARGE_INTEGER& value) noexcept {
  return static_cast<std::uint64_t>(value.QuadPart);
}

class BCryptAlgorithm final {
 public:
  BCryptAlgorithm() = default;
  BCryptAlgorithm(const BCryptAlgorithm&) = delete;
  BCryptAlgorithm& operator=(const BCryptAlgorithm&) = delete;
  ~BCryptAlgorithm() {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0U);
    }
  }

  BCRYPT_ALG_HANDLE* put() noexcept { return &handle_; }
  BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_{};
};

class BCryptHash final {
 public:
  BCryptHash() = default;
  BCryptHash(const BCryptHash&) = delete;
  BCryptHash& operator=(const BCryptHash&) = delete;
  ~BCryptHash() {
    if (handle_ != nullptr) {
      BCryptDestroyHash(handle_);
    }
  }

  BCRYPT_HASH_HANDLE* put() noexcept { return &handle_; }
  BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_{};
};

Result<WinReImageSha256> hash_file_handle(
    const HANDLE file,
    const std::uint64_t expected_length) {
  BCryptAlgorithm algorithm;
  NTSTATUS crypto = BCryptOpenAlgorithmProvider(
      algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
  if (!BCRYPT_SUCCESS(crypto)) {
    return Result<WinReImageSha256>::failure(registration_error(
        ErrorCode::internal_error,
        ERROR_INVALID_FUNCTION,
        L"WinRE候補SHA-256初期化",
        L"Windows CNG SHA-256を初期化できません"));
  }
  DWORD object_length = 0U;
  DWORD returned = 0U;
  crypto = BCryptGetProperty(
      algorithm.get(), BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length),
      sizeof(object_length), &returned, 0U);
  if (!BCRYPT_SUCCESS(crypto) || returned != sizeof(object_length) ||
      object_length == 0U || object_length > 1024U * 1024U) {
    return Result<WinReImageSha256>::failure(registration_error(
        ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"WinRE候補SHA-256作業領域",
        L"Windows CNGが安全な作業領域寸法を返しませんでした"));
  }
  std::vector<UCHAR> hash_object(object_length);
  BCryptHash hash;
  crypto = BCryptCreateHash(
      algorithm.get(), hash.put(), hash_object.data(), object_length,
      nullptr, 0U, 0U);
  if (!BCRYPT_SUCCESS(crypto)) {
    return Result<WinReImageSha256>::failure(registration_error(
        ErrorCode::internal_error,
        ERROR_INVALID_FUNCTION,
        L"WinRE候補SHA-256生成",
        L"Windows CNG SHA-256状態を生成できません"));
  }

  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
    return Result<WinReImageSha256>::failure(
        clonecore::make_win32_error(
            ErrorCode::io_failed,
            L"WinRE候補SHA-256読取り位置",
            GetLastError()));
  }
  std::vector<UCHAR> buffer(kHashReadBytes);
  std::uint64_t total = 0U;
  for (;;) {
    DWORD read = 0U;
    if (!ReadFile(
            file, buffer.data(), static_cast<DWORD>(buffer.size()),
            &read, nullptr)) {
      return Result<WinReImageSha256>::failure(
          clonecore::make_win32_error(
              ErrorCode::io_failed,
              L"WinRE候補SHA-256読取り",
              GetLastError()));
    }
    if (read == 0U) {
      break;
    }
    if (total > expected_length ||
        static_cast<std::uint64_t>(read) > expected_length - total) {
      return Result<WinReImageSha256>::failure(registration_error(
          ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"WinRE候補SHA-256寸法",
          L"ハッシュ中にWinre.wimの寸法が変化しました"));
    }
    crypto = BCryptHashData(hash.get(), buffer.data(), read, 0U);
    if (!BCRYPT_SUCCESS(crypto)) {
      return Result<WinReImageSha256>::failure(registration_error(
          ErrorCode::internal_error,
          ERROR_INVALID_FUNCTION,
          L"WinRE候補SHA-256更新",
          L"Windows CNG SHA-256更新に失敗しました"));
    }
    total += read;
  }
  if (total != expected_length) {
    return Result<WinReImageSha256>::failure(registration_error(
        ErrorCode::identity_mismatch,
        ERROR_HANDLE_EOF,
        L"WinRE候補SHA-256完全読取り",
        L"Winre.wimを確認済み寸法まで読み取れませんでした"));
  }
  WinReImageSha256 digest{};
  crypto = BCryptFinishHash(
      hash.get(), reinterpret_cast<PUCHAR>(digest.data()),
      static_cast<ULONG>(digest.size()), 0U);
  if (!BCRYPT_SUCCESS(crypto)) {
    return Result<WinReImageSha256>::failure(registration_error(
        ErrorCode::internal_error,
        ERROR_INVALID_FUNCTION,
        L"WinRE候補SHA-256確定",
        L"Windows CNG SHA-256を確定できません"));
  }
  return Result<WinReImageSha256>::success(digest);
}

Result<std::wstring> normalized_full_path(const std::wstring& path) {
  if (!is_drive_absolute_path(path) || path.size() > kMaximumPathCharacters ||
      has_forbidden_path_text(path)) {
    return Result<std::wstring>::failure(registration_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"WinRE候補絶対パス",
        L"Winre.wimには安全なドライブ絶対パスが必要です"));
  }
  const DWORD required = GetFullPathNameW(path.c_str(), 0U, nullptr, nullptr);
  if (required == 0U || required > kMaximumPathCharacters) {
    return Result<std::wstring>::failure(
        clonecore::make_win32_error(
            ErrorCode::invalid_argument,
            L"WinRE候補絶対パス正規化",
            required == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW));
  }
  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
  const DWORD length = GetFullPathNameW(
      path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
  if (length == 0U || length >= buffer.size()) {
    return Result<std::wstring>::failure(
        clonecore::make_win32_error(
            ErrorCode::invalid_argument,
            L"WinRE候補絶対パス正規化",
            length == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW));
  }
  std::wstring normalized(buffer.data(), length);
  std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
  if (!ends_with_case_insensitive(normalized, L"\\Winre.wim")) {
    return Result<std::wstring>::failure(registration_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_NAME,
        L"WinRE候補ファイル名",
        L"固定名Winre.wimだけを登録候補にできます"));
  }
  return Result<std::wstring>::success(std::move(normalized));
}

Result<std::vector<UniqueHandle>> lock_directory_chain(
    const std::wstring& file_path) {
  std::vector<std::wstring> directories;
  directories.push_back(file_path.substr(0U, 3U));
  const std::size_t final_separator = file_path.find_last_of(L'\\');
  if (final_separator == std::wstring::npos || final_separator < 3U) {
    return Result<std::vector<UniqueHandle>>::failure(
        registration_error(
            ErrorCode::invalid_argument,
            ERROR_INVALID_NAME,
            L"WinRE候補ディレクトリ列",
            L"Winre.wimの親ディレクトリを特定できません"));
  }
  std::size_t separator = file_path.find(L'\\', 3U);
  while (separator != std::wstring::npos &&
         separator <= final_separator) {
    if (separator > 3U) {
      directories.push_back(file_path.substr(0U, separator));
    }
    if (separator == final_separator) {
      break;
    }
    separator = file_path.find(L'\\', separator + 1U);
  }
  if (directories.empty() ||
      !equals_case_insensitive(directories.back(),
                               file_path.substr(0U, final_separator))) {
    directories.push_back(file_path.substr(0U, final_separator));
  }

  std::vector<UniqueHandle> locks;
  locks.reserve(directories.size());
  for (const std::wstring& directory : directories) {
    HANDLE raw = CreateFileW(
        directory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
      return Result<std::vector<UniqueHandle>>::failure(
          clonecore::make_win32_error(
              ErrorCode::io_failed,
              L"WinRE候補ディレクトリ固定",
              GetLastError()));
    }
    UniqueHandle handle(raw);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            handle.get(), FileAttributeTagInfo, &attributes,
            sizeof(attributes))) {
      return Result<std::vector<UniqueHandle>>::failure(
          clonecore::make_win32_error(
              ErrorCode::query_failed,
              L"WinRE候補ディレクトリ属性",
              GetLastError()));
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return Result<std::vector<UniqueHandle>>::failure(
          registration_error(
              ErrorCode::verification_failed,
              ERROR_REPARSE_TAG_INVALID,
              L"WinRE候補ディレクトリ通常性",
              L"Winre.wimの祖先にreparse pointまたは非ディレクトリがあります"));
    }
    locks.push_back(std::move(handle));
  }
  return Result<std::vector<UniqueHandle>>::success(std::move(locks));
}

Result<std::wstring> final_path_from_handle(const HANDLE file) {
  const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
  const DWORD required = GetFinalPathNameByHandleW(file, nullptr, 0U, flags);
  if (required == 0U || required > kMaximumPathCharacters) {
    return Result<std::wstring>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"WinRE候補最終パス長",
            required == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW));
  }
  std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1U);
  const DWORD length = GetFinalPathNameByHandleW(
      file, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
  if (length == 0U || length >= buffer.size()) {
    return Result<std::wstring>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"WinRE候補最終パス",
            length == 0U ? GetLastError() : ERROR_BUFFER_OVERFLOW));
  }
  return Result<std::wstring>::success(
      std::wstring(buffer.data(), length));
}

Result<WinReRegistrationImageIdentity> observe_opened_file(
    const HANDLE file,
    const std::wstring& requested_path) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_STANDARD_INFO standard{};
  FILE_BASIC_INFO basic{};
  FILE_ID_INFO file_id{};
  if (!GetFileInformationByHandleEx(
          file, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      !GetFileInformationByHandleEx(
          file, FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileInformationByHandleEx(
          file, FileBasicInfo, &basic, sizeof(basic)) ||
      !GetFileInformationByHandleEx(
          file, FileIdInfo, &file_id, sizeof(file_id))) {
    return Result<WinReRegistrationImageIdentity>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"WinRE候補ファイル証拠",
            GetLastError()));
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
      standard.Directory != FALSE || standard.NumberOfLinks != 1U ||
      standard.EndOfFile.QuadPart <= 0 ||
      static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) >
          kMaximumWinReImageBytes ||
      file_id.VolumeSerialNumber == 0U) {
    return Result<WinReRegistrationImageIdentity>::failure(
        registration_error(
            ErrorCode::verification_failed,
            ERROR_FILE_INVALID,
            L"WinRE候補通常ファイル検証",
            L"Winre.wimは単一リンクの通常ファイルで安全な寸法である必要があります"));
  }
  auto final_path = final_path_from_handle(file);
  if (!final_path) {
    return Result<WinReRegistrationImageIdentity>::failure(
        final_path.error());
  }
  WinReRegistrationImageIdentity identity{
      .requested_path = requested_path,
      .opened_final_path = final_path.take_value(),
      .volume_serial_number = file_id.VolumeSerialNumber,
      .length = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
      .last_write_time = file_time_value(basic.LastWriteTime),
      .change_time = file_time_value(basic.ChangeTime),
  };
  std::copy_n(
      reinterpret_cast<const std::byte*>(file_id.FileId.Identifier),
      identity.file_id.size(), identity.file_id.begin());
  auto digest = hash_file_handle(file, identity.length);
  if (!digest) {
    return Result<WinReRegistrationImageIdentity>::failure(
        digest.error());
  }
  identity.sha256 = digest.value();

  FILE_STANDARD_INFO after_standard{};
  FILE_BASIC_INFO after_basic{};
  FILE_ID_INFO after_file_id{};
  if (!GetFileInformationByHandleEx(
          file, FileStandardInfo, &after_standard,
          sizeof(after_standard)) ||
      !GetFileInformationByHandleEx(
          file, FileBasicInfo, &after_basic, sizeof(after_basic)) ||
      !GetFileInformationByHandleEx(
          file, FileIdInfo, &after_file_id, sizeof(after_file_id))) {
    return Result<WinReRegistrationImageIdentity>::failure(
        clonecore::make_win32_error(
            ErrorCode::query_failed,
            L"WinRE候補ハッシュ後再識別",
            GetLastError()));
  }
  if (after_standard.NumberOfLinks != 1U || after_standard.Directory != FALSE ||
      after_standard.EndOfFile.QuadPart != standard.EndOfFile.QuadPart ||
      after_basic.LastWriteTime.QuadPart != basic.LastWriteTime.QuadPart ||
      after_basic.ChangeTime.QuadPart != basic.ChangeTime.QuadPart ||
      after_file_id.VolumeSerialNumber != file_id.VolumeSerialNumber ||
      std::memcmp(after_file_id.FileId.Identifier,
                  file_id.FileId.Identifier,
                  sizeof(file_id.FileId.Identifier)) != 0) {
    return Result<WinReRegistrationImageIdentity>::failure(
        registration_error(
            ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"WinRE候補ハッシュ後再識別",
            L"Winre.wimのFile ID、寸法、時刻、リンク数が読取り中に変化しました"));
  }
  return Result<WinReRegistrationImageIdentity>::success(
      std::move(identity));
}

class WindowsWinReRegistrationImageLock final
    : public IWinReRegistrationImageLock {
 public:
  WindowsWinReRegistrationImageLock(
      std::vector<UniqueHandle> directories,
      UniqueHandle file,
      WinReRegistrationImageIdentity identity)
      : directories_(std::move(directories)),
        file_(std::move(file)),
        identity_(std::move(identity)) {}

  const WinReRegistrationImageIdentity& identity()
      const noexcept override {
    return identity_;
  }

 private:
  std::vector<UniqueHandle> directories_;
  UniqueHandle file_;
  WinReRegistrationImageIdentity identity_;
};

Result<std::unique_ptr<IWinReRegistrationImageLock>>
lock_image_with_windows_apis(const std::wstring& path) {
  auto normalized = normalized_full_path(path);
  if (!normalized) {
    return Result<std::unique_ptr<IWinReRegistrationImageLock>>::failure(
        normalized.error());
  }
  auto directories = lock_directory_chain(normalized.value());
  if (!directories) {
    return Result<std::unique_ptr<IWinReRegistrationImageLock>>::failure(
        directories.error());
  }
  HANDLE raw = CreateFileW(
      normalized.value().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return Result<std::unique_ptr<IWinReRegistrationImageLock>>::failure(
        clonecore::make_win32_error(
            ErrorCode::io_failed,
            L"WinRE候補読取り専用固定",
            GetLastError()));
  }
  UniqueHandle file(raw);
  auto identity = observe_opened_file(file.get(), normalized.value());
  if (!identity) {
    return Result<std::unique_ptr<IWinReRegistrationImageLock>>::failure(
        identity.error());
  }
  return Result<std::unique_ptr<IWinReRegistrationImageLock>>::success(
      std::make_unique<WindowsWinReRegistrationImageLock>(
          directories.take_value(), std::move(file),
          identity.take_value()));
}

class WindowsWinReRegistrationImageLocker final
    : public IWinReRegistrationImageLocker {
 public:
  Result<std::unique_ptr<IWinReRegistrationImageLock>>
  lock_regular_image(const std::wstring& path) override {
    return lock_image_with_windows_apis(path);
  }
};

}  // namespace

bool equivalent_winre_registration_image_identity(
    const WinReRegistrationImageIdentity& left,
    const WinReRegistrationImageIdentity& right) noexcept {
  return equals_case_insensitive(left.requested_path,
                                 right.requested_path) &&
      equals_case_insensitive(left.opened_final_path,
                              right.opened_final_path) &&
      left.volume_serial_number == right.volume_serial_number &&
      left.file_id == right.file_id && left.length == right.length &&
      left.last_write_time == right.last_write_time &&
      left.change_time == right.change_time && left.sha256 == right.sha256;
}

Result<std::vector<std::wstring>> build_reagentc_setreimage_arguments(
    const std::wstring& candidate_directory,
    const std::wstring& offline_windows_directory) {
  const Status candidate = validate_directory_path(
      candidate_directory, L"WinRE登録候補パス");
  if (!candidate) {
    return Result<std::vector<std::wstring>>::failure(
        candidate.error());
  }
  const Status windows =
      validate_offline_windows_directory(offline_windows_directory);
  if (!windows) {
    return Result<std::vector<std::wstring>>::failure(
        windows.error());
  }
  return Result<std::vector<std::wstring>>::success({
      L"/setreimage",
      L"/path",
      normalize_directory(candidate_directory),
      L"/target",
      normalize_directory(offline_windows_directory),
  });
}

Result<std::vector<std::wstring>> build_reagentc_enable_arguments(
    const std::wstring& offline_windows_directory) {
  const Status windows =
      validate_offline_windows_directory(offline_windows_directory);
  if (!windows) {
    return Result<std::vector<std::wstring>>::failure(
        windows.error());
  }
  return Result<std::vector<std::wstring>>::success({
      L"/enable",
      L"/target",
      normalize_directory(offline_windows_directory),
  });
}

Result<std::vector<std::wstring>> build_reagentc_disable_arguments(
    const std::wstring& offline_windows_directory) {
  const Status windows =
      validate_offline_windows_directory(offline_windows_directory);
  if (!windows) {
    return Result<std::vector<std::wstring>>::failure(
        windows.error());
  }
  return Result<std::vector<std::wstring>>::success({
      L"/disable",
      L"/target",
      normalize_directory(offline_windows_directory),
  });
}

Result<WinReRegistrationReport>
execute_winre_registration_transaction(
    const WinReRegistrationRequest& request,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IWinReRegistrationImageLocker& image_locker,
    IWinReRegistrationTargetGuard& target_guard,
    IWinReDiagnosticService& diagnostic_service) {
  if (request.intent ==
      WinReRegistrationIntent::normal_boot_only_partial) {
    if (request.reviewed_candidate.has_value() ||
        request.reviewed_rollback_image.has_value() ||
        !request.candidate_directory.empty() ||
        !request.rollback_candidate_directory.empty()) {
      return Result<WinReRegistrationReport>::failure(
          registration_error(
              ErrorCode::invalid_argument,
              ERROR_INVALID_DATA,
              L"WinRE部分修復入力",
              L"通常起動だけの部分修復へWinRE書込み候補を混在できません"));
    }
    return Result<WinReRegistrationReport>::success(
        WinReRegistrationReport{
            .outcome =
                WinReRegistrationOutcome::normal_boot_only_partial,
        });
  }
  if (request.intent !=
      WinReRegistrationIntent::register_verified_image) {
    return Result<WinReRegistrationReport>::failure(
        registration_error(
            ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"WinRE登録目的",
            L"未対応のWinRE登録目的です"));
  }

  const Status valid = validate_registration_request(request);
  if (!valid) {
    return Result<WinReRegistrationReport>::failure(valid.error());
  }
  auto set_arguments = build_reagentc_setreimage_arguments(
      request.candidate_directory,
      request.offline_windows_directory);
  auto enable_arguments = build_reagentc_enable_arguments(
      request.offline_windows_directory);
  if (!set_arguments) {
    return Result<WinReRegistrationReport>::failure(
        set_arguments.error());
  }
  if (!enable_arguments) {
    return Result<WinReRegistrationReport>::failure(
        enable_arguments.error());
  }

  auto candidate = image_locker.lock_regular_image(
      make_candidate_path(request.candidate_directory));
  if (!candidate) {
    return Result<WinReRegistrationReport>::failure(
        candidate.error());
  }
  if (!equivalent_winre_registration_image_identity(
          candidate.value()->identity(),
          *request.reviewed_candidate)) {
    return Result<WinReRegistrationReport>::failure(
        registration_error(
            ErrorCode::identity_mismatch,
            ERROR_FILE_INVALID,
            L"WinRE候補実行直前再識別",
            L"確認後にWinre.wimのFile ID、寸法、時刻またはSHA-256が変化しました"));
  }

  std::unique_ptr<IWinReRegistrationImageLock> rollback_candidate;
  if (request.prior_state_origin ==
          WinReRegistrationPriorStateOrigin::existing_target &&
      request.prior_diagnostic.source_state ==
      WinReSourceState::registered_partition) {
    auto locked = image_locker.lock_regular_image(
        make_candidate_path(request.rollback_candidate_directory));
    if (!locked) {
      return Result<WinReRegistrationReport>::failure(
          locked.error());
    }
    if (!equivalent_winre_registration_image_identity(
            locked.value()->identity(),
            *request.reviewed_rollback_image)) {
      return Result<WinReRegistrationReport>::failure(
          registration_error(
              ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"WinREロールバック候補実行直前再識別",
              L"既存登録のWinre.wimが確認後に変化しました"));
    }
    rollback_candidate = locked.take_value();
  }

  const std::wstring working_directory =
      normalize_directory(request.trusted_system_directory);
  const std::wstring executable =
      working_directory + L"\\reagentc.exe";
  const Status trusted =
      trust_verifier.verify_microsoft_signed(executable);
  if (!trusted) {
    return Result<WinReRegistrationReport>::failure(
        trusted.error());
  }

  WinReRegistrationReport report{
      .outcome =
          WinReRegistrationOutcome::failed_rollback_incomplete,
      .candidate_locked = true,
      .prior_candidate_locked = rollback_candidate != nullptr,
      .reagentc_signature_verified = true,
  };
  Status target = revalidate_target(target_guard, report);
  if (!target) {
    return Result<WinReRegistrationReport>::failure(
        target.error());
  }

  if (request.prior_state_origin ==
      WinReRegistrationPriorStateOrigin::cloned_source_stale) {
    auto disable_arguments = build_reagentc_disable_arguments(
        request.offline_windows_directory);
    if (!disable_arguments) {
      return Result<WinReRegistrationReport>::failure(
          disable_arguments.error());
    }
    auto disabled = run_checked(
        process_runner,
        executable,
        disable_arguments.value(),
        working_directory,
        L"WinREクローン元登録の事前解除");
    if (!disabled) {
      set_primary_failure(report, disabled.error());
      attempt_rollback(
          request, executable, working_directory, process_runner,
          target_guard, diagnostic_service, report, false);
      return Result<WinReRegistrationReport>::success(
          std::move(report));
    }
    report.cloned_source_registration_disabled = true;
    target = revalidate_target(target_guard, report);
    if (!target) {
      set_primary_failure(report, target.error());
      attempt_rollback(
          request, executable, working_directory, process_runner,
          target_guard, diagnostic_service, report, true);
      return Result<WinReRegistrationReport>::success(
          std::move(report));
    }
  }

  auto set = run_checked(
      process_runner,
      executable,
      set_arguments.value(),
      working_directory,
      L"WinRE登録先設定");
  if (!set) {
    set_primary_failure(report, set.error());
    attempt_rollback(
        request, executable, working_directory, process_runner,
        target_guard, diagnostic_service, report, false);
    return Result<WinReRegistrationReport>::success(std::move(report));
  }
  report.set_reimage_completed = true;

  target = revalidate_target(target_guard, report);
  if (!target) {
    set_primary_failure(report, target.error());
    attempt_rollback(
        request, executable, working_directory, process_runner,
        target_guard, diagnostic_service, report, true);
    return Result<WinReRegistrationReport>::success(std::move(report));
  }
  auto enabled = run_checked(
      process_runner,
      executable,
      enable_arguments.value(),
      working_directory,
      L"WinRE有効化");
  if (!enabled) {
    set_primary_failure(report, enabled.error());
    attempt_rollback(
        request, executable, working_directory, process_runner,
        target_guard, diagnostic_service, report, false);
    return Result<WinReRegistrationReport>::success(std::move(report));
  }
  report.enable_completed = true;

  target = revalidate_target(target_guard, report);
  if (!target) {
    set_primary_failure(report, target.error());
    attempt_rollback(
        request, executable, working_directory, process_runner,
        target_guard, diagnostic_service, report, true);
    return Result<WinReRegistrationReport>::success(std::move(report));
  }
  auto final_diagnostic = diagnostic_service.inspect(
      request.offline_windows_directory,
      request.expected_target_disk_number);
  if (!final_diagnostic) {
    set_primary_failure(report, final_diagnostic.error());
    attempt_rollback(
        request, executable, working_directory, process_runner,
        target_guard, diagnostic_service, report, false);
    return Result<WinReRegistrationReport>::success(std::move(report));
  }
  report.final_diagnostic = final_diagnostic.value();
  const Status verified = verify_completed_diagnostic(
      request, final_diagnostic.value());
  if (!verified) {
    set_primary_failure(report, verified.error());
    attempt_rollback(
        request, executable, working_directory, process_runner,
        target_guard, diagnostic_service, report, false);
    return Result<WinReRegistrationReport>::success(std::move(report));
  }

  report.registration_verified = true;
  report.outcome = WinReRegistrationOutcome::completed;
  return Result<WinReRegistrationReport>::success(std::move(report));
}

Result<WinReRegistrationImageIdentity>
observe_winre_registration_image_with_windows_apis(
    const std::wstring& path) {
  auto locked = lock_image_with_windows_apis(path);
  if (!locked) {
    return Result<WinReRegistrationImageIdentity>::failure(
        locked.error());
  }
  return Result<WinReRegistrationImageIdentity>::success(
      locked.value()->identity());
}

std::unique_ptr<IWinReRegistrationImageLocker>
make_windows_winre_registration_image_locker() {
  return std::make_unique<WindowsWinReRegistrationImageLocker>();
}

}  // namespace ytec::bootrepair

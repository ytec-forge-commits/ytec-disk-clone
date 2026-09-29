#include "ytec/clonecore/completion_power_action.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>
#include <powrprof.h>

#include <string>
#include <string_view>
#include <utility>

namespace ytec::clonecore {
namespace {

constexpr DWORD kCompletionShutdownTimeoutSeconds = 0U;
constexpr BOOL kCompletionShutdownForceApplicationsClosed = FALSE;
constexpr DWORD kCompletionShutdownReason =
    SHTDN_REASON_MAJOR_APPLICATION |
    SHTDN_REASON_MINOR_MAINTENANCE |
    SHTDN_REASON_FLAG_PLANNED;

[[nodiscard]] bool is_known_action(
    const CompletionPowerAction action) noexcept {
  switch (action) {
    case CompletionPowerAction::none:
    case CompletionPowerAction::sleep:
    case CompletionPowerAction::restart:
    case CompletionPowerAction::shutdown:
      return true;
  }
  return false;
}

[[nodiscard]] Error completion_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring_view operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = std::wstring(operation),
      .message = std::move(message),
  };
}

[[nodiscard]] CompletionPowerExecutionResult force_none(Error error) {
  return CompletionPowerExecutionResult{
      .disposition = CompletionPowerExecutionDisposition::forced_none,
      .error = std::move(error),
  };
}

[[nodiscard]] CompletionPowerAvailabilityState normalize_capability_state(
    const CompletionPowerAvailabilityState state,
    const std::uint32_t native_status) noexcept {
  switch (state) {
    case CompletionPowerAvailabilityState::available:
      return native_status == ERROR_SUCCESS
          ? CompletionPowerAvailabilityState::available
          : CompletionPowerAvailabilityState::unknown;
    case CompletionPowerAvailabilityState::unavailable:
      return CompletionPowerAvailabilityState::unavailable;
    case CompletionPowerAvailabilityState::unknown:
      return CompletionPowerAvailabilityState::unknown;
  }
  return CompletionPowerAvailabilityState::unknown;
}

[[nodiscard]] ErrorCode error_code_for_shutdown_failure(
    const DWORD native_code) noexcept {
  return native_code == ERROR_ACCESS_DENIED ||
          native_code == ERROR_PRIVILEGE_NOT_HELD
      ? ErrorCode::access_denied
      : ErrorCode::io_failed;
}

[[nodiscard]] Status request_windows_shutdown(const bool restart) {
  UniqueHandle token;
  HANDLE raw_token{};
  if (OpenProcessToken(
          GetCurrentProcess(),
          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
          &raw_token) == FALSE) {
    return Status::failure(make_win32_error(
        ErrorCode::access_denied,
        L"完了後の電源操作に必要な権限の確認",
        GetLastError()));
  }
  token.reset(raw_token);

  LUID shutdown_privilege{};
  if (LookupPrivilegeValueW(
          nullptr, SE_SHUTDOWN_NAME, &shutdown_privilege) == FALSE) {
    return Status::failure(make_win32_error(
        ErrorCode::query_failed,
        L"完了後の電源操作に必要な権限の識別",
        GetLastError()));
  }

  TOKEN_PRIVILEGES requested{};
  requested.PrivilegeCount = 1U;
  requested.Privileges[0].Luid = shutdown_privilege;
  requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  TOKEN_PRIVILEGES previous{};
  DWORD previous_size{};
  SetLastError(ERROR_SUCCESS);
  const BOOL adjusted = AdjustTokenPrivileges(
      token.get(),
      FALSE,
      &requested,
      sizeof(previous),
      &previous,
      &previous_size);
  const DWORD adjustment_error = GetLastError();
  if (adjusted == FALSE || adjustment_error == ERROR_NOT_ALL_ASSIGNED) {
    return Status::failure(make_win32_error(
        ErrorCode::access_denied,
        L"完了後の電源操作に必要な権限の有効化",
        adjusted == FALSE ? adjustment_error : ERROR_PRIVILEGE_NOT_HELD));
  }

  SetLastError(ERROR_SUCCESS);
  // SAFE-007 explicitly requires a user-confirmed completion restart or
  // shutdown. Keep the analyzer exception local to this fixed API call.
#pragma warning(suppress : 28159)
  const BOOL requested_shutdown = InitiateSystemShutdownExW(
      nullptr,
      nullptr,
      kCompletionShutdownTimeoutSeconds,
      kCompletionShutdownForceApplicationsClosed,
      restart ? TRUE : FALSE,
      kCompletionShutdownReason);
  DWORD shutdown_error = requested_shutdown == FALSE
      ? GetLastError()
      : ERROR_SUCCESS;
  if (requested_shutdown == FALSE && shutdown_error == ERROR_SUCCESS) {
    shutdown_error = ERROR_GEN_FAILURE;
  }

  if (previous_size != 0U) {
    static_cast<void>(AdjustTokenPrivileges(
        token.get(), FALSE, &previous, 0U, nullptr, nullptr));
  }

  if (requested_shutdown == FALSE) {
    return Status::failure(make_win32_error(
        error_code_for_shutdown_failure(shutdown_error),
        restart ? L"完了後の再起動要求" : L"完了後のシャットダウン要求",
        shutdown_error));
  }
  return success_status();
}

class WindowsCompletionPowerPlatform final
    : public ICompletionPowerPlatform {
 public:
  SleepCapabilityReport query_sleep_capability() override {
    if (IsPwrSuspendAllowed() != FALSE) {
      return SleepCapabilityReport{
          .state = CompletionPowerAvailabilityState::available,
      };
    }
    return SleepCapabilityReport{
        .state = CompletionPowerAvailabilityState::unavailable,
        .native_status = ERROR_NOT_SUPPORTED,
        .detail =
            L"この環境ではWindowsがスリープを許可していないため、"
            L"完了後のスリープは選択できません。",
    };
  }

  Status request_sleep() override {
    SetLastError(ERROR_SUCCESS);
    if (SetSuspendState(FALSE, FALSE, FALSE) == FALSE) {
      DWORD native_code = GetLastError();
      if (native_code == ERROR_SUCCESS) {
        native_code = ERROR_GEN_FAILURE;
      }
      return Status::failure(make_win32_error(
          ErrorCode::io_failed, L"完了後のスリープ要求", native_code));
    }
    return success_status();
  }

  Status request_restart() override {
    return request_windows_shutdown(true);
  }

  Status request_shutdown() override {
    return request_windows_shutdown(false);
  }
};

}  // namespace

bool CompletionPowerAvailability::is_available(
    const CompletionPowerAction action) const noexcept {
  switch (action) {
    case CompletionPowerAction::none:
      return none == CompletionPowerAvailabilityState::available;
    case CompletionPowerAction::sleep:
      return sleep == CompletionPowerAvailabilityState::available;
    case CompletionPowerAction::restart:
      return restart == CompletionPowerAvailabilityState::available;
    case CompletionPowerAction::shutdown:
      return shutdown == CompletionPowerAvailabilityState::available;
  }
  return false;
}

CompletionPowerAvailability query_completion_power_availability(
    const CompletionPowerEnvironment environment,
    ICompletionPowerPlatform& platform) {
  CompletionPowerAvailability availability{
      .environment = environment,
  };
  switch (environment) {
    case CompletionPowerEnvironment::windows: {
      const SleepCapabilityReport sleep = platform.query_sleep_capability();
      availability.sleep =
          normalize_capability_state(sleep.state, sleep.native_status);
      availability.restart = CompletionPowerAvailabilityState::available;
      availability.shutdown = CompletionPowerAvailabilityState::available;
      availability.native_status = sleep.native_status;
      availability.detail = sleep.detail;
      if (availability.sleep == CompletionPowerAvailabilityState::unknown &&
          availability.detail.empty()) {
        availability.detail =
            L"スリープ可否を確認できないため、完了後のスリープは選択できません。";
      }
      return availability;
    }
    case CompletionPowerEnvironment::winpe:
      availability.sleep = CompletionPowerAvailabilityState::unavailable;
      availability.restart = CompletionPowerAvailabilityState::available;
      availability.shutdown = CompletionPowerAvailabilityState::available;
      availability.native_status = ERROR_NOT_SUPPORTED;
      availability.detail =
          L"WinPEでは完了後のスリープを選択できません。";
      return availability;
    case CompletionPowerEnvironment::unknown:
      availability.detail =
          L"実行環境を確認できないため、完了後は「何もしない」だけを選択できます。";
      return availability;
  }

  availability.environment = CompletionPowerEnvironment::unknown;
  availability.detail =
      L"未知の実行環境を安全に判定できないため、完了後は「何もしない」だけを選択できます。";
  return availability;
}

std::vector<CompletionPowerAction> available_completion_power_actions(
    const CompletionPowerAvailability& availability) {
  std::vector<CompletionPowerAction> actions;
  actions.reserve(4U);
  for (const CompletionPowerAction action : {
           CompletionPowerAction::none,
           CompletionPowerAction::sleep,
           CompletionPowerAction::restart,
           CompletionPowerAction::shutdown,
       }) {
    if (availability.is_available(action)) {
      actions.push_back(action);
    }
  }
  return actions;
}

CompletionPowerExecutionResult execute_completion_power_action(
    const CompletionPowerExecutionRequest& request,
    ICompletionPowerPlatform& platform) {
  if (!is_known_action(request.selection.action)) {
    return force_none(completion_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"完了後動作の選択確認",
        L"未知の完了後動作は実行できません。"));
  }

  if (request.selection.action == CompletionPowerAction::none) {
    return {};
  }

  switch (request.operation_outcome) {
    case CompletionOperationOutcome::succeeded:
      break;
    case CompletionOperationOutcome::cancelled:
      return force_none(completion_error(
          ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"完了後動作の実行条件確認",
          L"操作が取り消されたため、完了後動作を「何もしない」に固定しました。"));
    case CompletionOperationOutcome::failed:
      return force_none(completion_error(
          ErrorCode::invalid_data,
          ERROR_OPERATION_ABORTED,
          L"完了後動作の実行条件確認",
          L"操作が失敗したため、完了後動作を「何もしない」に固定しました。"));
    case CompletionOperationOutcome::partial:
      return force_none(completion_error(
          ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"完了後動作の実行条件確認",
          L"部分完了は成功として扱わず、完了後動作を「何もしない」に固定しました。"));
    case CompletionOperationOutcome::unknown:
      return force_none(completion_error(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"完了後動作の実行条件確認",
          L"操作結果を確認できないため、完了後動作を「何もしない」に固定しました。"));
  }
  if (request.operation_outcome != CompletionOperationOutcome::succeeded) {
    return force_none(completion_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"完了後動作の実行条件確認",
        L"未知の操作結果は成功として扱いません。"));
  }

  if (request.mandatory_verification !=
      MandatoryVerificationState::completed) {
    std::wstring message =
        L"必須検証の完了を証明できないため、完了後動作を「何もしない」に固定しました。";
    if (request.mandatory_verification ==
        MandatoryVerificationState::failed) {
      message =
          L"必須検証が失敗したため、完了後動作を「何もしない」に固定しました。";
    } else if (request.mandatory_verification ==
               MandatoryVerificationState::incomplete) {
      message =
          L"必須検証が未完了のため、完了後動作を「何もしない」に固定しました。";
    }
    return force_none(completion_error(
        ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"完了後動作の必須検証確認",
        std::move(message)));
  }

  if (request.sleep_prevention_release !=
      SleepPreventionReleaseState::released) {
    std::wstring message =
        L"自動スリープ防止を解除済みと確認できないため、完了後動作を実行しません。";
    if (request.sleep_prevention_release ==
        SleepPreventionReleaseState::still_active) {
      message =
          L"自動スリープ防止が有効なままのため、完了後動作を実行しません。";
    } else if (request.sleep_prevention_release ==
               SleepPreventionReleaseState::release_failed) {
      message =
          L"自動スリープ防止の解除に失敗したため、完了後動作を実行しません。";
    }
    return force_none(completion_error(
        ErrorCode::confirmation_required,
        ERROR_INVALID_DATA,
        L"完了後動作のスリープ防止解除確認",
        std::move(message)));
  }

  const bool binding_valid = request.operation_binding != 0U &&
      request.selection.explicitly_selected &&
      request.selection.operation_binding == request.operation_binding &&
      request.reconfirmation.explicitly_reconfirmed_immediately_before_execution &&
      is_known_action(request.reconfirmation.action) &&
      request.reconfirmation.action == request.selection.action &&
      request.reconfirmation.operation_binding == request.operation_binding;
  if (!binding_valid) {
    return force_none(completion_error(
        ErrorCode::confirmation_required,
        ERROR_INVALID_DATA,
        L"完了後動作の再確認",
        L"この操作に結び付いた明示選択と実行直前の再確認が一致しないため、"
        L"完了後動作を実行しません。"));
  }

  const CompletionPowerAvailability availability =
      query_completion_power_availability(request.environment, platform);
  if (!availability.is_available(request.selection.action)) {
    std::wstring message = availability.detail;
    if (message.empty()) {
      message =
          L"選択された完了後動作は現在の環境で利用できないため、実行しません。";
    }
    return force_none(completion_error(
        ErrorCode::unsupported_platform,
        availability.native_status == ERROR_SUCCESS
            ? ERROR_NOT_SUPPORTED
            : availability.native_status,
        L"完了後動作の利用可否確認",
        std::move(message)));
  }

  Status platform_result = success_status();
  switch (request.selection.action) {
    case CompletionPowerAction::sleep:
      platform_result = platform.request_sleep();
      break;
    case CompletionPowerAction::restart:
      platform_result = platform.request_restart();
      break;
    case CompletionPowerAction::shutdown:
      platform_result = platform.request_shutdown();
      break;
    case CompletionPowerAction::none:
      return {};
  }

  if (!platform_result.has_value()) {
    return CompletionPowerExecutionResult{
        .disposition = CompletionPowerExecutionDisposition::request_failed,
        .requested_action = request.selection.action,
        .error = platform_result.error(),
    };
  }
  return CompletionPowerExecutionResult{
      .disposition = CompletionPowerExecutionDisposition::request_accepted,
      .requested_action = request.selection.action,
      .effective_action = request.selection.action,
  };
}

std::unique_ptr<ICompletionPowerPlatform>
make_windows_completion_power_platform() {
  return std::make_unique<WindowsCompletionPowerPlatform>();
}

}  // namespace ytec::clonecore

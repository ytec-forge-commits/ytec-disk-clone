#include "ytec/clonecore/completion_power_action.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::clonecore::CompletionOperationOutcome;
using ytec::clonecore::CompletionPowerAction;
using ytec::clonecore::CompletionPowerAvailabilityState;
using ytec::clonecore::CompletionPowerEnvironment;
using ytec::clonecore::CompletionPowerExecutionDisposition;
using ytec::clonecore::CompletionPowerExecutionRequest;
using ytec::clonecore::Error;
using ytec::clonecore::ErrorCode;
using ytec::clonecore::ICompletionPowerPlatform;
using ytec::clonecore::MandatoryVerificationState;
using ytec::clonecore::SleepCapabilityReport;
using ytec::clonecore::SleepPreventionReleaseState;
using ytec::clonecore::Status;

constexpr std::uint64_t kOperationBinding = 0x53414645303037ULL;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

[[nodiscard]] Error synthetic_error(const wchar_t* const operation) {
  return Error{
      .code = ErrorCode::io_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = operation,
      .message = L"synthetic platform failure",
  };
}

class MockPlatform final : public ICompletionPowerPlatform {
 public:
  SleepCapabilityReport sleep_capability{
      .state = CompletionPowerAvailabilityState::available,
  };
  std::optional<Error> sleep_error;
  std::optional<Error> restart_error;
  std::optional<Error> shutdown_error;
  std::uint32_t sleep_query_calls{};
  std::uint32_t sleep_calls{};
  std::uint32_t restart_calls{};
  std::uint32_t shutdown_calls{};

  SleepCapabilityReport query_sleep_capability() override {
    ++sleep_query_calls;
    return sleep_capability;
  }

  Status request_sleep() override {
    ++sleep_calls;
    return sleep_error.has_value()
        ? Status::failure(*sleep_error)
        : ytec::clonecore::success_status();
  }

  Status request_restart() override {
    ++restart_calls;
    return restart_error.has_value()
        ? Status::failure(*restart_error)
        : ytec::clonecore::success_status();
  }

  Status request_shutdown() override {
    ++shutdown_calls;
    return shutdown_error.has_value()
        ? Status::failure(*shutdown_error)
        : ytec::clonecore::success_status();
  }

  [[nodiscard]] std::uint32_t request_calls() const noexcept {
    return sleep_calls + restart_calls + shutdown_calls;
  }
};

[[nodiscard]] CompletionPowerExecutionRequest ready_request(
    const CompletionPowerAction action,
    const CompletionPowerEnvironment environment =
        CompletionPowerEnvironment::windows) {
  return CompletionPowerExecutionRequest{
      .environment = environment,
      .operation_outcome = CompletionOperationOutcome::succeeded,
      .mandatory_verification = MandatoryVerificationState::completed,
      .sleep_prevention_release = SleepPreventionReleaseState::released,
      .operation_binding = kOperationBinding,
      .selection = {
          .action = action,
          .operation_binding = kOperationBinding,
          .explicitly_selected = true,
      },
      .reconfirmation = {
          .action = action,
          .operation_binding = kOperationBinding,
          .explicitly_reconfirmed_immediately_before_execution = true,
      },
  };
}

void default_is_none_and_never_dispatches() {
  MockPlatform platform;
  const CompletionPowerExecutionRequest request{};
  const auto result =
      ytec::clonecore::execute_completion_power_action(request, platform);
  check(
      request.selection.action == CompletionPowerAction::none &&
          result.disposition ==
              CompletionPowerExecutionDisposition::no_action &&
          result.requested_action == CompletionPowerAction::none &&
          result.effective_action == CompletionPowerAction::none &&
          !result.error.has_value(),
      "The default completion action must be none");
  check(
      platform.sleep_query_calls == 0U && platform.request_calls() == 0U,
      "The default request must not query or dispatch a power API");
}

void choices_are_environment_and_capability_specific() {
  MockPlatform windows;
  const auto windows_availability =
      ytec::clonecore::query_completion_power_availability(
          CompletionPowerEnvironment::windows, windows);
  const std::vector<CompletionPowerAction> windows_choices =
      ytec::clonecore::available_completion_power_actions(
          windows_availability);
  check(
      windows_choices == std::vector<CompletionPowerAction>({
          CompletionPowerAction::none,
          CompletionPowerAction::sleep,
          CompletionPowerAction::restart,
          CompletionPowerAction::shutdown,
      }) &&
          windows.sleep_query_calls == 1U,
      "Windows must offer the fixed ordered choices when sleep is supported");

  MockPlatform no_sleep;
  no_sleep.sleep_capability = {
      .state = CompletionPowerAvailabilityState::unavailable,
      .native_status = ERROR_NOT_SUPPORTED,
      .detail = L"synthetic sleep unavailable",
  };
  const auto no_sleep_availability =
      ytec::clonecore::query_completion_power_availability(
          CompletionPowerEnvironment::windows, no_sleep);
  const auto no_sleep_choices =
      ytec::clonecore::available_completion_power_actions(
          no_sleep_availability);
  check(
      no_sleep_choices == std::vector<CompletionPowerAction>({
          CompletionPowerAction::none,
          CompletionPowerAction::restart,
          CompletionPowerAction::shutdown,
      }) &&
          no_sleep_availability.native_status == ERROR_NOT_SUPPORTED &&
          !no_sleep_availability.detail.empty(),
      "Unsupported Windows sleep must be omitted and safely reported");

  MockPlatform winpe;
  const auto winpe_availability =
      ytec::clonecore::query_completion_power_availability(
          CompletionPowerEnvironment::winpe, winpe);
  check(
      ytec::clonecore::available_completion_power_actions(
          winpe_availability) == std::vector<CompletionPowerAction>({
          CompletionPowerAction::none,
          CompletionPowerAction::restart,
          CompletionPowerAction::shutdown,
      }) &&
          winpe.sleep_query_calls == 0U &&
          !winpe_availability.detail.empty(),
      "WinPE must exclude sleep without probing the platform");

  MockPlatform unknown;
  const auto unknown_availability =
      ytec::clonecore::query_completion_power_availability(
          static_cast<CompletionPowerEnvironment>(0xFFU), unknown);
  check(
      ytec::clonecore::available_completion_power_actions(
          unknown_availability) == std::vector<CompletionPowerAction>({
          CompletionPowerAction::none,
      }) &&
          unknown.sleep_query_calls == 0U &&
          !unknown_availability.is_available(
              static_cast<CompletionPowerAction>(0xFFU)),
      "Unknown environments and actions must fail closed to none");
}

void invalid_sleep_capability_is_not_treated_as_available() {
  MockPlatform platform;
  platform.sleep_capability = {
      .state = CompletionPowerAvailabilityState::available,
      .native_status = ERROR_INVALID_DATA,
  };
  const auto report =
      ytec::clonecore::query_completion_power_availability(
          CompletionPowerEnvironment::windows, platform);
  check(
      report.sleep == CompletionPowerAvailabilityState::unknown &&
          !report.is_available(CompletionPowerAction::sleep) &&
          !report.detail.empty(),
      "Contradictory sleep capability evidence must become unknown");

  platform.sleep_capability = {
      .state = static_cast<CompletionPowerAvailabilityState>(0xFFU),
  };
  const auto unknown =
      ytec::clonecore::query_completion_power_availability(
          CompletionPowerEnvironment::windows, platform);
  check(
      unknown.sleep == CompletionPowerAvailabilityState::unknown &&
          !unknown.is_available(CompletionPowerAction::sleep),
      "Unknown capability values must never enable sleep");
}

void only_success_with_completed_verification_can_dispatch() {
  for (const CompletionOperationOutcome outcome : {
           CompletionOperationOutcome::cancelled,
           CompletionOperationOutcome::failed,
           CompletionOperationOutcome::partial,
           CompletionOperationOutcome::unknown,
           static_cast<CompletionOperationOutcome>(0xFFU),
       }) {
    MockPlatform platform;
    auto request = ready_request(CompletionPowerAction::restart);
    request.operation_outcome = outcome;
    const auto result =
        ytec::clonecore::execute_completion_power_action(request, platform);
    check(
        result.disposition ==
                CompletionPowerExecutionDisposition::forced_none &&
            result.requested_action == CompletionPowerAction::none &&
            result.effective_action == CompletionPowerAction::none &&
            result.error.has_value() && platform.sleep_query_calls == 0U &&
            platform.request_calls() == 0U,
        "Cancelled, failed, partial, and unknown outcomes must force none");
  }

  for (const MandatoryVerificationState verification : {
           MandatoryVerificationState::incomplete,
           MandatoryVerificationState::failed,
           MandatoryVerificationState::unknown,
           static_cast<MandatoryVerificationState>(0xFFU),
       }) {
    MockPlatform platform;
    auto request = ready_request(CompletionPowerAction::restart);
    request.mandatory_verification = verification;
    const auto result =
        ytec::clonecore::execute_completion_power_action(request, platform);
    check(
        result.disposition ==
                CompletionPowerExecutionDisposition::forced_none &&
            result.error.has_value() &&
            result.error->code == ErrorCode::verification_failed &&
            platform.sleep_query_calls == 0U && platform.request_calls() == 0U,
        "Mandatory verification must be completed before any power request");
  }
}

void sleep_prevention_must_be_proven_released() {
  for (const SleepPreventionReleaseState release : {
           SleepPreventionReleaseState::still_active,
           SleepPreventionReleaseState::release_failed,
           SleepPreventionReleaseState::unknown,
           static_cast<SleepPreventionReleaseState>(0xFFU),
       }) {
    MockPlatform platform;
    auto request = ready_request(CompletionPowerAction::restart);
    request.sleep_prevention_release = release;
    const auto result =
        ytec::clonecore::execute_completion_power_action(request, platform);
    check(
        result.disposition ==
                CompletionPowerExecutionDisposition::forced_none &&
            result.error.has_value() &&
            result.error->code == ErrorCode::confirmation_required &&
            platform.sleep_query_calls == 0U && platform.request_calls() == 0U,
        "Missing sleep-prevention release proof must block every power API");
  }
}

void selection_and_immediate_reconfirmation_are_operation_bound() {
  std::array<CompletionPowerExecutionRequest, 6U> invalid{
      ready_request(CompletionPowerAction::restart),
      ready_request(CompletionPowerAction::restart),
      ready_request(CompletionPowerAction::restart),
      ready_request(CompletionPowerAction::restart),
      ready_request(CompletionPowerAction::restart),
      ready_request(CompletionPowerAction::restart),
  };
  invalid[0].operation_binding = 0U;
  invalid[1].selection.explicitly_selected = false;
  invalid[2].selection.operation_binding ^= 1U;
  invalid[3].reconfirmation.action = CompletionPowerAction::shutdown;
  invalid[4].reconfirmation.operation_binding ^= 1U;
  invalid[5].reconfirmation
      .explicitly_reconfirmed_immediately_before_execution = false;

  for (const auto& request : invalid) {
    MockPlatform platform;
    const auto result =
        ytec::clonecore::execute_completion_power_action(request, platform);
    check(
        result.disposition ==
                CompletionPowerExecutionDisposition::forced_none &&
            result.error.has_value() &&
            result.error->code == ErrorCode::confirmation_required &&
            platform.sleep_query_calls == 0U && platform.request_calls() == 0U,
        "Selection and immediate reconfirmation must bind to one operation");
  }
}

void unavailable_and_unknown_actions_never_dispatch() {
  MockPlatform platform;
  const auto pe_sleep = ytec::clonecore::execute_completion_power_action(
      ready_request(
          CompletionPowerAction::sleep,
          CompletionPowerEnvironment::winpe),
      platform);
  check(
      pe_sleep.disposition ==
              CompletionPowerExecutionDisposition::forced_none &&
          pe_sleep.error.has_value() &&
          pe_sleep.error->code == ErrorCode::unsupported_platform &&
          platform.sleep_query_calls == 0U && platform.request_calls() == 0U,
      "WinPE sleep must remain unavailable at execution time");

  MockPlatform no_sleep;
  no_sleep.sleep_capability = {
      .state = CompletionPowerAvailabilityState::unavailable,
      .native_status = ERROR_NOT_SUPPORTED,
  };
  const auto windows_sleep =
      ytec::clonecore::execute_completion_power_action(
          ready_request(CompletionPowerAction::sleep), no_sleep);
  check(
      windows_sleep.disposition ==
              CompletionPowerExecutionDisposition::forced_none &&
          no_sleep.sleep_query_calls == 1U && no_sleep.request_calls() == 0U,
      "Sleep capability must be rechecked immediately before dispatch");

  MockPlatform unknown_environment;
  auto unknown_environment_request =
      ready_request(CompletionPowerAction::restart);
  unknown_environment_request.environment =
      static_cast<CompletionPowerEnvironment>(0xFFU);
  const auto unknown_environment_result =
      ytec::clonecore::execute_completion_power_action(
          unknown_environment_request, unknown_environment);
  check(
      unknown_environment_result.disposition ==
              CompletionPowerExecutionDisposition::forced_none &&
          unknown_environment_result.error.has_value() &&
          unknown_environment_result.error->code ==
              ErrorCode::unsupported_platform &&
          unknown_environment.sleep_query_calls == 0U &&
          unknown_environment.request_calls() == 0U,
      "An unknown runtime must force none at execution time");

  MockPlatform unknown;
  auto unknown_request = ready_request(CompletionPowerAction::restart);
  unknown_request.selection.action =
      static_cast<CompletionPowerAction>(0xFFU);
  unknown_request.reconfirmation.action = unknown_request.selection.action;
  const auto unknown_result =
      ytec::clonecore::execute_completion_power_action(
          unknown_request, unknown);
  check(
      unknown_result.disposition ==
              CompletionPowerExecutionDisposition::forced_none &&
          unknown.sleep_query_calls == 0U && unknown.request_calls() == 0U,
      "Unknown action values must stop before any platform call");
}

void each_valid_action_dispatches_only_to_the_mock_seam() {
  for (const CompletionPowerAction action : {
           CompletionPowerAction::sleep,
           CompletionPowerAction::restart,
           CompletionPowerAction::shutdown,
       }) {
    MockPlatform platform;
    const auto result =
        ytec::clonecore::execute_completion_power_action(
            ready_request(action), platform);
    check(
        result.disposition ==
                CompletionPowerExecutionDisposition::request_accepted &&
            result.requested_action == action &&
            result.effective_action == action && !result.error.has_value() &&
            platform.request_calls() == 1U,
        "A valid completion action must dispatch exactly once to the seam");
    check(
        platform.sleep_calls ==
                (action == CompletionPowerAction::sleep ? 1U : 0U) &&
            platform.restart_calls ==
                (action == CompletionPowerAction::restart ? 1U : 0U) &&
            platform.shutdown_calls ==
                (action == CompletionPowerAction::shutdown ? 1U : 0U),
        "The selected action must not dispatch through another power method");
  }
}

void platform_failure_is_reported_without_claiming_effect() {
  MockPlatform platform;
  platform.restart_error = synthetic_error(L"mock restart");
  const auto result = ytec::clonecore::execute_completion_power_action(
      ready_request(CompletionPowerAction::restart), platform);
  check(
      result.disposition ==
              CompletionPowerExecutionDisposition::request_failed &&
          result.requested_action == CompletionPowerAction::restart &&
          result.effective_action == CompletionPowerAction::none &&
          result.error.has_value() &&
          result.error->native_code == ERROR_GEN_FAILURE &&
          platform.restart_calls == 1U,
      "A failed platform request must not be reported as an effective action");
}

}  // namespace

int main() {
  try {
    default_is_none_and_never_dispatches();
    choices_are_environment_and_capability_specific();
    invalid_sleep_capability_is_not_treated_as_available();
    only_success_with_completed_verification_can_dispatch();
    sleep_prevention_must_be_proven_released();
    selection_and_immediate_reconfirmation_are_operation_bound();
    unavailable_and_unknown_actions_never_dispatch();
    each_valid_action_dispatches_only_to_the_mock_seam();
    platform_failure_is_reported_without_claiming_effect();
    std::cout << "completion power action tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr <<
        "completion power action tests: FAIL: " << exception.what() << '\n';
    return 1;
  }
}

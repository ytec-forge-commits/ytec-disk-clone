#include "ytec/winpeapp/completion_power_ui.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using ytec::clonecore::CompletionOperationOutcome;
using ytec::clonecore::CompletionPowerAction;
using ytec::clonecore::CompletionPowerExecutionDisposition;
using ytec::clonecore::CompletionPowerAvailabilityState;
using ytec::clonecore::SleepCapabilityReport;
using ytec::clonecore::SleepPreventionReleaseState;
using ytec::clonecore::Status;
using ytec::winpeapp::WinPeCompletionPowerOperation;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class MockPowerPlatform final
    : public ytec::clonecore::ICompletionPowerPlatform {
 public:
  SleepCapabilityReport query_sleep_capability() override {
    ++sleep_query_calls;
    return SleepCapabilityReport{
        .state = CompletionPowerAvailabilityState::available,
    };
  }

  Status request_sleep() override {
    ++sleep_calls;
    return ytec::clonecore::success_status();
  }

  Status request_restart() override {
    ++restart_calls;
    return ytec::clonecore::success_status();
  }

  Status request_shutdown() override {
    ++shutdown_calls;
    return ytec::clonecore::success_status();
  }

  std::uint32_t sleep_query_calls{};
  std::uint32_t sleep_calls{};
  std::uint32_t restart_calls{};
  std::uint32_t shutdown_calls{};
};

ytec::winpeapp::WinPeCompletionPowerProof ready_proof(
    const WinPeCompletionPowerOperation operation =
        WinPeCompletionPowerOperation::clone) {
  return ytec::winpeapp::make_winpe_completion_power_proof(
      operation,
      CompletionOperationOutcome::succeeded,
      true,
      SleepPreventionReleaseState::released,
      73U);
}

void binding_allocator_never_wraps_to_a_reused_value() {
  std::uint64_t next = 1U;
  check(
      ytec::winpeapp::take_winpe_completion_power_operation_binding(next) ==
              1U &&
          next == 2U,
      "First WinPE completion binding must be nonzero and monotonic");
  next = (std::numeric_limits<std::uint64_t>::max)();
  check(
      ytec::winpeapp::take_winpe_completion_power_operation_binding(next) ==
              (std::numeric_limits<std::uint64_t>::max)() &&
          next == 0U &&
          ytec::winpeapp::take_winpe_completion_power_operation_binding(next) ==
              0U,
      "Binding exhaustion must stay at the fail-closed zero sentinel");
}

void prompt_requires_every_completion_gate() {
  const auto ready = ready_proof();
  for (const auto operation : {
           WinPeCompletionPowerOperation::clone,
           WinPeCompletionPowerOperation::image_create,
           WinPeCompletionPowerOperation::image_restore,
           WinPeCompletionPowerOperation::boot_repair,
       }) {
    check(
        ytec::winpeapp::plan_winpe_completion_power_prompt(
            ready_proof(operation))
            .prompt_allowed,
        "Every product operation must require and accept the same full proof");
  }

  for (const auto outcome : {
           CompletionOperationOutcome::cancelled,
           CompletionOperationOutcome::failed,
           CompletionOperationOutcome::partial,
           CompletionOperationOutcome::unknown,
       }) {
    auto proof = ready;
    proof.outcome = outcome;
    check(
        !ytec::winpeapp::plan_winpe_completion_power_prompt(proof)
             .prompt_allowed,
        "Cancelled, failed, partial, and unknown work must not prompt");
  }
  for (const auto release : {
           SleepPreventionReleaseState::still_active,
           SleepPreventionReleaseState::release_failed,
           SleepPreventionReleaseState::unknown,
       }) {
    auto proof = ready;
    proof.sleep_prevention_release = release;
    check(
        !ytec::winpeapp::plan_winpe_completion_power_prompt(proof)
             .prompt_allowed,
        "Missing sleep-prevention release evidence must not prompt");
  }
  auto incomplete = ready;
  incomplete.mandatory_verification =
      ytec::clonecore::MandatoryVerificationState::incomplete;
  check(
      !ytec::winpeapp::plan_winpe_completion_power_prompt(incomplete)
           .prompt_allowed,
      "Incomplete mandatory verification must not prompt");
  auto unbound = ready;
  unbound.operation_binding = 0U;
  check(
      !ytec::winpeapp::plan_winpe_completion_power_prompt(unbound)
           .prompt_allowed,
      "A zero operation binding must not prompt");
  auto unknown = ready;
  unknown.operation = static_cast<WinPeCompletionPowerOperation>(0xFFU);
  check(
      !ytec::winpeapp::plan_winpe_completion_power_prompt(unknown)
           .prompt_allowed,
      "An unknown operation must not prompt");
}

void winpe_offers_no_sleep_and_rechecks_both_confirmations() {
  MockPowerPlatform platform;
  const auto proof = ready_proof(
      WinPeCompletionPowerOperation::image_restore);
  const auto availability =
      ytec::clonecore::query_completion_power_availability(
          ytec::clonecore::CompletionPowerEnvironment::winpe,
          platform);
  const auto available_actions =
      ytec::clonecore::available_completion_power_actions(availability);
  check(
      available_actions.size() == 3U &&
          available_actions[0] == CompletionPowerAction::none &&
          available_actions[1] == CompletionPowerAction::restart &&
          available_actions[2] == CompletionPowerAction::shutdown &&
          platform.sleep_query_calls == 0U,
      "WinPE must expose exactly none, restart, and shutdown without probing sleep");

  auto request = ytec::winpeapp::
      make_winpe_completion_power_execution_request(
          proof, CompletionPowerAction::sleep, true, true);
  auto result = ytec::clonecore::execute_completion_power_action(
      request, platform);
  check(
      result.disposition == CompletionPowerExecutionDisposition::no_action &&
          request.selection.action == CompletionPowerAction::none &&
          platform.sleep_query_calls == 0U && platform.sleep_calls == 0U &&
          platform.restart_calls == 0U && platform.shutdown_calls == 0U,
      "WinPE sleep must become none before capability or platform access");

  request = ytec::winpeapp::make_winpe_completion_power_execution_request(
      proof, CompletionPowerAction::restart, true, false);
  result = ytec::clonecore::execute_completion_power_action(
      request, platform);
  check(
      result.disposition ==
              CompletionPowerExecutionDisposition::forced_none &&
          platform.restart_calls == 0U,
      "A missing immediate reconfirmation must prevent restart dispatch");

  request = ytec::winpeapp::make_winpe_completion_power_execution_request(
      proof, CompletionPowerAction::restart, true, true);
  result = ytec::clonecore::execute_completion_power_action(
      request, platform);
  check(
      request.environment ==
              ytec::clonecore::CompletionPowerEnvironment::winpe &&
          result.disposition ==
              CompletionPowerExecutionDisposition::request_accepted &&
          result.effective_action == CompletionPowerAction::restart &&
          platform.restart_calls == 1U &&
          platform.shutdown_calls == 0U && platform.sleep_calls == 0U,
      "A fully bound and reconfirmed WinPE restart may reach only the mock");

  request = ytec::winpeapp::make_winpe_completion_power_execution_request(
      proof, CompletionPowerAction::shutdown, true, true);
  result = ytec::clonecore::execute_completion_power_action(
      request, platform);
  check(
      result.disposition ==
              CompletionPowerExecutionDisposition::request_accepted &&
          result.effective_action == CompletionPowerAction::shutdown &&
          platform.restart_calls == 1U && platform.shutdown_calls == 1U &&
          platform.sleep_calls == 0U,
      "A fully bound and reconfirmed WinPE shutdown may reach only the mock");
}

void partial_rescue_style_proof_never_reaches_the_platform() {
  MockPowerPlatform platform;
  const auto proof = ytec::winpeapp::make_winpe_completion_power_proof(
      WinPeCompletionPowerOperation::image_create,
      CompletionOperationOutcome::partial,
      true,
      SleepPreventionReleaseState::released,
      99U);
  const auto request = ytec::winpeapp::
      make_winpe_completion_power_execution_request(
          proof, CompletionPowerAction::shutdown, true, true);
  const auto result = ytec::clonecore::execute_completion_power_action(
      request, platform);
  check(
      request.selection.action == CompletionPowerAction::none &&
          result.disposition == CompletionPowerExecutionDisposition::no_action &&
          platform.shutdown_calls == 0U,
      "A partial/rescue result must be forced to none before dispatch");
}

void only_restart_and_shutdown_expect_the_session_to_end() {
  check(
      !ytec::winpeapp::winpe_completion_power_action_expects_ui_session_end(
          CompletionPowerAction::none) &&
          !ytec::winpeapp::
               winpe_completion_power_action_expects_ui_session_end(
                   CompletionPowerAction::sleep) &&
          ytec::winpeapp::winpe_completion_power_action_expects_ui_session_end(
              CompletionPowerAction::restart) &&
          ytec::winpeapp::winpe_completion_power_action_expects_ui_session_end(
              CompletionPowerAction::shutdown),
      "Only restart/shutdown end the WinPE UI session");
}

}  // namespace

int main() {
  try {
    binding_allocator_never_wraps_to_a_reused_value();
    prompt_requires_every_completion_gate();
    winpe_offers_no_sleep_and_rechecks_both_confirmations();
    partial_rescue_style_proof_never_reaches_the_platform();
    only_restart_and_shutdown_expect_the_session_to_end();
    std::cout << "winpe completion power UI tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& exception) {
    std::cerr << "winpe completion power UI tests: FAIL: "
              << exception.what() << '\n';
    return EXIT_FAILURE;
  }
}

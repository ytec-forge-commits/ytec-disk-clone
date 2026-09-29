#include "ytec/winpeapp/completion_power_ui.h"

#include <limits>

namespace ytec::winpeapp {
namespace {

[[nodiscard]] bool is_known_operation(
    const WinPeCompletionPowerOperation operation) noexcept {
  switch (operation) {
    case WinPeCompletionPowerOperation::clone:
    case WinPeCompletionPowerOperation::image_create:
    case WinPeCompletionPowerOperation::image_restore:
    case WinPeCompletionPowerOperation::boot_repair:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_allowed_winpe_action(
    const clonecore::CompletionPowerAction action) noexcept {
  switch (action) {
    case clonecore::CompletionPowerAction::none:
    case clonecore::CompletionPowerAction::restart:
    case clonecore::CompletionPowerAction::shutdown:
      return true;
    case clonecore::CompletionPowerAction::sleep:
      return false;
  }
  return false;
}

}  // namespace

std::uint64_t take_winpe_completion_power_operation_binding(
    std::uint64_t& next_binding) noexcept {
  if (next_binding == 0U) {
    return 0U;
  }
  const std::uint64_t binding = next_binding;
  if (next_binding == (std::numeric_limits<std::uint64_t>::max)()) {
    next_binding = 0U;
  } else {
    ++next_binding;
  }
  return binding;
}

WinPeCompletionPowerProof make_winpe_completion_power_proof(
    const WinPeCompletionPowerOperation operation,
    const clonecore::CompletionOperationOutcome outcome,
    const bool mandatory_verification_completed,
    const clonecore::SleepPreventionReleaseState sleep_prevention_release,
    const std::uint64_t operation_binding) noexcept {
  return WinPeCompletionPowerProof{
      .operation = operation,
      .outcome = outcome,
      .mandatory_verification = mandatory_verification_completed
          ? clonecore::MandatoryVerificationState::completed
          : clonecore::MandatoryVerificationState::incomplete,
      .sleep_prevention_release = sleep_prevention_release,
      .operation_binding = operation_binding,
  };
}

WinPeCompletionPowerPromptPlan plan_winpe_completion_power_prompt(
    const WinPeCompletionPowerProof& proof) noexcept {
  return WinPeCompletionPowerPromptPlan{
      .prompt_allowed = is_known_operation(proof.operation) &&
          proof.outcome == clonecore::CompletionOperationOutcome::succeeded &&
          proof.mandatory_verification ==
              clonecore::MandatoryVerificationState::completed &&
          proof.sleep_prevention_release ==
              clonecore::SleepPreventionReleaseState::released &&
          proof.operation_binding != 0U,
  };
}

clonecore::CompletionPowerExecutionRequest
make_winpe_completion_power_execution_request(
    const WinPeCompletionPowerProof& proof,
    const clonecore::CompletionPowerAction selected_action,
    const bool explicitly_selected,
    const bool explicitly_reconfirmed_immediately_before_execution) noexcept {
  const auto plan = plan_winpe_completion_power_prompt(proof);
  const bool action_valid = is_allowed_winpe_action(selected_action);
  const clonecore::CompletionPowerAction effective_selection =
      plan.prompt_allowed && action_valid
      ? selected_action
      : clonecore::kDefaultCompletionPowerAction;
  const bool non_default =
      effective_selection != clonecore::kDefaultCompletionPowerAction;
  return clonecore::CompletionPowerExecutionRequest{
      .environment = clonecore::CompletionPowerEnvironment::winpe,
      .operation_outcome = plan.prompt_allowed
          ? proof.outcome
          : clonecore::CompletionOperationOutcome::unknown,
      .mandatory_verification = plan.prompt_allowed
          ? proof.mandatory_verification
          : clonecore::MandatoryVerificationState::unknown,
      .sleep_prevention_release = plan.prompt_allowed
          ? proof.sleep_prevention_release
          : clonecore::SleepPreventionReleaseState::unknown,
      .operation_binding = plan.prompt_allowed
          ? proof.operation_binding
          : 0U,
      .selection = {
          .action = effective_selection,
          .operation_binding = non_default ? proof.operation_binding : 0U,
          .explicitly_selected = non_default && explicitly_selected,
      },
      .reconfirmation = {
          .action = effective_selection,
          .operation_binding = non_default ? proof.operation_binding : 0U,
          .explicitly_reconfirmed_immediately_before_execution =
              non_default &&
              explicitly_reconfirmed_immediately_before_execution,
      },
  };
}

bool winpe_completion_power_action_expects_ui_session_end(
    const clonecore::CompletionPowerAction action) noexcept {
  switch (action) {
    case clonecore::CompletionPowerAction::restart:
    case clonecore::CompletionPowerAction::shutdown:
      return true;
    case clonecore::CompletionPowerAction::none:
    case clonecore::CompletionPowerAction::sleep:
      return false;
  }
  return false;
}

}  // namespace ytec::winpeapp

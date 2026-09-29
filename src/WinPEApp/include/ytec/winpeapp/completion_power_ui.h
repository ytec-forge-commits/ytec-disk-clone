#pragma once

#include "ytec/clonecore/completion_power_action.h"

#include <cstdint>

namespace ytec::winpeapp {

enum class WinPeCompletionPowerOperation : std::uint8_t {
  clone,
  image_create,
  image_restore,
  boot_repair,
};

// Copied by value from one completed WinPE worker. The UI may offer a power
// transition only when this proof remains operation-bound and every gate is
// positive. Rescue/partial results deliberately retain a partial outcome.
struct WinPeCompletionPowerProof final {
  WinPeCompletionPowerOperation operation{
      WinPeCompletionPowerOperation::clone};
  clonecore::CompletionOperationOutcome outcome{
      clonecore::CompletionOperationOutcome::unknown};
  clonecore::MandatoryVerificationState mandatory_verification{
      clonecore::MandatoryVerificationState::unknown};
  clonecore::SleepPreventionReleaseState sleep_prevention_release{
      clonecore::SleepPreventionReleaseState::unknown};
  std::uint64_t operation_binding{};
};

struct WinPeCompletionPowerPromptPlan final {
  bool prompt_allowed{};
  clonecore::CompletionPowerAction default_action{
      clonecore::kDefaultCompletionPowerAction};
};

// Monotonic process-local binding allocator. Exhaustion remains at zero so a
// later completion dialog cannot reuse an earlier operation's binding.
[[nodiscard]] std::uint64_t take_winpe_completion_power_operation_binding(
    std::uint64_t& next_binding) noexcept;

// The operation-specific controller must derive mandatory_verification from
// its complete report, never from UI text alone. This function preserves the
// exact outcome and release evidence without upgrading partial/failed work.
[[nodiscard]] WinPeCompletionPowerProof make_winpe_completion_power_proof(
    WinPeCompletionPowerOperation operation,
    clonecore::CompletionOperationOutcome outcome,
    bool mandatory_verification_completed,
    clonecore::SleepPreventionReleaseState sleep_prevention_release,
    std::uint64_t operation_binding) noexcept;

[[nodiscard]] WinPeCompletionPowerPromptPlan
plan_winpe_completion_power_prompt(
    const WinPeCompletionPowerProof& proof) noexcept;

// Invalid/incomplete proof and WinPE-inapplicable sleep are converted to the
// safe default. A non-none request must carry the same binding through both
// the explicit selection and the immediate reconfirmation.
[[nodiscard]] clonecore::CompletionPowerExecutionRequest
make_winpe_completion_power_execution_request(
    const WinPeCompletionPowerProof& proof,
    clonecore::CompletionPowerAction selected_action,
    bool explicitly_selected,
    bool explicitly_reconfirmed_immediately_before_execution) noexcept;

[[nodiscard]] bool winpe_completion_power_action_expects_ui_session_end(
    clonecore::CompletionPowerAction action) noexcept;

}  // namespace ytec::winpeapp

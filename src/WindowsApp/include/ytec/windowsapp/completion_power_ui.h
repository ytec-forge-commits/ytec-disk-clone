#pragma once

#include "ytec/clonecore/completion_power_action.h"
#include "ytec/vssrequester/online_tsumugi_backup.h"
#include "ytec/windowsapp/media_creation.h"
#include "ytec/windowsapp/media_wizard.h"
#include "ytec/windowsapp/online_direct_clone_operation.h"
#include "ytec/windowsapp/online_direct_shrink_clone.h"
#include "ytec/windowsapp/online_image_restore_operation.h"
#include "ytec/windowsapp/online_shrink_image_restore.h"

#include <cstdint>

namespace ytec::windowsapp {

enum class WindowsCompletionPowerOperation : std::uint8_t {
  clone,
  image_create,
  image_restore,
  rescue_media,
};

// Proof copied from one completed worker payload. The UI may offer a power
// action only when every field is positive and the binding is non-zero.
struct WindowsCompletionPowerProof final {
  WindowsCompletionPowerOperation operation{
      WindowsCompletionPowerOperation::clone};
  clonecore::CompletionOperationOutcome outcome{
      clonecore::CompletionOperationOutcome::unknown};
  clonecore::MandatoryVerificationState mandatory_verification{
      clonecore::MandatoryVerificationState::unknown};
  clonecore::SleepPreventionReleaseState sleep_prevention_release{
      clonecore::SleepPreventionReleaseState::unknown};
  std::uint64_t operation_binding{};
};

struct WindowsCompletionPowerPromptPlan final {
  bool prompt_allowed{};
  clonecore::CompletionPowerAction default_action{
      clonecore::kDefaultCompletionPowerAction};
};

// Monotonic process-local binding allocator. Exhaustion remains at zero so a
// later operation cannot accidentally reuse an earlier prompt binding.
[[nodiscard]] std::uint64_t take_completion_power_operation_binding(
    std::uint64_t& next_binding) noexcept;

[[nodiscard]] WindowsCompletionPowerProof
make_clone_completion_power_proof(
    const OnlineDirectCloneOperationReport& report,
    clonecore::SleepPreventionReleaseState sleep_prevention_release,
    std::uint64_t operation_binding) noexcept;

[[nodiscard]] WindowsCompletionPowerProof
make_direct_shrink_clone_completion_power_proof(
    const WindowsDirectShrinkCloneOperationReport& report,
    clonecore::SleepPreventionReleaseState sleep_prevention_release,
    std::uint64_t operation_binding) noexcept;

[[nodiscard]] WindowsCompletionPowerProof
make_image_create_completion_power_proof(
    const vssrequester::OnlineTsumugiBackupReport& report,
    bool rescue_mode,
    clonecore::SleepPreventionReleaseState sleep_prevention_release,
    std::uint64_t operation_binding) noexcept;

[[nodiscard]] WindowsCompletionPowerProof
make_exact_restore_completion_power_proof(
    const OnlineImageRestoreOperationReport& report,
    clonecore::SleepPreventionReleaseState sleep_prevention_release,
    std::uint64_t operation_binding) noexcept;

[[nodiscard]] WindowsCompletionPowerProof
make_shrink_restore_completion_power_proof(
    const WindowsOnlineShrinkRestoreOperationReport& report,
    clonecore::SleepPreventionReleaseState sleep_prevention_release,
    std::uint64_t operation_binding) noexcept;

[[nodiscard]] WindowsCompletionPowerProof
make_rescue_media_completion_power_proof(
    const RescueMediaCreationReport& report,
    RescueMediaKind requested_kind,
    clonecore::SleepPreventionReleaseState sleep_prevention_release,
    std::uint64_t operation_binding) noexcept;

[[nodiscard]] WindowsCompletionPowerPromptPlan
plan_windows_completion_power_prompt(
    const WindowsCompletionPowerProof& proof) noexcept;

// A successful sleep request returns after resume; only restart/shutdown are
// expected to end this UI session and may skip the normal post-operation
// refresh path.
[[nodiscard]] bool completion_power_action_expects_ui_session_end(
    clonecore::CompletionPowerAction action) noexcept;

// Invalid/incomplete proof is converted to an explicit safe default request.
// A non-none request carries the same binding through both confirmations.
[[nodiscard]] clonecore::CompletionPowerExecutionRequest
make_windows_completion_power_execution_request(
    const WindowsCompletionPowerProof& proof,
    clonecore::CompletionPowerAction selected_action,
    bool explicitly_selected,
    bool explicitly_reconfirmed_immediately_before_execution) noexcept;

}  // namespace ytec::windowsapp

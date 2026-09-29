#include "ytec/winpeapp/tsumugi_password_ui.h"

#include "ytec/imageformat/tsumugi_crypto.h"

namespace ytec::winpeapp {

WinPeTsumugiCreatePasswordDecision
decide_winpe_tsumugi_create_password(
    const bool encryption_enabled,
    const std::string_view password,
    const std::string_view confirmation,
    const bool weak_warning_accepted) noexcept {
  const auto evaluation = evaluate_winpe_tsumugi_create_password(
      encryption_enabled, password, confirmation);
  if (!encryption_enabled) {
    return WinPeTsumugiCreatePasswordDecision::encryption_disabled;
  }
  if (!evaluation.accepted) {
    return WinPeTsumugiCreatePasswordDecision::rejected;
  }
  if (evaluation.weak && !weak_warning_accepted) {
    return WinPeTsumugiCreatePasswordDecision::weak_warning_required;
  }
  return WinPeTsumugiCreatePasswordDecision::password_ready;
}

WinPeTsumugiPasswordEvaluation
evaluate_winpe_tsumugi_create_password(
    const bool encryption_enabled,
    const std::string_view password,
    const std::string_view confirmation) noexcept {
  if (!encryption_enabled) {
    return WinPeTsumugiPasswordEvaluation{
        .accepted = true,
        .weak = false,
        .confirmation_matches = true,
    };
  }
  const auto assessment =
      imageformat::assess_tsumugi_password(password);
  const bool matches = password == confirmation;
  return WinPeTsumugiPasswordEvaluation{
      .accepted = assessment.accepted && matches,
      .weak = assessment.weak,
      .confirmation_matches = matches,
  };
}

WinPeTsumugiPasswordEvaluation
evaluate_winpe_tsumugi_restore_password(
    const bool password_required,
    const std::string_view password) noexcept {
  if (!password_required) {
    return WinPeTsumugiPasswordEvaluation{
        .accepted = true,
        .weak = false,
        .confirmation_matches = true,
    };
  }
  const auto assessment =
      imageformat::assess_tsumugi_password(password);
  return WinPeTsumugiPasswordEvaluation{
      .accepted = assessment.accepted,
      .weak = assessment.weak,
      .confirmation_matches = true,
  };
}

WinPeTsumugiRestorePasswordPromptDecision
evaluate_winpe_tsumugi_restore_password_prompt(
    const WinPeTsumugiRestorePasswordPromptState& state) noexcept {
  if (!state.header_probe_succeeded) {
    return WinPeTsumugiRestorePasswordPromptDecision::stop;
  }
  if (!state.encrypted) {
    return WinPeTsumugiRestorePasswordPromptDecision::
        no_password_required;
  }
  if (!state.prompt_completed) {
    return WinPeTsumugiRestorePasswordPromptDecision::prompt_required;
  }
  if (!state.prompt_accepted || !state.password_available) {
    return WinPeTsumugiRestorePasswordPromptDecision::stop;
  }
  return WinPeTsumugiRestorePasswordPromptDecision::password_ready;
}

WinPeTsumugiCompleteVerificationGate
evaluate_winpe_tsumugi_complete_verification_gate(
    const bool encrypted,
    const bool password_available,
    const bool complete_verification_succeeded) noexcept {
  const bool authenticated = complete_verification_succeeded &&
      (!encrypted || password_available);
  return WinPeTsumugiCompleteVerificationGate{
      .target_selection_allowed = authenticated,
      .target_io_allowed = authenticated,
  };
}

}  // namespace ytec::winpeapp

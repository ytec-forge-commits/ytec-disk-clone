#include "ytec/winpeapp/tsumugi_password_ui.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void create_requires_confirmed_ascii_and_a_separate_weak_acknowledgement() {
  using Decision =
      ytec::winpeapp::WinPeTsumugiCreatePasswordDecision;

  check(
      ytec::winpeapp::decide_winpe_tsumugi_create_password(
          false, {}, {}, false) == Decision::encryption_disabled,
      "plain exact create must not retain or request a password");
  check(
      ytec::winpeapp::decide_winpe_tsumugi_create_password(
          true, "Tsumugi-Drive-2026!", "different-password", false) ==
          Decision::rejected,
      "create must reject nonmatching confirmation");
  check(
      ytec::winpeapp::decide_winpe_tsumugi_create_password(
          true, "valid\npassword", "valid\npassword", false) ==
          Decision::rejected,
      "create must reject non-printable ASCII");
  check(
      ytec::winpeapp::decide_winpe_tsumugi_create_password(
          true, "abcdefgh", "abcdefgh", false) ==
          Decision::weak_warning_required,
      "accepted weak syntax must still require a separate warning decision");
  check(
      ytec::winpeapp::decide_winpe_tsumugi_create_password(
          true, "abcdefgh", "abcdefgh", true) ==
          Decision::password_ready,
      "the explicit weak-password acknowledgement may continue");
  check(
      ytec::winpeapp::decide_winpe_tsumugi_create_password(
          true,
          "Tsumugi-Drive-2026!",
          "Tsumugi-Drive-2026!",
          false) == Decision::password_ready,
      "a confirmed strong password must be ready without a weak warning");
}

void bounded_probe_controls_plain_prompt_and_cancel_flow() {
  using Decision =
      ytec::winpeapp::WinPeTsumugiRestorePasswordPromptDecision;
  using State = ytec::winpeapp::WinPeTsumugiRestorePasswordPromptState;

  check(
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password_prompt(
          State{}) == Decision::stop,
      "a failed bounded header probe must stop");
  check(
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password_prompt(State{
          .header_probe_succeeded = true,
          .encrypted = false,
      }) == Decision::no_password_required,
      "a plaintext header must never show a password prompt");
  check(
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password_prompt(State{
          .header_probe_succeeded = true,
          .encrypted = true,
      }) == Decision::prompt_required,
      "an encrypted header must request exactly one in-memory password");
  check(
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password_prompt(State{
          .header_probe_succeeded = true,
          .encrypted = true,
          .prompt_completed = true,
          .prompt_accepted = false,
      }) == Decision::stop,
      "cancel must stop before complete verification");
  check(
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password_prompt(State{
          .header_probe_succeeded = true,
          .encrypted = true,
          .prompt_completed = true,
          .prompt_accepted = true,
          .password_available = false,
      }) == Decision::stop,
      "an accepted prompt without an owned secret must stop");
  check(
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password_prompt(State{
          .header_probe_succeeded = true,
          .encrypted = true,
          .prompt_completed = true,
          .prompt_accepted = true,
          .password_available = true,
      }) == Decision::password_ready,
      "an owned encrypted password may enter complete verification");
}

void restore_requires_printable_ascii_only_when_encrypted() {
  const auto plaintext =
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password(false, {});
  check(
      plaintext.accepted && !plaintext.weak,
      "plaintext restore must require no password");

  const auto too_short =
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password(
          true, "short");
  check(!too_short.accepted, "encrypted restore must reject a short secret");

  const auto non_printable =
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password(
          true, "valid\tpass");
  check(
      !non_printable.accepted,
      "encrypted restore must reject non-printable ASCII");

  const auto accepted =
      ytec::winpeapp::evaluate_winpe_tsumugi_restore_password(
          true, "abcdefgh");
  check(
      accepted.accepted && accepted.weak,
      "an existing weak-but-valid image password must remain usable");
}

void wrong_password_keeps_target_selection_and_io_closed() {
  const auto wrong_password =
      ytec::winpeapp::evaluate_winpe_tsumugi_complete_verification_gate(
          true, true, false);
  check(
      !wrong_password.target_selection_allowed &&
          !wrong_password.target_io_allowed,
      "failed AES-GCM authentication must expose no target operation");

  const auto missing_password =
      ytec::winpeapp::evaluate_winpe_tsumugi_complete_verification_gate(
          true, false, true);
  check(
      !missing_password.target_selection_allowed &&
          !missing_password.target_io_allowed,
      "encrypted verification without an owned password must fail closed");

  const auto authenticated =
      ytec::winpeapp::evaluate_winpe_tsumugi_complete_verification_gate(
          true, true, true);
  check(
      authenticated.target_selection_allowed &&
          authenticated.target_io_allowed,
      "only complete authenticated verification may unlock target review");

  const auto plaintext =
      ytec::winpeapp::evaluate_winpe_tsumugi_complete_verification_gate(
          false, false, true);
  check(
      plaintext.target_selection_allowed && plaintext.target_io_allowed,
      "a completely verified plaintext image needs no secret");
}

void secret_policy_forbids_logs_persistence_and_recovery_key() {
  constexpr auto policy =
      ytec::winpeapp::winpe_tsumugi_password_security_policy();
  static_assert(policy.no_recovery_key);
  static_assert(policy.secret_memory_only);
  static_assert(!policy.secret_logging_allowed);
  static_assert(!policy.secret_persistence_allowed);
  static_assert(policy.erase_secret_on_scope_exit);
  check(
      policy.no_recovery_key && policy.secret_memory_only &&
          !policy.secret_logging_allowed &&
          !policy.secret_persistence_allowed &&
          policy.erase_secret_on_scope_exit,
      "WinPE secret policy must remain memory-only, no-log, and no-recovery");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"create_requires_confirmed_ascii_and_a_separate_weak_acknowledgement",
       create_requires_confirmed_ascii_and_a_separate_weak_acknowledgement},
      {"bounded_probe_controls_plain_prompt_and_cancel_flow",
       bounded_probe_controls_plain_prompt_and_cancel_flow},
      {"restore_requires_printable_ascii_only_when_encrypted",
       restore_requires_printable_ascii_only_when_encrypted},
      {"wrong_password_keeps_target_selection_and_io_closed",
       wrong_password_keeps_target_selection_and_io_closed},
      {"secret_policy_forbids_logs_persistence_and_recovery_key",
       secret_policy_forbids_logs_persistence_and_recovery_key},
  };

  try {
    for (const auto& [name, test] : tests) {
      test();
      std::cout << "[PASS] " << name << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << "[FAIL] " << error.what() << '\n';
    return 1;
  }
  return 0;
}

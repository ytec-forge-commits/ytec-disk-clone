#pragma once

#include <cstdint>
#include <string_view>

namespace ytec::winpeapp {

struct WinPeTsumugiPasswordEvaluation final {
  bool accepted{};
  bool weak{};
  bool confirmation_matches{};
};

enum class WinPeTsumugiCreatePasswordDecision : std::uint8_t {
  encryption_disabled,
  rejected,
  weak_warning_required,
  password_ready,
};

// The weak-password warning is a separate, explicit user decision. A weak
// password never becomes ready merely because it meets the minimum syntax.
[[nodiscard]] WinPeTsumugiCreatePasswordDecision
decide_winpe_tsumugi_create_password(
    bool encryption_enabled,
    std::string_view password,
    std::string_view confirmation,
    bool weak_warning_accepted) noexcept;

// Creation requires two exact ASCII-password entries. When encryption is
// disabled no password is retained or required.
[[nodiscard]] WinPeTsumugiPasswordEvaluation
evaluate_winpe_tsumugi_create_password(
    bool encryption_enabled,
    std::string_view password,
    std::string_view confirmation) noexcept;

// Restore requires one accepted ASCII password only for an encrypted image.
// Authentication is still performed by the complete immutable image reader.
[[nodiscard]] WinPeTsumugiPasswordEvaluation
evaluate_winpe_tsumugi_restore_password(
    bool password_required,
    std::string_view password) noexcept;

enum class WinPeTsumugiRestorePasswordPromptDecision : std::uint8_t {
  stop,
  no_password_required,
  prompt_required,
  password_ready,
};

struct WinPeTsumugiRestorePasswordPromptState final {
  bool header_probe_succeeded{};
  bool encrypted{};
  bool prompt_completed{};
  bool prompt_accepted{};
  bool password_available{};
};

// The bounded fixed-header probe alone decides whether the modal prompt is
// shown. Cancel, probe failure, or an accepted prompt without an owned secret
// all stop before complete verification and target selection.
[[nodiscard]] WinPeTsumugiRestorePasswordPromptDecision
evaluate_winpe_tsumugi_restore_password_prompt(
    const WinPeTsumugiRestorePasswordPromptState& state) noexcept;

struct WinPeTsumugiCompleteVerificationGate final {
  bool target_selection_allowed{};
  bool target_io_allowed{};
};

// A password is only syntax-checked by the dialog. Authentication belongs to
// the complete immutable image verifier. Wrong passwords and authentication
// failures therefore keep both target selection and target I/O closed.
[[nodiscard]] WinPeTsumugiCompleteVerificationGate
evaluate_winpe_tsumugi_complete_verification_gate(
    bool encrypted,
    bool password_available,
    bool complete_verification_succeeded) noexcept;

struct WinPeTsumugiPasswordSecurityPolicy final {
  bool no_recovery_key{};
  bool secret_memory_only{};
  bool secret_logging_allowed{};
  bool secret_persistence_allowed{};
  bool erase_secret_on_scope_exit{};
};

[[nodiscard]] constexpr WinPeTsumugiPasswordSecurityPolicy
winpe_tsumugi_password_security_policy() noexcept {
  return WinPeTsumugiPasswordSecurityPolicy{
      .no_recovery_key = true,
      .secret_memory_only = true,
      .secret_logging_allowed = false,
      .secret_persistence_allowed = false,
      .erase_secret_on_scope_exit = true,
  };
}

}  // namespace ytec::winpeapp

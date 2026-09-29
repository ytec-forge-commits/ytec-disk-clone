#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::clonecore {

enum class CompletionPowerAction : std::uint8_t {
  none,
  sleep,
  restart,
  shutdown,
};

inline constexpr CompletionPowerAction kDefaultCompletionPowerAction =
    CompletionPowerAction::none;

enum class CompletionPowerEnvironment : std::uint8_t {
  windows,
  winpe,
  unknown,
};

enum class CompletionPowerAvailabilityState : std::uint8_t {
  available,
  unavailable,
  unknown,
};

struct SleepCapabilityReport final {
  CompletionPowerAvailabilityState state{
      CompletionPowerAvailabilityState::unknown};
  std::uint32_t native_status{};
  std::wstring detail;
};

struct CompletionPowerAvailability final {
  CompletionPowerEnvironment environment{CompletionPowerEnvironment::unknown};
  CompletionPowerAvailabilityState none{
      CompletionPowerAvailabilityState::available};
  CompletionPowerAvailabilityState sleep{
      CompletionPowerAvailabilityState::unknown};
  CompletionPowerAvailabilityState restart{
      CompletionPowerAvailabilityState::unknown};
  CompletionPowerAvailabilityState shutdown{
      CompletionPowerAvailabilityState::unknown};
  std::uint32_t native_status{};
  std::wstring detail;

  [[nodiscard]] bool is_available(CompletionPowerAction action) const noexcept;
};

struct CompletionPowerExecutionRequest;
struct CompletionPowerExecutionResult;

class ICompletionPowerPlatform {
 public:
  virtual ~ICompletionPowerPlatform() = default;

 private:
  friend CompletionPowerAvailability query_completion_power_availability(
      CompletionPowerEnvironment environment,
      ICompletionPowerPlatform& platform);
  friend CompletionPowerExecutionResult execute_completion_power_action(
      const CompletionPowerExecutionRequest& request,
      ICompletionPowerPlatform& platform);

  // Deliberately private: production callers can reach transition APIs only
  // through execute_completion_power_action and its fail-closed gates.
  [[nodiscard]] virtual SleepCapabilityReport query_sleep_capability() = 0;
  [[nodiscard]] virtual Status request_sleep() = 0;
  [[nodiscard]] virtual Status request_restart() = 0;
  [[nodiscard]] virtual Status request_shutdown() = 0;
};

// Returns only choices that are valid for the supplied runtime. WinPE never
// probes or offers sleep. An unknown runtime offers only the safe default.
[[nodiscard]] CompletionPowerAvailability query_completion_power_availability(
    CompletionPowerEnvironment environment,
    ICompletionPowerPlatform& platform);

// Fixed UI order: none, sleep, restart, shutdown. Unknown enum values are
// omitted rather than guessed.
[[nodiscard]] std::vector<CompletionPowerAction>
available_completion_power_actions(
    const CompletionPowerAvailability& availability);

enum class CompletionOperationOutcome : std::uint8_t {
  succeeded,
  cancelled,
  failed,
  partial,
  unknown,
};

enum class MandatoryVerificationState : std::uint8_t {
  completed,
  incomplete,
  failed,
  unknown,
};

enum class SleepPreventionReleaseState : std::uint8_t {
  released,
  still_active,
  release_failed,
  unknown,
};

struct CompletionPowerSelectionBinding final {
  CompletionPowerAction action{kDefaultCompletionPowerAction};
  std::uint64_t operation_binding{};
  bool explicitly_selected{};
};

struct CompletionPowerReconfirmation final {
  CompletionPowerAction action{kDefaultCompletionPowerAction};
  std::uint64_t operation_binding{};
  bool explicitly_reconfirmed_immediately_before_execution{};
};

struct CompletionPowerExecutionRequest final {
  CompletionPowerEnvironment environment{CompletionPowerEnvironment::unknown};
  CompletionOperationOutcome operation_outcome{
      CompletionOperationOutcome::unknown};
  MandatoryVerificationState mandatory_verification{
      MandatoryVerificationState::unknown};
  SleepPreventionReleaseState sleep_prevention_release{
      SleepPreventionReleaseState::unknown};
  std::uint64_t operation_binding{};
  CompletionPowerSelectionBinding selection;
  CompletionPowerReconfirmation reconfirmation;
};

enum class CompletionPowerExecutionDisposition : std::uint8_t {
  no_action,
  forced_none,
  request_accepted,
  request_failed,
};

struct CompletionPowerExecutionResult final {
  CompletionPowerExecutionDisposition disposition{
      CompletionPowerExecutionDisposition::no_action};
  CompletionPowerAction requested_action{kDefaultCompletionPowerAction};
  CompletionPowerAction effective_action{kDefaultCompletionPowerAction};
  std::optional<Error> error;
};

// Rechecks every fail-closed gate and runtime capability immediately before
// dispatch. The caller must provide an operation-bound explicit selection and
// a matching immediate reconfirmation. No action is dispatched for a default
// request, incomplete/failed/partial operations, incomplete verification, or
// missing proof that sleep prevention was released.
[[nodiscard]] CompletionPowerExecutionResult execute_completion_power_action(
    const CompletionPowerExecutionRequest& request,
    ICompletionPowerPlatform& platform);

// Production adapter. It uses documented Windows APIs with fixed, conservative
// flags. Creating the adapter does not request any power transition.
[[nodiscard]] std::unique_ptr<ICompletionPowerPlatform>
make_windows_completion_power_platform();

}  // namespace ytec::clonecore

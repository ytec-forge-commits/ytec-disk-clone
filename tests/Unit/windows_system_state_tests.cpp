#include "ytec/vssrequester/windows_backend.h"
#include "ytec/windowsapp/system_state.h"

#include <Windows.h>
#include <objbase.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void confirmation_notes_fail_closed() {
  using ytec::windowsapp::PendingRestartState;
  using ytec::windowsapp::pending_restart_confirmation_note;

  check(
      pending_restart_confirmation_note(PendingRestartState::absent).empty(),
      "Proven-absent state should not add a warning");
  const std::wstring required =
      pending_restart_confirmation_note(PendingRestartState::required);
  check(
      required.find(L"再起動が必要") != std::wstring::npos &&
          required.find(L"推奨") != std::wstring::npos,
      "Required state should explain the restart recommendation");
  const std::wstring unknown =
      pending_restart_confirmation_note(PendingRestartState::unknown);
  check(
      unknown.find(L"確認できません") != std::wstring::npos &&
          !unknown.empty(),
      "Unknown state must remain a visible warning");
}

void live_probe_returns_a_bounded_state() {
  const auto observation =
      ytec::windowsapp::query_windows_update_pending_restart();
  using ytec::windowsapp::PendingRestartState;
  check(
      observation.state == PendingRestartState::absent ||
          observation.state == PendingRestartState::required ||
          observation.state == PendingRestartState::unknown,
      "Live read-only probe must return a bounded state");
}

void power_policy_requires_confirmation_below_half_or_without_ac() {
  using ytec::windowsapp::BatteryPresence;
  using ytec::windowsapp::ExternalPowerState;
  using ytec::windowsapp::PowerObservation;
  using ytec::windowsapp::evaluate_long_operation_power;

  const auto desktop = evaluate_long_operation_power(PowerObservation{
      .external_power = ExternalPowerState::connected,
      .battery_presence = BatteryPresence::absent,
  });
  check(
      !desktop.additional_confirmation_required && desktop.message.empty(),
      "A proven AC-only desktop should not add a power warning");

  const auto boundary = evaluate_long_operation_power(PowerObservation{
      .external_power = ExternalPowerState::connected,
      .battery_presence = BatteryPresence::present,
      .battery_percent = static_cast<std::uint8_t>(50U),
  });
  check(
      !boundary.additional_confirmation_required,
      "Exactly 50 percent on AC should pass the policy boundary");

  const auto low = evaluate_long_operation_power(PowerObservation{
      .external_power = ExternalPowerState::connected,
      .battery_presence = BatteryPresence::present,
      .battery_percent = static_cast<std::uint8_t>(49U),
  });
  check(
      low.additional_confirmation_required &&
          low.message.find(L"49%") != std::wstring::npos,
      "Below 50 percent must require an explicit warning confirmation");

  const auto battery = evaluate_long_operation_power(PowerObservation{
      .external_power = ExternalPowerState::disconnected,
      .battery_presence = BatteryPresence::present,
      .battery_percent = static_cast<std::uint8_t>(90U),
  });
  check(
      battery.additional_confirmation_required &&
          battery.ac_connection_recommended &&
          battery.message.find(L"AC電源") != std::wstring::npos,
      "Running without AC must recommend connection even at high charge");

  const auto unknown = evaluate_long_operation_power(PowerObservation{});
  check(
      unknown.additional_confirmation_required &&
          unknown.ac_connection_recommended && !unknown.message.empty(),
      "Unknown power state must remain a visible confirmation");
}

void live_power_probe_returns_bounded_values() {
  const auto observation =
      ytec::windowsapp::query_windows_power_observation();
  using ytec::windowsapp::BatteryPresence;
  using ytec::windowsapp::ExternalPowerState;
  check(
      observation.external_power == ExternalPowerState::connected ||
          observation.external_power == ExternalPowerState::disconnected ||
          observation.external_power == ExternalPowerState::unknown,
      "Live power query must return a bounded AC state");
  check(
      observation.battery_presence == BatteryPresence::absent ||
          observation.battery_presence == BatteryPresence::present ||
          observation.battery_presence == BatteryPresence::unknown,
      "Live power query must return a bounded battery state");
  check(
      !observation.battery_percent.has_value() ||
          observation.battery_percent.value() <= 100U,
      "Live battery percentage must be absent or bounded");
}

void vss_process_security_precedes_other_com_clients() {
  const HRESULT initialized =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  check(
      SUCCEEDED(initialized),
      "The application apartment should initialize before COM security");

  const auto first =
      ytec::vssrequester::initialize_vss_process_security();
  check(
      first.has_value(),
      "VSS process security should initialize before another COM client");

  const auto observation =
      ytec::windowsapp::query_windows_update_pending_restart();
  using ytec::windowsapp::PendingRestartState;
  check(
      observation.state == PendingRestartState::absent ||
          observation.state == PendingRestartState::required ||
          observation.state == PendingRestartState::unknown,
      "The later COM client should still return a bounded state");

  const auto repeated =
      ytec::vssrequester::initialize_vss_process_security();
  check(
      repeated.has_value(),
      "The VSS backend safety check should reuse startup security");
  CoUninitialize();
}

}  // namespace

int main() {
  try {
    vss_process_security_precedes_other_com_clients();
    confirmation_notes_fail_closed();
    live_probe_returns_a_bounded_state();
    power_policy_requires_confirmation_below_half_or_without_ac();
    live_power_probe_returns_bounded_values();
    std::cout << "windows system state tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "windows system state tests: FAIL: "
              << exception.what() << '\n';
    return 1;
  }
}

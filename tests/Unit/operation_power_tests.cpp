#include "ytec/clonecore/operation_power.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void policy_uses_the_exact_fifty_percent_boundary() {
  using namespace ytec::clonecore;
  const auto fifty = evaluate_long_operation_power(PowerObservation{
      .external_power = ExternalPowerState::connected,
      .battery_presence = BatteryPresence::present,
      .battery_percent = static_cast<std::uint8_t>(50U),
  });
  const auto forty_nine = evaluate_long_operation_power(PowerObservation{
      .external_power = ExternalPowerState::connected,
      .battery_presence = BatteryPresence::present,
      .battery_percent = static_cast<std::uint8_t>(49U),
  });
  check(
      !fifty.additional_confirmation_required,
      "Exactly fifty percent on AC must not require the extra prompt");
  check(
      forty_nine.additional_confirmation_required &&
          forty_nine.message.find(L"49%") != std::wstring::npos,
      "Below fifty percent must require a visible extra prompt");
}

void battery_and_unknown_states_fail_visible() {
  using namespace ytec::clonecore;
  const auto battery = evaluate_long_operation_power(PowerObservation{
      .external_power = ExternalPowerState::disconnected,
      .battery_presence = BatteryPresence::present,
      .battery_percent = static_cast<std::uint8_t>(90U),
  });
  const auto unknown = evaluate_long_operation_power(PowerObservation{});
  check(
      battery.additional_confirmation_required &&
          battery.ac_connection_recommended &&
          battery.message.find(L"AC電源") != std::wstring::npos,
      "Battery operation must recommend AC at any charge level");
  check(
      unknown.additional_confirmation_required &&
          unknown.ac_connection_recommended && !unknown.message.empty(),
      "Unknown power state must not silently pass");
}

void proven_desktop_state_is_quiet() {
  using namespace ytec::clonecore;
  const auto desktop = evaluate_long_operation_power(PowerObservation{
      .external_power = ExternalPowerState::connected,
      .battery_presence = BatteryPresence::absent,
  });
  check(
      !desktop.additional_confirmation_required && desktop.message.empty(),
      "A proven AC-only desktop should not receive a battery warning");
}

void live_query_is_bounded() {
  using namespace ytec::clonecore;
  const auto observation = query_windows_power_observation();
  check(
      observation.external_power == ExternalPowerState::connected ||
          observation.external_power == ExternalPowerState::disconnected ||
          observation.external_power == ExternalPowerState::unknown,
      "The live AC state must be bounded");
  check(
      observation.battery_presence == BatteryPresence::absent ||
          observation.battery_presence == BatteryPresence::present ||
          observation.battery_presence == BatteryPresence::unknown,
      "The live battery presence must be bounded");
  check(
      !observation.battery_percent.has_value() ||
          observation.battery_percent.value() <= 100U,
      "The live battery percentage must be absent or bounded");
}

}  // namespace

int main() {
  try {
    policy_uses_the_exact_fifty_percent_boundary();
    battery_and_unknown_states_fail_visible();
    proven_desktop_state_is_quiet();
    live_query_is_bounded();
    std::cout << "operation power tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "operation power tests: FAIL: " << exception.what() << '\n';
    return 1;
  }
}

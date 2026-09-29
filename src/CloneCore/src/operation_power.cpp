#include "ytec/clonecore/operation_power.h"

#include <Windows.h>

#include <utility>

namespace ytec::clonecore {

PowerObservation query_windows_power_observation() noexcept {
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status) == FALSE) {
    return PowerObservation{
        .native_status = GetLastError(),
    };
  }

  const auto external_power = status.ACLineStatus == 1U
      ? ExternalPowerState::connected
      : status.ACLineStatus == 0U
          ? ExternalPowerState::disconnected
          : ExternalPowerState::unknown;
  BatteryPresence battery_presence = BatteryPresence::unknown;
  if (status.BatteryFlag != 255U) {
    battery_presence = (status.BatteryFlag & 128U) != 0U
        ? BatteryPresence::absent
        : BatteryPresence::present;
  }
  std::optional<std::uint8_t> battery_percent;
  if (battery_presence == BatteryPresence::present &&
      status.BatteryLifePercent <= 100U) {
    battery_percent = status.BatteryLifePercent;
  }
  return PowerObservation{
      .external_power = external_power,
      .battery_presence = battery_presence,
      .battery_percent = battery_percent,
      .native_status = ERROR_SUCCESS,
  };
}

LongOperationPowerAdvisory evaluate_long_operation_power(
    const PowerObservation& observation) {
  if (observation.battery_presence == BatteryPresence::absent &&
      observation.external_power == ExternalPowerState::connected) {
    return {};
  }

  if (observation.battery_presence == BatteryPresence::unknown ||
      observation.external_power == ExternalPowerState::unknown ||
      (observation.battery_presence == BatteryPresence::present &&
       !observation.battery_percent.has_value())) {
    return LongOperationPowerAdvisory{
        .additional_confirmation_required = true,
        .ac_connection_recommended = true,
        .message =
            L"バッテリー残量またはAC接続状態を確認できません。\n"
            L"AC電源へ接続し、処理中に電源が切れないことを確認してください。",
    };
  }

  const bool on_battery =
      observation.external_power == ExternalPowerState::disconnected;
  const bool below_half = observation.battery_percent.has_value() &&
      observation.battery_percent.value() < 50U;
  if (!on_battery && !below_half) {
    return {};
  }

  std::wstring message;
  if (observation.battery_percent.has_value()) {
    message = L"現在のバッテリー残量は" +
        std::to_wstring(observation.battery_percent.value()) + L"%です。";
  } else {
    message = L"現在のバッテリー残量を確認できません。";
  }
  if (on_battery) {
    message += L"\nAC電源へ接続してから開始することを強く推奨します。";
  } else if (below_half) {
    message += L"\nAC接続中ですが、50%以上まで充電してから開始することを推奨します。";
  }
  return LongOperationPowerAdvisory{
      .additional_confirmation_required = true,
      .ac_connection_recommended = on_battery,
      .message = std::move(message),
  };
}

}  // namespace ytec::clonecore

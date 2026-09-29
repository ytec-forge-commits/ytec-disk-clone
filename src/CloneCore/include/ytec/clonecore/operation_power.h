#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ytec::clonecore {

enum class ExternalPowerState : std::uint8_t {
  connected,
  disconnected,
  unknown,
};

enum class BatteryPresence : std::uint8_t {
  absent,
  present,
  unknown,
};

struct PowerObservation final {
  ExternalPowerState external_power{ExternalPowerState::unknown};
  BatteryPresence battery_presence{BatteryPresence::unknown};
  std::optional<std::uint8_t> battery_percent;
  std::uint32_t native_status{};
};

struct LongOperationPowerAdvisory final {
  bool additional_confirmation_required{};
  bool ac_connection_recommended{};
  std::wstring message;
};

// Read-only documented Windows power-status query. Unknown fields remain
// unknown and are never converted into a reassuring state.
[[nodiscard]] PowerObservation query_windows_power_observation() noexcept;

// Pure policy shared by Windows and WinPE: below 50%, running on battery, or
// an unknown power state requires an additional explicit confirmation. It
// only warns and never changes a disk or stops an active operation.
[[nodiscard]] LongOperationPowerAdvisory evaluate_long_operation_power(
    const PowerObservation& observation);

}  // namespace ytec::clonecore

#pragma once

#include "ytec/clonecore/operation_power.h"

#include <cstdint>
#include <string>

namespace ytec::windowsapp {

enum class PendingRestartState : std::uint8_t {
  absent,
  required,
  unknown,
};

struct PendingRestartObservation final {
  PendingRestartState state{PendingRestartState::unknown};
  std::uint32_t native_status{};
};

// Queries the documented Windows Update Agent read-only RebootRequired
// property. Failure to initialize/query WUA is reported as unknown and never
// treated as evidence that no restart is pending.
[[nodiscard]] PendingRestartObservation
query_windows_update_pending_restart() noexcept;

// Returns an empty string only when the documented query proves that no
// update restart is pending. Required/unknown states remain visible warnings.
[[nodiscard]] std::wstring pending_restart_confirmation_note(
    PendingRestartState state);

using ExternalPowerState = clonecore::ExternalPowerState;
using BatteryPresence = clonecore::BatteryPresence;
using PowerObservation = clonecore::PowerObservation;
using LongOperationPowerAdvisory = clonecore::LongOperationPowerAdvisory;

// Preserve the existing WindowsApp API surface while sharing one policy and
// one read-only implementation with WinPE.
using clonecore::evaluate_long_operation_power;
using clonecore::query_windows_power_observation;

}  // namespace ytec::windowsapp

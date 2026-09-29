#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ytec::diskmodel {

enum class DiskHealthState : std::uint8_t {
  unknown,
  healthy,
  caution,
  failing,
};

struct DiskHealthObservation final {
  std::optional<bool> smart_predict_failure;
  std::optional<std::uint8_t> nvme_critical_warning;
  std::optional<std::uint8_t> nvme_available_spare_percent;
  std::optional<std::uint8_t> nvme_available_spare_threshold_percent;
  std::optional<std::uint8_t> nvme_percentage_used;
  std::optional<std::int32_t> temperature_celsius;
  std::optional<std::int32_t> warning_temperature_celsius;
  std::optional<std::int32_t> critical_temperature_celsius;
};

struct DiskHealthInfo final {
  DiskHealthState state{DiskHealthState::unknown};
  bool smart_status_available{};
  bool nvme_health_available{};
  bool temperature_warning{};
  std::optional<std::int32_t> temperature_celsius;
  std::optional<std::int32_t> warning_temperature_celsius;
  std::optional<std::int32_t> critical_temperature_celsius;
  std::optional<std::uint8_t> nvme_available_spare_percent;
  std::optional<std::uint8_t> nvme_percentage_used;
  std::uint8_t nvme_critical_warning{};
};

// Pure parsers for the bounded byte ranges returned by the corresponding
// Windows storage IOCTLs. They commit observation fields only after the whole
// response has passed its structural checks. Unsupported or malformed device
// responses therefore remain "unknown" and cannot be mistaken for healthy
// evidence. These functions perform no device I/O.
[[nodiscard]] bool parse_storage_predict_failure_response(
    std::span<const std::byte> response,
    DiskHealthObservation& observation) noexcept;

[[nodiscard]] bool parse_storage_temperature_response(
    std::span<const std::byte> response,
    DiskHealthObservation& observation) noexcept;

[[nodiscard]] bool parse_nvme_health_response(
    std::span<const std::byte> response,
    DiskHealthObservation& observation) noexcept;

enum class DiskHealthOperationAdvice : std::uint8_t {
  proceed,
  recommend_rescue,
  block_target,
};

// Normalizes only device-health evidence. A temperature warning is kept
// separate and never upgrades the health state, because product policy treats
// temperature as display/warning information rather than an automatic stop.
[[nodiscard]] DiskHealthInfo normalize_disk_health(
    const DiskHealthObservation& observation) noexcept;

[[nodiscard]] DiskHealthOperationAdvice disk_health_operation_advice(
    const DiskHealthInfo& health,
    bool is_source_disk) noexcept;

[[nodiscard]] std::wstring_view disk_health_state_name(
    DiskHealthState state) noexcept;

}  // namespace ytec::diskmodel

#include "ytec/diskmodel/disk_health.h"

#include <Windows.h>
#include <winioctl.h>
#include <nvme.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

namespace ytec::diskmodel {
namespace {

constexpr std::uint8_t kNvmeAvailableSpaceLow = 1U << 0U;
constexpr std::uint8_t kNvmeTemperatureThreshold = 1U << 1U;
constexpr std::uint8_t kNvmeReliabilityDegraded = 1U << 2U;
constexpr std::uint8_t kNvmeReadOnly = 1U << 3U;
constexpr std::uint8_t kNvmeVolatileMemoryBackupFailed = 1U << 4U;
constexpr std::uint8_t kNvmeFailingWarnings =
    kNvmeReliabilityDegraded | kNvmeReadOnly |
    kNvmeVolatileMemoryBackupFailed;
constexpr std::uint8_t kNvmeKnownWarnings =
    kNvmeAvailableSpaceLow | kNvmeTemperatureThreshold |
    kNvmeFailingWarnings;

bool temperature_is_at_or_above(
    const std::optional<std::int32_t>& temperature,
    const std::optional<std::int32_t>& threshold) noexcept {
  return temperature.has_value() && threshold.has_value() &&
         temperature.value() >= threshold.value();
}

template <typename T>
bool read_pod(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    T& value) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    return false;
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

bool plausible_celsius(const SHORT value) noexcept {
  constexpr SHORT kNotReported =
      static_cast<SHORT>(STORAGE_TEMPERATURE_VALUE_NOT_REPORTED);
  return value == kNotReported || (value >= -273 && value <= 1000);
}

}  // namespace

bool parse_storage_predict_failure_response(
    const std::span<const std::byte> response,
    DiskHealthObservation& observation) noexcept {
  constexpr std::size_t kMinimumResponse =
      offsetof(STORAGE_PREDICT_FAILURE, VendorSpecific);
  ULONG prediction = 0U;
  if (response.size() < kMinimumResponse ||
      !read_pod(response, offsetof(STORAGE_PREDICT_FAILURE, PredictFailure),
                prediction)) {
    return false;
  }
  observation.smart_predict_failure = prediction != 0U;
  return true;
}

bool parse_storage_temperature_response(
    const std::span<const std::byte> response,
    DiskHealthObservation& observation) noexcept {
  constexpr std::size_t kInfoOffset =
      offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo);
  DWORD version = 0U;
  DWORD declared_size = 0U;
  SHORT critical = 0;
  SHORT warning = 0;
  WORD info_count = 0U;
  if (response.size() < kInfoOffset ||
      !read_pod(response, offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, Version),
                version) ||
      !read_pod(response, offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, Size),
                declared_size) ||
      !read_pod(response,
                offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR,
                         CriticalTemperature),
                critical) ||
      !read_pod(response,
                offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR,
                         WarningTemperature),
                warning) ||
      !read_pod(response,
                offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, InfoCount),
                info_count) ||
      static_cast<std::size_t>(version) < kInfoOffset ||
      version > declared_size ||
      static_cast<std::size_t>(declared_size) < kInfoOffset ||
      static_cast<std::size_t>(declared_size) > response.size() ||
      !plausible_celsius(critical) || !plausible_celsius(warning)) {
    return false;
  }
  const std::size_t available_count =
      (static_cast<std::size_t>(declared_size) - kInfoOffset) /
      sizeof(STORAGE_TEMPERATURE_INFO);
  if (static_cast<std::size_t>(info_count) > available_count) {
    return false;
  }

  constexpr SHORT kNotReported =
      static_cast<SHORT>(STORAGE_TEMPERATURE_VALUE_NOT_REPORTED);
  std::optional<SHORT> selected_temperature;
  std::optional<SHORT> selected_over_threshold;
  bool selected_is_composite = false;
  for (std::size_t index = 0U; index < info_count; ++index) {
    STORAGE_TEMPERATURE_INFO sensor{};
    const std::size_t sensor_offset =
        kInfoOffset + index * sizeof(STORAGE_TEMPERATURE_INFO);
    if (!read_pod(response, sensor_offset, sensor) ||
        !plausible_celsius(sensor.Temperature) ||
        !plausible_celsius(sensor.OverThreshold) ||
        !plausible_celsius(sensor.UnderThreshold)) {
      return false;
    }
    if (sensor.Temperature == kNotReported || selected_is_composite) {
      continue;
    }
    selected_temperature = sensor.Temperature;
    selected_over_threshold =
        sensor.OverThreshold == kNotReported
            ? std::nullopt
            : std::optional<SHORT>(sensor.OverThreshold);
    selected_is_composite = sensor.Index == 0U;
  }

  DiskHealthObservation parsed = observation;
  if (warning != kNotReported) {
    parsed.warning_temperature_celsius = warning;
  }
  if (critical != kNotReported) {
    parsed.critical_temperature_celsius = critical;
  }
  if (selected_temperature.has_value()) {
    parsed.temperature_celsius = selected_temperature.value();
    if (selected_over_threshold.has_value()) {
      const std::int32_t sensor_warning =
          selected_over_threshold.value();
      parsed.warning_temperature_celsius =
          parsed.warning_temperature_celsius.has_value()
              ? (std::min)(parsed.warning_temperature_celsius.value(),
                           sensor_warning)
              : sensor_warning;
    }
  }
  observation = parsed;
  return true;
}

bool parse_nvme_health_response(
    const std::span<const std::byte> response,
    DiskHealthObservation& observation) noexcept {
  constexpr std::size_t kProtocolOffset =
      offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
  if (response.size() <
      kProtocolOffset + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)) {
    return false;
  }

  DWORD version = 0U;
  DWORD declared_size = 0U;
  STORAGE_PROTOCOL_SPECIFIC_DATA protocol{};
  if (!read_pod(response,
                offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, Version),
                version) ||
      !read_pod(response,
                offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, Size),
                declared_size) ||
      !read_pod(response, kProtocolOffset, protocol) ||
      version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) ||
      declared_size != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) ||
      protocol.ProtocolType != ProtocolTypeNvme ||
      protocol.DataType != NVMeDataTypeLogPage ||
      protocol.ProtocolDataRequestValue !=
          static_cast<DWORD>(NVME_LOG_PAGE_HEALTH_INFO) ||
      protocol.ProtocolDataOffset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) ||
      protocol.ProtocolDataLength < sizeof(NVME_HEALTH_INFO_LOG)) {
    return false;
  }
  const std::size_t data_offset =
      kProtocolOffset + static_cast<std::size_t>(protocol.ProtocolDataOffset);
  if (data_offset < kProtocolOffset || data_offset > response.size() ||
      static_cast<std::size_t>(protocol.ProtocolDataLength) >
          response.size() - data_offset ||
      sizeof(NVME_HEALTH_INFO_LOG) > response.size() - data_offset) {
    return false;
  }

  NVME_HEALTH_INFO_LOG health_log{};
  if (!read_pod(response, data_offset, health_log) ||
      health_log.AvailableSpare > 100U ||
      health_log.AvailableSpareThreshold > 100U) {
    return false;
  }

  DiskHealthObservation parsed = observation;
  parsed.nvme_critical_warning = health_log.CriticalWarning.AsUchar;
  parsed.nvme_available_spare_percent = health_log.AvailableSpare;
  parsed.nvme_available_spare_threshold_percent =
      health_log.AvailableSpareThreshold;
  parsed.nvme_percentage_used = health_log.PercentageUsed;
  const std::uint16_t kelvin =
      static_cast<std::uint16_t>(health_log.Temperature[0]) |
      (static_cast<std::uint16_t>(health_log.Temperature[1]) << 8U);
  if (!parsed.temperature_celsius.has_value() && kelvin != 0U &&
      kelvin != std::numeric_limits<std::uint16_t>::max() && kelvin <= 1000U) {
    parsed.temperature_celsius = static_cast<std::int32_t>(kelvin) - 273;
  }
  observation = parsed;
  return true;
}

DiskHealthInfo normalize_disk_health(
    const DiskHealthObservation& observation) noexcept {
  DiskHealthInfo health;
  health.smart_status_available = observation.smart_predict_failure.has_value();
  health.nvme_health_available = observation.nvme_critical_warning.has_value();
  health.temperature_celsius = observation.temperature_celsius;
  health.warning_temperature_celsius =
      observation.warning_temperature_celsius;
  health.critical_temperature_celsius =
      observation.critical_temperature_celsius;
  health.nvme_available_spare_percent =
      observation.nvme_available_spare_percent;
  health.nvme_percentage_used = observation.nvme_percentage_used;
  health.nvme_critical_warning =
      observation.nvme_critical_warning.value_or(0U);

  const bool nvme_temperature_warning =
      (health.nvme_critical_warning & kNvmeTemperatureThreshold) != 0U;
  health.temperature_warning =
      nvme_temperature_warning ||
      temperature_is_at_or_above(
          health.temperature_celsius, health.warning_temperature_celsius) ||
      temperature_is_at_or_above(
          health.temperature_celsius, health.critical_temperature_celsius);

  const bool smart_failing =
      observation.smart_predict_failure.value_or(false);
  const bool nvme_failing =
      (health.nvme_critical_warning & kNvmeFailingWarnings) != 0U;
  if (smart_failing || nvme_failing) {
    health.state = DiskHealthState::failing;
    return health;
  }

  const bool nvme_spare_warning =
      (health.nvme_critical_warning & kNvmeAvailableSpaceLow) != 0U ||
      (observation.nvme_available_spare_percent.has_value() &&
       observation.nvme_available_spare_threshold_percent.has_value() &&
       observation.nvme_available_spare_percent.value() <=
           observation.nvme_available_spare_threshold_percent.value());
  const bool endurance_used =
      observation.nvme_percentage_used.value_or(0U) >= 100U;
  const bool nvme_unknown_warning =
      observation.nvme_critical_warning.has_value() &&
      (health.nvme_critical_warning &
       static_cast<std::uint8_t>(~kNvmeKnownWarnings)) != 0U;
  if (nvme_spare_warning || endurance_used || nvme_unknown_warning) {
    health.state = DiskHealthState::caution;
    return health;
  }

  if (health.smart_status_available || health.nvme_health_available) {
    health.state = DiskHealthState::healthy;
  }
  return health;
}

DiskHealthOperationAdvice disk_health_operation_advice(
    const DiskHealthInfo& health,
    const bool is_source_disk) noexcept {
  if (health.state != DiskHealthState::caution &&
      health.state != DiskHealthState::failing) {
    return DiskHealthOperationAdvice::proceed;
  }
  return is_source_disk ? DiskHealthOperationAdvice::recommend_rescue
                        : DiskHealthOperationAdvice::block_target;
}

std::wstring_view disk_health_state_name(const DiskHealthState state) noexcept {
  switch (state) {
    case DiskHealthState::healthy:
      return L"警告なし";
    case DiskHealthState::caution:
      return L"注意";
    case DiskHealthState::failing:
      return L"異常";
    case DiskHealthState::unknown:
    default:
      return L"未取得";
  }
}

}  // namespace ytec::diskmodel

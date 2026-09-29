#include "ytec/diskmodel/disk_health.h"

#include <Windows.h>
#include <winioctl.h>
#include <nvme.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

template <typename T>
void write_pod(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T& value) {
  check(
      offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
      "synthetic response write must stay in bounds");
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

std::vector<std::byte> make_temperature_response(
    const std::span<const STORAGE_TEMPERATURE_INFO> sensors) {
  constexpr std::size_t kOffset =
      offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo);
  std::vector<std::byte> bytes(
      kOffset + sensors.size() * sizeof(STORAGE_TEMPERATURE_INFO));
  const DWORD version = sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR);
  const DWORD size = static_cast<DWORD>(bytes.size());
  const SHORT critical = 85;
  const SHORT warning = 70;
  const WORD count = static_cast<WORD>(sensors.size());
  write_pod(
      bytes, offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, Version), version);
  write_pod(
      bytes, offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, Size), size);
  write_pod(
      bytes,
      offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, CriticalTemperature),
      critical);
  write_pod(
      bytes,
      offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, WarningTemperature),
      warning);
  write_pod(
      bytes, offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, InfoCount), count);
  for (std::size_t index = 0U; index < sensors.size(); ++index) {
    write_pod(bytes, kOffset + index * sizeof(sensors[index]), sensors[index]);
  }
  return bytes;
}

std::vector<std::byte> make_nvme_response(
    const NVME_HEALTH_INFO_LOG& health) {
  constexpr std::size_t kProtocolOffset =
      offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
  constexpr std::size_t kDataOffset =
      kProtocolOffset + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
  std::vector<std::byte> bytes(kDataOffset + sizeof(health));
  STORAGE_PROTOCOL_DATA_DESCRIPTOR descriptor{};
  descriptor.Version = sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR);
  descriptor.Size = sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR);
  descriptor.ProtocolSpecificData.ProtocolType = ProtocolTypeNvme;
  descriptor.ProtocolSpecificData.DataType = NVMeDataTypeLogPage;
  descriptor.ProtocolSpecificData.ProtocolDataRequestValue =
      static_cast<DWORD>(NVME_LOG_PAGE_HEALTH_INFO);
  descriptor.ProtocolSpecificData.ProtocolDataOffset =
      sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
  descriptor.ProtocolSpecificData.ProtocolDataLength = sizeof(health);
  write_pod(bytes, 0U, descriptor);
  write_pod(bytes, kDataOffset, health);
  return bytes;
}

void test_unknown_observation_remains_non_blocking() {
  const auto health = ytec::diskmodel::normalize_disk_health({});
  check(
      health.state == ytec::diskmodel::DiskHealthState::unknown,
      "unsupported health queries must remain unknown");
  check(
      ytec::diskmodel::disk_health_operation_advice(health, false) ==
          ytec::diskmodel::DiskHealthOperationAdvice::proceed,
      "unknown target health must not be called an abnormal disk");
}

void test_smart_failure_blocks_target_and_recommends_source_rescue() {
  ytec::diskmodel::DiskHealthObservation observation;
  observation.smart_predict_failure = true;
  const auto health = ytec::diskmodel::normalize_disk_health(observation);
  check(
      health.state == ytec::diskmodel::DiskHealthState::failing,
      "SMART predicted failure must be failing");
  check(
      ytec::diskmodel::disk_health_operation_advice(health, false) ==
          ytec::diskmodel::DiskHealthOperationAdvice::block_target,
      "failing target must be blocked");
  check(
      ytec::diskmodel::disk_health_operation_advice(health, true) ==
          ytec::diskmodel::DiskHealthOperationAdvice::recommend_rescue,
      "failing source must recommend rescue mode");
}

void test_nvme_media_warnings_are_distinct_from_temperature() {
  ytec::diskmodel::DiskHealthObservation reliability;
  reliability.nvme_critical_warning = std::uint8_t{0x04U};
  check(
      ytec::diskmodel::normalize_disk_health(reliability).state ==
          ytec::diskmodel::DiskHealthState::failing,
      "NVMe reliability degradation must be failing");

  ytec::diskmodel::DiskHealthObservation spare;
  spare.nvme_critical_warning = std::uint8_t{0U};
  spare.nvme_available_spare_percent = std::uint8_t{9U};
  spare.nvme_available_spare_threshold_percent = std::uint8_t{10U};
  check(
      ytec::diskmodel::normalize_disk_health(spare).state ==
          ytec::diskmodel::DiskHealthState::caution,
      "NVMe spare at threshold must be caution");

  ytec::diskmodel::DiskHealthObservation endurance;
  endurance.nvme_critical_warning = std::uint8_t{0U};
  endurance.nvme_percentage_used = std::uint8_t{100U};
  check(
      ytec::diskmodel::normalize_disk_health(endurance).state ==
          ytec::diskmodel::DiskHealthState::caution,
      "NVMe endurance at 100 percent must be caution");

  ytec::diskmodel::DiskHealthObservation future_warning;
  future_warning.nvme_critical_warning = std::uint8_t{0x80U};
  check(
      ytec::diskmodel::normalize_disk_health(future_warning).state ==
          ytec::diskmodel::DiskHealthState::caution,
      "Unknown NVMe critical warning bits must never be called healthy");
}

void test_temperature_warns_without_automatic_stop() {
  ytec::diskmodel::DiskHealthObservation observation;
  observation.nvme_critical_warning = std::uint8_t{0U};
  observation.temperature_celsius = 70;
  observation.warning_temperature_celsius = 65;
  const auto health = ytec::diskmodel::normalize_disk_health(observation);
  check(health.temperature_warning, "temperature threshold must warn");
  check(
      health.state == ytec::diskmodel::DiskHealthState::healthy,
      "temperature warning alone must not change device health state");
  check(
      ytec::diskmodel::disk_health_operation_advice(health, false) ==
      ytec::diskmodel::DiskHealthOperationAdvice::proceed,
      "temperature warning alone must not block the target");

  ytec::diskmodel::DiskHealthObservation nvme_temperature;
  nvme_temperature.nvme_critical_warning = std::uint8_t{0x02U};
  const auto nvme_health =
      ytec::diskmodel::normalize_disk_health(nvme_temperature);
  check(
      nvme_health.temperature_warning,
      "NVMe composite temperature warning bit must warn without a numeric threshold");
  check(
      nvme_health.state == ytec::diskmodel::DiskHealthState::healthy,
      "NVMe temperature warning bit alone must not change device health state");
}

void test_predict_failure_parser_is_atomic_and_bounded() {
  STORAGE_PREDICT_FAILURE response{};
  response.PredictFailure = 1U;
  ytec::diskmodel::DiskHealthObservation observation;
  check(
      ytec::diskmodel::parse_storage_predict_failure_response(
          std::as_bytes(std::span{&response, std::size_t{1U}}), observation),
      "complete failure-prediction response must parse");
  check(
      observation.smart_predict_failure.value_or(false),
      "parsed failure prediction must be retained");

  const std::array<std::byte, 3U> truncated{};
  observation.smart_predict_failure = false;
  check(
      !ytec::diskmodel::parse_storage_predict_failure_response(
          truncated, observation),
      "truncated failure-prediction response must be rejected");
  check(
      observation.smart_predict_failure == false,
      "rejected response must not alter the prior observation");
}

void test_temperature_parser_prefers_composite_and_rejects_malformed() {
  constexpr SHORT kNotReported =
      static_cast<SHORT>(STORAGE_TEMPERATURE_VALUE_NOT_REPORTED);
  const std::array sensors{
      STORAGE_TEMPERATURE_INFO{
          .Index = 3U,
          .Temperature = 41,
          .OverThreshold = 75,
          .UnderThreshold = kNotReported,
      },
      STORAGE_TEMPERATURE_INFO{
          .Index = 0U,
          .Temperature = 55,
          .OverThreshold = 68,
          .UnderThreshold = kNotReported,
      },
  };
  auto bytes = make_temperature_response(sensors);
  ytec::diskmodel::DiskHealthObservation observation;
  check(
      ytec::diskmodel::parse_storage_temperature_response(bytes, observation),
      "bounded temperature descriptor must parse");
  check(
      observation.temperature_celsius == 55 &&
          observation.warning_temperature_celsius == 68 &&
          observation.critical_temperature_celsius == 85,
      "composite sensor and strictest warning must be selected");

  const WORD impossible_count = 3U;
  write_pod(
      bytes,
      offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, InfoCount),
      impossible_count);
  observation.temperature_celsius = 19;
  check(
      !ytec::diskmodel::parse_storage_temperature_response(bytes, observation),
      "InfoCount beyond returned bytes must be rejected");
  check(
      observation.temperature_celsius == 19,
      "malformed temperature response must not partially update fields");

  bytes = make_temperature_response(sensors);
  SHORT impossible_temperature = 2000;
  write_pod(
      bytes,
      offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo) +
          offsetof(STORAGE_TEMPERATURE_INFO, Temperature),
      impossible_temperature);
  check(
      !ytec::diskmodel::parse_storage_temperature_response(bytes, observation),
      "implausible signed temperature must be rejected");
}

void test_nvme_parser_checks_protocol_bounds_and_commits_atomically() {
  NVME_HEALTH_INFO_LOG health{};
  health.CriticalWarning.AsUchar = 0x08U;
  health.AvailableSpare = 80U;
  health.AvailableSpareThreshold = 10U;
  health.PercentageUsed = 22U;
  constexpr std::uint16_t kelvin = 333U;
  health.Temperature[0] = static_cast<UCHAR>(kelvin & 0xFFU);
  health.Temperature[1] = static_cast<UCHAR>(kelvin >> 8U);
  auto bytes = make_nvme_response(health);
  ytec::diskmodel::DiskHealthObservation observation;
  check(
      ytec::diskmodel::parse_nvme_health_response(bytes, observation),
      "valid NVMe health response must parse");
  check(
      observation.nvme_critical_warning == std::uint8_t{0x08U} &&
          observation.nvme_available_spare_percent == std::uint8_t{80U} &&
          observation.nvme_percentage_used == std::uint8_t{22U} &&
          observation.temperature_celsius == 60,
      "NVMe fields and Kelvin temperature must be normalized");

  constexpr std::size_t kProtocolOffset =
      offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
  DWORD bad_offset = 0xFFFFFFF0U;
  write_pod(
      bytes,
      kProtocolOffset +
          offsetof(STORAGE_PROTOCOL_SPECIFIC_DATA, ProtocolDataOffset),
      bad_offset);
  observation.nvme_percentage_used = std::uint8_t{7U};
  check(
      !ytec::diskmodel::parse_nvme_health_response(bytes, observation),
      "out-of-range NVMe data offset must be rejected");
  check(
      observation.nvme_percentage_used == std::uint8_t{7U},
      "malformed NVMe response must not partially update fields");

  health.AvailableSpare = 101U;
  bytes = make_nvme_response(health);
  check(
      !ytec::diskmodel::parse_nvme_health_response(bytes, observation),
      "invalid NVMe spare percentage must be rejected");
}

}  // namespace

int main() {
  try {
    test_unknown_observation_remains_non_blocking();
    test_smart_failure_blocks_target_and_recommends_source_rescue();
    test_nvme_media_warnings_are_distinct_from_temperature();
    test_temperature_warns_without_automatic_stop();
    test_predict_failure_parser_is_atomic_and_bounded();
    test_temperature_parser_prefers_composite_and_rejects_malformed();
    test_nvme_parser_checks_protocol_bounds_and_commits_atomically();
    std::cout << "disk health tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "disk health tests: FAIL: " << failure.message << '\n';
    return 1;
  }
}

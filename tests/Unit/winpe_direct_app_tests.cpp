#include "ytec/winpeapp/app_runner.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class MockInventoryProvider final
    : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  explicit MockInventoryProvider(ytec::diskmodel::InventoryReport report)
      : report_(std::move(report)) {}

  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++call_count;
    return ytec::clonecore::Result<
        ytec::diskmodel::InventoryReport>::success(report_);
  }

  std::size_t call_count{};

 private:
  ytec::diskmodel::InventoryReport report_;
};

class MockExecutionService final
    : public ytec::winpeapp::ICloneExecutionService {
 public:
  ytec::clonecore::Result<ytec::winpeapp::CloneExecutionReport> execute(
      const ytec::winpeapp::CloneExecutionRequest& request) override {
    ++call_count;
    last_request = request;
    return ytec::clonecore::Result<
        ytec::winpeapp::CloneExecutionReport>::success(report);
  }

  std::size_t call_count{};
  ytec::winpeapp::CloneExecutionRequest last_request;
  ytec::winpeapp::CloneExecutionReport report{
      .partition_style = ytec::winpeapp::ClonePartitionStyle::gpt,
      .copied_data_bytes = 123456U,
      .copied_partition_count = 1U,
      .read_back_verified = true,
      .partition_table_committed = true,
      .target_returned_online = false,
      .target_left_offline = true,
      .boot_finalization_required = false,
  };
};

ytec::diskmodel::DiskInfo make_disk(
    const std::uint32_t number,
    const std::uint64_t size,
    std::string serial,
    const ytec::diskmodel::PartitionStyle style) {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = number;
  disk.device_path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
  disk.device_instance_id = L"MOCK\\DISK\\" + std::to_wstring(number);
  disk.model = L"SYNTHETIC DISK";
  disk.size_bytes = size;
  disk.sector_count = size / 512U;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 512U;
  disk.bus_type = L"SATA";
  disk.serial_suffix = std::move(serial);
  disk.partition_style = style;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = false;
  if (style == ytec::diskmodel::PartitionStyle::gpt) {
    disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
        .number = 1U,
        .offset_bytes = 1'048'576U,
        .size_bytes = 536'870'912U,
        .style = ytec::diskmodel::PartitionStyle::gpt,
        .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
    });
  }
  return disk;
}

ytec::diskmodel::InventoryReport valid_report() {
  ytec::diskmodel::InventoryReport report;
  report.disks.push_back(make_disk(
      0U,
      2ULL * 1024U * 1024U * 1024U,
      "SOURCE01",
      ytec::diskmodel::PartitionStyle::gpt));
  report.disks.push_back(make_disk(
      1U,
      3ULL * 1024U * 1024U * 1024U,
      "TARGET01",
      ytec::diskmodel::PartitionStyle::raw));
  return report;
}

void help_exposes_read_only_preflight_only() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--help"}, provider, output, errors);
  check(exit_code == 0, "help must succeed");
  check(output.str().find("--clone-preflight") != std::string::npos,
        "direct preflight must be documented");
  check(output.str().find("--clone-execute") == std::string::npos,
        "destructive clone CLI must not be documented");
  check(output.str().find("--acknowledge-target-erasure") ==
            std::string::npos,
        "target erasure CLI bypass must not be documented");
  check(output.str().find("--confirmation") == std::string::npos,
        "fixed confirmation CLI must not be documented");
  check(output.str().find("--job-") == std::string::npos,
        "removed job CLI must not be documented");
  check(provider.call_count == 0U, "help must not enumerate disks");
}

void removed_job_arguments_stop_before_all_io() {
  for (const std::wstring argument :
       {L"--job-preflight", L"--job-execute", L"--job-path"}) {
    MockInventoryProvider provider(valid_report());
    std::ostringstream output;
    std::ostringstream errors;
    const int exit_code = ytec::winpeapp::run_winpe_app(
        {argument, L"X:\\legacy-do-not-open.json"},
        provider,
        output,
        errors);
    check(exit_code == 64, "removed job argument must be rejected");
    check(provider.call_count == 0U,
          "removed job argument must not enumerate or search media");
  }
}

void destructive_cli_arguments_stop_before_all_io() {
  const std::vector<std::vector<std::wstring>> rejected{
      {L"--clone-execute", L"--source", L"0", L"--target", L"1",
       L"--acknowledge-target-erasure", L"--confirmation", L"OK"},
      {L"--acknowledge-target-erasure"},
      {L"--confirmation", L"OK"},
      {L"--clone-preflight", L"--source", L"0", L"--target", L"1",
       L"--confirmation", L"OK"},
  };
  for (const auto& arguments : rejected) {
    MockInventoryProvider provider(valid_report());
    std::ostringstream output;
    std::ostringstream errors;
    const int exit_code = ytec::winpeapp::run_winpe_app(
        arguments, provider, output, errors);
    check(exit_code == 64,
          "destructive or fixed-confirmation CLI arguments must be rejected");
    check(provider.call_count == 0U,
          "rejected destructive CLI arguments must stop before inventory");
  }
}

void direct_preflight_is_read_only() {
  MockInventoryProvider provider(valid_report());
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-preflight", L"--source", L"0", L"--target", L"1"},
      provider,
      output,
      errors);
  check(exit_code == 0, "direct preflight must succeed");
  check(provider.call_count == 1U, "preflight must enumerate once");
  check(output.str().find("実行確認語: OK") != std::string::npos,
        "preflight must show the short confirmation");
  check(output.str().find("書き込んでいません") != std::string::npos,
        "preflight must state its read-only boundary");
}

void abnormal_target_health_fails_preflight() {
  auto report = valid_report();
  report.disks[1].health.state =
      ytec::diskmodel::DiskHealthState::caution;
  MockInventoryProvider provider(std::move(report));
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-preflight", L"--source", L"0", L"--target", L"1"},
      provider,
      output,
      errors);
  check(exit_code == 1,
        "SMART/NVMe caution target must fail closed");
  check(provider.call_count == 1U,
        "health gate may use one read-only inventory");
}

void abnormal_source_health_remains_visible_and_recommends_rescue() {
  auto report = valid_report();
  report.disks[0].health.state =
      ytec::diskmodel::DiskHealthState::caution;
  report.disks[0].health.temperature_celsius = 69;
  report.disks[0].health.temperature_warning = true;
  MockInventoryProvider provider(std::move(report));
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::winpeapp::run_winpe_app(
      {L"--clone-preflight", L"--source", L"0", L"--target", L"1"},
      provider,
      output,
      errors);
  check(exit_code == 0,
        "An abnormal source should remain reviewable for rescue guidance");
  check(output.str().find("救出モードを推奨") != std::string::npos,
        "The PE review must recommend rescue mode for an abnormal source");
  check(output.str().find("温度状態を確認") != std::string::npos,
        "A PE source temperature warning must remain visible before OK");
}

void direct_internal_api_requires_exact_ok_and_leaves_target_offline() {
  {
    MockInventoryProvider provider(valid_report());
    MockExecutionService service;
    const auto plan = ytec::winpeapp::prepare_direct_clone_operation(
        0U, 1U, provider);
    check(plan.has_value(), "valid direct plan must prepare");
    const auto result = ytec::winpeapp::execute_direct_clone_operation(
        plan.value(), true, L"ok", L"", service);
    check(!result.has_value(), "lowercase confirmation must fail");
    check(service.call_count == 0U,
          "invalid confirmation must not reach the service");
  }

  MockInventoryProvider provider(valid_report());
  MockExecutionService service;
  const auto plan = ytec::winpeapp::prepare_direct_clone_operation(
      0U, 1U, provider);
  check(plan.has_value(), "valid direct plan must prepare");
  const auto result = ytec::winpeapp::execute_direct_clone_operation(
      plan.value(), true, L"OK", L"", service);
  check(result.has_value(), "exact OK confirmation must execute");
  check(service.call_count == 1U, "service must execute once");
  check(service.last_request.leave_target_offline,
        "direct clone must request an offline completed target");
  check(service.last_request.confirmation.first_step_acknowledged,
        "target erasure acknowledgement must be target-bound");
  check(service.last_request.confirmation.typed_token == L"OK",
        "service confirmation must preserve the approved exact OK token");
  check(result.value().target_left_offline,
        "validated completion must leave the target offline");
}

}  // namespace

int main() {
  try {
    help_exposes_read_only_preflight_only();
    removed_job_arguments_stop_before_all_io();
    destructive_cli_arguments_stop_before_all_io();
    direct_preflight_is_read_only();
    abnormal_target_health_fails_preflight();
    abnormal_source_health_remains_visible_and_recommends_rescue();
    direct_internal_api_requires_exact_ok_and_leaves_target_offline();
    std::cout << "winpe direct app tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "winpe direct app tests: FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

#include "ytec/winpeapp/app_runner.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class StaticInventory final
    : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  explicit StaticInventory(ytec::diskmodel::InventoryReport report)
      : report_(std::move(report)) {}

  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++calls;
    return ytec::clonecore::Result<
        ytec::diskmodel::InventoryReport>::success(report_);
  }

  std::size_t calls{};

 private:
  ytec::diskmodel::InventoryReport report_;
};

ytec::diskmodel::DiskInfo make_mbr_source() {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 4U;
  disk.device_path = L"\\\\.\\PhysicalDrive4";
  disk.device_instance_id = L"SYNTHETIC\\MBR2GPT\\SOURCE";
  disk.model = L"SYNTHETIC MBR SOURCE";
  disk.size_bytes = 4ULL * 1024U * 1024U * 1024U;
  disk.sector_count = disk.size_bytes / 512U;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 4096U;
  disk.bus_type = L"SATA";
  disk.serial_suffix = "MGRSRC01";
  disk.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  disk.disk_identifier = L"A1B2C3D4";
  disk.offline = false;
  disk.read_only = false;
  disk.removable = false;
  disk.partitions = {
      ytec::diskmodel::PartitionInfo{
          .number = 1U,
          .offset_bytes = 1ULL * 1024U * 1024U,
          .size_bytes = 512ULL * 1024U * 1024U,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
          .identifier = L"mbr-1",
          .name = L"System",
          .bootable = true,
      },
      ytec::diskmodel::PartitionInfo{
          .number = 2U,
          .offset_bytes = 513ULL * 1024U * 1024U,
          .size_bytes = 3ULL * 1024U * 1024U * 1024U,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
          .identifier = L"mbr-2",
          .name = L"Windows",
      },
  };
  return disk;
}

ytec::diskmodel::DiskInfo make_target() {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 8U;
  disk.device_path = L"\\\\.\\PhysicalDrive8";
  disk.device_instance_id = L"SYNTHETIC\\MBR2GPT\\TARGET";
  disk.model = L"SYNTHETIC GPT TARGET";
  disk.size_bytes = 6ULL * 1024U * 1024U * 1024U;
  disk.sector_count = disk.size_bytes / 512U;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 4096U;
  disk.bus_type = L"USB";
  disk.serial_suffix = "MGRTGT01";
  disk.partition_style = ytec::diskmodel::PartitionStyle::raw;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = false;
  return disk;
}

ytec::diskmodel::InventoryReport valid_report() {
  ytec::diskmodel::InventoryReport report;
  report.disks.push_back(make_mbr_source());
  report.disks.push_back(make_target());
  return report;
}

ytec::winpeapp::Mbr2GptDirectExecutionReport valid_execution_report() {
  ytec::winpeapp::Mbr2GptDirectExecutionReport report;
  report.clone.partition_style =
      ytec::winpeapp::ClonePartitionStyle::mbr;
  report.clone.copied_data_bytes = 4096U;
  report.clone.copied_partition_count = 2U;
  report.clone.read_back_verified = true;
  report.clone.partition_table_committed = true;
  report.clone.target_returned_online = true;
  report.clone.target_left_offline = false;
  report.clone.boot_finalization_required = true;
  report.clone.boot_repair.bcdboot.microsoft_signature_verified = true;
  report.clone.boot_repair.bcdboot.exit_code = 0U;
  report.clone.boot_repair.boot_store_verified = true;
  report.clone.temporary_mounts_released = true;
  report.clone.boot_finalization_verified = true;
  report.conversion.validation.exit_code = 0U;
  report.conversion.conversion.exit_code = 0U;
  report.conversion.microsoft_signature_verified = true;
  report.conversion.target_reidentified_before_conversion = true;
  report.boot_repair.bcdboot.microsoft_signature_verified = true;
  report.boot_repair.bcdboot.exit_code = 0U;
  report.boot_repair.boot_store_verified = true;
  report.source_reidentified_unchanged = true;
  report.source_left_read_only = true;
  report.target_reidentified_as_gpt = true;
  report.efi_system_partition_verified = true;
  report.microsoft_reserved_partition_verified = true;
  report.offline_windows_verified = true;
  report.temporary_windows_mount_released = true;
  report.final_layout_verified = true;
  report.final_target_left_offline = true;
  return report;
}

class MockMigrationService final
    : public ytec::winpeapp::IMbr2GptDirectExecutionService {
 public:
  ytec::clonecore::Result<
      ytec::winpeapp::Mbr2GptDirectExecutionReport>
  execute(
      const ytec::winpeapp::Mbr2GptDirectExecutionRequest& request)
      override {
    ++calls;
    last_request = request;
    return ytec::clonecore::Result<
        ytec::winpeapp::Mbr2GptDirectExecutionReport>::success(report);
  }

  std::size_t calls{};
  ytec::winpeapp::Mbr2GptDirectExecutionRequest last_request;
  ytec::winpeapp::Mbr2GptDirectExecutionReport report =
      valid_execution_report();
};

void read_only_plan_accepts_only_the_narrow_supported_layout() {
  StaticInventory inventory(valid_report());
  auto plan = ytec::winpeapp::prepare_mbr2gpt_direct_operation(
      4U, 8U, inventory);
  check(plan.has_value(), "valid narrow MBR plan must pass");
  check(inventory.calls == 1U, "review must use one read-only enumeration");
  check(plan.value().clone.source_partition_style ==
            ytec::diskmodel::PartitionStyle::mbr,
        "review must retain MBR source style");
  check(plan.value().primary_partition_count == 2U,
        "review must retain checked primary count");
  check(plan.value().active_system_partition_number == 1U,
        "review must retain the unique active system partition");
  check(plan.value().clone.expected_source.disk_number == 4U &&
            plan.value().clone.expected_target.disk_number == 8U,
        "review must retain both stable identities by value");
  check(plan.value().source_layout.partitions.size() == 2U &&
            plan.value().target_layout.partitions.empty(),
        "review must retain both canonical partition layouts by value");
}

void unsupported_layouts_fail_before_the_service() {
  {
    auto report = valid_report();
    report.disks[0].partition_style =
        ytec::diskmodel::PartitionStyle::gpt;
    for (auto& partition : report.disks[0].partitions) {
      partition.style = ytec::diskmodel::PartitionStyle::gpt;
      partition.type =
          L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
    }
    StaticInventory inventory(std::move(report));
    check(!ytec::winpeapp::prepare_mbr2gpt_direct_operation(
               4U, 8U, inventory),
          "GPT source must not enter MBR-to-GPT route");
  }
  {
    auto report = valid_report();
    report.disks[0].partitions[0].bootable = false;
    StaticInventory inventory(std::move(report));
    check(!ytec::winpeapp::prepare_mbr2gpt_direct_operation(
               4U, 8U, inventory),
          "missing active NTFS system partition must fail");
  }
  {
    auto report = valid_report();
    report.disks[0].partitions.push_back(
        ytec::diskmodel::PartitionInfo{
            .number = 3U,
            .offset_bytes = 3600ULL * 1024U * 1024U,
            .size_bytes = 64ULL * 1024U * 1024U,
            .style = ytec::diskmodel::PartitionStyle::mbr,
            .type = L"0x83",
        });
    StaticInventory inventory(std::move(report));
    check(!ytec::winpeapp::prepare_mbr2gpt_direct_operation(
               4U, 8U, inventory),
          "unknown filesystem/type must fail closed");
  }
  {
    auto report = valid_report();
    report.disks[0].health.state =
        ytec::diskmodel::DiskHealthState::caution;
    StaticInventory inventory(std::move(report));
    check(!ytec::winpeapp::prepare_mbr2gpt_direct_operation(
               4U, 8U, inventory),
          "unhealthy source must use the mutually exclusive rescue route");
  }
}

void exact_ok_routes_the_retained_plan_to_only_the_mock_service() {
  StaticInventory inventory(valid_report());
  auto plan = ytec::winpeapp::prepare_mbr2gpt_direct_operation(
      4U, 8U, inventory);
  check(plan.has_value(), "fixture plan must pass");

  MockMigrationService service;
  check(!ytec::winpeapp::execute_mbr2gpt_direct_operation(
             plan.value(), true, L"ok", service),
        "lowercase token must fail");
  check(service.calls == 0U,
        "invalid confirmation must not reach the destructive seam");
  check(!ytec::winpeapp::execute_mbr2gpt_direct_operation(
             plan.value(), false, L"OK", service),
        "missing target-erasure acknowledgement must fail");
  check(service.calls == 0U,
        "missing acknowledgement must not reach the destructive seam");

  auto result = ytec::winpeapp::execute_mbr2gpt_direct_operation(
      plan.value(), true, L"OK", service);
  check(result.has_value(), "fully verified mock report must pass");
  check(service.calls == 1U, "mock service must be invoked exactly once");
  check(service.last_request.expected_source.disk_number == 4U &&
            service.last_request.expected_target.disk_number == 8U,
        "execution must use the identities retained by the reviewed plan");
  check(service.last_request.expected_source_layout ==
                plan.value().source_layout &&
            service.last_request.expected_target_layout ==
                plan.value().target_layout,
        "execution must retain both reviewed canonical layouts");
  check(service.last_request.confirmation.first_step_acknowledged &&
            service.last_request.confirmation.typed_token == L"OK",
        "execution must translate exact UI OK to target-bound confirmation");
}

void same_count_layout_changes_fail_exact_binding() {
  StaticInventory inventory(valid_report());
  auto plan = ytec::winpeapp::prepare_mbr2gpt_direct_operation(
      4U, 8U, inventory);
  check(plan.has_value(), "fixture plan must pass");

  auto reordered = valid_report();
  std::reverse(
      reordered.disks[0].partitions.begin(),
      reordered.disks[0].partitions.end());
  check(ytec::winpeapp::validate_mbr2gpt_reviewed_layouts(
            plan.value().source_layout,
            plan.value().target_layout,
            reordered.disks[0],
            reordered.disks[1])
            .has_value(),
        "enumeration order alone must not change the canonical layout");

  auto changed_source_geometry = valid_report();
  changed_source_geometry.disks[0].partitions[1].offset_bytes += 512U;
  changed_source_geometry.disks[0].partitions[1].size_bytes -= 512U;
  check(!ytec::winpeapp::validate_mbr2gpt_reviewed_layouts(
             plan.value().source_layout,
             plan.value().target_layout,
             changed_source_geometry.disks[0],
             changed_source_geometry.disks[1]),
        "same-count source geometry change must fail exact binding");

  auto target_with_partition = valid_report();
  target_with_partition.disks[1].partition_style =
      ytec::diskmodel::PartitionStyle::mbr;
  target_with_partition.disks[1].disk_identifier = L"B1C2D3E4";
  target_with_partition.disks[1].partitions.push_back(
      ytec::diskmodel::PartitionInfo{
          .number = 1U,
          .offset_bytes = 1ULL * 1024U * 1024U,
          .size_bytes = 1024ULL * 1024U * 1024U,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
      });
  StaticInventory target_inventory(target_with_partition);
  auto target_plan = ytec::winpeapp::prepare_mbr2gpt_direct_operation(
      4U, 8U, target_inventory);
  check(target_plan.has_value(), "supported non-empty target plan must pass");
  target_with_partition.disks[1].partitions[0].type = L"0x27";
  check(!ytec::winpeapp::validate_mbr2gpt_reviewed_layouts(
             target_plan.value().source_layout,
             target_plan.value().target_layout,
             target_with_partition.disks[0],
             target_with_partition.disks[1]),
        "same-count target type change must fail exact binding");
}

void tampered_reviewed_layout_never_reaches_the_service() {
  StaticInventory inventory(valid_report());
  auto plan = ytec::winpeapp::prepare_mbr2gpt_direct_operation(
      4U, 8U, inventory);
  check(plan.has_value(), "fixture plan must pass");

  MockMigrationService service;
  plan.value().source_layout.partitions[0].offset_bytes += 512U;
  check(!ytec::winpeapp::execute_mbr2gpt_direct_operation(
             plan.value(), true, L"OK", service),
        "tampered reviewed layout must fail its SHA-256 binding");
  check(service.calls == 0U,
        "tampered reviewed layout must not reach the destructive seam");
}

void malformed_reviewed_plan_never_reaches_the_service() {
  StaticInventory inventory(valid_report());
  auto plan = ytec::winpeapp::prepare_mbr2gpt_direct_operation(
      4U, 8U, inventory);
  check(plan.has_value(), "fixture plan must pass");

  MockMigrationService service;
  plan.value().primary_partition_count = 3U;
  check(!ytec::winpeapp::execute_mbr2gpt_direct_operation(
             plan.value(), true, L"OK", service),
        "tampered reviewed structure must fail closed");
  check(service.calls == 0U,
        "malformed reviewed plan must not reach the destructive seam");
}

void incomplete_or_online_final_result_never_becomes_success() {
  StaticInventory inventory(valid_report());
  auto plan = ytec::winpeapp::prepare_mbr2gpt_direct_operation(
      4U, 8U, inventory);
  check(plan.has_value(), "fixture plan must pass");

  MockMigrationService service;
  service.report.final_target_left_offline = false;
  check(!ytec::winpeapp::execute_mbr2gpt_direct_operation(
             plan.value(), true, L"OK", service),
        "online final target must never be reported as product success");

  service.report = valid_execution_report();
  service.report.source_left_read_only = false;
  check(!ytec::winpeapp::execute_mbr2gpt_direct_operation(
             plan.value(), true, L"OK", service),
        "writable final source must never be reported as product success");

  service.report = valid_execution_report();
  service.report.conversion.microsoft_signature_verified = false;
  check(!ytec::winpeapp::execute_mbr2gpt_direct_operation(
             plan.value(), true, L"OK", service),
        "unverified converter must never be reported as product success");
}

}  // namespace

int main() {
  try {
    read_only_plan_accepts_only_the_narrow_supported_layout();
    unsupported_layouts_fail_before_the_service();
    exact_ok_routes_the_retained_plan_to_only_the_mock_service();
    same_count_layout_changes_fail_exact_binding();
    tampered_reviewed_layout_never_reaches_the_service();
    malformed_reviewed_plan_never_reaches_the_service();
    incomplete_or_online_final_result_never_becomes_success();
    std::cout << "winpe mbr2gpt direct app tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "winpe mbr2gpt direct app tests: FAIL: "
              << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

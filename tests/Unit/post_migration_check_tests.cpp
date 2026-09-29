#include "ytec/windowsapp/post_migration_check.h"

#include <Windows.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef YTEC_WINDOWS_APP_MAIN_SOURCE_PATH
#error YTEC_WINDOWS_APP_MAIN_SOURCE_PATH must name the product main source
#endif

namespace {

using ytec::windowsapp::IPostMigrationReadOnlyPlatform;
using ytec::windowsapp::PostMigrationBitLockerConversionState;
using ytec::windowsapp::PostMigrationBitLockerProtectionState;
using ytec::windowsapp::PostMigrationCheckReport;
using ytec::windowsapp::PostMigrationEvidence;
using ytec::windowsapp::PostMigrationEvidenceState;
using ytec::windowsapp::PostMigrationWinReState;

void check(const bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

PostMigrationEvidence verified(std::wstring summary) {
  return PostMigrationEvidence{
      .state = PostMigrationEvidenceState::verified,
      .summary = std::move(summary),
      .detail = L"read-only verified evidence",
  };
}

PostMigrationCheckReport valid_report() {
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 3U,
      .device_path = L"\\\\.\\PhysicalDrive3",
      .model = L"Synthetic boot disk",
      .size_bytes = 256U * 1024U * 1024U,
      .sector_count = 524288U,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"Virtual",
      .serial_suffix = "1234",
      .partition_style = ytec::diskmodel::PartitionStyle::gpt,
      .disk_identifier = L"{11111111-2222-3333-4444-555555555555}",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = true,
      .partitions = {
          ytec::diskmodel::PartitionInfo{
              .number = 1U,
              .offset_bytes = 1U * 1024U * 1024U,
              .size_bytes = 32U * 1024U * 1024U,
              .style = ytec::diskmodel::PartitionStyle::gpt,
              .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
              .identifier =
                  L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}",
              .name = L"EFI",
          },
          ytec::diskmodel::PartitionInfo{
              .number = 3U,
              .offset_bytes = 64U * 1024U * 1024U,
              .size_bytes = 160U * 1024U * 1024U,
              .style = ytec::diskmodel::PartitionStyle::gpt,
              .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
              .identifier =
                  L"{BBBBBBBB-CCCC-DDDD-EEEE-FFFFFFFFFFFF}",
              .name = L"Windows",
          },
      },
  };
  disk.health.state = ytec::diskmodel::DiskHealthState::healthy;
  disk.health.smart_status_available = true;

  return PostMigrationCheckReport{
      .current_boot_disk = std::move(disk),
      .windows_partition_number = 3U,
      .system_partition_number = 1U,
      .windows_volume_name =
          L"\\\\?\\Volume{BBBBBBBB-CCCC-DDDD-EEEE-FFFFFFFFFFFF}\\",
      .windows_file_system = L"NTFS",
      .system_volume_name =
          L"\\\\?\\Volume{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}\\",
      .system_file_system = L"FAT32",
      .winre_state = PostMigrationWinReState::registered,
      .bitlocker = {
          .conversion =
              PostMigrationBitLockerConversionState::fully_encrypted,
          .protection = PostMigrationBitLockerProtectionState::on,
          .encryption_percentage = 100U,
      },
      .boot_disk_and_layout = verified(L"layout"),
      .bcd_and_boot_manager = verified(L"bcd"),
      .winre = verified(L"winre"),
      .file_system_and_disk_health = verified(L"filesystem"),
      .bitlocker_status = verified(L"BitLocker"),
      .read_only_operations_only = true,
      .preboot_success_guaranteed = false,
  };
}

class FakePlatform final : public IPostMigrationReadOnlyPlatform {
 public:
  explicit FakePlatform(PostMigrationCheckReport report)
      : report_(std::move(report)) {}

  ytec::clonecore::Result<PostMigrationCheckReport>
  inspect_read_only() override {
    ++calls;
    return ytec::clonecore::Result<PostMigrationCheckReport>::success(
        report_);
  }

  int calls{};

 private:
  PostMigrationCheckReport report_;
};

class FailingPlatform final : public IPostMigrationReadOnlyPlatform {
 public:
  ytec::clonecore::Result<PostMigrationCheckReport>
  inspect_read_only() override {
    return ytec::clonecore::Result<PostMigrationCheckReport>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::query_failed,
            .native_code = ERROR_NOT_FOUND,
            .operation = L"synthetic post-migration probe",
            .message = L"synthetic failure",
        });
  }
};

void valid_read_only_report_is_accepted_and_formatted() {
  FakePlatform platform(valid_report());
  const auto result =
      ytec::windowsapp::run_post_migration_check(platform);
  check(result.has_value(), "valid report must pass validation");
  check(platform.calls == 1, "platform must be called exactly once");
  const std::wstring text =
      ytec::windowsapp::format_post_migration_check_report(result.value());
  check(
      text.find(L"換装後の読取り専用チェック") != std::wstring::npos &&
          text.find(L"換装前の起動成功を保証しません") !=
              std::wstring::npos &&
          text.find(L"[PASS] layout") != std::wstring::npos &&
          text.find(L"BitLocker") != std::wstring::npos,
      "formatted report must retain the read-only/non-guarantee disclosure");
}

void write_capable_or_preboot_claim_is_rejected() {
  auto write_capable = valid_report();
  write_capable.read_only_operations_only = false;
  check(
      !ytec::windowsapp::validate_post_migration_check_report(
           std::move(write_capable))
           .has_value(),
      "a write-capable diagnostic must be rejected");

  auto false_guarantee = valid_report();
  false_guarantee.preboot_success_guaranteed = true;
  check(
      !ytec::windowsapp::validate_post_migration_check_report(
           std::move(false_guarantee))
           .has_value(),
      "post-boot evidence must never claim preboot success");
}

void inconsistent_verified_evidence_is_rejected() {
  auto report = valid_report();
  report.bcd_and_boot_manager.error = ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::verification_failed,
      .native_code = ERROR_INVALID_DATA,
      .operation = L"synthetic BCD",
      .message = L"synthetic mismatch",
  };
  check(
      !ytec::windowsapp::validate_post_migration_check_report(
           std::move(report))
           .has_value(),
      "VERIFIED evidence cannot also contain an error");
}

void malformed_identity_and_percentage_are_rejected() {
  auto missing_partition = valid_report();
  missing_partition.windows_partition_number = 99U;
  check(
      !ytec::windowsapp::validate_post_migration_check_report(
           std::move(missing_partition))
           .has_value(),
      "unknown Windows partition must fail closed");

  auto invalid_percentage = valid_report();
  invalid_percentage.bitlocker.encryption_percentage = 101U;
  check(
      !ytec::windowsapp::validate_post_migration_check_report(
           std::move(invalid_percentage))
           .has_value(),
      "BitLocker percentage above 100 must fail closed");
}

void platform_failure_is_propagated() {
  FailingPlatform platform;
  const auto result =
      ytec::windowsapp::run_post_migration_check(platform);
  check(!result.has_value(), "platform failure must be propagated");
  check(
      result.error().native_code == ERROR_NOT_FOUND,
      "platform failure must retain its native code");
}

void windows_factory_is_construction_only() {
  const auto platform =
      ytec::windowsapp::make_windows_post_migration_read_only_platform();
  check(platform != nullptr, "Windows platform factory must construct");
}

void product_ui_wires_only_read_only_or_synthetic_entry_points() {
  std::ifstream input(
      YTEC_WINDOWS_APP_MAIN_SOURCE_PATH, std::ios::in | std::ios::binary);
  check(input.good(), "product main source must be readable");
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string source = buffer.str();
  check(
      source.find("kPostMigrationCheckActionId") != std::string::npos &&
          source.find("start_post_migration_check(*state)") !=
              std::string::npos &&
          source.find("run_post_migration_check_with_windows_apis()") !=
              std::string::npos &&
          source.find("validate_post_migration_check_report(") !=
              std::string::npos &&
          source.find("#if defined(YTEC_UI_ACCEPTANCE_BUILD)") !=
              std::string::npos &&
          source.find("read_only_operations_only = true") !=
              std::string::npos &&
          source.find("preboot_success_guaranteed = false") !=
              std::string::npos,
      "product UI must wire the read-only production path and synthetic acceptance path");
}

}  // namespace

int main() {
  try {
    valid_read_only_report_is_accepted_and_formatted();
    write_capable_or_preboot_claim_is_rejected();
    inconsistent_verified_evidence_is_rejected();
    malformed_identity_and_percentage_are_rejected();
    platform_failure_is_propagated();
    windows_factory_is_construction_only();
    product_ui_wires_only_read_only_or_synthetic_entry_points();
    std::cout << "post migration check tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "post migration check tests: FAIL: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
}

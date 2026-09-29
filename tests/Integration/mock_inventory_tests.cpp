#include "ytec/clitools/cli_runner.h"

#include "ytec/clonecore/result.h"

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
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

class MockInventoryProvider final : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  explicit MockInventoryProvider(ytec::diskmodel::InventoryReport report)
      : report_(std::move(report)) {}

  explicit MockInventoryProvider(ytec::clonecore::Error error)
      : error_(std::move(error)), should_fail_(true) {}

  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate() override {
    ++call_count;
    if (should_fail_) {
      return ytec::clonecore::Result<ytec::diskmodel::InventoryReport>::failure(
          error_);
    }
    return ytec::clonecore::Result<ytec::diskmodel::InventoryReport>::success(
        report_);
  }

  int call_count{};

 private:
  ytec::diskmodel::InventoryReport report_;
  ytec::clonecore::Error error_;
  bool should_fail_{};
};

ytec::diskmodel::InventoryReport mock_report(const bool with_issue) {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 7;
  disk.device_path = L"\\\\.\\PhysicalDrive7";
  disk.model = L"合成テストディスク";
  disk.size_bytes = 1'000'000;
  disk.logical_sector_size = 512;
  disk.physical_sector_size = 4096;
  disk.bus_type = L"Virtual";
  disk.serial_suffix = "MOCK0007";
  disk.partition_style = ytec::diskmodel::PartitionStyle::gpt;

  ytec::diskmodel::InventoryReport report;
  report.disks.push_back(std::move(disk));
  if (with_issue) {
    report.issues.push_back(ytec::diskmodel::InventoryIssue{
        .device = L"合成テストディスク",
        .error = ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::query_failed,
            .native_code = ERROR_ACCESS_DENIED,
            .operation = L"合成クエリ",
            .message = L"アクセスが拒否されました",
        },
    });
  }
  return report;
}

void test_json_success_uses_mock_only() {
  MockInventoryProvider provider(mock_report(false));
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::clitools::run_inventory_cli(
      {L"--json"}, provider, output, errors);

  check(exit_code == 0, "A complete mock report should succeed");
  check(provider.call_count == 1, "The injected mock should be called exactly once");
  check(output.str().find("\"diskNumber\":7") != std::string::npos,
        "Mock disk data should be serialized");
  check(errors.str().empty(), "Successful JSON output should not use stderr");
}

void test_partial_diagnostics_exit_code() {
  MockInventoryProvider provider(mock_report(true));
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::clitools::run_inventory_cli(
      {L"--text"}, provider, output, errors);

  check(exit_code == 2, "Partial diagnostics should use a distinct exit code");
  check(output.str().find("未取得・警告") != std::string::npos,
        "Partial diagnostics should remain visible in text output");
}

void test_provider_failure_is_safe() {
  MockInventoryProvider provider(ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::access_denied,
      .native_code = ERROR_ACCESS_DENIED,
      .operation = L"照会専用オープン",
      .message = L"アクセスが拒否されました",
  });
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code =
      ytec::clitools::run_inventory_cli({}, provider, output, errors);

  check(exit_code == 1, "Provider failures must return failure");
  check(output.str().empty(), "Provider failures must not emit misleading data");
  check(errors.str().find("アクセスが拒否") != std::string::npos,
        "A safe failure should explain the read-only access problem");
}

void test_invalid_arguments_do_not_touch_provider() {
  MockInventoryProvider provider(mock_report(false));
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::clitools::run_inventory_cli(
      {L"--clone"}, provider, output, errors);

  check(exit_code == 64, "Unknown arguments must be rejected");
  check(provider.call_count == 0,
        "Rejected arguments must not trigger disk enumeration");
}

void test_winpe_help_declares_read_only_boundary() {
  MockInventoryProvider provider(mock_report(false));
  std::ostringstream output;
  std::ostringstream errors;
  const int exit_code = ytec::clitools::run_inventory_cli(
      {L"--help"},
      provider,
      output,
      errors,
      ytec::clitools::InventoryCliPresentation{
          .title = "Y-TEC WinPE ディスク診断（読み取り専用）",
          .executable_name = "ytec-winpe-app",
      });

  check(exit_code == 0, "WinPE help should succeed");
  check(provider.call_count == 0,
        "WinPE help must not trigger disk enumeration");
  check(output.str().find("ytec-winpe-app") != std::string::npos,
        "WinPE help should name the product executable");
  check(output.str().find("書き込み・クローン・復元機能はありません") !=
            std::string::npos,
        "WinPE help must declare the read-only product boundary");
  check(errors.str().empty(), "WinPE help should not use stderr");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"json_success_uses_mock_only", test_json_success_uses_mock_only},
      {"partial_diagnostics_exit_code", test_partial_diagnostics_exit_code},
      {"provider_failure_is_safe", test_provider_failure_is_safe},
      {"invalid_arguments_do_not_touch_provider",
       test_invalid_arguments_do_not_touch_provider},
      {"winpe_help_declares_read_only_boundary",
       test_winpe_help_declares_read_only_boundary},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }

  return failures == 0 ? 0 : 1;
}

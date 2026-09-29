#include "ytec/bootrepair/winre_diagnostic.h"

#include <Windows.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error make_error(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    const wchar_t* const operation) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = operation,
      .message = L"mock failure",
  };
}

class MockTrustVerifier final
    : public ytec::bootrepair::IExecutableTrustVerifier {
 public:
  ytec::clonecore::Status verify_microsoft_signed(
      const std::wstring& executable_path) override {
    ++calls;
    observed_path = executable_path;
    if (error.has_value()) {
      return ytec::clonecore::Status::failure(*error);
    }
    return ytec::clonecore::success_status();
  }

  int calls{};
  std::wstring observed_path;
  std::optional<ytec::clonecore::Error> error;
};

class MockProcessRunner final
    : public ytec::bootrepair::IProcessRunner {
 public:
  ytec::clonecore::Result<ytec::bootrepair::ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) override {
    ++calls;
    observed_executable = executable_path;
    observed_arguments = arguments;
    observed_working_directory = working_directory;
    if (error.has_value()) {
      return ytec::clonecore::Result<
          ytec::bootrepair::ProcessResult>::failure(*error);
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::ProcessResult>::success(result);
  }

  int calls{};
  std::wstring observed_executable;
  std::vector<std::wstring> observed_arguments;
  std::wstring observed_working_directory;
  ytec::bootrepair::ProcessResult result;
  std::optional<ytec::clonecore::Error> error;
};

class MockImageProbe final
    : public ytec::bootrepair::IWinReImageProbe {
 public:
  ytec::clonecore::Result<
      ytec::bootrepair::WinReImageObservation>
  inspect_regular_image(const std::wstring& path) override {
    observed_paths.push_back(path);
    if (error.has_value()) {
      return ytec::clonecore::Result<
          ytec::bootrepair::WinReImageObservation>::failure(
          *error);
    }
    if (next_observation < observations.size()) {
      return ytec::clonecore::Result<
          ytec::bootrepair::WinReImageObservation>::success(
          observations[next_observation++]);
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::WinReImageObservation>::success({});
  }

  std::vector<std::wstring> observed_paths;
  std::vector<ytec::bootrepair::WinReImageObservation>
      observations;
  std::size_t next_observation{};
  std::optional<ytec::clonecore::Error> error;
};

ytec::bootrepair::WinReDiagnosticRequest standard_request() {
  return ytec::bootrepair::WinReDiagnosticRequest{
      .offline_windows_directory = L"W:\\Windows",
      .trusted_system_directory = L"X:\\Windows\\System32",
      .expected_target_disk_number = 5U,
  };
}

std::string registered_output(
    const std::uint32_t disk,
    const std::uint32_t partition) {
  return "Windows RE status: Enabled\r\n"
         "Windows RE location: \\\\?\\GLOBALROOT\\device\\harddisk" +
         std::to_string(disk) + "\\partition" +
         std::to_string(partition) +
         "\\Recovery\\WindowsRE\r\n";
}

void test_arguments_are_fixed_and_paths_are_validated() {
  const auto valid =
      ytec::bootrepair::build_reagentc_info_arguments(
          L"W:/Windows/");
  check(valid.has_value(), "A normalized offline Windows path should pass");
  check(
      valid.value() ==
          std::vector<std::wstring>{
              L"/info", L"/target", L"W:\\Windows"},
      "REAgentC must receive only /info and /target");

  const auto traversal =
      ytec::bootrepair::build_reagentc_info_arguments(
          L"W:\\Windows\\..\\Other");
  check(!traversal.has_value(), "Traversal must fail closed");
  const auto relative =
      ytec::bootrepair::build_reagentc_info_arguments(
          L"Windows");
  check(!relative.has_value(), "Relative paths must fail closed");
}

void test_registered_location_parser_accepts_ascii_and_utf16le() {
  const std::string ascii = registered_output(12U, 4U);
  const auto parsed =
      ytec::bootrepair::parse_reagentc_registered_location(
          ascii);
  check(
      parsed.has_value() && parsed.value().has_value(),
      "An ASCII GLOBALROOT location should be found");
  check(
      parsed.value()->disk_number == 12U &&
          parsed.value()->partition_number == 4U &&
          parsed.value()->path_kind ==
              ytec::bootrepair::WinReRegisteredPathKind::
                  recovery_windows_re,
      "Disk and partition numbers should be parsed");

  std::string utf16le;
  utf16le.reserve(ascii.size() * 2U);
  for (const char value : ascii) {
    utf16le.push_back(value);
    utf16le.push_back('\0');
  }
  const auto wide_capture =
      ytec::bootrepair::parse_reagentc_registered_location(
          utf16le);
  check(
      wide_capture.has_value() &&
          wide_capture.value().has_value() &&
          wide_capture.value()->disk_number == 12U &&
          wide_capture.value()->partition_number == 4U,
      "UTF-16LE capture bytes should preserve the ASCII path");
}

void test_registered_location_parser_accepts_windows_fallback_path() {
  const auto parsed =
      ytec::bootrepair::parse_reagentc_registered_location(
          "Windows RE location: "
          "\\\\?\\GLOBALROOT\\device\\harddisk5\\partition2"
          "\\Windows\\System32\\Recovery\\r\\n");
  check(
      parsed.has_value() && parsed.value().has_value() &&
          parsed.value()->disk_number == 5U &&
          parsed.value()->partition_number == 2U &&
          parsed.value()->path_kind ==
              ytec::bootrepair::WinReRegisteredPathKind::
                  windows_system32_recovery,
      "REAgentC fallback registration should preserve its path kind");
}

void test_parser_rejects_malformed_or_ambiguous_locations() {
  const auto malformed =
      ytec::bootrepair::parse_reagentc_registered_location(
          "location=\\\\?\\GLOBALROOT\\device\\harddiskX"
          "\\partition4\\Recovery\\WindowsRE");
  check(!malformed.has_value(), "Malformed disk numbers must fail");

  const std::string ambiguous =
      registered_output(5U, 3U) + registered_output(5U, 4U);
  const auto multiple =
      ytec::bootrepair::parse_reagentc_registered_location(
          ambiguous);
  check(
      !multiple.has_value() &&
          multiple.error().code ==
              ytec::clonecore::ErrorCode::identity_mismatch,
      "Different registered locations must fail closed");
}

void test_registered_partition_is_verified_read_only() {
  MockTrustVerifier trust;
  MockProcessRunner runner;
  runner.result = ytec::bootrepair::ProcessResult{
      .exit_code = 0U,
      .standard_output = registered_output(5U, 3U),
  };
  MockImageProbe probe;
  probe.observations.push_back({
      .exists = true,
      .length = 700ULL * kMiB,
  });

  const auto report = ytec::bootrepair::inspect_winre_source(
      standard_request(), trust, runner, probe);
  check(report.has_value(), "A verified registered WinRE should pass");
  check(
      trust.observed_path ==
              L"X:\\Windows\\System32\\reagentc.exe" &&
          runner.observed_arguments ==
              std::vector<std::wstring>{
                  L"/info", L"/target", L"W:\\Windows"},
      "Only trusted System32 REAgentC /info /target may run");
  check(
      report.value().source_state ==
              ytec::bootrepair::WinReSourceState::
                  registered_partition &&
          report.value().registered_partition_number == 3U &&
          report.value().winre_image_size_bytes ==
              700ULL * kMiB,
      "Registered partition facts should be retained");
  check(
      report.value().microsoft_signature_verified &&
          report.value().read_only_command &&
          report.value().registered_location_matches_expected_disk &&
          report.value().registered_image_present,
      "All read-only and identity gates should be reported");
  check(
      probe.observed_paths ==
          std::vector<std::wstring>{
              L"\\\\?\\GLOBALROOT\\device\\harddisk5"
              L"\\partition3\\Recovery\\WindowsRE\\Winre.wim"},
      "Only the canonical registered Winre.wim should be probed");
}

void test_registered_disk_mismatch_stops_before_image_open() {
  MockTrustVerifier trust;
  MockProcessRunner runner;
  runner.result = ytec::bootrepair::ProcessResult{
      .exit_code = 0U,
      .standard_output = registered_output(7U, 3U),
  };
  MockImageProbe probe;

  const auto result = ytec::bootrepair::inspect_winre_source(
      standard_request(), trust, runner, probe);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::identity_mismatch,
      "A registered location on another disk must fail");
  check(
      probe.observed_paths.empty(),
      "Mismatched disk paths must not be opened");
}

void test_cloned_source_stale_mode_never_opens_foreign_path() {
  MockTrustVerifier trust;
  MockProcessRunner runner;
  runner.result = ytec::bootrepair::ProcessResult{
      .exit_code = 0U,
      .standard_output = registered_output(7U, 3U),
  };
  MockImageProbe probe;
  auto request = standard_request();
  request.allow_mismatched_registered_location_as_cloned_source_stale =
      true;

  const auto result = ytec::bootrepair::inspect_winre_source(
      request, trust, runner, probe);
  check(
      result.has_value() &&
          result.value().source_state ==
              ytec::bootrepair::WinReSourceState::registered_partition &&
          result.value().registered_location_reported &&
          result.value().registered_path_kind_reported &&
          !result.value().registered_location_matches_expected_disk &&
          result.value().
              registered_location_mismatch_classified_as_cloned_source_stale &&
          result.value().registered_partition_number == 3U &&
          !result.value().registered_image_present &&
          result.value().winre_image_size_bytes == 0U,
      "Reviewed clone mode should classify but not dereference source state");
  check(
      probe.observed_paths.empty(),
      "A foreign cloned-source GLOBALROOT path must never be opened");
}

void test_windows_fallback_image_can_fill_missing_registration() {
  MockTrustVerifier trust;
  MockProcessRunner runner;
  runner.result = ytec::bootrepair::ProcessResult{
      .exit_code = 0U,
      .standard_output = "Windows RE status: Disabled\r\n",
  };
  MockImageProbe probe;
  probe.observations.push_back({
      .exists = true,
      .length = 650ULL * kMiB,
  });

  const auto report = ytec::bootrepair::inspect_winre_source(
      standard_request(), trust, runner, probe);
  check(report.has_value(), "A verified fallback Winre.wim should pass");
  check(
      report.value().source_state ==
              ytec::bootrepair::WinReSourceState::
                  image_available_in_windows &&
          report.value().fallback_image_present &&
          report.value().winre_image_size_bytes ==
              650ULL * kMiB,
      "Fallback image state and size should be reported");
  check(
      probe.observed_paths ==
          std::vector<std::wstring>{
              L"W:\\Windows\\System32\\Recovery\\Winre.wim"},
      "Only the fixed offline Windows fallback path should be probed");
}

void test_missing_stale_and_command_failure_are_distinct() {
  MockTrustVerifier trust;
  MockProcessRunner runner;
  runner.result = ytec::bootrepair::ProcessResult{
      .exit_code = 0U,
      .standard_output = "Windows RE status: Disabled\r\n",
  };
  MockImageProbe missing_probe;
  missing_probe.observations.push_back({});
  const auto missing =
      ytec::bootrepair::inspect_winre_source(
          standard_request(), trust, runner, missing_probe);
  check(
      missing.has_value() &&
          missing.value().source_state ==
              ytec::bootrepair::WinReSourceState::missing,
      "No registration and no fallback should be reported missing");

  runner.result.standard_output = registered_output(5U, 3U);
  MockImageProbe stale_probe;
  stale_probe.observations = {{}, {}};
  const auto stale = ytec::bootrepair::inspect_winre_source(
      standard_request(), trust, runner, stale_probe);
  check(
      stale.has_value() &&
          stale.value().source_state ==
              ytec::bootrepair::WinReSourceState::unknown &&
          stale.value().registered_location_reported,
      "A stale registration without either image must remain unknown");

  runner.result = ytec::bootrepair::ProcessResult{
      .exit_code = 5U,
      .standard_error = "access denied",
  };
  MockImageProbe not_called;
  const auto failed_command =
      ytec::bootrepair::inspect_winre_source(
          standard_request(), trust, runner, not_called);
  check(
      failed_command.has_value() &&
          failed_command.value().source_state ==
              ytec::bootrepair::WinReSourceState::unknown &&
          failed_command.value().exit_code == 5U,
      "Nonzero REAgentC must be a recorded unknown diagnostic");
  check(
      not_called.observed_paths.empty(),
      "A failed REAgentC command must not enable fallback inference");
}

void test_trust_failure_prevents_process_launch() {
  MockTrustVerifier trust;
  trust.error = make_error(
      ytec::clonecore::ErrorCode::verification_failed,
      static_cast<DWORD>(TRUST_E_NOSIGNATURE),
      L"mock trust");
  MockProcessRunner runner;
  MockImageProbe probe;

  const auto result = ytec::bootrepair::inspect_winre_source(
      standard_request(), trust, runner, probe);
  check(!result.has_value(), "Untrusted REAgentC must fail");
  check(runner.calls == 0, "Trust failure must prevent process launch");
  check(
      probe.observed_paths.empty(),
      "Trust failure must prevent Winre.wim probing");
}

void test_verified_report_populates_rebuild_request() {
  ytec::bootrepair::Mbr2GptRebuildRequest request;
  const ytec::bootrepair::WinReDiagnosticReport registered{
      .exit_code = 0U,
      .source_state =
          ytec::bootrepair::WinReSourceState::
              registered_partition,
      .registered_partition_number = 3U,
      .winre_image_size_bytes = 700ULL * kMiB,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .registered_location_reported = true,
      .registered_location_matches_expected_disk = true,
      .registered_image_present = true,
  };
  const auto applied =
      ytec::bootrepair::apply_winre_diagnostic_to_rebuild_request(
          registered, request);
  check(applied.has_value(), "Verified registered WinRE should apply");
  check(
      request.winre_state ==
              ytec::bootrepair::WinReSourceState::
                  registered_partition &&
          request.registered_winre_partition_number == 3U &&
          request.winre_image_size_bytes == 700ULL * kMiB,
      "Only verified facts should reach the rebuild request");

  ytec::bootrepair::WinReDiagnosticReport unknown = registered;
  unknown.source_state =
      ytec::bootrepair::WinReSourceState::unknown;
  check(
      !ytec::bootrepair::
           apply_winre_diagnostic_to_rebuild_request(
               unknown, request)
           .has_value(),
      "Unknown WinRE diagnostics must never enable planning");
}

}  // namespace

int main() {
  try {
    test_arguments_are_fixed_and_paths_are_validated();
    test_registered_location_parser_accepts_ascii_and_utf16le();
    test_registered_location_parser_accepts_windows_fallback_path();
    test_parser_rejects_malformed_or_ambiguous_locations();
    test_registered_partition_is_verified_read_only();
    test_registered_disk_mismatch_stops_before_image_open();
    test_cloned_source_stale_mode_never_opens_foreign_path();
    test_windows_fallback_image_can_fill_missing_registration();
    test_missing_stale_and_command_failure_are_distinct();
    test_trust_failure_prevents_process_launch();
    test_verified_report_populates_rebuild_request();
    std::cout << "WinRE diagnostic tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "WinRE diagnostic tests: FAIL: "
              << error.what() << '\n';
    return 1;
  }
}

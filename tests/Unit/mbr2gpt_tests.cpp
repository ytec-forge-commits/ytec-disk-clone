#include "ytec/bootrepair/mbr2gpt.h"
#include "ytec/bootrepair/offline_windows.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
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

ytec::clonecore::Error mock_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::query_failed,
      .native_code = ERROR_INVALID_DATA,
      .operation = operation,
      .message = L"モック失敗",
  };
}

ytec::clonecore::StableDiskIdentity target_identity(
    const std::uint32_t disk_number = 7) {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = disk_number,
      .model = L"VBOX HARDDISK",
      .size_bytes = 56ULL * 1024ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512,
      .serial_suffix = "A1B2C3D4",
      .device_instance_id = L"SCSI\\DISK&VEN_VBOX&PROD_HARDDISK\\PHASE4",
      .is_system_disk = false,
  };
}

ytec::bootrepair::Mbr2GptConversionRequest valid_request() {
  const auto identity = target_identity();
  return ytec::bootrepair::Mbr2GptConversionRequest{
      .candidate_disk_number = identity.disk_number,
      .expected_target = identity,
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token =
              ytec::clonecore::make_target_confirmation_token(identity),
      },
  };
}

class MockTargetObserver final
    : public ytec::bootrepair::IMbr2GptTargetObserver {
 public:
  ytec::clonecore::Result<ytec::clonecore::StableDiskIdentity> observe_target(
      const std::uint32_t candidate_disk_number) override {
    requested_disk_numbers.push_back(candidate_disk_number);
    if (should_fail) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(
          mock_error(L"モックディスク再列挙"));
    }
    if (next_observation >= observations.size()) {
      return ytec::clonecore::Result<
          ytec::clonecore::StableDiskIdentity>::failure(
          mock_error(L"モック観測不足"));
    }
    return ytec::clonecore::Result<
        ytec::clonecore::StableDiskIdentity>::success(
        observations[next_observation++]);
  }

  std::vector<ytec::clonecore::StableDiskIdentity> observations{
      target_identity(), target_identity()};
  std::vector<std::uint32_t> requested_disk_numbers;
  std::size_t next_observation{};
  bool should_fail{};
};

class MockTrustVerifier final
    : public ytec::bootrepair::IExecutableTrustVerifier {
 public:
  ytec::clonecore::Status verify_microsoft_signed(
      const std::wstring& executable_path) override {
    ++call_count;
    received_path = executable_path;
    if (should_fail) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モック署名検証"));
    }
    return ytec::clonecore::success_status();
  }

  std::wstring received_path;
  int call_count{};
  bool should_fail{};
};

struct ProcessCall final {
  std::wstring executable_path;
  std::vector<std::wstring> arguments;
  std::wstring working_directory;
};

class MockProcessRunner final : public ytec::bootrepair::IProcessRunner {
 public:
  ytec::clonecore::Result<ytec::bootrepair::ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) override {
    calls.push_back(ProcessCall{
        .executable_path = executable_path,
        .arguments = arguments,
        .working_directory = working_directory,
    });
    const std::size_t index = calls.size() - 1;
    const std::uint32_t exit_code =
        index < exit_codes.size() ? exit_codes[index] : 0;
    return ytec::clonecore::Result<
        ytec::bootrepair::ProcessResult>::success(
        ytec::bootrepair::ProcessResult{
            .exit_code = exit_code,
            .standard_output = "mock stdout",
            .standard_error = "mock stderr",
        });
  }

  std::vector<ProcessCall> calls;
  std::vector<std::uint32_t> exit_codes;
};

std::array<std::byte, 512> make_pe_header(
    const std::uint16_t machine,
    const std::uint16_t optional_magic,
    const std::uint16_t optional_size = 0xF0) {
  std::array<std::byte, 512> image{};
  const auto set_u16 = [&](const std::size_t offset, const std::uint16_t value) {
    image[offset] = static_cast<std::byte>(value & 0xFFU);
    image[offset + 1] =
        static_cast<std::byte>((value >> 8U) & 0xFFU);
  };
  const auto set_u32 = [&](const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      image[offset + index] =
          static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
  };
  constexpr std::size_t kPeOffset = 0x80;
  set_u16(0, 0x5A4D);
  set_u32(0x3C, static_cast<std::uint32_t>(kPeOffset));
  set_u32(kPeOffset, 0x00004550);
  set_u16(kPeOffset + 4, machine);
  set_u16(kPeOffset + 20, optional_size);
  set_u16(kPeOffset + 24, optional_magic);
  return image;
}

void test_pe_architecture_accepts_only_matching_machine_and_magic() {
  const auto amd64 = ytec::bootrepair::inspect_pe_architecture(
      make_pe_header(0x8664, 0x020B));
  const auto x86 = ytec::bootrepair::inspect_pe_architecture(
      make_pe_header(0x014C, 0x010B));
  const auto arm64 = ytec::bootrepair::inspect_pe_architecture(
      make_pe_header(0xAA64, 0x020B));
  const auto mismatched = ytec::bootrepair::inspect_pe_architecture(
      make_pe_header(0x8664, 0x010B));
  check(amd64.has_value() &&
            amd64.value() == ytec::bootrepair::PeArchitecture::amd64,
        "AMD64 PE32+ kernel must be accepted as x64");
  check(x86.has_value() &&
            x86.value() == ytec::bootrepair::PeArchitecture::x86,
        "I386 PE32 kernel must be classified as x86");
  check(arm64.has_value() &&
            arm64.value() == ytec::bootrepair::PeArchitecture::arm64,
        "ARM64 kernel must remain unsupported but identifiable");
  check(mismatched.has_value() &&
            mismatched.value() == ytec::bootrepair::PeArchitecture::unknown,
        "Mismatched machine and optional-header magic must not pass");
}

void test_pe_architecture_rejects_truncated_and_invalid_headers() {
  std::array<std::byte, 64> truncated{};
  const auto missing_mz =
      ytec::bootrepair::inspect_pe_architecture(truncated);
  check(!missing_mz.has_value(), "Missing MZ signature must fail");

  auto out_of_range = make_pe_header(0x8664, 0x020B);
  out_of_range[0x3C] = std::byte{0xF0};
  out_of_range[0x3D] = std::byte{0xFF};
  const auto bad_offset =
      ytec::bootrepair::inspect_pe_architecture(out_of_range);
  check(!bad_offset.has_value(), "Out-of-range PE offset must fail");

  const auto oversized_optional = ytec::bootrepair::inspect_pe_architecture(
      make_pe_header(0x8664, 0x020B, 0xFFFF));
  check(!oversized_optional.has_value(),
        "Optional header extending past the prefix must fail");
}

void test_arguments_are_fixed_and_winpe_only() {
  const auto validate = ytec::bootrepair::build_mbr2gpt_arguments(
      ytec::bootrepair::Mbr2GptAction::validate, 7);
  const auto convert = ytec::bootrepair::build_mbr2gpt_arguments(
      ytec::bootrepair::Mbr2GptAction::convert, 7);
  check(validate.has_value() && convert.has_value(),
        "Known actions must build arguments");
  check(validate.value().size() == 2,
        "Validate must use exactly two arguments");
  check(validate.value()[0] == L"/validate", "Validate action must be fixed");
  check(validate.value()[1] == L"/disk:7", "Disk number must be numeric argv");
  check(convert.value()[0] == L"/convert", "Convert action must be fixed");
  for (const auto& argument : validate.value()) {
    check(argument != L"/allowFullOS", "Full Windows execution stays disabled");
    check(!argument.starts_with(L"/map:"), "Custom partition maps stay disabled");
  }
  for (const auto& argument : convert.value()) {
    check(argument != L"/allowFullOS", "Full Windows conversion stays disabled");
    check(!argument.starts_with(L"/map:"), "Custom partition maps stay disabled");
  }
  const auto unknown = ytec::bootrepair::build_mbr2gpt_arguments(
      static_cast<ytec::bootrepair::Mbr2GptAction>(255), 7);
  check(!unknown.has_value(), "Unknown action must fail closed");
}

void test_success_reidentifies_between_validate_and_convert() {
  MockTargetObserver observer;
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  const auto result = ytec::bootrepair::execute_mbr2gpt_conversion(
      valid_request(),
      L"X:\\Windows\\System32",
      observer,
      verifier,
      runner);
  check(result.has_value(), "Successful mock conversion should pass");
  check(observer.requested_disk_numbers.size() == 2,
        "Target must be freshly observed twice");
  check(verifier.call_count == 1, "Executable signature must be checked");
  check(runner.calls.size() == 2, "Validate must precede convert");
  check(runner.calls[0].arguments[0] == L"/validate",
        "First process must be read-only validation");
  check(runner.calls[1].arguments[0] == L"/convert",
        "Second process may convert only after all gates");
  check(verifier.received_path == L"X:\\Windows\\System32\\mbr2gpt.exe",
        "PATH lookup must not be used");
  check(runner.calls[0].executable_path == verifier.received_path &&
            runner.calls[1].executable_path == verifier.received_path,
        "Only the verified executable may run");
  check(result.value().target_reidentified_before_conversion,
        "Audit report must record final reidentification");
}

void test_wrong_confirmation_stops_before_signature_and_process() {
  auto request = valid_request();
  request.confirmation.typed_token = L"WRONG";
  MockTargetObserver observer;
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  const auto result = ytec::bootrepair::execute_mbr2gpt_conversion(
      request, L"X:\\Windows\\System32", observer, verifier, runner);
  check(!result.has_value(), "Wrong confirmation must fail closed");
  check(result.error().code ==
            ytec::clonecore::ErrorCode::confirmation_required,
        "Confirmation failure must be explicit");
  check(verifier.call_count == 0, "Trust work starts only after confirmation");
  check(runner.calls.empty(), "No Microsoft process may start");
}

void test_signature_failure_stops_validation() {
  MockTargetObserver observer;
  MockTrustVerifier verifier;
  verifier.should_fail = true;
  MockProcessRunner runner;
  const auto result = ytec::bootrepair::execute_mbr2gpt_conversion(
      valid_request(),
      L"X:\\Windows\\System32",
      observer,
      verifier,
      runner);
  check(!result.has_value(), "Untrusted MBR2GPT must fail");
  check(runner.calls.empty(), "Untrusted executable must never run");
}

void test_validation_failure_never_runs_convert() {
  MockTargetObserver observer;
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  runner.exit_codes = {5};
  const auto result = ytec::bootrepair::execute_mbr2gpt_conversion(
      valid_request(),
      L"X:\\Windows\\System32",
      observer,
      verifier,
      runner);
  check(!result.has_value(), "Failed official validation must fail");
  check(runner.calls.size() == 1, "Convert must not run after failed validate");
  check(observer.requested_disk_numbers.size() == 1,
        "No destructive-stage observation is needed after failed validate");
}

void test_identity_change_after_validation_stops_convert() {
  MockTargetObserver observer;
  auto replaced = target_identity();
  replaced.serial_suffix = "REPLACED";
  observer.observations[1] = replaced;
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  const auto result = ytec::bootrepair::execute_mbr2gpt_conversion(
      valid_request(),
      L"X:\\Windows\\System32",
      observer,
      verifier,
      runner);
  check(!result.has_value(), "Disk replacement must fail closed");
  check(result.error().code == ytec::clonecore::ErrorCode::identity_mismatch,
        "Replacement must be reported as identity mismatch");
  check(runner.calls.size() == 1, "Convert must not run on a replaced disk");
}

void test_disk_number_change_after_validation_stops_convert() {
  MockTargetObserver observer;
  observer.observations[1].disk_number = 8;
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  const auto result = ytec::bootrepair::execute_mbr2gpt_conversion(
      valid_request(),
      L"X:\\Windows\\System32",
      observer,
      verifier,
      runner);
  check(!result.has_value(), "Disk renumbering must restart the workflow");
  check(runner.calls.size() == 1, "Validated disk number must not be reused");
}

void test_nonzero_convert_is_never_success() {
  MockTargetObserver observer;
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  runner.exit_codes = {0, 9};
  const auto result = ytec::bootrepair::execute_mbr2gpt_conversion(
      valid_request(),
      L"X:\\Windows\\System32",
      observer,
      verifier,
      runner);
  check(!result.has_value(), "Failed conversion must not report success");
  check(runner.calls.size() == 2, "Both process attempts should be audited");
  check(result.error().code ==
            ytec::clonecore::ErrorCode::verification_failed,
        "Nonzero conversion exit is a verification failure");
}

void test_invalid_system_path_stops_before_observation() {
  MockTargetObserver observer;
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  const auto result = ytec::bootrepair::execute_mbr2gpt_conversion(
      valid_request(),
      L"X:\\Windows\\System32\\..\\Temp",
      observer,
      verifier,
      runner);
  check(!result.has_value(), "Untrusted system directory must fail");
  check(observer.requested_disk_numbers.empty(),
        "Invalid static request stops before disk observation");
  check(verifier.call_count == 0 && runner.calls.empty(),
        "Invalid path must not reach trust or process boundaries");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"pe_architecture_accepts_only_matching_machine_and_magic",
       test_pe_architecture_accepts_only_matching_machine_and_magic},
      {"pe_architecture_rejects_truncated_and_invalid_headers",
       test_pe_architecture_rejects_truncated_and_invalid_headers},
      {"arguments_are_fixed_and_winpe_only",
       test_arguments_are_fixed_and_winpe_only},
      {"success_reidentifies_between_validate_and_convert",
       test_success_reidentifies_between_validate_and_convert},
      {"wrong_confirmation_stops_before_signature_and_process",
       test_wrong_confirmation_stops_before_signature_and_process},
      {"signature_failure_stops_validation",
       test_signature_failure_stops_validation},
      {"validation_failure_never_runs_convert",
       test_validation_failure_never_runs_convert},
      {"identity_change_after_validation_stops_convert",
       test_identity_change_after_validation_stops_convert},
      {"disk_number_change_after_validation_stops_convert",
       test_disk_number_change_after_validation_stops_convert},
      {"nonzero_convert_is_never_success",
       test_nonzero_convert_is_never_success},
      {"invalid_system_path_stops_before_observation",
       test_invalid_system_path_stops_before_observation},
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

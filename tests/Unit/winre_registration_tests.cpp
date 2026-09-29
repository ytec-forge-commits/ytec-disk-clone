#include "ytec/bootrepair/winre_registration.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::bootrepair::IExecutableTrustVerifier;
using ytec::bootrepair::IProcessRunner;
using ytec::bootrepair::IWinReDiagnosticService;
using ytec::bootrepair::IWinReRegistrationImageLock;
using ytec::bootrepair::IWinReRegistrationImageLocker;
using ytec::bootrepair::IWinReRegistrationTargetGuard;
using ytec::bootrepair::ProcessResult;
using ytec::bootrepair::WinReDiagnosticReport;
using ytec::bootrepair::WinReRegistrationImageIdentity;
using ytec::bootrepair::WinReRegistrationOutcome;
using ytec::bootrepair::WinReRegistrationRequest;
using ytec::clonecore::Error;
using ytec::clonecore::ErrorCode;
using ytec::clonecore::Result;
using ytec::clonecore::Status;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Error mock_error(
    const ErrorCode code,
    const DWORD native_code,
    const wchar_t* const operation) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = operation,
      .message = L"synthetic failure",
  };
}

WinReRegistrationImageIdentity image_identity(
    const std::wstring& requested_path,
    const std::uint64_t length,
    const std::byte seed) {
  WinReRegistrationImageIdentity identity{
      .requested_path = requested_path,
      .opened_final_path = L"\\\\?\\" + requested_path,
      .volume_serial_number = 0x12345678ULL,
      .length = length,
      .last_write_time = 111U,
      .change_time = 222U,
  };
  identity.file_id.fill(seed);
  identity.sha256.fill(static_cast<std::byte>(
      static_cast<unsigned char>(seed) ^ 0x5AU));
  return identity;
}

WinReDiagnosticReport missing_diagnostic() {
  return WinReDiagnosticReport{
      .exit_code = 0U,
      .source_state = ytec::bootrepair::WinReSourceState::missing,
      .microsoft_signature_verified = true,
      .read_only_command = true,
  };
}

WinReDiagnosticReport fallback_diagnostic(const std::uint64_t length) {
  return WinReDiagnosticReport{
      .exit_code = 0U,
      .source_state =
          ytec::bootrepair::WinReSourceState::image_available_in_windows,
      .winre_image_size_bytes = length,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .fallback_image_present = true,
  };
}

WinReDiagnosticReport registered_diagnostic(
    const std::uint32_t partition,
    const std::uint64_t length,
    const ytec::bootrepair::WinReRegisteredPathKind path_kind =
        ytec::bootrepair::WinReRegisteredPathKind::recovery_windows_re) {
  return WinReDiagnosticReport{
      .exit_code = 0U,
      .source_state =
          ytec::bootrepair::WinReSourceState::registered_partition,
      .registered_partition_number = partition,
      .registered_path_kind = path_kind,
      .winre_image_size_bytes = length,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .registered_location_reported = true,
      .registered_path_kind_reported = true,
      .registered_location_matches_expected_disk = true,
      .registered_image_present = true,
  };
}

WinReDiagnosticReport cloned_source_stale_diagnostic(
    const std::uint32_t source_partition) {
  return WinReDiagnosticReport{
      .exit_code = 0U,
      .source_state =
          ytec::bootrepair::WinReSourceState::registered_partition,
      .registered_partition_number = source_partition,
      .microsoft_signature_verified = true,
      .read_only_command = true,
      .registered_location_reported = true,
      .registered_path_kind_reported = true,
      .registered_location_matches_expected_disk = false,
      .registered_location_mismatch_classified_as_cloned_source_stale =
          true,
  };
}

WinReRegistrationRequest standard_request() {
  constexpr std::uint64_t kCandidateLength = 650ULL * kMiB;
  return WinReRegistrationRequest{
      .intent =
          ytec::bootrepair::WinReRegistrationIntent::
              register_verified_image,
      .offline_windows_directory = L"W:\\Windows",
      .trusted_system_directory = L"X:\\Windows\\System32",
      .candidate_directory = L"R:\\Recovery\\WindowsRE",
      .expected_target_disk_number = 5U,
      .expected_target_partition_number = 4U,
      .reviewed_candidate = image_identity(
          L"R:\\Recovery\\WindowsRE\\Winre.wim",
          kCandidateLength,
          std::byte{0x11}),
      .prior_diagnostic = missing_diagnostic(),
  };
}

class MockTrust final : public IExecutableTrustVerifier {
 public:
  Status verify_microsoft_signed(
      const std::wstring& executable_path) override {
    ++calls;
    path = executable_path;
    if (error.has_value()) {
      return Status::failure(*error);
    }
    return ytec::clonecore::success_status();
  }

  std::uint32_t calls{};
  std::wstring path;
  std::optional<Error> error;
};

struct ProcessInvocation final {
  std::wstring executable;
  std::vector<std::wstring> arguments;
  std::wstring working_directory;
};

class MockProcess final : public IProcessRunner {
 public:
  Result<ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) override {
    invocations.push_back({executable_path, arguments, working_directory});
    if (next >= results.size()) {
      return Result<ProcessResult>::success(ProcessResult{});
    }
    const auto& result = results[next++];
    if (result.error.has_value()) {
      return Result<ProcessResult>::failure(*result.error);
    }
    return Result<ProcessResult>::success(result.process);
  }

  struct NextResult final {
    ProcessResult process;
    std::optional<Error> error;
  };

  std::vector<ProcessInvocation> invocations;
  std::vector<NextResult> results;
  std::size_t next{};
};

class MockImageLock final : public IWinReRegistrationImageLock {
 public:
  explicit MockImageLock(WinReRegistrationImageIdentity identity)
      : identity_(std::move(identity)) {}

  const WinReRegistrationImageIdentity& identity()
      const noexcept override {
    return identity_;
  }

 private:
  WinReRegistrationImageIdentity identity_;
};

class MockLocker final : public IWinReRegistrationImageLocker {
 public:
  Result<std::unique_ptr<IWinReRegistrationImageLock>>
  lock_regular_image(const std::wstring& path) override {
    paths.push_back(path);
    const std::size_t index = paths.size() - 1U;
    if (index < errors.size() && errors[index].has_value()) {
      return Result<std::unique_ptr<IWinReRegistrationImageLock>>::failure(
          *errors[index]);
    }
    if (index >= identities.size()) {
      return Result<std::unique_ptr<IWinReRegistrationImageLock>>::failure(
          mock_error(ErrorCode::internal_error, ERROR_INVALID_DATA,
                     L"mock locker exhausted"));
    }
    return Result<std::unique_ptr<IWinReRegistrationImageLock>>::success(
        std::make_unique<MockImageLock>(identities[index]));
  }

  std::vector<std::wstring> paths;
  std::vector<WinReRegistrationImageIdentity> identities;
  std::vector<std::optional<Error>> errors;
};

class MockGuard final : public IWinReRegistrationTargetGuard {
 public:
  Status revalidate_target() override {
    const std::size_t index = calls++;
    if (index < errors.size() && errors[index].has_value()) {
      return Status::failure(*errors[index]);
    }
    return ytec::clonecore::success_status();
  }

  std::size_t calls{};
  std::vector<std::optional<Error>> errors;
};

class MockDiagnostic final : public IWinReDiagnosticService {
 public:
  Result<WinReDiagnosticReport> inspect(
      const std::wstring& offline_windows_directory,
      const std::uint32_t expected_target_disk_number) override {
    windows_paths.push_back(offline_windows_directory);
    disk_numbers.push_back(expected_target_disk_number);
    const std::size_t index = windows_paths.size() - 1U;
    if (index < errors.size() && errors[index].has_value()) {
      return Result<WinReDiagnosticReport>::failure(*errors[index]);
    }
    if (index >= reports.size()) {
      return Result<WinReDiagnosticReport>::failure(
          mock_error(ErrorCode::internal_error, ERROR_INVALID_DATA,
                     L"mock diagnostic exhausted"));
    }
    return Result<WinReDiagnosticReport>::success(reports[index]);
  }

  std::vector<std::wstring> windows_paths;
  std::vector<std::uint32_t> disk_numbers;
  std::vector<WinReDiagnosticReport> reports;
  std::vector<std::optional<Error>> errors;
};

struct Fixture final {
  Fixture() {
    request = standard_request();
    locker.identities.push_back(*request.reviewed_candidate);
    diagnostic.reports.push_back(registered_diagnostic(
        request.expected_target_partition_number,
        request.reviewed_candidate->length));
  }

  WinReRegistrationRequest request;
  MockTrust trust;
  MockProcess process;
  MockLocker locker;
  MockGuard guard;
  MockDiagnostic diagnostic;
};

void test_argument_builders_are_fixed_and_validate_paths() {
  const auto set =
      ytec::bootrepair::build_reagentc_setreimage_arguments(
          L"R:/Recovery/WindowsRE/", L"W:/Windows/");
  check(set.has_value(), "Valid setreimage arguments should pass");
  check(
      set.value() ==
          std::vector<std::wstring>{
              L"/setreimage", L"/path", L"R:\\Recovery\\WindowsRE",
              L"/target", L"W:\\Windows"},
      "Setreimage arguments must be fixed and normalized");

  const auto enable =
      ytec::bootrepair::build_reagentc_enable_arguments(
          L"W:\\Windows");
  const auto disable =
      ytec::bootrepair::build_reagentc_disable_arguments(
          L"W:\\Windows");
  check(
      enable.has_value() &&
          enable.value() == std::vector<std::wstring>{
              L"/enable", L"/target", L"W:\\Windows"},
      "Enable arguments must be fixed");
  check(
      disable.has_value() &&
          disable.value() == std::vector<std::wstring>{
              L"/disable", L"/target", L"W:\\Windows"},
      "Disable arguments must be fixed");
  check(
      !ytec::bootrepair::build_reagentc_setreimage_arguments(
           L"R:\\Recovery\\..\\Other", L"W:\\Windows")
           .has_value(),
      "Traversal must fail before any process launch");
  check(
      !ytec::bootrepair::build_reagentc_enable_arguments(
           L"W:\\Other")
           .has_value(),
      "Only an offline Windows directory may be targeted");
}

void test_partial_outcome_performs_zero_io() {
  Fixture fixture;
  fixture.request = {};
  fixture.request.intent =
      ytec::bootrepair::WinReRegistrationIntent::
          normal_boot_only_partial;
  const auto result =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(
      result.has_value() &&
          result.value().outcome ==
              WinReRegistrationOutcome::normal_boot_only_partial,
      "Unknown WinRE should become an explicit partial result");
  check(
      fixture.trust.calls == 0U && fixture.process.invocations.empty() &&
          fixture.locker.paths.empty() && fixture.guard.calls == 0U &&
          fixture.diagnostic.windows_paths.empty(),
      "Partial repair must perform zero WinRE I/O");

  fixture.request.candidate_directory = L"R:\\Recovery\\WindowsRE";
  const auto mixed =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(
      !mixed.has_value(),
      "Partial repair must reject hidden registration inputs");
}

void test_success_locks_identity_and_verifies_registration() {
  Fixture fixture;
  const auto result =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(result.has_value(), "A fully verified registration should return");
  const auto& report = result.value();
  check(
      report.outcome == WinReRegistrationOutcome::completed &&
          report.candidate_locked &&
          !report.prior_candidate_locked &&
          report.reagentc_signature_verified &&
          report.set_reimage_completed && report.enable_completed &&
          report.registration_verified &&
          !report.rollback_attempted &&
          report.target_revalidation_count == 3U,
      "Success must contain every mandatory verification fact");
  check(
      fixture.locker.paths ==
          std::vector<std::wstring>{
              L"R:\\Recovery\\WindowsRE\\Winre.wim"},
      "Only the reviewed candidate should be locked");
  check(
      fixture.trust.calls == 1U &&
          fixture.trust.path ==
              L"X:\\Windows\\System32\\reagentc.exe",
      "Only trusted System32 REAgentC may execute");
  check(
      fixture.process.invocations.size() == 2U &&
          fixture.process.invocations[0].arguments ==
              std::vector<std::wstring>{
                  L"/setreimage", L"/path",
                  L"R:\\Recovery\\WindowsRE", L"/target",
                  L"W:\\Windows"} &&
          fixture.process.invocations[1].arguments ==
              std::vector<std::wstring>{
                  L"/enable", L"/target", L"W:\\Windows"},
      "Registration must use only fixed setreimage and enable commands");
  check(
      fixture.diagnostic.windows_paths ==
              std::vector<std::wstring>{L"W:\\Windows"} &&
          fixture.diagnostic.disk_numbers ==
              std::vector<std::uint32_t>{5U},
      "Commit verification must diagnose the exact target disk");
}

void test_expected_registered_path_kind_is_immutable() {
  Fixture windows_fallback;
  windows_fallback.request.candidate_directory =
      L"W:\\Windows\\System32\\Recovery";
  windows_fallback.request.expected_target_partition_number = 2U;
  windows_fallback.request.expected_registered_path_kind =
      ytec::bootrepair::WinReRegisteredPathKind::windows_system32_recovery;
  windows_fallback.request.reviewed_candidate = image_identity(
      L"W:\\Windows\\System32\\Recovery\\Winre.wim",
      650ULL * kMiB,
      std::byte{0x11});
  windows_fallback.locker.identities[0] =
      *windows_fallback.request.reviewed_candidate;
  windows_fallback.diagnostic.reports[0] = registered_diagnostic(
      2U,
      windows_fallback.request.reviewed_candidate->length,
      ytec::bootrepair::WinReRegisteredPathKind::windows_system32_recovery);

  const auto completed =
      ytec::bootrepair::execute_winre_registration_transaction(
          windows_fallback.request, windows_fallback.trust,
          windows_fallback.process, windows_fallback.locker,
          windows_fallback.guard, windows_fallback.diagnostic);
  check(
      completed.has_value() &&
          completed.value().outcome == WinReRegistrationOutcome::completed &&
          completed.value().registration_verified,
      "A reviewed Windows fallback path kind should complete");

  Fixture mismatched;
  mismatched.request.candidate_directory =
      L"W:\\Windows\\System32\\Recovery";
  mismatched.request.expected_target_partition_number = 2U;
  mismatched.request.expected_registered_path_kind =
      ytec::bootrepair::WinReRegisteredPathKind::windows_system32_recovery;
  mismatched.request.reviewed_candidate = image_identity(
      L"W:\\Windows\\System32\\Recovery\\Winre.wim",
      650ULL * kMiB,
      std::byte{0x11});
  mismatched.locker.identities[0] = *mismatched.request.reviewed_candidate;
  mismatched.diagnostic.reports[0] = registered_diagnostic(
      2U, mismatched.request.reviewed_candidate->length);
  mismatched.diagnostic.reports.push_back(missing_diagnostic());
  const auto rejected =
      ytec::bootrepair::execute_winre_registration_transaction(
          mismatched.request, mismatched.trust, mismatched.process,
          mismatched.locker, mismatched.guard, mismatched.diagnostic);
  check(
      rejected.has_value() &&
          rejected.value().outcome ==
              WinReRegistrationOutcome::failed_rolled_back &&
          rejected.value().rollback_verified &&
          !rejected.value().registration_verified,
      "A final diagnostic with a different path kind must roll back safely");

  Fixture invalid_shape;
  invalid_shape.request.expected_registered_path_kind =
      ytec::bootrepair::WinReRegisteredPathKind::windows_system32_recovery;
  const auto preflight_rejected =
      ytec::bootrepair::execute_winre_registration_transaction(
          invalid_shape.request, invalid_shape.trust,
          invalid_shape.process, invalid_shape.locker,
          invalid_shape.guard, invalid_shape.diagnostic);
  check(
      !preflight_rejected.has_value() &&
          invalid_shape.locker.paths.empty() &&
          invalid_shape.process.invocations.empty(),
      "A candidate directory that contradicts its reviewed path kind must stop before I/O");
}

void test_validation_trust_and_identity_fail_before_mutation() {
  Fixture invalid;
  invalid.request.prior_diagnostic.source_state =
      ytec::bootrepair::WinReSourceState::unknown;
  const auto unknown =
      ytec::bootrepair::execute_winre_registration_transaction(
          invalid.request, invalid.trust, invalid.process,
          invalid.locker, invalid.guard, invalid.diagnostic);
  check(
      !unknown.has_value() && invalid.locker.paths.empty() &&
          invalid.process.invocations.empty(),
      "Unknown prior state must stop before candidate I/O");

  Fixture mismatch;
  mismatch.locker.identities[0].sha256[0] = std::byte{0xEE};
  const auto changed =
      ytec::bootrepair::execute_winre_registration_transaction(
          mismatch.request, mismatch.trust, mismatch.process,
          mismatch.locker, mismatch.guard, mismatch.diagnostic);
  check(
      !changed.has_value() && mismatch.trust.calls == 0U &&
          mismatch.process.invocations.empty() &&
          mismatch.guard.calls == 0U,
      "Changed SHA-256 must stop before trust, target, or process I/O");

  Fixture untrusted;
  untrusted.trust.error = mock_error(
      ErrorCode::verification_failed,
      static_cast<DWORD>(TRUST_E_NOSIGNATURE),
      L"synthetic unsigned reagentc");
  const auto rejected =
      ytec::bootrepair::execute_winre_registration_transaction(
          untrusted.request, untrusted.trust, untrusted.process,
          untrusted.locker, untrusted.guard, untrusted.diagnostic);
  check(
      !rejected.has_value() && untrusted.guard.calls == 0U &&
          untrusted.process.invocations.empty(),
      "Untrusted REAgentC must stop before target mutation");
}

void test_set_failure_restores_verified_prior_registration() {
  constexpr std::uint64_t kOldLength = 700ULL * kMiB;
  Fixture fixture;
  fixture.request.prior_diagnostic = registered_diagnostic(3U, kOldLength);
  fixture.request.rollback_candidate_directory =
      L"Q:\\Recovery\\WindowsRE";
  fixture.request.reviewed_rollback_image = image_identity(
      L"Q:\\Recovery\\WindowsRE\\Winre.wim",
      kOldLength,
      std::byte{0x22});
  fixture.locker.identities.push_back(
      *fixture.request.reviewed_rollback_image);
  fixture.process.results = {
      {.process = {.exit_code = 5U}},
      {.process = {.exit_code = 0U}},
      {.process = {.exit_code = 0U}},
  };
  fixture.diagnostic.reports[0] = registered_diagnostic(3U, kOldLength);

  const auto result =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(result.has_value(), "Operational failure should return a report");
  const auto& report = result.value();
  check(
      report.outcome == WinReRegistrationOutcome::failed_rolled_back &&
          report.candidate_locked && report.prior_candidate_locked &&
          report.rollback_attempted && report.rollback_completed &&
          report.rollback_verified &&
          report.primary_failure.has_value() &&
          !report.rollback_failure.has_value(),
      "A setreimage failure must restore and verify the prior registration");
  check(
      fixture.process.invocations.size() == 3U &&
          fixture.process.invocations[1].arguments ==
              std::vector<std::wstring>{
                  L"/setreimage", L"/path",
                  L"Q:\\Recovery\\WindowsRE", L"/target",
                  L"W:\\Windows"} &&
          fixture.process.invocations[2].arguments.front() == L"/enable",
      "Rollback must restore the reviewed prior path then enable it");
  check(
      fixture.guard.calls == 4U,
      "Mutation, rollback commands, and rollback diagnostic need fresh guards");
}

void test_enable_failure_disables_previously_unregistered_winre() {
  Fixture fixture;
  fixture.request.prior_diagnostic = fallback_diagnostic(500ULL * kMiB);
  fixture.process.results = {
      {.process = {.exit_code = 0U}},
      {.process = {.exit_code = 9U}},
      {.process = {.exit_code = 0U}},
  };
  fixture.diagnostic.reports[0] = fixture.request.prior_diagnostic;

  const auto result =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(
      result.has_value() &&
          result.value().outcome ==
              WinReRegistrationOutcome::failed_rolled_back &&
          result.value().set_reimage_completed &&
          !result.value().enable_completed &&
          result.value().rollback_verified,
      "Enable failure must return to the reviewed disabled state");
  check(
      fixture.process.invocations.size() == 3U &&
          fixture.process.invocations[2].arguments ==
              std::vector<std::wstring>{
                  L"/disable", L"/target", L"W:\\Windows"},
      "A previously unregistered state must roll back with fixed disable");
}

void test_failed_commit_verification_rolls_back() {
  Fixture fixture;
  fixture.diagnostic.reports = {
      registered_diagnostic(8U, fixture.request.reviewed_candidate->length),
      missing_diagnostic(),
  };
  const auto result =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(
      result.has_value() &&
          result.value().outcome ==
              WinReRegistrationOutcome::failed_rolled_back &&
          result.value().set_reimage_completed &&
          result.value().enable_completed &&
          !result.value().registration_verified &&
          result.value().primary_failure.has_value() &&
          result.value().rollback_verified,
      "A mismatched final partition must trigger verified rollback");
  check(
      fixture.process.invocations.size() == 3U &&
          fixture.process.invocations.back().arguments.front() == L"/disable",
      "Final verification failure must undo the new registration");
}

void test_identity_loss_after_mutation_stops_additional_writes() {
  Fixture fixture;
  fixture.guard.errors = {
      std::nullopt,
      mock_error(ErrorCode::identity_mismatch,
                 ERROR_DEVICE_NOT_CONNECTED,
                 L"synthetic target swap"),
  };
  const auto result =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(
      result.has_value() &&
          result.value().outcome ==
              WinReRegistrationOutcome::failed_rollback_incomplete &&
          result.value().set_reimage_completed &&
          !result.value().enable_completed &&
          result.value().rollback_attempted &&
          !result.value().rollback_completed &&
          result.value().rollback_failure.has_value(),
      "Lost target identity must be reported as incomplete rollback");
  check(
      fixture.process.invocations.size() == 1U &&
          fixture.diagnostic.windows_paths.empty(),
      "Target identity loss must prevent every later process and diagnostic");
}

void test_rollback_failure_is_distinct() {
  Fixture fixture;
  fixture.process.results = {
      {.process = {.exit_code = 0U}},
      {.process = {.exit_code = 13U}},
      {.process = {.exit_code = 14U}},
  };
  const auto result =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(
      result.has_value() &&
          result.value().outcome ==
              WinReRegistrationOutcome::failed_rollback_incomplete &&
          result.value().primary_failure.has_value() &&
          result.value().rollback_failure.has_value() &&
          !result.value().rollback_completed &&
          !result.value().rollback_verified,
      "Rollback command failure must never be reported as restored");
  check(
      fixture.diagnostic.windows_paths.empty(),
      "An incomplete rollback must not fabricate diagnostic success");
}

void test_cloned_source_stale_failure_becomes_safe_unregistered() {
  Fixture fixture;
  fixture.request.prior_state_origin =
      ytec::bootrepair::WinReRegistrationPriorStateOrigin::
          cloned_source_stale;
  fixture.request.prior_diagnostic =
      cloned_source_stale_diagnostic(6U);
  fixture.process.results = {
      {.process = {.exit_code = 0U}},
      {.process = {.exit_code = 17U}},
      {.process = {.exit_code = 0U}},
  };
  fixture.diagnostic.reports[0] = missing_diagnostic();

  const auto result =
      ytec::bootrepair::execute_winre_registration_transaction(
          fixture.request, fixture.trust, fixture.process,
          fixture.locker, fixture.guard, fixture.diagnostic);
  check(
      result.has_value() &&
          result.value().outcome ==
              WinReRegistrationOutcome::failed_safe_unregistered &&
          result.value().cloned_source_registration_disabled &&
          result.value().rollback_completed &&
          result.value().rollback_verified &&
          !result.value().prior_candidate_locked,
      "A cloned source record must fail into a verified unregistered state");
  check(
      fixture.process.invocations.size() == 3U &&
          fixture.process.invocations.back().arguments ==
              std::vector<std::wstring>{
                  L"/disable", L"/target", L"W:\\Windows"},
      "Cloned source state must never be restored from the source path");

  Fixture invalid;
  invalid.request.prior_state_origin =
      ytec::bootrepair::WinReRegistrationPriorStateOrigin::
          cloned_source_stale;
  invalid.request.prior_diagnostic =
      registered_diagnostic(4U, 650ULL * kMiB);
  const auto same_target =
      ytec::bootrepair::execute_winre_registration_transaction(
          invalid.request, invalid.trust, invalid.process,
          invalid.locker, invalid.guard, invalid.diagnostic);
  check(
      !same_target.has_value() && invalid.locker.paths.empty() &&
          invalid.process.invocations.empty(),
      "A target-local registration must not masquerade as cloned stale state");
}

void test_identity_equivalence_binds_every_field() {
  auto first = image_identity(
      L"R:\\Recovery\\WindowsRE\\Winre.wim",
      650ULL * kMiB,
      std::byte{0x33});
  auto same = first;
  check(
      ytec::bootrepair::equivalent_winre_registration_image_identity(
          first, same),
      "Identical evidence should compare equal");
  same.change_time += 1U;
  check(
      !ytec::bootrepair::equivalent_winre_registration_image_identity(
          first, same),
      "Change time must be part of immutable evidence");
  same = first;
  same.sha256[31] ^= std::byte{0x01};
  check(
      !ytec::bootrepair::equivalent_winre_registration_image_identity(
          first, same),
      "Complete SHA-256 must be part of immutable evidence");
}

}  // namespace

int main() {
  try {
    test_argument_builders_are_fixed_and_validate_paths();
    test_partial_outcome_performs_zero_io();
    test_success_locks_identity_and_verifies_registration();
    test_expected_registered_path_kind_is_immutable();
    test_validation_trust_and_identity_fail_before_mutation();
    test_set_failure_restores_verified_prior_registration();
    test_enable_failure_disables_previously_unregistered_winre();
    test_failed_commit_verification_rolls_back();
    test_identity_loss_after_mutation_stops_additional_writes();
    test_rollback_failure_is_distinct();
    test_cloned_source_stale_failure_becomes_safe_unregistered();
    test_identity_equivalence_binds_every_field();
    std::cout << "WinRE registration tests: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "WinRE registration tests: FAIL: "
              << error.what() << '\n';
    return 1;
  }
}

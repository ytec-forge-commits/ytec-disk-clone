#include "ytec/windowsapp/startup_data_policy.h"

#include "ytec/clonecore/error.h"

#include <Windows.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Status failure(
    const std::wstring& operation,
    const DWORD native_code = ERROR_ACCESS_DENIED) {
  return ytec::clonecore::Status::failure({
      .code = ytec::clonecore::ErrorCode::access_denied,
      .native_code = native_code,
      .operation = operation,
      .message = L"synthetic failure",
  });
}

class SyntheticPlatform final
    : public ytec::windowsapp::IStartupDataPlatform {
 public:
  bool executable_available{true};
  std::wstring executable{L"C:\\Portable\\TsumugiDrive.exe"};
  unsigned int fail_directory_check{};
  bool creation_succeeds{true};
  bool probe_succeeds{true};
  bool throw_during_probe{};
  unsigned int directory_check_count{};
  std::vector<std::wstring> calls;

  ytec::clonecore::Result<std::wstring> executable_path() override {
    calls.push_back(L"executable");
    if (!executable_available) {
      return ytec::clonecore::Result<std::wstring>::failure({
          .code = ytec::clonecore::ErrorCode::query_failed,
          .native_code = ERROR_FILE_NOT_FOUND,
          .operation = L"synthetic executable",
          .message = L"synthetic failure",
      });
    }
    return ytec::clonecore::Result<std::wstring>::success(executable);
  }

  ytec::clonecore::Status require_regular_non_reparse_directory(
      const std::wstring& path) override {
    ++directory_check_count;
    calls.push_back(L"directory:" + path);
    if (fail_directory_check == directory_check_count) {
      return failure(L"synthetic directory", ERROR_REPARSE_TAG_INVALID);
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status create_directory_if_missing(
      const std::wstring& path) override {
    calls.push_back(L"create:" + path);
    return creation_succeeds
               ? ytec::clonecore::success_status()
               : failure(L"synthetic create");
  }

  ytec::clonecore::Status verify_directory_write_access(
      const std::wstring& path) override {
    calls.push_back(L"probe:" + path);
    if (throw_during_probe) {
      throw std::runtime_error("synthetic exception");
    }
    return probe_succeeds
               ? ytec::clonecore::success_status()
               : failure(L"synthetic probe");
  }
};

void writable_adjacent_data_is_the_only_write_capable_result() {
  SyntheticPlatform platform;
  const auto policy =
      ytec::windowsapp::evaluate_startup_data_policy(platform);

  check(policy.write_operations_permitted(),
        "A fully verified adjacent data directory should permit writes");
  check(!policy.diagnostic_only(),
        "A fully verified adjacent data directory is not diagnostic-only");
  check(policy.persistent_logging_permitted(),
        "The full probe is the only persistent logging result");
  check(policy.data_directory == L"C:\\Portable\\data",
        "The policy must select only the EXE-adjacent data directory");
  check(platform.calls.size() == 6U,
        "The successful policy must run both safety checks and the probe");
  check(platform.calls[1] == L"directory:C:\\Portable" &&
            platform.calls[2] == L"create:C:\\Portable\\data" &&
            platform.calls[3] == L"directory:C:\\Portable\\data" &&
            platform.calls[4] == L"probe:C:\\Portable\\data" &&
            platform.calls[5] == L"directory:C:\\Portable\\data",
        "The app directory must be checked before adjacent data creation");
}

void bootstrap_is_bounded_ram_and_performs_zero_platform_calls() {
  SyntheticPlatform untouched_platform;
  const auto policy =
      ytec::windowsapp::make_read_only_bootstrap_data_policy();

  check(policy.write_operations_permitted() &&
            policy.intentionally_ram_isolated() &&
            !policy.persistent_logging_permitted() &&
            !policy.diagnostic_only(),
        "Read-only startup must permit operations while retaining logs in RAM");
  check(untouched_platform.calls.empty(),
        "Read-only bootstrap must perform zero filesystem platform calls");
}

void backing_relationship_requires_complete_disjoint_proof() {
  const std::vector<std::uint32_t> protected_disks{3U, 7U};
  check(
      ytec::windowsapp::classify_startup_data_backing(
          3U, protected_disks) ==
          ytec::windowsapp::StartupDataBackingRelationship::protected_disk,
      "The source/target backing disk must force RAM isolation");
  check(
      ytec::windowsapp::classify_startup_data_backing(
          9U, protected_disks) ==
          ytec::windowsapp::StartupDataBackingRelationship::disjoint,
      "A different stabilized backing disk may enable persistent logging");
  check(
      ytec::windowsapp::classify_startup_data_backing(
          std::nullopt, protected_disks) ==
          ytec::windowsapp::StartupDataBackingRelationship::unknown,
      "An unknown backing disk must remain in RAM");
  check(
      ytec::windowsapp::classify_startup_data_backing(
          9U, std::span<const std::uint32_t>{}) ==
          ytec::windowsapp::StartupDataBackingRelationship::unknown,
      "An incomplete protected disk set must remain in RAM");
}

void tsumugi_destination_gate_blocks_the_complete_data_tree() {
  using ytec::windowsapp::evaluate_tsumugi_portable_data_path_gate;
  const std::wstring_view executable =
      L"C:\\Portable\\TsumugiDrive.exe";
  const std::vector<std::wstring> blocked{
      L"C:\\Portable\\data\\backup.tsumugi",
      L"c:\\portable\\DATA\\sub\\BACKUP.TSUMUGI",
      L"C:\\Portable\\images\\..\\data\\backup.tsumugi",
      L"C:/Portable/data/./sub/../backup.tsumugi",
      L"C:\\Portable\\data2\\..\\data\\backup.tsumugi",
  };
  for (const auto& path : blocked) {
    const auto result =
        evaluate_tsumugi_portable_data_path_gate(executable, path);
    check(!result.has_value() &&
              result.error().native_code == ERROR_ACCESS_DENIED,
          "Every spelling of data or a data child must be rejected");
  }
}

void tsumugi_destination_gate_keeps_component_boundaries() {
  using ytec::windowsapp::evaluate_tsumugi_portable_data_path_gate;
  const auto sibling = evaluate_tsumugi_portable_data_path_gate(
      L"c:/Portable/./bin/../TsumugiDrive.exe",
      L"C:\\Portable\\data2\\backup.TSUMUGI");
  check(sibling.has_value() &&
            sibling.value().canonical_data_directory ==
                L"C:\\Portable\\data" &&
            sibling.value().canonical_final_path ==
                L"C:\\Portable\\data2\\backup.TSUMUGI" &&
            sibling.value().canonical_partial_path ==
                L"C:\\Portable\\data2\\backup.TSUMUGI.partial",
        "A data2 sibling must not be confused with the owned data tree");

  const auto outside = evaluate_tsumugi_portable_data_path_gate(
      L"C:\\Portable\\TsumugiDrive.exe",
      L"D:\\Images\\.\\archive.tsumugi");
  check(outside.has_value() &&
            outside.value().canonical_final_path ==
                L"D:\\Images\\archive.tsumugi",
        "A normalized absolute path on another local drive should pass");
}

void tsumugi_destination_gate_rejects_ambiguous_or_invalid_paths() {
  using ytec::windowsapp::evaluate_tsumugi_portable_data_path_gate;
  const std::wstring_view executable =
      L"C:\\Portable\\TsumugiDrive.exe";
  const std::vector<std::wstring> invalid{
      L"backup.tsumugi",
      L"C:backup.tsumugi",
      L"\\\\server\\share\\backup.tsumugi",
      L"\\\\?\\C:\\Images\\backup.tsumugi",
      L"C:\\..\\backup.tsumugi",
      L"C:\\Images\\\\backup.tsumugi",
      L"C:\\Images\\backup.tsumugi\\",
      L"C:\\Images\\backup.tsumugi.",
      L"C:\\Images\\backup.tsumugi ",
      L"C:\\Images\\backup:stream.tsumugi",
      L"C:\\Images\\CON.tsumugi",
      L"C:\\Images\\backup.img",
  };
  for (const auto& path : invalid) {
    check(!evaluate_tsumugi_portable_data_path_gate(
               executable, path)
               .has_value(),
          "Invalid, ambiguous, non-local, or non-.tsumugi paths must fail closed");
  }

  std::wstring embedded_nul = L"C:\\Images\\good.tsumugi";
  embedded_nul.insert(10U, 1U, L'\0');
  check(!evaluate_tsumugi_portable_data_path_gate(
             executable,
             std::wstring_view(embedded_nul.data(), embedded_nul.size()))
             .has_value(),
        "An embedded NUL must fail closed");

  check(!evaluate_tsumugi_portable_data_path_gate(
             L"TsumugiDrive.exe", L"D:\\Images\\backup.tsumugi")
             .has_value(),
        "A relative executable path must make the central gate fail closed");
}

void unavailable_or_relative_executable_path_fails_closed() {
  SyntheticPlatform unavailable;
  unavailable.executable_available = false;
  const auto missing =
      ytec::windowsapp::evaluate_startup_data_policy(unavailable);
  check(missing.diagnostic_only() &&
            missing.issue == ytec::windowsapp::StartupDataIssue::
                                 executable_path_unavailable,
        "An unavailable executable path must be diagnostic-only");

  SyntheticPlatform relative;
  relative.executable = L"TsumugiDrive.exe";
  const auto invalid =
      ytec::windowsapp::evaluate_startup_data_policy(relative);
  check(invalid.diagnostic_only() &&
            invalid.issue == ytec::windowsapp::StartupDataIssue::
                                 executable_path_invalid &&
            relative.calls.size() == 1U,
        "A relative executable path must stop before filesystem mutation");
}

void application_or_data_reparse_observation_fails_closed() {
  SyntheticPlatform application_reparse;
  application_reparse.fail_directory_check = 1U;
  const auto application =
      ytec::windowsapp::evaluate_startup_data_policy(application_reparse);
  check(application.diagnostic_only() &&
            application.issue == ytec::windowsapp::StartupDataIssue::
                                     application_directory_unsafe &&
            application_reparse.calls.size() == 2U,
        "An unsafe application directory must stop before data creation");

  SyntheticPlatform data_reparse;
  data_reparse.fail_directory_check = 2U;
  const auto data =
      ytec::windowsapp::evaluate_startup_data_policy(data_reparse);
  check(data.diagnostic_only() &&
            data.issue ==
                ytec::windowsapp::StartupDataIssue::data_directory_unsafe &&
            data_reparse.calls.size() == 4U,
        "A reparse data directory must stop before the write probe");

  SyntheticPlatform replaced_after_probe;
  replaced_after_probe.fail_directory_check = 3U;
  const auto replaced =
      ytec::windowsapp::evaluate_startup_data_policy(replaced_after_probe);
  check(replaced.diagnostic_only() &&
            replaced.issue ==
                ytec::windowsapp::StartupDataIssue::data_directory_unsafe &&
            replaced_after_probe.calls.size() == 6U,
        "A data directory changed after probing must still fail closed");
}

void create_or_write_failure_has_no_fallback() {
  SyntheticPlatform create_denied;
  create_denied.creation_succeeds = false;
  const auto create =
      ytec::windowsapp::evaluate_startup_data_policy(create_denied);
  check(create.diagnostic_only() &&
            create.issue == ytec::windowsapp::StartupDataIssue::
                                data_directory_unavailable &&
            create.data_directory == L"C:\\Portable\\data" &&
            create_denied.calls.size() == 3U,
        "Data creation denial must not try another storage location");

  SyntheticPlatform probe_denied;
  probe_denied.probe_succeeds = false;
  const auto probe =
      ytec::windowsapp::evaluate_startup_data_policy(probe_denied);
  check(probe.diagnostic_only() &&
            probe.issue ==
                ytec::windowsapp::StartupDataIssue::write_probe_failed &&
            probe_denied.calls.size() == 5U,
        "Write/readback/delete probe failure must be diagnostic-only");
  for (const auto& call : probe_denied.calls) {
    check(call.find(L"AppData") == std::wstring::npos,
          "The startup policy must never probe AppData");
  }
}

void unexpected_platform_failure_fails_closed() {
  SyntheticPlatform platform;
  platform.throw_during_probe = true;
  const auto policy =
      ytec::windowsapp::evaluate_startup_data_policy(platform);
  check(policy.diagnostic_only() &&
            policy.issue ==
                ytec::windowsapp::StartupDataIssue::unexpected_failure,
        "Unexpected platform failures must be converted to diagnostic-only");
}

}  // namespace

int main() {
  try {
    writable_adjacent_data_is_the_only_write_capable_result();
    bootstrap_is_bounded_ram_and_performs_zero_platform_calls();
    backing_relationship_requires_complete_disjoint_proof();
    tsumugi_destination_gate_blocks_the_complete_data_tree();
    tsumugi_destination_gate_keeps_component_boundaries();
    tsumugi_destination_gate_rejects_ambiguous_or_invalid_paths();
    unavailable_or_relative_executable_path_fails_closed();
    application_or_data_reparse_observation_fails_closed();
    create_or_write_failure_has_no_fallback();
    unexpected_platform_failure_fails_closed();
    std::cout << "startup data policy tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "startup data policy tests: FAIL: "
              << exception.what() << '\n';
    return 1;
  }
}

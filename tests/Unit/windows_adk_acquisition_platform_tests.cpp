#include "ytec/windowsapp/windows_adk_acquisition_platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::windowsapp::AdkInstallerKind;
using ytec::windowsapp::AdkOfflineStageRequest;
using ytec::windowsapp::AdkPayloadKind;
using ytec::windowsapp::AdkReleaseManifest;
using ytec::windowsapp::AdkSilentInstallRequest;
using ytec::windowsapp::AdkVerifiedPayload;
using ytec::windowsapp::IWindowsAdkProcessLauncher;
using ytec::windowsapp::WindowsAdkAcquisitionPlatform;
using ytec::windowsapp::WindowsAdkInstalledObservation;
using ytec::windowsapp::WindowsAdkLaunchPlan;
using ytec::windowsapp::WindowsAdkPrelaunchObservation;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

std::string repeated_hash(const char character) {
  return std::string(64U, character);
}

class NeverLaunch final : public IWindowsAdkProcessLauncher {
 public:
  explicit NeverLaunch(int* calls) : calls_(calls) {}

  ytec::clonecore::Result<std::uint32_t> launch_and_wait(
      const WindowsAdkLaunchPlan&) override {
    ++*calls_;
    return ytec::clonecore::Result<std::uint32_t>::failure(
        ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::internal_error,
            .native_code = ERROR_INVALID_FUNCTION,
            .operation = L"test launcher",
            .message = L"A unit test must never launch an installer",
        });
  }

 private:
  int* calls_{};
};

AdkSilentInstallRequest make_launch_request(
    const AdkInstallerKind installer_kind) {
  const bool patch_installer = installer_kind ==
                               AdkInstallerKind::windows_installer_patch_msp;
  const AdkPayloadKind payload_kind =
      installer_kind == AdkInstallerKind::microsoft_bootstrap_exe
          ? AdkPayloadKind::deployment_tools
          : AdkPayloadKind::servicing_update;
  std::wstring extension = L".exe";
  if (installer_kind == AdkInstallerKind::windows_update_msu) {
    extension = L".msu";
  } else if (installer_kind ==
             AdkInstallerKind::windows_installer_patch_msp) {
    extension = L".msp";
  }
  return AdkSilentInstallRequest{
      .payload = AdkVerifiedPayload{
          .kind = payload_kind,
          .installer_kind = installer_kind,
          .staged_path = std::filesystem::path(L"C:\\staging") /
                         (L"payload" + extension),
          .byte_count = 123U,
          .sha256 = repeated_hash('A'),
          .signer_subject = patch_installer
                                ? L"CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"
                                : L"Microsoft Corporation",
          .payload_version = patch_installer ? L"" : L"10.1.26100.1",
          .msp_revision_guid = patch_installer
                                   ? L"{11111111-2222-3333-4444-555555555555}"
                                   : L"",
      },
      .fixed_arguments =
          ytec::windowsapp::fixed_silent_install_arguments(
              payload_kind, installer_kind),
  };
}

WindowsAdkPrelaunchObservation valid_observation(
    const AdkSilentInstallRequest& request) {
  return WindowsAdkPrelaunchObservation{
      .owned_staged_identity_matches = true,
      .payload_regular_non_reparse = true,
      .payload_single_link = true,
      .payload_byte_count = request.payload.byte_count,
      .payload_sha256 = request.payload.sha256,
      .payload_signature_trusted = true,
      .payload_signer_subject = request.payload.signer_subject,
      .payload_version = request.payload.payload_version,
      .msp_revision_guid = request.payload.msp_revision_guid,
      .system_handler_regular_non_reparse = true,
      .system_handler_signature_trusted = true,
  };
}

void test_download_policy_is_exact_ordered_https_allowlist() {
  ytec::windowsapp::AdkDownloadRequest request{
      .exact_source_url =
          L"https://go.microsoft.com/fwlink/?linkid=123",
      .exact_allowed_urls = {
          L"https://go.microsoft.com/fwlink/?linkid=123",
          L"https://download.microsoft.com/download/a/payload.exe",
      },
      .create_new_destination = L"C:\\stage\\payload.exe",
      .maximum_bytes = 1024U,
  };
  const auto valid =
      ytec::windowsapp::validate_windows_adk_download_policy(request);
  check(valid.has_value(), "Pinned Microsoft HTTPS chain should pass");
  check(
      valid.value().ordered_exact_urls == request.exact_allowed_urls,
      "Policy must preserve the exact redirect order");

  request.exact_allowed_urls[1] = L"https://example.com/payload.exe";
  check(
      !ytec::windowsapp::validate_windows_adk_download_policy(request),
      "A non-Microsoft redirect must fail closed");
  request.exact_allowed_urls[1] = request.exact_allowed_urls[0];
  check(
      !ytec::windowsapp::validate_windows_adk_download_policy(request),
      "A duplicate redirect must fail closed");
  request.exact_allowed_urls = {request.exact_source_url};
  request.create_new_destination = L"\\\\server\\share\\payload.exe";
  check(
      !ytec::windowsapp::validate_windows_adk_download_policy(request),
      "A UNC destination must fail before network access");
  request.create_new_destination = L"C:\\stage\\NUL.exe";
  check(
      !ytec::windowsapp::validate_windows_adk_download_policy(request),
      "A reserved DOS device name must fail before CREATE_NEW");
  request.create_new_destination = L"C:\\stage\\payload.exe:stream";
  check(
      !ytec::windowsapp::validate_windows_adk_download_policy(request),
      "An alternate data stream destination must fail before CREATE_NEW");
}

void test_offline_scope_is_local_relative_and_owned() {
  AdkOfflineStageRequest request{
      .layout_root = L"C:\\adk-layout",
      .exact_relative_path = L"installers\\payload.exe",
      .create_new_destination = L"C:\\owned-stage\\payload.exe",
      .maximum_bytes = 1024U,
  };
  check(
      ytec::windowsapp::validate_windows_adk_offline_stage_scope(
          request, L"C:\\owned-stage")
          .has_value(),
      "A local relative source and direct owned destination should pass");

  request.exact_relative_path = L"..\\payload.exe";
  check(
      !ytec::windowsapp::validate_windows_adk_offline_stage_scope(
          request, L"C:\\owned-stage"),
      "Relative traversal must fail closed");
  request.exact_relative_path = L"payload.exe";
  request.layout_root = L"\\\\server\\layout";
  check(
      !ytec::windowsapp::validate_windows_adk_offline_stage_scope(
          request, L"C:\\owned-stage"),
      "UNC offline layouts must fail closed");
  request.layout_root = L"C:\\adk-layout";
  request.create_new_destination = L"C:\\other\\payload.exe";
  check(
      !ytec::windowsapp::validate_windows_adk_offline_stage_scope(
          request, L"C:\\owned-stage"),
      "A destination outside the owned stage must fail closed");
}

void test_installed_observation_never_invents_servicing() {
  AdkReleaseManifest manifest{};
  manifest.required_servicing_update_id = L"KB5101684";
  WindowsAdkInstalledObservation observation{
      .deployment_tools_present = true,
      .winpe_addon_present = true,
      .servicing_update_present = false,
      .microsoft_binaries_trusted = true,
      .deployment_tools_version = L"10.1.1.1",
      .winpe_addon_version = L"10.1.1.1",
      .serviced_dism_version = L"10.0.1.1",
  };
  auto state = ytec::windowsapp::evaluate_windows_adk_installed_observation(
      manifest, observation);
  check(
      state.servicing_update_id.empty(),
      "An unobserved servicing update must not receive the manifest ID");
  observation.servicing_update_present = true;
  state = ytec::windowsapp::evaluate_windows_adk_installed_observation(
      manifest, observation);
  check(
      state.servicing_update_id == L"KB5101684",
      "An independently observed update may receive the fixed ID");
}

void test_shell_free_launch_plans_are_fixed() {
  const std::filesystem::path system_directory = L"C:\\Windows\\System32";
  {
    const auto request = make_launch_request(
        AdkInstallerKind::microsoft_bootstrap_exe);
    const auto plan =
        ytec::windowsapp::validate_and_build_windows_adk_launch_plan(
            request, system_directory, valid_observation(request));
    check(plan.has_value(), "A fully reverified bootstrap should pass");
    check(
        !plan.value().uses_shell &&
            plan.value().executable_path == request.payload.staged_path &&
            plan.value().arguments == request.fixed_arguments,
        "Bootstrap must execute the exact staged EXE without a shell");
  }
  {
    const auto request = make_launch_request(
        AdkInstallerKind::windows_update_msu);
    const auto plan =
        ytec::windowsapp::validate_and_build_windows_adk_launch_plan(
            request, system_directory, valid_observation(request));
    check(plan.has_value(), "A fully reverified MSU should pass");
    check(
        !plan.value().uses_shell &&
            plan.value().executable_path ==
                system_directory / L"wusa.exe" &&
            plan.value().arguments.front() ==
                request.payload.staged_path.native(),
        "MSU must use absolute System32 wusa.exe and the exact payload");
  }
  {
    const auto request = make_launch_request(
        AdkInstallerKind::windows_installer_patch_msp);
    const auto plan =
        ytec::windowsapp::validate_and_build_windows_adk_launch_plan(
            request, system_directory, valid_observation(request));
    check(plan.has_value(), "A fully reverified MSP should pass");
    check(
        !plan.value().uses_shell &&
            plan.value().executable_path ==
                system_directory / L"msiexec.exe" &&
            plan.value().arguments.size() >= 2U &&
            plan.value().arguments[0] == L"/p" &&
            plan.value().arguments[1] ==
                request.payload.staged_path.native(),
        "MSP must use absolute System32 msiexec.exe /p without a shell");
  }
}

void test_prelaunch_tamper_and_argument_changes_fail_closed() {
  auto request = make_launch_request(
      AdkInstallerKind::microsoft_bootstrap_exe);
  auto observation = valid_observation(request);
  observation.payload_sha256 = repeated_hash('B');
  check(
      !ytec::windowsapp::validate_and_build_windows_adk_launch_plan(
          request, L"C:\\Windows\\System32", observation),
      "A changed prelaunch hash must fail closed");

  observation = valid_observation(request);
  observation.payload_single_link = false;
  check(
      !ytec::windowsapp::validate_and_build_windows_adk_launch_plan(
          request, L"C:\\Windows\\System32", observation),
      "A hard-linked staged file must fail closed");

  observation = valid_observation(request);
  request.fixed_arguments.push_back(L"/unapproved");
  check(
      !ytec::windowsapp::validate_and_build_windows_adk_launch_plan(
          request, L"C:\\Windows\\System32", observation),
      "An unapproved installer argument must fail closed");
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto base = std::filesystem::temp_directory_path();
    for (unsigned int attempt = 0; attempt < 100U; ++attempt) {
      path_ = base /
              (L"ytec-adk-platform-test-" +
               std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
    }
    throw TestFailure{"Could not create isolated temporary layout"};
  }

  ~TemporaryDirectory() {
    if (!path_.empty()) {
      std::error_code ignored;
      (void)std::filesystem::remove_all(path_, ignored);
    }
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

void write_three_bytes(const std::filesystem::path& path) {
  const HANDLE handle = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw TestFailure{"Could not create synthetic offline payload"};
  }
  const char bytes[] = {'a', 'b', 'c'};
  DWORD written{};
  const BOOL success = WriteFile(
      handle, bytes, static_cast<DWORD>(sizeof(bytes)), &written, nullptr);
  CloseHandle(handle);
  if (!success || written != sizeof(bytes)) {
    throw TestFailure{"Could not write synthetic offline payload"};
  }
}

void test_real_staging_offline_copy_hash_and_cleanup_are_safe() {
  TemporaryDirectory layout;
  const auto source = layout.path() / L"payload.exe";
  write_three_bytes(source);

  int launch_calls{};
  WindowsAdkAcquisitionPlatform platform(
      std::make_unique<NeverLaunch>(&launch_calls));
  const auto staging = platform.create_new_staging_area(1024U * 1024U);
  check(staging.has_value(), "A new local non-reparse stage should be made");
  check(
      staging.value().created_new && !staging.value().reparse_point,
      "Stage receipt must attest CREATE_NEW and non-reparse");

  const AdkOfflineStageRequest stage_request{
      .layout_root = layout.path(),
      .exact_relative_path = L"payload.exe",
      .create_new_destination = staging.value().root / L"payload.exe",
      .maximum_bytes = 1024U,
  };
  const auto path_scope =
      ytec::windowsapp::validate_windows_adk_offline_stage_scope(
          stage_request, staging.value().root);
  check(path_scope.has_value(), "Runtime offline path scope should pass");
  const auto staged = platform.stage_offline_payload(stage_request);
  if (!staged) {
    std::cerr << "stage native error: " << staged.error().native_code
              << '\n';
  }
  check(staged.has_value(), "A bounded local regular file should stage");
  check(
      staged.value().created_new && staged.value().byte_count == 3U &&
          staged.value().source_regular_file &&
          !staged.value().source_reparse_point,
      "Offline receipt must preserve the bounded identity facts");
  check(
      !platform.stage_offline_payload(stage_request),
      "The same destination must never be overwritten");

  const auto hash = platform.sha256_file(staged.value().staged_path, 1024U);
  check(hash.has_value(), "The owned staged file should hash");
  check(
      hash.value() ==
          "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",
      "The staged bytes must have the known SHA-256");
  check(
      !platform.verify_authenticode(
          staged.value().staged_path, L"Microsoft Corporation"),
      "An unsigned synthetic file must fail Authenticode verification");
  check(launch_calls == 0, "No installer may run in the unit test");

  const auto unowned = staging.value().root / L"unowned.txt";
  write_three_bytes(unowned);
  const auto refused_cleanup =
      platform.remove_staging_area(staging.value());
  check(
      !refused_cleanup,
      "Cleanup must fail closed when an unowned file appears");
  check(
      GetFileAttributesW(unowned.c_str()) != INVALID_FILE_ATTRIBUTES,
      "Cleanup must preserve the unowned file");
  check(
      DeleteFileW(unowned.c_str()) != FALSE,
      "The test should remove only its own synthetic unowned file");

  const auto removed = platform.remove_staging_area(staging.value());
  check(removed.has_value(), "The exact owned stage should clean up");
  check(
      GetFileAttributesW(staging.value().root.c_str()) ==
          INVALID_FILE_ATTRIBUTES,
      "The owned leaf stage should no longer exist");
  check(launch_calls == 0, "Cleanup must not invoke a process");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"download_policy_is_exact_ordered_https_allowlist",
       test_download_policy_is_exact_ordered_https_allowlist},
      {"offline_scope_is_local_relative_and_owned",
       test_offline_scope_is_local_relative_and_owned},
      {"installed_observation_never_invents_servicing",
       test_installed_observation_never_invents_servicing},
      {"shell_free_launch_plans_are_fixed",
       test_shell_free_launch_plans_are_fixed},
      {"prelaunch_tamper_and_argument_changes_fail_closed",
       test_prelaunch_tamper_and_argument_changes_fail_closed},
      {"real_staging_offline_copy_hash_and_cleanup_are_safe",
       test_real_staging_offline_copy_hash_and_cleanup_are_safe},
  };

  int failures{};
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name
                << ": unexpected exception: " << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

#include "ytec/windowsapp/media_creation.h"
#include "ytec/windowsapp/media_wizard.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

ytec::windowsapp::MediaPreflightView ready_preflight(
    const bool bootex = true) {
  return ytec::windowsapp::MediaPreflightView{
      .base_layout_ready = true,
      .bootex_layout_ready = bootex,
      .media_creation_permitted = true,
  };
}

ytec::diskmodel::DiskInfo safe_usb() {
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 7U;
  disk.device_instance_id = L"USB\\VID_1234&PID_5678\\MOCK";
  disk.model = L"Mock USB Memory";
  disk.size_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 4096U;
  disk.bus_type = L"USB";
  disk.serial_suffix = "A1B2C3D4";
  disk.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = true;
  disk.is_system_disk = false;
  disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
      .number = 1U,
      .offset_bytes = 1024U * 1024U,
      .size_bytes = disk.size_bytes - 1024U * 1024U,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
  });
  return disk;
}

ytec::windowsapp::RescueUsbStoragePlan
reviewed_initialization_plan(
    const ytec::diskmodel::DiskInfo& disk,
    const ytec::windowsapp::RescueUsbDataFileSystem file_system =
        ytec::windowsapp::RescueUsbDataFileSystem::ntfs) {
  const auto plan = ytec::windowsapp::plan_rescue_usb_storage({
      .target = &disk,
      .mode = ytec::windowsapp::
          RescueUsbProvisioningMode::initialize_all,
      .data_file_system = file_system,
  });
  check(plan.has_value(), "Mock USB storage plan should be valid");
  return plan.value();
}

class MockIsoExecutor final
    : public ytec::windowsapp::IRescueMediaIsoExecutor {
 public:
  ytec::clonecore::Result<
      ytec::windowsapp::RescueMediaCreationReport>
  execute(
      const ytec::windowsapp::RescueMediaIsoExecutionRequest&
          request) override {
    ++calls;
    observed = request;
    if (custom) {
      return custom(request);
    }
    return ytec::clonecore::Result<
        ytec::windowsapp::RescueMediaCreationReport>::success({
        .final_iso_path = request.final_iso_path,
        .manifest_path =
            request.final_iso_path + L".manifest.json",
        .retained_work_root = request.work_root,
        .iso_length = 400ULL * 1024ULL * 1024ULL,
        .iso_sha256 = std::string(64U, 'A'),
        .complete_iso_verified = true,
        .published_without_overwrite = true,
    });
  }

  int calls{};
  ytec::windowsapp::RescueMediaIsoExecutionRequest observed;
  std::function<ytec::clonecore::Result<
      ytec::windowsapp::RescueMediaCreationReport>(
      const ytec::windowsapp::RescueMediaIsoExecutionRequest&)>
      custom;
};

class MockUsbExecutor final
    : public ytec::windowsapp::IRescueMediaUsbExecutor {
 public:
  ytec::clonecore::Result<
      ytec::windowsapp::RescueMediaCreationReport>
  execute(
      const ytec::windowsapp::RescueMediaUsbExecutionRequest&
          request) override {
    ++calls;
    observed = request;
    if (custom) {
      return custom(request);
    }
    return ytec::clonecore::Result<
        ytec::windowsapp::RescueMediaCreationReport>::success({
        .manifest_path =
            request.work_root + L"\\usb-media-manifest.json",
        .retained_work_root = request.work_root,
        .usb_root_path = request.mapping.root_path,
        .usb_boot_wim_sha256 = std::string(64U, 'B'),
        .usb_layout_verified = true,
        .usb_boot_staged_without_overwrite = true,
        .usb_data_preserved = false,
        .complete_usb_verified = true,
    });
  }

  int calls{};
  ytec::windowsapp::RescueMediaUsbExecutionRequest observed;
  std::function<ytec::clonecore::Result<
      ytec::windowsapp::RescueMediaCreationReport>(
      const ytec::windowsapp::RescueMediaUsbExecutionRequest&)>
      custom;
};

ytec::windowsapp::RescueUsbTargetAuthorization
usb_authorization() {
  const auto disk = safe_usb();
  const auto identity =
      ytec::diskmodel::make_stable_disk_identity(
          disk, false);
  check(identity.has_value(), "Mock USB identity should be stable");
  const auto storage_plan = reviewed_initialization_plan(disk);
  return {
      .target = identity.value(),
      .confirmation_token = L"OK",
      .partition_count = 1U,
      .storage_plan = storage_plan,
      .physical_write_started = false,
  };
}

ytec::windowsapp::RescueUsbDriveLetterResolution
usb_mapping(const bool after_write = false) {
  const auto authorization = usb_authorization();
  return {
      .target_identity = authorization.target,
      .drive_letter = L'R',
      .root_path = L"R:\\",
      .partition_number = after_write ? 1U : 0U,
      .extent_start = after_write ? 1024U * 1024U : 0U,
      .extent_length = after_write
          ? ytec::windowsapp::kRescueUsbBootPartitionBytes
          : 0U,
      .drive_letter_was_unassigned = !after_write,
      .physical_write_started = false,
  };
}

ytec::windowsapp::ProductMediaPayloadPaths product_payload() {
  return {
      .builder_script = L"C:\\Product\\tools\\Build.ps1",
      .environment_diagnostic =
          L"C:\\Product\\tools\\environment.exe",
      .winpe_cli = L"C:\\Product\\winpe\\cli.exe",
      .winpe_gui = L"C:\\Product\\winpe\\gui.exe",
      .powershell = L"C:\\Windows\\System32\\powershell.exe",
      .package_root = L"C:\\Product",
  };
}

ytec::windowsapp::RescueMediaCreationDependencies
usb_creation_dependencies(
    MockUsbExecutor& executor,
    int& environment_calls,
    int& verification_calls,
    std::function<ytec::clonecore::Result<
        ytec::windowsapp::RescueUsbDriveLetterResolution>(
        const ytec::windowsapp::RescueUsbStoragePlan&,
        ytec::windowsapp::RescueUsbDestinationVerificationPoint,
        wchar_t)> verifier = {}) {
  return {
      .inspect_environment =
          [&] {
            ++environment_calls;
            return ready_preflight();
          },
      .resolve_payload =
          [] {
            return ytec::clonecore::Result<
                ytec::windowsapp::ProductMediaPayloadPaths>::success(
                product_payload());
          },
      .create_usb_work_root_name =
          [] {
            return ytec::clonecore::Result<std::wstring>::success(
                L"C:\\Temp\\Tsumugi-USB-MOCK.work");
          },
      .verify_usb_destination =
          [&, verifier = std::move(verifier)](
              const ytec::windowsapp::RescueUsbStoragePlan& plan,
              const ytec::windowsapp::
                  RescueUsbDestinationVerificationPoint point,
              const wchar_t drive_letter) {
            ++verification_calls;
            if (verifier) {
              return verifier(plan, point, drive_letter);
            }
            auto mapping = usb_mapping(
                point == ytec::windowsapp::
                    RescueUsbDestinationVerificationPoint::after_write);
            mapping.target_identity = plan.expected_target;
            mapping.drive_letter = drive_letter;
            mapping.root_path =
                std::wstring{drive_letter, L':', L'\\'};
            return ytec::clonecore::Result<
                ytec::windowsapp::
                    RescueUsbDriveLetterResolution>::success(
                std::move(mapping));
          },
      .usb_executor = &executor,
  };
}

ytec::windowsapp::RescueMediaCreationDependencies creation_dependencies(
    MockIsoExecutor& executor,
    int& environment_calls,
    int& destination_calls) {
  return {
      .inspect_environment =
          [&] {
            ++environment_calls;
            return ready_preflight();
          },
      .resolve_payload =
          [] {
            return ytec::clonecore::Result<
                ytec::windowsapp::ProductMediaPayloadPaths>::success(
                product_payload());
          },
      .create_work_root_name =
          [](const std::wstring&) {
            return ytec::clonecore::Result<std::wstring>::success(
                L"C:\\Output\\.Tsumugi-Media-MOCK.work");
          },
      .verify_new_iso_destination =
          [&](const std::wstring&) {
            ++destination_calls;
            return ytec::clonecore::success_status();
          },
      .iso_executor = &executor,
  };
}

void test_environment_gate_is_first_and_fail_closed() {
  const auto missing =
      ytec::windowsapp::evaluate_rescue_media_plan({});
  check(
      missing.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              environment_check_required,
      "Missing preflight must stop at step one");
  check(!missing.ready_for_confirmation, "Missing preflight must block");

  auto blocked = ready_preflight();
  blocked.media_creation_permitted = false;
  const auto result =
      ytec::windowsapp::evaluate_rescue_media_plan({
          .preflight = &blocked,
          .iso_destination = L"C:\\Output\\Tsumugi.iso",
      });
  check(
      result.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              environment_not_ready,
      "Blocked ADK report must stop");
}

void test_2023_ca_requires_bootex() {
  const auto preflight = ready_preflight(false);
  const auto result =
      ytec::windowsapp::evaluate_rescue_media_plan({
          .preflight = &preflight,
          .boot_profile =
              ytec::windowsapp::RescueMediaBootProfile::
                  windows_uefi_2023_ca,
          .iso_destination = L"C:\\Output\\Tsumugi.iso",
      });
  check(
      result.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              boot_profile_unavailable,
      "2023 CA must require /bootex");
  check(!result.ready_for_confirmation, "Unavailable profile must block");
}

void test_iso_path_syntax_is_bounded_and_local() {
  using ytec::windowsapp::is_safe_iso_destination_syntax;
  check(
      is_safe_iso_destination_syntax(
          L"C:\\Output\\Tsumugi-Rescue.iso"),
      "Local absolute ISO path should pass");
  check(
      is_safe_iso_destination_syntax(
          L"d:\\救済\\TSUMUGI.ISO"),
      "ISO extension should be case insensitive");
  check(
      !is_safe_iso_destination_syntax(
          L"relative\\Tsumugi.iso"),
      "Relative path must fail");
  check(
      !is_safe_iso_destination_syntax(
          L"\\\\server\\share\\Tsumugi.iso"),
      "UNC path must fail");
  check(
      !is_safe_iso_destination_syntax(
          L"C:\\Output\\..\\Tsumugi.iso"),
      "Traversal component must fail");
  check(
      !is_safe_iso_destination_syntax(
          L"C:\\Output\\Tsumugi.img"),
      "Non-ISO extension must fail");
}

void test_valid_iso_reaches_review_without_enabling_execution() {
  const auto preflight = ready_preflight();
  const auto result =
      ytec::windowsapp::evaluate_rescue_media_plan({
          .preflight = &preflight,
          .iso_destination = L"C:\\Output\\Tsumugi.iso",
      });
  check(
      result.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              ready_for_confirmation,
      "Valid ISO plan should reach review");
  check(result.current_step == 3U, "Review must be step three");
  check(result.ready_for_confirmation, "ISO review should be ready");
  check(
      result.summary.find(L"まだ") == std::wstring::npos,
      "Summary should contain only the requested plan");
}

void test_usb_requires_explicit_safe_removable_usb() {
  const auto preflight = ready_preflight();
  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(safe_usb());
  auto reviewed_plan =
      reviewed_initialization_plan(inventory.disks.front());

  auto input = ytec::windowsapp::RescueMediaPlanInput{
      .preflight = &preflight,
      .kind = ytec::windowsapp::RescueMediaKind::usb_drive,
      .inventory = &inventory,
      .usb_target_index = 0U,
      .reviewed_usb_storage_plan = &reviewed_plan,
  };
  auto result =
      ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(result.ready_for_confirmation, "Safe USB should reach review");
  check(
      result.usb_target_identity.has_value(),
      "Safe USB must retain a stable identity");
  check(
      result.confirmation_token == L"OK",
      "USB confirmation token must be the short fixed OK token");
  check(
      result.summary.find(L"USBディスク全体") !=
          std::wstring::npos,
      "USB review must state the exact data-loss boundary");

  inventory.disks[0] = safe_usb();
  inventory.disks[0].partition_style =
      ytec::diskmodel::PartitionStyle::gpt;
  inventory.disks[0].partitions.front().style =
      ytec::diskmodel::PartitionStyle::gpt;
  inventory.disks[0].partitions.front().type =
      L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
  reviewed_plan = reviewed_initialization_plan(inventory.disks.front());
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.ready_for_confirmation &&
          result.message.find(L"自動初期化") != std::wstring::npos,
      "A normal single-partition GPT USB should be auto-initializable");

  inventory.disks[0].partitions.front().type = L"UNKNOWN";
  reviewed_plan = reviewed_initialization_plan(inventory.disks.front());
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.ready_for_confirmation,
      "An unknown-media GPT type may be erased only through the reviewed whole-disk initialization plan");

  inventory.disks[0] = safe_usb();
  inventory.disks[0].partition_style =
      ytec::diskmodel::PartitionStyle::gpt;
  inventory.disks[0].partitions.clear();
  reviewed_plan = reviewed_initialization_plan(inventory.disks.front());
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.ready_for_confirmation &&
          result.confirmation_token == L"OK",
      "An empty GPT USB should be recoverable with the short token");

  inventory.disks[0].is_system_disk = true;
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              usb_target_is_system,
      "System disk must be rejected");

  inventory.disks[0] = safe_usb();
  inventory.disks[0].removable = std::nullopt;
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              usb_target_state_unknown,
      "Unknown removable state must fail closed");

  inventory.disks[0] = safe_usb();
  inventory.disks[0].bus_type = L"NVMe";
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              usb_target_not_usb,
      "Non-USB bus must be rejected");

  inventory.disks[0] = safe_usb();
  inventory.disks[0].read_only = true;
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              usb_target_read_only,
      "Read-only USB must be rejected");

  inventory.disks[0] = safe_usb();
  inventory.disks[0].partitions.front().size_bytes =
      8ULL * 1024ULL * 1024ULL * 1024ULL;
  inventory.disks[0].partitions.push_back(
      ytec::diskmodel::PartitionInfo{
          .number = 2U,
          .offset_bytes =
              8ULL * 1024ULL * 1024ULL * 1024ULL +
              1024ULL * 1024ULL,
          .size_bytes = inventory.disks[0].size_bytes -
              (8ULL * 1024ULL * 1024ULL * 1024ULL +
               1024ULL * 1024ULL),
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
      });
  reviewed_plan = reviewed_initialization_plan(inventory.disks.front());
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.ready_for_confirmation &&
          result.summary.find(L"全消去") != std::wstring::npos,
      "A structurally valid unknown multi-partition USB must remain eligible only for explicit whole-disk initialization");

  inventory.disks[0] = safe_usb();
  inventory.disks[0].serial_suffix.clear();
  inventory.disks[0].device_instance_id.clear();
  result = ytec::windowsapp::evaluate_rescue_media_plan(input);
  check(
      result.issue ==
          ytec::windowsapp::RescueMediaPlanIssue::
              usb_target_identity_unstable,
      "USB without stable identity must be rejected");
}

void test_usb_authorization_reprobes_identity_and_confirmation() {
  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(safe_usb());
  const auto expected =
      ytec::diskmodel::make_stable_disk_identity(
          inventory.disks.front(), false);
  check(expected.has_value(), "Mock USB identity should be stable");

  auto request =
      ytec::windowsapp::RescueUsbAuthorizationRequest{
          .expected_target = expected.value(),
          .fresh_inventory = &inventory,
          .first_step_acknowledged = true,
          .typed_confirmation = L"OK",
          .reviewed_storage_plan =
              reviewed_initialization_plan(inventory.disks.front()),
      };
  const auto authorized =
      ytec::windowsapp::authorize_rescue_usb_target(request);
  check(authorized.has_value(), "Unchanged safe USB should be authorized");
  check(
      authorized.value().target.disk_number == 7U &&
          authorized.value().partition_count == 1U &&
          !authorized.value().physical_write_started,
      "Authorization must identify the target without starting a write");

  request.typed_confirmation = L"ok";
  const auto wrong_confirmation =
      ytec::windowsapp::authorize_rescue_usb_target(request);
  check(
      !wrong_confirmation.has_value() &&
          wrong_confirmation.error().code ==
              ytec::clonecore::ErrorCode::confirmation_required,
      "Wrong USB confirmation must fail before any writer");
}

void test_usb_authorization_allows_empty_gpt_recovery() {
  ytec::diskmodel::InventoryReport inventory;
  auto disk = safe_usb();
  disk.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  disk.partitions.clear();
  inventory.disks.push_back(std::move(disk));
  const auto expected =
      ytec::diskmodel::make_stable_disk_identity(
          inventory.disks.front(), false);
  check(expected.has_value(), "Empty GPT USB identity should be stable");

  const auto authorized =
      ytec::windowsapp::authorize_rescue_usb_target({
          .expected_target = expected.value(),
          .fresh_inventory = &inventory,
          .first_step_acknowledged = true,
          .typed_confirmation = L"OK",
          .reviewed_storage_plan =
              reviewed_initialization_plan(inventory.disks.front()),
      });
  check(
      authorized.has_value() &&
          authorized.value().partition_count == 0U &&
          !authorized.value().physical_write_started,
      "Empty GPT USB recovery must retain a zero-partition authorization");
}

void test_usb_authorization_rejects_runtime_changes() {
  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(safe_usb());
  const auto expected =
      ytec::diskmodel::make_stable_disk_identity(
          inventory.disks.front(), false);
  check(expected.has_value(), "Mock USB identity should be stable");
  const auto reviewed_plan =
      reviewed_initialization_plan(inventory.disks.front());
  const auto authorize =
      [&](const ytec::diskmodel::InventoryReport& current) {
        return ytec::windowsapp::authorize_rescue_usb_target({
            .expected_target = expected.value(),
            .fresh_inventory = &current,
            .first_step_acknowledged = true,
            .typed_confirmation = L"OK",
            .reviewed_storage_plan = reviewed_plan,
        });
      };

  inventory.disks.front().disk_number = 8U;
  auto result = authorize(inventory);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::identity_mismatch,
      "A changed disk number must require selection again");

  inventory.disks.front() = safe_usb();
  inventory.disks.front().serial_suffix = "DIFFERENT";
  result = authorize(inventory);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::identity_mismatch,
      "A swapped USB must fail identity matching");

  inventory.disks.front() = safe_usb();
  inventory.disks.front().is_system_disk = true;
  result = authorize(inventory);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::verification_failed,
      "A USB that becomes the system disk must be rejected");

  inventory.disks.front() = safe_usb();
  inventory.disks.front().offline = true;
  result = authorize(inventory);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::verification_failed,
      "A USB that becomes offline must be rejected");

  inventory.disks.front() = safe_usb();
  inventory.disks.front().read_only = true;
  result = authorize(inventory);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::verification_failed,
      "A USB that becomes read-only must be rejected");

  inventory.disks.front() = safe_usb();
  inventory.issues.push_back(ytec::diskmodel::InventoryIssue{});
  result = authorize(inventory);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::enumeration_failed,
      "Partial fresh inventory must fail closed");
}

void test_iso_creation_rechecks_and_executes_exact_request() {
  MockIsoExecutor executor;
  int environment_calls = 0;
  int destination_calls = 0;
  auto dependencies = creation_dependencies(
      executor, environment_calls, destination_calls);
  std::vector<ytec::windowsapp::RescueMediaCreationProgress> progress;

  const auto result =
      ytec::windowsapp::execute_rescue_media_creation(
          {
              .kind =
                  ytec::windowsapp::RescueMediaKind::iso_file,
              .boot_profile =
                  ytec::windowsapp::RescueMediaBootProfile::
                      windows_uefi_2023_ca,
              .final_iso_path =
                  L"C:\\Output\\Y-TEC-Tsumugi-Drive.iso",
              .administrator = true,
              .callbacks =
                  {
                      .progress =
                          [&](const auto& value) {
                            progress.push_back(value);
                          },
                  },
          },
          dependencies);

  check(result.has_value(), "Valid ISO creation should succeed");
  check(
      environment_calls == 1 && destination_calls == 1,
      "Execution must freshly recheck environment and destination");
  check(executor.calls == 1, "ISO executor must be called exactly once");
  check(
      executor.observed.boot_profile ==
              ytec::windowsapp::RescueMediaBootProfile::
                  windows_uefi_2023_ca &&
          executor.observed.final_iso_path ==
              L"C:\\Output\\Y-TEC-Tsumugi-Drive.iso",
      "The exact profile and final ISO path must reach the executor");
  check(
      !progress.empty() && progress.front().percent == 3U &&
          progress.back().percent == 100U,
      "Progress must cover validation through verified completion");
  check(
      result.value().complete_iso_verified &&
          result.value().published_without_overwrite,
      "Only verified non-overwriting publication may succeed");
}

void test_media_builder_identity_is_compile_time_pinned() {
  constexpr std::string_view kAuditedSha256 =
      "0827588ABA847847453E4311D7A3F12C"
      "AAF0FA91223DD5DD9784C35E0E47AFA2";
  check(
      ytec::windowsapp::matches_embedded_media_builder_identity(
          110'127U, kAuditedSha256),
      "The current audited MediaBuilder identity must match");
  check(
      ytec::windowsapp::matches_embedded_media_builder_identity(
          110'127U,
          "0827588aba847847453e4311d7a3f12c"
          "aaf0fa91223dd5dd9784c35e0e47afa2"),
      "SHA-256 comparison may accept lowercase hexadecimal");
  check(
      !ytec::windowsapp::matches_embedded_media_builder_identity(
          110'128U, kAuditedSha256),
      "A changed MediaBuilder length must fail closed");
  check(
      !ytec::windowsapp::matches_embedded_media_builder_identity(
          110'127U,
          "1827588ABA847847453E4311D7A3F12C"
          "AAF0FA91223DD5DD9784C35E0E47AFA2"),
      "A changed MediaBuilder digest must fail closed");
}

void test_media_builder_failure_prefers_bounded_standard_error() {
  const std::string standard_output(2'000U, 'O');
  const std::string standard_error =
      "manifest write failed: unsupported utf8NoBOM";
  const std::wstring message =
      ytec::windowsapp::format_media_builder_failure_message(
          standard_output, standard_error);
  check(
      message.find(L"unsupported utf8NoBOM") != std::wstring::npos,
      "MediaBuilder stderr should be included in the product error");
  check(
      message.find(L"OOOO") == std::wstring::npos,
      "Stderr should take priority over verbose stdout");

  const std::wstring bounded =
      ytec::windowsapp::format_media_builder_failure_message(
          std::string(3'000U, 'X'), {});
  check(
      bounded.size() < 1'800U &&
          bounded.find(L"…") != std::wstring::npos,
      "Fallback stdout should be bounded before display");
}

void test_media_builder_usb_drive_marker_is_strict_and_unique() {
  const auto valid =
      ytec::windowsapp::parse_media_builder_usb_drive_marker(
          "YTEC_MEDIA_PROGRESS={}\r\n"
          "WINPE_APP_USB_DRIVE=K:\r\n"
          "WINPE_APP_USB_PASS=C:\\Temp\\manifest.json\r\n");
  check(
      valid.has_value() && valid.value() == L'K',
      "A single uppercase USB drive marker should be accepted");

  for (const std::string output : {
           "WINPE_APP_USB_PASS=C:\\Temp\\manifest.json\r\n",
           "WINPE_APP_USB_DRIVE=k:\r\n",
           "prefix WINPE_APP_USB_DRIVE=K:\r\n",
           "WINPE_APP_USB_DRIVE=K:\r\n"
           "WINPE_APP_USB_DRIVE=L:\r\n",
       }) {
    check(
        !ytec::windowsapp::parse_media_builder_usb_drive_marker(output)
             .has_value(),
        "Missing, malformed, embedded, or duplicate drive markers must fail");
  }
}

void test_media_creation_rejects_non_elevated() {
  MockIsoExecutor executor;
  int environment_calls = 0;
  int destination_calls = 0;
  auto dependencies = creation_dependencies(
      executor, environment_calls, destination_calls);

  ytec::windowsapp::RescueMediaCreationRequest request{};
  request.kind = ytec::windowsapp::RescueMediaKind::iso_file;
  request.final_iso_path = L"C:\\Output\\Tsumugi.iso";
  request.administrator = false;
  const auto result =
      ytec::windowsapp::execute_rescue_media_creation(
      request, dependencies);
  check(!result.has_value(), "Standard-user ISO execution must stop");
  check(
      result.error().code == ytec::clonecore::ErrorCode::access_denied,
      "Standard-user execution should request explicit elevation");
  check(
      environment_calls == 0 && destination_calls == 0 &&
          executor.calls == 0,
      "Rejected requests must not inspect outputs or invoke the executor");
}

void test_usb_creation_rechecks_exact_target_before_and_after() {
  MockUsbExecutor executor;
  int environment_calls = 0;
  int verification_calls = 0;
  auto dependencies = usb_creation_dependencies(
      executor, environment_calls, verification_calls);
  std::vector<ytec::windowsapp::RescueMediaCreationProgress> progress;
  const auto authorization = usb_authorization();
  const auto mapping = usb_mapping();

  const auto result =
      ytec::windowsapp::execute_rescue_media_creation(
          {
              .kind =
                  ytec::windowsapp::RescueMediaKind::usb_drive,
              .boot_profile =
                  ytec::windowsapp::RescueMediaBootProfile::
                      windows_uefi_2023_ca,
              .administrator = true,
              .usb_authorization = authorization,
              .usb_mapping = mapping,
              .callbacks =
                  {
                      .progress =
                          [&](const auto& value) {
                            progress.push_back(value);
                          },
                  },
          },
          dependencies);

  check(result.has_value(), "Valid target-bound USB creation should succeed");
  check(
      environment_calls == 1 && verification_calls == 2,
      "USB identity and drive letter must be rechecked before and after execution");
  check(executor.calls == 1, "USB executor must run exactly once");
  check(
      executor.observed.boot_profile ==
              ytec::windowsapp::RescueMediaBootProfile::
                  windows_uefi_2023_ca &&
          executor.observed.authorization.target.disk_number == 7U &&
          executor.observed.mapping.drive_letter == L'R',
      "The exact authorized target must reach the USB executor");
  check(
      result.value().complete_usb_verified &&
          !result.value().complete_iso_verified &&
          result.value().usb_root_path == L"R:\\",
      "Only a verified USB report may become success");
  check(
      !progress.empty() && progress.front().percent == 3U &&
          progress.back().percent == 100U,
      "USB progress must cover validation through verified completion");
}

void test_unpartitioned_usb_uses_proposed_letter_then_verifies_volume() {
  MockUsbExecutor executor;
  int environment_calls = 0;
  int verification_calls = 0;
  int verifier_sequence = 0;
  auto authorization = usb_authorization();
  authorization.partition_count = 0U;
  auto empty_disk = safe_usb();
  empty_disk.partitions.clear();
  const auto empty_plan =
      ytec::windowsapp::plan_rescue_usb_storage({
          .target = &empty_disk,
          .mode = ytec::windowsapp::
              RescueUsbProvisioningMode::initialize_all,
          .data_file_system =
              ytec::windowsapp::RescueUsbDataFileSystem::ntfs,
      });
  check(empty_plan.has_value(), "Empty USB storage plan should be valid");
  authorization.storage_plan = empty_plan.value();
  auto proposed = usb_mapping();
  proposed.partition_number = 0U;
  proposed.extent_start = 0U;
  proposed.extent_length = 0U;
  proposed.drive_letter_was_unassigned = true;
  std::vector<wchar_t> verified_letters;

  executor.custom =
      [](const auto& execution) {
        return ytec::clonecore::Result<
            ytec::windowsapp::RescueMediaCreationReport>::success({
            .manifest_path =
                execution.work_root + L"\\usb-media-manifest.json",
            .retained_work_root = execution.work_root,
            .usb_root_path = L"T:\\",
            .usb_boot_wim_sha256 = std::string(64U, 'B'),
            .usb_layout_verified = true,
            .usb_boot_staged_without_overwrite = true,
            .usb_data_preserved = false,
            .complete_usb_verified = true,
        });
      };

  auto dependencies = usb_creation_dependencies(
      executor,
      environment_calls,
       verification_calls,
       [&](const auto& plan,
           const auto point,
           const wchar_t drive_letter) {
         verified_letters.push_back(drive_letter);
         auto current = usb_mapping(
             point == ytec::windowsapp::
                 RescueUsbDestinationVerificationPoint::after_write);
         current.target_identity = plan.expected_target;
        current.drive_letter = drive_letter;
        current.root_path =
            std::wstring{drive_letter, L':', L'\\'};
        if (++verifier_sequence == 1) {
          current.drive_letter = L'S';
          current.root_path = L"S:\\";
          current.partition_number = 0U;
          current.extent_start = 0U;
          current.extent_length = 0U;
          current.drive_letter_was_unassigned = true;
        }
        return ytec::clonecore::Result<
            ytec::windowsapp::
                RescueUsbDriveLetterResolution>::success(
            std::move(current));
      });
  const auto result =
      ytec::windowsapp::execute_rescue_media_creation(
          {
              .kind =
                  ytec::windowsapp::RescueMediaKind::usb_drive,
              .administrator = true,
              .usb_authorization = authorization,
              .usb_mapping = proposed,
          },
          dependencies);

  check(result.has_value(),
        "An empty USB may use a different safe letter after initialization");
  check(
      verification_calls == 2 && executor.calls == 1 &&
          executor.observed.mapping.drive_letter_was_unassigned &&
          executor.observed.mapping.partition_number == 0U &&
          executor.observed.mapping.drive_letter == L'S',
      "A refreshed unpartitioned proposal must reach the writer");
  check(
          verified_letters.size() == 2U &&
          verified_letters[0] == L'R' &&
          verified_letters[1] == L'T' &&
          result.value().usb_root_path == L"T:\\",
      "The actual post-initialization letter must be reverified against the same USB");
}

void test_usb_work_root_factory_only_reserves_a_new_temp_name() {
  const auto result =
      ytec::windowsapp::
          make_usb_media_work_root_name_with_windows_apis();
  check(result.has_value(), "USB work-root naming should succeed");
  check(
      result.value().find(L"Y-TEC-Tsumugi-Drive-USB-") !=
          std::wstring::npos,
      "USB work-root name should identify its bounded purpose");
  check(
      !std::filesystem::exists(result.value()),
      "Naming a USB work root must not create any file or directory");
}

void test_usb_creation_stops_on_missing_or_swapped_target() {
  MockUsbExecutor executor;
  int environment_calls = 0;
  int verification_calls = 0;
  auto dependencies = usb_creation_dependencies(
      executor, environment_calls, verification_calls);
  ytec::windowsapp::RescueMediaCreationRequest request{};
  request.kind = ytec::windowsapp::RescueMediaKind::usb_drive;
  request.administrator = true;
  auto result = ytec::windowsapp::execute_rescue_media_creation(
      request, dependencies);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::confirmation_required,
      "Missing two-step USB authorization must stop");
  check(
      verification_calls == 0 && executor.calls == 0,
      "Missing authorization must stop before any target verifier");

  const auto authorization = usb_authorization();
  request.usb_authorization = authorization;
  request.usb_mapping = usb_mapping();
  verification_calls = 0;
  dependencies = usb_creation_dependencies(
      executor,
      environment_calls,
      verification_calls,
       [](const auto&, const auto, const wchar_t) {
         auto changed = usb_mapping();
        changed.target_identity.serial_suffix = "SWAPPED";
        return ytec::clonecore::Result<
            ytec::windowsapp::
                RescueUsbDriveLetterResolution>::success(
            std::move(changed));
      });
  result = ytec::windowsapp::execute_rescue_media_creation(
      request, dependencies);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::identity_mismatch,
      "A swapped USB must stop at the final pre-write verifier");
  check(
      verification_calls == 1 && executor.calls == 0,
      "A swapped USB must stop before invoking the writer");
}

void test_usb_creation_rejects_bad_or_changed_post_write_report() {
  MockUsbExecutor executor;
  int environment_calls = 0;
  int verification_calls = 0;
  auto dependencies = usb_creation_dependencies(
      executor, environment_calls, verification_calls);
  const auto authorization = usb_authorization();
  const auto mapping = usb_mapping();
  ytec::windowsapp::RescueMediaCreationRequest request{};
  request.kind = ytec::windowsapp::RescueMediaKind::usb_drive;
  request.administrator = true;
  request.usb_authorization = authorization;
  request.usb_mapping = mapping;

  executor.custom =
      [](const auto& execution) {
        return ytec::clonecore::Result<
            ytec::windowsapp::RescueMediaCreationReport>::success({
            .manifest_path =
                execution.work_root + L"\\usb-media-manifest.json",
            .retained_work_root = execution.work_root,
            .usb_root_path = execution.mapping.root_path,
            .usb_boot_wim_sha256 = "BAD",
            .complete_usb_verified = false,
        });
      };
  auto result = ytec::windowsapp::execute_rescue_media_creation(
      request, dependencies);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::verification_failed,
      "An incomplete USB executor report must never become success");

  executor.custom = {};
  executor.calls = 0;
  verification_calls = 0;
  int verifier_sequence = 0;
  dependencies = usb_creation_dependencies(
      executor,
      environment_calls,
      verification_calls,
       [&](const auto& plan,
           const auto point,
           const wchar_t drive_letter) {
         auto current = usb_mapping(
             point == ytec::windowsapp::
                 RescueUsbDestinationVerificationPoint::after_write);
         current.target_identity = plan.expected_target;
        current.drive_letter = drive_letter;
        current.root_path =
            std::wstring{drive_letter, L':', L'\\'};
        if (++verifier_sequence == 2) {
          current.target_identity.device_instance_id =
              L"USB\\SWAPPED";
        }
        return ytec::clonecore::Result<
            ytec::windowsapp::
                RescueUsbDriveLetterResolution>::success(
            std::move(current));
      });
  result = ytec::windowsapp::execute_rescue_media_creation(
      request, dependencies);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::identity_mismatch,
      "A target change after writing must prevent a completed report");
  check(
      verification_calls == 2 && executor.calls == 1,
      "Post-write identity must be checked after one writer invocation");
}

void test_media_creation_cancels_before_write_and_rejects_bad_report() {
  MockIsoExecutor executor;
  int environment_calls = 0;
  int destination_calls = 0;
  auto dependencies = creation_dependencies(
      executor, environment_calls, destination_calls);
  bool cancel = true;
  ytec::windowsapp::RescueMediaCreationRequest request{};
  request.kind = ytec::windowsapp::RescueMediaKind::iso_file;
  request.final_iso_path = L"C:\\Output\\Tsumugi.iso";
  request.administrator = true;
  request.callbacks.cancellation_requested =
      [&] { return cancel; };
  auto result = ytec::windowsapp::execute_rescue_media_creation(
      request, dependencies);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::cancelled,
      "Cancellation before execution must fail closed");
  check(executor.calls == 0, "Cancelled request must not invoke executor");

  cancel = false;
  executor.custom =
      [](const auto& execution) {
        return ytec::clonecore::Result<
            ytec::windowsapp::RescueMediaCreationReport>::success({
            .final_iso_path = execution.final_iso_path,
            .manifest_path =
                execution.final_iso_path + L".manifest.json",
            .retained_work_root = execution.work_root,
            .iso_length = 1U,
            .iso_sha256 = "BAD",
            .complete_iso_verified = false,
            .published_without_overwrite = true,
        });
      };
  result = ytec::windowsapp::execute_rescue_media_creation(
      request, dependencies);
  check(
      !result.has_value() &&
          result.error().code ==
              ytec::clonecore::ErrorCode::verification_failed,
      "Incomplete executor report must never become success");
}

}  // namespace

int main() {
  try {
    test_environment_gate_is_first_and_fail_closed();
    test_2023_ca_requires_bootex();
    test_iso_path_syntax_is_bounded_and_local();
    test_valid_iso_reaches_review_without_enabling_execution();
    test_usb_requires_explicit_safe_removable_usb();
    test_usb_authorization_reprobes_identity_and_confirmation();
    test_usb_authorization_allows_empty_gpt_recovery();
    test_usb_authorization_rejects_runtime_changes();
    test_iso_creation_rechecks_and_executes_exact_request();
    test_media_builder_identity_is_compile_time_pinned();
    test_media_builder_failure_prefers_bounded_standard_error();
    test_media_builder_usb_drive_marker_is_strict_and_unique();
    test_media_creation_rejects_non_elevated();
    test_usb_creation_rechecks_exact_target_before_and_after();
    test_unpartitioned_usb_uses_proposed_letter_then_verifies_volume();
    test_usb_work_root_factory_only_reserves_a_new_temp_name();
    test_usb_creation_stops_on_missing_or_swapped_target();
    test_usb_creation_rejects_bad_or_changed_post_write_report();
    test_media_creation_cancels_before_write_and_rejects_bad_report();
    std::cout << "media wizard tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "media wizard tests: FAIL: "
              << failure.message << '\n';
    return 1;
  }
}

#include "ytec/diskmodel/physical_disk.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
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

ytec::clonecore::Error fake_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = operation,
      .message = L"モック物理ディスク失敗",
  };
}

class FakeInventory final : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++call_count;
    return ytec::clonecore::Result<ytec::diskmodel::InventoryReport>::success(
        report);
  }

  ytec::diskmodel::InventoryReport report;
  int call_count{};
};

class DummySource final : public ytec::clonecore::ISourceDiskReader {
 public:
  DummySource(const std::uint64_t size, const std::uint32_t sector)
      : size_(size), sector_(sector) {}

  std::uint64_t size_bytes() const noexcept override { return size_; }
  std::uint32_t logical_sector_size() const noexcept override {
    return sector_;
  }
  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t,
      const std::size_t length) const override {
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(length));
  }

 private:
  std::uint64_t size_{};
  std::uint32_t sector_{};
};

class DummyTarget final : public ytec::clonecore::ITargetDiskWriter {
 public:
  DummyTarget(const std::uint64_t size, const std::uint32_t sector)
      : size_(size), sector_(sector) {}

  std::uint64_t size_bytes() const noexcept override { return size_; }
  std::uint32_t logical_sector_size() const noexcept override {
    return sector_;
  }
  ytec::clonecore::Status write_target(
      const std::uint64_t,
      const std::span<const std::byte>) override {
    return ytec::clonecore::success_status();
  }
  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t,
      const std::size_t length) const override {
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(length));
  }
  ytec::clonecore::Status flush_target() override {
    return ytec::clonecore::success_status();
  }

 private:
  std::uint64_t size_{};
  std::uint32_t sector_{};
};

class FakeBackend final
    : public ytec::diskmodel::IWindowsPhysicalDiskBackend {
 public:
  explicit FakeBackend(FakeInventory& inventory) : inventory_(inventory) {}

  ytec::clonecore::Result<
      std::unique_ptr<ytec::clonecore::ISourceDiskReader>>
  open_source(const ytec::diskmodel::DiskInfo& disk) override {
    ++source_open_count;
    if (fail_source_open) {
      return ytec::clonecore::Result<
          std::unique_ptr<ytec::clonecore::ISourceDiskReader>>::failure(
          fake_error(L"モックコピー元オープン"));
    }
    return ytec::clonecore::Result<
        std::unique_ptr<ytec::clonecore::ISourceDiskReader>>::success(
        std::make_unique<DummySource>(
            wrong_source_geometry ? disk.size_bytes - 512
                                  : disk.size_bytes,
            disk.logical_sector_size));
  }

  ytec::clonecore::Result<
      std::unique_ptr<ytec::clonecore::ITargetDiskWriter>>
  open_offline_target(const ytec::diskmodel::DiskInfo& disk) override {
    ++target_open_count;
    if (fail_target_open) {
      return ytec::clonecore::Result<
          std::unique_ptr<ytec::clonecore::ITargetDiskWriter>>::failure(
          fake_error(L"モックコピー先オープン"));
    }
    return ytec::clonecore::Result<
        std::unique_ptr<ytec::clonecore::ITargetDiskWriter>>::success(
        std::make_unique<DummyTarget>(
            wrong_target_geometry ? disk.size_bytes - 512
                                  : disk.size_bytes,
            disk.logical_sector_size));
  }

  ytec::clonecore::Status set_target_offline(
      const ytec::diskmodel::DiskInfo& disk,
      const bool offline) override {
    ++state_change_count;
    if (fail_state_change) {
      return ytec::clonecore::Status::failure(
          fake_error(L"モックoffline状態変更"));
    }
    if (reflect_state_change) {
      for (auto& candidate : inventory_.report.disks) {
        if (candidate.disk_number == disk.disk_number) {
          candidate.offline = offline;
        }
      }
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status set_source_read_only(
      const ytec::diskmodel::DiskInfo& disk,
      const bool read_only) override {
    ++source_read_only_change_count;
    if (fail_source_read_only_change) {
      return ytec::clonecore::Status::failure(
          fake_error(L"モックread-only状態変更"));
    }
    if (reflect_source_read_only_change) {
      for (auto& candidate : inventory_.report.disks) {
        if (candidate.disk_number == disk.disk_number) {
          candidate.read_only = read_only;
        }
      }
    }
    return ytec::clonecore::success_status();
  }

  FakeInventory& inventory_;
  int source_open_count{};
  int target_open_count{};
  int state_change_count{};
  int source_read_only_change_count{};
  bool reflect_state_change{true};
  bool fail_source_open{};
  bool wrong_source_geometry{};
  bool fail_target_open{};
  bool wrong_target_geometry{};
  bool fail_state_change{};
  bool reflect_source_read_only_change{true};
  bool fail_source_read_only_change{};
};

ytec::diskmodel::DiskInfo disk(
    const std::uint32_t number,
    std::wstring model,
    std::string serial,
    std::wstring instance,
    const std::uint64_t size,
    const bool offline,
    const bool system) {
  ytec::diskmodel::DiskInfo value;
  value.disk_number = number;
  value.device_path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
  value.device_instance_id = std::move(instance);
  value.model = std::move(model);
  value.size_bytes = size;
  value.sector_count = size / 512;
  value.logical_sector_size = 512;
  value.physical_sector_size = 4096;
  value.bus_type = L"Virtual";
  value.serial_suffix = std::move(serial);
  value.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  value.offline = offline;
  value.read_only = false;
  value.removable = false;
  value.is_system_disk = system;
  return value;
}

struct Fixture final {
  Fixture() {
    inventory.report.disks.push_back(disk(
        3,
        L"VBOX SOURCE",
        "SRC00001",
        L"SCSI\\SOURCE",
        16U * 1024U * 1024U,
        false,
        false));
    inventory.report.disks.push_back(disk(
        7,
        L"VBOX TARGET",
        "DST00002",
        L"SCSI\\TARGET",
        20U * 1024U * 1024U,
        false,
        false));
    source = ytec::diskmodel::make_stable_disk_identity(
                 inventory.report.disks[0], false)
                 .take_value();
    target = ytec::diskmodel::make_stable_disk_identity(
                 inventory.report.disks[1], false)
                 .take_value();
    confirmation.first_step_acknowledged = true;
    confirmation.typed_token =
        ytec::clonecore::make_target_confirmation_token(target);
  }

  FakeInventory inventory;
  ytec::clonecore::StableDiskIdentity source;
  ytec::clonecore::StableDiskIdentity target;
  ytec::clonecore::TargetConfirmation confirmation;
};

void test_reidentification_does_not_trust_disk_number() {
  Fixture fixture;
  fixture.inventory.report.disks[0].disk_number = 9;
  fixture.inventory.report.disks[0].device_path = L"\\\\.\\PhysicalDrive9";
  const auto observed = ytec::diskmodel::reidentify_physical_clone(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      fixture.inventory);
  check(observed.has_value(), "Stable signals should survive disk renumbering");
  check(observed.value().source.disk_number == 9,
        "The current disk number should come from fresh enumeration");
}

void test_read_only_clone_selection_precedes_confirmation() {
  Fixture fixture;
  fixture.confirmation.typed_token = L"ok";
  const auto observed =
      ytec::diskmodel::reidentify_physical_clone_selection(
          fixture.source, fixture.target, fixture.inventory);
  check(
      observed.has_value(),
      "Read-only OperationCore selection should not consume confirmation");
  const auto destructive = ytec::diskmodel::reidentify_physical_clone(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      fixture.inventory);
  check(
      !destructive.has_value() &&
          destructive.error().code ==
              ytec::clonecore::ErrorCode::confirmation_required,
      "Destructive boundary must still reject lowercase confirmation");
}

void test_confirmation_failure_blocks_backend() {
  Fixture fixture;
  fixture.confirmation.typed_token = L"ERASE WRONG";
  FakeBackend backend(fixture.inventory);
  const auto status = ytec::diskmodel::set_verified_target_offline(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      true,
      fixture.inventory,
      backend);
  check(!status.has_value(), "Wrong confirmation must fail");
  check(backend.state_change_count == 0,
        "No state change may occur after failed confirmation");
}

void test_system_target_is_rejected() {
  Fixture fixture;
  fixture.inventory.report.disks[1].is_system_disk = true;
  FakeBackend backend(fixture.inventory);
  const auto status = ytec::diskmodel::set_verified_target_offline(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      true,
      fixture.inventory,
      backend);
  check(!status.has_value(), "The running system disk must be rejected");
  check(backend.state_change_count == 0,
        "System target rejection must happen before backend access");
}

void test_offline_transition_is_reenumerated() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  const auto status = ytec::diskmodel::set_verified_target_offline(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      true,
      fixture.inventory,
      backend);
  check(status.has_value(), "A reflected offline transition should pass");
  check(backend.state_change_count == 1, "The target changes state once");
  check(fixture.inventory.call_count == 2,
        "State transition must enumerate before and after the change");
}

void test_unverified_offline_transition_fails_closed() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  backend.reflect_state_change = false;
  const auto status = ytec::diskmodel::set_verified_target_offline(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      true,
      fixture.inventory,
      backend);
  check(!status.has_value(), "Unreflected state change must fail");
  check(status.error().code ==
            ytec::clonecore::ErrorCode::verification_failed,
        "The failure should identify post-change verification");
}

void test_open_requires_offline_writable_target() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  auto handles = ytec::diskmodel::open_verified_physical_clone(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      fixture.inventory,
      backend);
  check(!handles.has_value(), "An online target must not be opened for writing");
  check(backend.source_open_count == 0 && backend.target_open_count == 0,
        "Attribute validation must precede all raw opens");

  fixture.inventory.report.disks[1].offline = true;
  handles = ytec::diskmodel::open_verified_physical_clone(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      fixture.inventory,
      backend);
  check(handles.has_value(), "An offline writable target should open");
  check(backend.source_open_count == 1 && backend.target_open_count == 1,
        "Source and target opens should both be reached after validation");
}

void test_phase1_rejects_running_system_source() {
  Fixture fixture;
  fixture.inventory.report.disks[0].is_system_disk = true;
  fixture.inventory.report.disks[1].offline = true;
  FakeBackend backend(fixture.inventory);
  const auto handles = ytec::diskmodel::open_verified_physical_clone(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      fixture.inventory,
      backend);
  check(!handles.has_value(),
        "Phase 1 must not raw-clone the running Windows system disk");
  check(backend.source_open_count == 0 && backend.target_open_count == 0,
        "System source rejection must happen before raw opens");
}

void test_inventory_diagnostic_blocks_destructive_path() {
  Fixture fixture;
  fixture.inventory.report.issues.push_back(ytec::diskmodel::InventoryIssue{
      .device = L"mock",
      .error = fake_error(L"モック列挙診断"),
  });
  FakeBackend backend(fixture.inventory);
  const auto status = ytec::diskmodel::set_verified_target_offline(
      fixture.source,
      fixture.target,
      fixture.confirmation,
      true,
      fixture.inventory,
      backend);
  check(!status.has_value(), "Any unresolved inventory issue must block writes");
  check(backend.state_change_count == 0,
        "Inventory diagnostics must block the backend");
}

void test_restore_target_confirmation_blocks_backend() {
  Fixture fixture;
  fixture.confirmation.typed_token = L"ERASE WRONG";
  FakeBackend backend(fixture.inventory);
  const auto status =
      ytec::diskmodel::set_verified_physical_target_offline(
          fixture.target,
          fixture.confirmation,
          true,
          fixture.inventory,
          backend);
  check(!status.has_value(),
        "Wrong restore target confirmation must fail closed");
  check(backend.state_change_count == 0 &&
            backend.target_open_count == 0 &&
            backend.source_open_count == 0,
        "Wrong restore confirmation must precede every backend access");
}

void test_restore_target_rejects_system_removable_and_unknown() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  fixture.inventory.report.disks[1].is_system_disk = true;
  auto observed = ytec::diskmodel::reidentify_physical_target(
      fixture.target, fixture.confirmation, fixture.inventory);
  check(!observed.has_value(),
        "The running system disk must not be a restore target");

  fixture.inventory.report.disks[1].is_system_disk = false;
  fixture.inventory.report.disks[1].removable = true;
  observed = ytec::diskmodel::reidentify_physical_target(
      fixture.target, fixture.confirmation, fixture.inventory);
  check(!observed.has_value(),
        "A removable disk must not pass the product restore gate");

  fixture.inventory.report.disks[1].removable.reset();
  observed = ytec::diskmodel::reidentify_physical_target(
      fixture.target, fixture.confirmation, fixture.inventory);
  check(!observed.has_value(),
        "Unknown destructive target attributes must fail closed");
  check(backend.state_change_count == 0 &&
            backend.target_open_count == 0,
        "Restore target policy failures must not reach the backend");
}

void test_restore_target_offline_transition_is_reverified() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  const auto status =
      ytec::diskmodel::set_verified_physical_target_offline(
          fixture.target,
          fixture.confirmation,
          true,
          fixture.inventory,
          backend);
  check(status.has_value(),
        "A reflected restore-target offline transition should pass");
  check(backend.state_change_count == 1,
        "Restore target should change state exactly once");
  check(fixture.inventory.call_count == 2,
        "Restore target state must be enumerated before and after change");
  check(backend.source_open_count == 0,
        "Target-only restore boundary must never open a source disk");
}

void test_restore_target_open_requires_offline_and_rechecks_geometry() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  auto opened = ytec::diskmodel::open_verified_physical_target(
      fixture.target,
      fixture.confirmation,
      fixture.inventory,
      backend);
  check(!opened.has_value(),
        "An online restore target must not open for writing");
  check(backend.target_open_count == 0,
        "Offline validation must precede the target open");

  fixture.inventory.report.disks[1].offline = true;
  backend.wrong_target_geometry = true;
  opened = ytec::diskmodel::open_verified_physical_target(
      fixture.target,
      fixture.confirmation,
      fixture.inventory,
      backend);
  check(!opened.has_value(),
        "Substituted target writer geometry must fail closed");
  check(opened.error().code ==
            ytec::clonecore::ErrorCode::identity_mismatch,
        "Writer geometry substitution should be an identity mismatch");

  backend.wrong_target_geometry = false;
  opened = ytec::diskmodel::open_verified_physical_target(
      fixture.target,
      fixture.confirmation,
      fixture.inventory,
      backend);
  check(opened.has_value(),
        "An offline verified restore target should open");
  check(backend.source_open_count == 0,
        "Restore target open must remain target-only");
}

void test_read_only_source_reidentifies_and_allows_system_disk() {
  Fixture fixture;
  fixture.inventory.report.disks[0].disk_number = 9;
  fixture.inventory.report.disks[0].device_path =
      L"\\\\.\\PhysicalDrive9";
  fixture.inventory.report.disks[0].is_system_disk = true;
  const auto expected = ytec::diskmodel::make_stable_disk_identity(
      fixture.inventory.report.disks[0], true);
  check(expected.has_value(), "System source identity should build");
  FakeBackend backend(fixture.inventory);
  const auto opened =
      ytec::diskmodel::open_verified_read_only_physical_disk(
          expected.value(), fixture.inventory, backend);
  check(
      opened.has_value(),
      "Read-only image source may be the running system disk");
  check(
      opened.value().observed.observed.disk_number == 9 &&
          opened.value().observed.identity.is_system_disk &&
          backend.source_open_count == 1 &&
          backend.target_open_count == 0,
      "Read-only source must use fresh identity and never open a target");
}

void test_read_only_source_reader_geometry_is_rechecked() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  backend.wrong_source_geometry = true;
  const auto opened =
      ytec::diskmodel::open_verified_read_only_physical_disk(
          fixture.source, fixture.inventory, backend);
  check(!opened.has_value(), "Substituted reader geometry must fail");
  check(
      opened.error().code ==
          ytec::clonecore::ErrorCode::identity_mismatch,
      "Reader geometry substitution should be an identity mismatch");
}

void test_inventory_diagnostic_blocks_read_only_source_open() {
  Fixture fixture;
  fixture.inventory.report.issues.push_back(
      ytec::diskmodel::InventoryIssue{
          .device = L"mock",
          .error = fake_error(L"モック列挙診断"),
      });
  FakeBackend backend(fixture.inventory);
  const auto opened =
      ytec::diskmodel::open_verified_read_only_physical_disk(
          fixture.source, fixture.inventory, backend);
  check(!opened.has_value(), "Inventory issue must block source identity");
  check(
      backend.source_open_count == 0,
      "Inventory diagnostics must stop before raw source open");
}

void test_source_read_only_transition_is_nonpersistent_boundary() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  const auto protected_source =
      ytec::diskmodel::set_verified_source_read_only(
          fixture.source, true, fixture.inventory, backend);
  check(protected_source.has_value(),
        "A reflected source read-only transition should pass");
  check(backend.source_read_only_change_count == 1,
        "The source attribute should change exactly once");
  check(fixture.inventory.call_count == 2,
        "Source identity and attribute must be checked before and after");
  check(fixture.inventory.report.disks[0].read_only.value_or(false),
        "The mock inventory must reflect the protected source");

  const auto restored_source =
      ytec::diskmodel::set_verified_source_read_only(
          fixture.source, false, fixture.inventory, backend);
  check(restored_source.has_value(),
        "The temporary source attribute should be restorable");
  check(backend.source_read_only_change_count == 2,
        "Protection and restoration should each change state once");
  check(!fixture.inventory.report.disks[0].read_only.value_or(true),
        "The mock source must return to its original writable state");
}

void test_unverified_source_read_only_transition_fails_closed() {
  Fixture fixture;
  FakeBackend backend(fixture.inventory);
  backend.reflect_source_read_only_change = false;
  const auto status = ytec::diskmodel::set_verified_source_read_only(
      fixture.source, true, fixture.inventory, backend);
  check(!status.has_value(),
        "An unreflected source protection transition must fail");
  check(status.error().code ==
            ytec::clonecore::ErrorCode::verification_failed,
        "Failure to observe read-only must be a verification failure");
  check(backend.source_open_count == 0 && backend.target_open_count == 0,
        "Source protection verification must not open data handles");
}

void test_local_path_disk_mapping_rejects_nonlocal_and_parent_traversal() {
  check(
      !ytec::diskmodel::query_single_disk_number_for_local_path(
           L"relative\\backup.dcmig")
           .has_value(),
      "Relative destination paths must fail before volume access");
  check(
      !ytec::diskmodel::query_single_disk_number_for_local_path(
           L"\\\\server\\share\\backup.dcmig")
           .has_value(),
      "UNC destination paths must fail before volume access");
  check(
      !ytec::diskmodel::query_single_disk_number_for_local_path(
           L"C:\\safe\\..\\backup.dcmig")
           .has_value(),
      "Parent traversal must fail before destination mapping");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"reidentification_does_not_trust_disk_number",
       test_reidentification_does_not_trust_disk_number},
      {"read_only_clone_selection_precedes_confirmation",
       test_read_only_clone_selection_precedes_confirmation},
      {"confirmation_failure_blocks_backend",
       test_confirmation_failure_blocks_backend},
      {"system_target_is_rejected", test_system_target_is_rejected},
      {"offline_transition_is_reenumerated",
       test_offline_transition_is_reenumerated},
      {"unverified_offline_transition_fails_closed",
       test_unverified_offline_transition_fails_closed},
      {"open_requires_offline_writable_target",
       test_open_requires_offline_writable_target},
      {"phase1_rejects_running_system_source",
       test_phase1_rejects_running_system_source},
      {"inventory_diagnostic_blocks_destructive_path",
       test_inventory_diagnostic_blocks_destructive_path},
      {"restore_target_confirmation_blocks_backend",
       test_restore_target_confirmation_blocks_backend},
      {"restore_target_rejects_system_removable_and_unknown",
       test_restore_target_rejects_system_removable_and_unknown},
      {"restore_target_offline_transition_is_reverified",
       test_restore_target_offline_transition_is_reverified},
      {"restore_target_open_requires_offline_and_rechecks_geometry",
       test_restore_target_open_requires_offline_and_rechecks_geometry},
      {"read_only_source_reidentifies_and_allows_system_disk",
       test_read_only_source_reidentifies_and_allows_system_disk},
      {"read_only_source_reader_geometry_is_rechecked",
       test_read_only_source_reader_geometry_is_rechecked},
      {"inventory_diagnostic_blocks_read_only_source_open",
       test_inventory_diagnostic_blocks_read_only_source_open},
      {"source_read_only_transition_is_nonpersistent_boundary",
       test_source_read_only_transition_is_nonpersistent_boundary},
      {"unverified_source_read_only_transition_fails_closed",
       test_unverified_source_read_only_transition_fails_closed},
      {"local_path_disk_mapping_rejects_nonlocal_and_parent_traversal",
       test_local_path_disk_mapping_rejects_nonlocal_and_parent_traversal},
  };

  int failed = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
    }
  }
  return failed == 0 ? 0 : 1;
}

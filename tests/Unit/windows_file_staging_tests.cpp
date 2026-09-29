#include "ytec/imageformat/windows_file_staging.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
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

ytec::clonecore::Error mock_error(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

ytec::clonecore::StableDiskIdentity identity(
    const std::uint32_t number,
    std::wstring instance,
    std::string serial) {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = number,
      .model = L"MOCK DISK " + std::to_wstring(number),
      .size_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512,
      .serial_suffix = std::move(serial),
      .device_instance_id = std::move(instance),
      .is_system_disk = false,
  };
}

ytec::imageformat::WindowsFileStagingRequest request() {
  return ytec::imageformat::WindowsFileStagingRequest{
      .final_path = L"D:\\backup\\system.dcimg",
      .expected_source_disk =
          identity(1, L"MOCK\\SOURCE\\ONE", "SOURCE01"),
      .expected_clone_target_disk = std::nullopt,
  };
}

class MockBackend final
    : public ytec::imageformat::IWindowsFileStagingBackend {
 public:
  MockBackend() {
    observation.canonical_final_path =
        L"D:\\backup\\system.dcimg";
    observation.partial_path =
        observation.canonical_final_path + L".partial";
    observation.destination_disk =
        identity(2, L"MOCK\\DESTINATION\\TWO", "DEST0002");
    observation.connected_disks = {
        request().expected_source_disk,
        observation.destination_disk,
    };
    observation.available_bytes =
        4ULL * 1024ULL * 1024ULL * 1024ULL;
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::imageformat::WindowsFileDestinationObservation>
  observe_destination(const std::wstring&) override {
    ++observe_count;
    auto value = observation;
    value.partial_exists = value.partial_exists || owns;
    value.final_exists = value.final_exists || committed;
    if (observe_count >= change_identity_at_observation) {
      value.destination_disk =
          identity(9, L"MOCK\\REPLACED\\NINE", "REPLACE9");
    }
    const auto destination_present = std::any_of(
        value.connected_disks.begin(),
        value.connected_disks.end(),
        [&](const ytec::clonecore::StableDiskIdentity& candidate) {
          return candidate.device_instance_id ==
              value.destination_disk.device_instance_id;
        });
    if (!destination_present) {
      value.connected_disks.push_back(value.destination_disk);
    }
    if (observe_count >= disconnect_source_at_observation) {
      const auto source = request().expected_source_disk;
      std::erase_if(
          value.connected_disks,
          [&](const ytec::clonecore::StableDiskIdentity& candidate) {
            return candidate.device_instance_id ==
                source.device_instance_id;
          });
    }
    return ytec::clonecore::Result<
        ytec::imageformat::WindowsFileDestinationObservation>::success(
            std::move(value));
  }

  [[nodiscard]] ytec::clonecore::Status
  create_new_restricted_partial(
      const std::wstring& partial_path,
      const std::uint64_t expected_length) override {
    ++create_count;
    if (fail_create_before_ownership) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モック新規作成", L"所有前失敗"));
    }
    owns = true;
    open = true;
    owned_path = partial_path;
    bytes.assign(
        static_cast<std::size_t>(expected_length), std::byte{0});
    if (fail_create_after_ownership) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モック長予約", L"所有後失敗"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] bool owns_partial() const noexcept override {
    return owns;
  }

  [[nodiscard]] ytec::clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> data) override {
    if (!open || offset > bytes.size() ||
        data.size() > bytes.size() - offset) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モック書込み", L"範囲外"));
    }
    std::copy(
        data.begin(),
        data.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]]
  ytec::clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) override {
    if (!open || offset > bytes.size() ||
        length > bytes.size() - offset) {
      return ytec::clonecore::Result<
          std::vector<std::byte>>::failure(
              mock_error(L"モック読戻し", L"範囲外"));
    }
    return ytec::clonecore::Result<
        std::vector<std::byte>>::success(
            std::vector<std::byte>(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() +
                    static_cast<std::ptrdiff_t>(offset + length)));
  }

  [[nodiscard]] ytec::clonecore::Status resize_before_verification(
      const std::uint64_t final_length) override {
    if (!open || final_length == 0U || final_length > bytes.size()) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モック最終長", L"範囲外"));
    }
    bytes.resize(static_cast<std::size_t>(final_length));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status flush() override {
    ++flush_count;
    if (!open) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モックflush", L"未オープン"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status close_file() override {
    ++close_count;
    open = false;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status commit_no_replace(
      const std::wstring& partial_path,
      const std::wstring& final_path) override {
    ++commit_count;
    if (fail_commit || !owns || !open ||
        partial_path != owned_path ||
        final_path != observation.canonical_final_path) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モック非上書き確定", L"確定失敗"));
    }
    owns = false;
    open = false;
    committed = true;
    owned_path.clear();
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status remove_owned_partial(
      const std::wstring& partial_path) override {
    ++remove_count;
    if (fail_remove) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モック未完了破棄", L"破棄失敗"));
    }
    if (owns && partial_path == owned_path) {
      owns = false;
      open = false;
      owned_path.clear();
      bytes.clear();
    }
    return ytec::clonecore::success_status();
  }

  ytec::imageformat::WindowsFileDestinationObservation observation;
  std::vector<std::byte> bytes;
  std::wstring owned_path;
  std::size_t observe_count{};
  std::size_t create_count{};
  std::size_t flush_count{};
  std::size_t close_count{};
  std::size_t commit_count{};
  std::size_t remove_count{};
  std::size_t change_identity_at_observation{
      std::numeric_limits<std::size_t>::max()};
  std::size_t disconnect_source_at_observation{
      std::numeric_limits<std::size_t>::max()};
  bool owns{};
  bool open{};
  bool committed{};
  bool fail_create_before_ownership{};
  bool fail_create_after_ownership{};
  bool fail_commit{};
  bool fail_remove{};
};

struct TargetAndBackend final {
  std::unique_ptr<ytec::imageformat::IDcimgStagingTarget> target;
  MockBackend* backend{};
};

TargetAndBackend make_target(
    ytec::imageformat::WindowsFileStagingRequest value = request(),
    std::unique_ptr<MockBackend> backend =
        std::make_unique<MockBackend>()) {
  MockBackend* raw = backend.get();
  auto result =
      ytec::imageformat::make_windows_file_staging_target_with_backend(
          value, std::move(backend));
  check(result.has_value(), "A valid target factory request should pass");
  return TargetAndBackend{
      .target = result.take_value(),
      .backend = raw,
  };
}

void test_verified_file_flow_commits_without_replace() {
  auto pair = make_target();
  check(pair.target->begin(4096).has_value(),
        "A distinct destination should begin");
  const std::vector<std::byte> data{
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
  check(pair.target->write_at(128, data).has_value(),
        "Bounded file write should pass");
  const auto readback = pair.target->read_at(128, data.size());
  check(readback.has_value() && readback.value() == data,
        "File readback should match");
  check(pair.target->flush().has_value(), "Explicit flush should pass");
  check(pair.target->commit_verified().has_value(),
        "Verified file should commit");
  check(pair.backend->committed && !pair.backend->owns,
        "Commit must consume only the owned partial file");
  check(pair.backend->commit_count == 1 &&
            pair.backend->remove_count == 0,
        "Success must rename without abort cleanup");
}

void test_source_disk_destination_is_rejected_before_create() {
  auto backend = std::make_unique<MockBackend>();
  backend->observation.destination_disk = request().expected_source_disk;
  auto pair = make_target(request(), std::move(backend));
  const auto result = pair.target->begin(4096);
  check(!result.has_value(), "Source disk destination must fail");
  check(pair.backend->create_count == 0 && !pair.backend->owns,
        "Source-disk rejection must happen before file creation");
}

void test_missing_source_identity_is_rejected_before_create() {
  auto backend = std::make_unique<MockBackend>();
  backend->observation.connected_disks.erase(
      backend->observation.connected_disks.begin());
  auto pair = make_target(request(), std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "A fabricated or disconnected source identity must fail");
  check(pair.backend->create_count == 0,
        "Missing source must be rejected before file creation");
}

void test_duplicate_source_identity_is_rejected_before_create() {
  auto backend = std::make_unique<MockBackend>();
  auto duplicate = request().expected_source_disk;
  duplicate.disk_number = 7;
  backend->observation.connected_disks.push_back(duplicate);
  auto pair = make_target(request(), std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "A non-unique source identity must fail closed");
  check(pair.backend->create_count == 0,
        "Ambiguous source identity must precede file creation");
}

void test_clone_target_destination_is_rejected_before_create() {
  auto value = request();
  value.expected_clone_target_disk =
      identity(3, L"MOCK\\CLONE\\TARGET", "TARGET03");
  auto backend = std::make_unique<MockBackend>();
  backend->observation.destination_disk =
      *value.expected_clone_target_disk;
  auto pair = make_target(value, std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "Clone target disk destination must fail");
  check(pair.backend->create_count == 0,
        "Clone-target rejection must happen before creation");
}

void test_distinct_clone_target_is_reidentified_and_allowed() {
  auto value = request();
  value.expected_clone_target_disk =
      identity(3, L"MOCK\\CLONE\\TARGET", "TARGET03");
  auto backend = std::make_unique<MockBackend>();
  backend->observation.connected_disks.push_back(
      *value.expected_clone_target_disk);
  auto pair = make_target(value, std::move(backend));
  check(pair.target->begin(4096).has_value(),
        "A distinct connected clone target should pass preflight");
  check(pair.target->abort_incomplete().has_value(),
        "The owned test partial should remain abortable");
}

void test_insufficient_space_is_rejected_before_create() {
  auto backend = std::make_unique<MockBackend>();
  backend->observation.available_bytes = 4095;
  auto pair = make_target(request(), std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "Insufficient free space must fail");
  check(pair.backend->create_count == 0,
        "Capacity failure must not create partial output");
}

void test_reparse_parent_is_rejected_before_create() {
  auto backend = std::make_unique<MockBackend>();
  backend->observation.parent_is_reparse = true;
  auto pair = make_target(request(), std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "Reparse parent must fail");
  check(pair.backend->create_count == 0,
        "Reparse failure must precede file creation");
}

void test_existing_final_is_never_overwritten() {
  auto backend = std::make_unique<MockBackend>();
  backend->observation.final_exists = true;
  auto pair = make_target(request(), std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "Existing final file must fail");
  check(pair.backend->create_count == 0 &&
            pair.backend->commit_count == 0,
        "Existing final must never be opened or replaced");
}

void test_existing_partial_is_never_claimed_or_deleted() {
  auto backend = std::make_unique<MockBackend>();
  backend->observation.partial_exists = true;
  auto pair = make_target(request(), std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "Existing partial file must fail");
  check(pair.target->abort_incomplete().has_value(),
        "Abort without ownership should be a no-op");
  check(pair.backend->create_count == 0 &&
            pair.backend->remove_count == 0,
        "A pre-existing partial must never be claimed or removed");
}

void test_destination_identity_change_blocks_commit_and_aborts() {
  auto backend = std::make_unique<MockBackend>();
  backend->change_identity_at_observation = 3;
  auto pair = make_target(request(), std::move(backend));
  check(pair.target->begin(4096).has_value(),
        "Initial destination should begin");
  check(!pair.target->commit_verified().has_value(),
        "Destination replacement before commit must fail");
  check(pair.backend->commit_count == 0,
        "Identity change must block the final rename");
  check(pair.target->abort_incomplete().has_value(),
        "Owned partial should still be removable");
  check(pair.backend->remove_count == 1 && !pair.backend->owns,
        "Identity change cleanup must remove only owned partial");
}

void test_source_disappearance_blocks_commit_and_aborts() {
  auto backend = std::make_unique<MockBackend>();
  backend->disconnect_source_at_observation = 3;
  auto pair = make_target(request(), std::move(backend));
  check(pair.target->begin(4096).has_value(),
        "Connected source should allow begin");
  check(!pair.target->commit_verified().has_value(),
        "Source disappearance before commit must fail");
  check(pair.backend->commit_count == 0,
        "Missing source must block final rename");
  check(pair.target->abort_incomplete().has_value(),
        "Owned partial should be removed after source loss");
  check(pair.backend->remove_count == 1 && !pair.backend->owns,
        "Source-loss cleanup must remove only the owned partial");
}

void test_commit_failure_preserves_ownership_for_abort() {
  auto backend = std::make_unique<MockBackend>();
  backend->fail_commit = true;
  auto pair = make_target(request(), std::move(backend));
  check(pair.target->begin(4096).has_value(), "Begin should pass");
  check(!pair.target->commit_verified().has_value(),
        "Injected rename failure must fail");
  check(pair.backend->owns,
        "Failed commit must retain partial ownership");
  check(pair.target->abort_incomplete().has_value(),
        "Failed commit must remain abortable");
  check(!pair.backend->owns && pair.backend->remove_count == 1,
        "Abort should remove the owned partial");
}

void test_create_failure_after_ownership_remains_abortable() {
  auto backend = std::make_unique<MockBackend>();
  backend->fail_create_after_ownership = true;
  auto pair = make_target(request(), std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "Post-create allocation failure must fail begin");
  check(pair.backend->owns,
        "Backend must expose ownership after partial creation");
  check(pair.target->abort_incomplete().has_value(),
        "Partially-created file must be abortable");
  check(!pair.backend->owns && pair.backend->remove_count == 1,
        "Abort must remove the partially-created owned file");
}

void test_create_failure_before_ownership_deletes_nothing() {
  auto backend = std::make_unique<MockBackend>();
  backend->fail_create_before_ownership = true;
  auto pair = make_target(request(), std::move(backend));
  check(!pair.target->begin(4096).has_value(),
        "Pre-create failure must fail begin");
  check(pair.target->abort_incomplete().has_value(),
        "Abort without ownership should pass");
  check(pair.backend->remove_count == 0,
        "No file may be deleted when ownership was never acquired");
}

void test_invalid_extension_is_rejected_by_factory() {
  auto value = request();
  value.final_path = L"D:\\backup\\system.zip";
  auto result =
      ytec::imageformat::make_windows_file_staging_target_with_backend(
          value, std::make_unique<MockBackend>());
  check(!result.has_value(),
        "Only the working .dcimg extension should be accepted");
}

void test_abort_failure_is_reported_and_not_committed() {
  auto backend = std::make_unique<MockBackend>();
  backend->fail_remove = true;
  auto pair = make_target(request(), std::move(backend));
  check(pair.target->begin(4096).has_value(), "Begin should pass");
  check(!pair.target->abort_incomplete().has_value(),
        "Partial removal failure must be reported");
  check(pair.backend->owns && !pair.backend->committed,
        "Failed cleanup must not become committed");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"verified_file_flow_commits_without_replace",
       test_verified_file_flow_commits_without_replace},
      {"source_disk_destination_is_rejected_before_create",
       test_source_disk_destination_is_rejected_before_create},
      {"missing_source_identity_is_rejected_before_create",
       test_missing_source_identity_is_rejected_before_create},
      {"duplicate_source_identity_is_rejected_before_create",
       test_duplicate_source_identity_is_rejected_before_create},
      {"clone_target_destination_is_rejected_before_create",
       test_clone_target_destination_is_rejected_before_create},
      {"distinct_clone_target_is_reidentified_and_allowed",
       test_distinct_clone_target_is_reidentified_and_allowed},
      {"insufficient_space_is_rejected_before_create",
       test_insufficient_space_is_rejected_before_create},
      {"reparse_parent_is_rejected_before_create",
       test_reparse_parent_is_rejected_before_create},
      {"existing_final_is_never_overwritten",
       test_existing_final_is_never_overwritten},
      {"existing_partial_is_never_claimed_or_deleted",
       test_existing_partial_is_never_claimed_or_deleted},
      {"destination_identity_change_blocks_commit_and_aborts",
       test_destination_identity_change_blocks_commit_and_aborts},
      {"source_disappearance_blocks_commit_and_aborts",
       test_source_disappearance_blocks_commit_and_aborts},
      {"commit_failure_preserves_ownership_for_abort",
       test_commit_failure_preserves_ownership_for_abort},
      {"create_failure_after_ownership_remains_abortable",
       test_create_failure_after_ownership_remains_abortable},
      {"create_failure_before_ownership_deletes_nothing",
       test_create_failure_before_ownership_deletes_nothing},
      {"invalid_extension_is_rejected_by_factory",
       test_invalid_extension_is_rejected_by_factory},
      {"abort_failure_is_reported_and_not_committed",
       test_abort_failure_is_reported_and_not_committed},
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
      std::cerr << "FAIL " << name
                << ": unexpected exception: " << exception.what()
                << '\n';
    }
  }
  std::cout << (tests.size() - failures) << "/" << tests.size()
            << " tests passed\n";
  return failures == 0 ? 0 : 1;
}

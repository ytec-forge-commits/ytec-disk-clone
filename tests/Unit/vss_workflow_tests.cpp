#include "ytec/vssrequester/snapshot_reader.h"
#include "ytec/vssrequester/workflow.h"
#include "ytec/vssrequester/windows_backend.h"

#include <Windows.h>
#include <vsserror.h>

#include <cstddef>
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

std::wstring volume_path(const wchar_t suffix) {
  return std::wstring(L"\\\\?\\Volume{00000000-0000-0000-0000-") +
         std::wstring(11, L'0') + suffix + L"}\\";
}

std::wstring guid_value(const wchar_t suffix) {
  return std::wstring(L"{00000000-0000-0000-0000-") +
         std::wstring(11, L'0') + suffix + L"}";
}

ytec::vssrequester::WorkflowRequest valid_request() {
  return ytec::vssrequester::WorkflowRequest{
      .administrator = true,
      .volumes = {
          ytec::vssrequester::VolumeRequest{
              .volume_guid_path = volume_path(L'1'),
              .file_system = L"NTFS",
          },
          ytec::vssrequester::VolumeRequest{
              .volume_guid_path = volume_path(L'2'),
              .file_system = L"ntfs",
          },
      },
  };
}

class MockBackend final : public ytec::vssrequester::IWorkflowBackend {
 public:
  ytec::clonecore::Status step(const std::string& name) {
    calls.push_back(name);
    if (fail_at == name) {
      return ytec::clonecore::Status::failure(
          mock_error(std::wstring(name.begin(), name.end())));
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status initialize_components() override {
    return step("initialize");
  }
  ytec::clonecore::Status set_backup_state() override {
    return step("set-backup-state");
  }
  ytec::clonecore::Status gather_writer_metadata() override {
    return step("gather-metadata");
  }
  ytec::clonecore::Result<std::wstring> start_snapshot_set() override {
    calls.push_back("start-snapshot-set");
    if (fail_at == "start-snapshot-set") {
      return ytec::clonecore::Result<std::wstring>::failure(
          mock_error(L"start-snapshot-set"));
    }
    return ytec::clonecore::Result<std::wstring>::success(snapshot_set_id);
  }
  ytec::clonecore::Status add_volume(
      const std::wstring& received_snapshot_set_id,
      const std::wstring& volume_guid_path) override {
    check(received_snapshot_set_id == snapshot_set_id,
          "Snapshot set identity must remain fixed");
    added_volumes.push_back(volume_guid_path);
    return step("add-volume");
  }
  ytec::clonecore::Status prepare_for_backup() override {
    return step("prepare");
  }
  ytec::clonecore::Status do_snapshot_set() override {
    return step("snapshot");
  }
  ytec::clonecore::Result<std::vector<ytec::vssrequester::WriterStatus>>
  query_writer_statuses() override {
    calls.push_back("query-writers");
    if (fail_at == "query-writers") {
      return ytec::clonecore::Result<
          std::vector<ytec::vssrequester::WriterStatus>>::failure(
          mock_error(L"query-writers"));
    }
    return ytec::clonecore::Result<
        std::vector<ytec::vssrequester::WriterStatus>>::success(writers);
  }
  ytec::clonecore::Result<std::vector<ytec::vssrequester::SnapshotMapping>>
  query_snapshot_devices(
      const std::wstring& received_snapshot_set_id,
      const std::vector<ytec::vssrequester::VolumeRequest>& volumes) override {
    calls.push_back("query-snapshot-devices");
    check(received_snapshot_set_id == snapshot_set_id,
          "Mapping query must use the created set");
    if (fail_at == "query-snapshot-devices") {
      return ytec::clonecore::Result<
          std::vector<ytec::vssrequester::SnapshotMapping>>::failure(
          mock_error(L"query-snapshot-devices"));
    }
    if (mappings.empty()) {
      for (std::size_t index = 0; index < volumes.size(); ++index) {
        mappings.push_back(ytec::vssrequester::SnapshotMapping{
            .original_volume_guid_path = volumes[index].volume_guid_path,
            .snapshot_id = guid_value(
                static_cast<wchar_t>(L'1' + index)),
            .snapshot_device_path =
                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy" +
                std::to_wstring(index + 1),
            .provider_id = L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}",
            .creation_timestamp =
                static_cast<std::int64_t>(1'000U + index),
        });
      }
    }
    return ytec::clonecore::Result<
        std::vector<ytec::vssrequester::SnapshotMapping>>::success(mappings);
  }
  ytec::clonecore::Status copy_snapshot_data(
      const std::vector<ytec::vssrequester::SnapshotMapping>& received)
      override {
    copied_mapping_count = received.size();
    return step("copy");
  }
  ytec::clonecore::Status backup_complete() override {
    return step("backup-complete");
  }
  ytec::clonecore::Status delete_snapshot_set(
      const std::wstring& received_snapshot_set_id) override {
    check(received_snapshot_set_id == snapshot_set_id ||
              snapshot_set_id.empty(),
          "Cleanup must target only the created set");
    return step("delete");
  }

  std::vector<std::string> calls;
  std::vector<std::wstring> added_volumes;
  std::vector<ytec::vssrequester::WriterStatus> writers{
      ytec::vssrequester::WriterStatus{
          .name = L"System Writer",
          .state = ytec::vssrequester::WriterState::stable,
          .status_code = S_OK,
      },
  };
  std::vector<ytec::vssrequester::SnapshotMapping> mappings;
  std::wstring snapshot_set_id = L"{11111111-1111-1111-1111-111111111111}";
  std::string fail_at;
  std::size_t copied_mapping_count{};
};

class FakeAsyncOperation final
    : public ytec::vssrequester::IVssAsyncOperation {
 public:
  HRESULT wait(const DWORD milliseconds) noexcept override {
    wait_slices.push_back(milliseconds);
    return wait_result;
  }

  HRESULT query_status(HRESULT* const operation_status) noexcept override {
    ++query_calls;
    if (FAILED(query_result)) {
      return query_result;
    }
    if (operation_status == nullptr) {
      return E_POINTER;
    }
    if (statuses.empty()) {
      *operation_status = VSS_S_ASYNC_PENDING;
      return S_OK;
    }
    const std::size_t index =
        (std::min)(status_index, statuses.size() - 1);
    *operation_status = statuses[index];
    ++status_index;
    return S_OK;
  }

  HRESULT cancel() noexcept override {
    ++cancel_calls;
    return cancel_result;
  }

  std::vector<DWORD> wait_slices;
  std::vector<HRESULT> statuses;
  std::size_t status_index{};
  std::size_t query_calls{};
  std::size_t cancel_calls{};
  HRESULT wait_result{S_OK};
  HRESULT query_result{S_OK};
  HRESULT cancel_result{S_OK};
};

class DummySnapshotReader final
    : public ytec::clonecore::ISourceDiskReader {
 public:
  std::uint64_t size_bytes() const noexcept override {
    return 1024ULL * 1024ULL;
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return 512;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t,
      const std::size_t length) const override {
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(length));
  }
};

class MockSnapshotVolumeBackend final
    : public ytec::vssrequester::ISnapshotVolumeBackend {
 public:
  ytec::clonecore::Result<
      std::unique_ptr<ytec::clonecore::ISourceDiskReader>>
  open_read_only(
      const ytec::vssrequester::SnapshotVolumeOpenRequest& request)
      override {
    ++open_calls;
    received = request;
    return ytec::clonecore::Result<
        std::unique_ptr<ytec::clonecore::ISourceDiskReader>>::success(
        std::make_unique<DummySnapshotReader>());
  }

  std::size_t open_calls{};
  ytec::vssrequester::SnapshotVolumeOpenRequest received;
};

void test_success_uses_specified_order_and_cleans_up() {
  MockBackend backend;
  const auto result =
      ytec::vssrequester::execute_backup_workflow(valid_request(), backend);
  check(result.has_value(), "Valid mock VSS workflow should pass");
  const std::vector<std::string> expected{
      "initialize",          "set-backup-state", "gather-metadata",
      "start-snapshot-set", "add-volume",       "add-volume",
      "prepare",            "snapshot",         "query-writers",
      "query-snapshot-devices", "copy",          "backup-complete",
      "delete"};
  check(backend.calls == expected, "VSS steps must follow the fixed order");
  check(backend.copied_mapping_count == 2,
        "Copy must receive one verified mapping per volume");
  check(result.value().backup_completed && result.value().snapshots_deleted,
        "Report must record completion and cleanup");
}

void test_non_administrator_stops_before_backend() {
  auto request = valid_request();
  request.administrator = false;
  MockBackend backend;
  const auto result =
      ytec::vssrequester::execute_backup_workflow(request, backend);
  check(!result.has_value(), "Non-administrator must fail closed");
  check(result.error().code == ytec::clonecore::ErrorCode::access_denied,
        "Privilege failure must be explicit");
  check(backend.calls.empty(), "No VSS operation may start without privilege");
}

void test_invalid_or_duplicate_volume_stops_before_backend() {
  auto invalid = valid_request();
  invalid.volumes[0].volume_guid_path = L"C:\\";
  MockBackend invalid_backend;
  const auto invalid_result =
      ytec::vssrequester::execute_backup_workflow(invalid, invalid_backend);
  check(!invalid_result.has_value() && invalid_backend.calls.empty(),
        "Drive letters must not reach VSS");

  auto duplicate = valid_request();
  duplicate.volumes[1].volume_guid_path =
      duplicate.volumes[0].volume_guid_path;
  MockBackend duplicate_backend;
  const auto duplicate_result =
      ytec::vssrequester::execute_backup_workflow(
          duplicate, duplicate_backend);
  check(!duplicate_result.has_value() && duplicate_backend.calls.empty(),
        "Duplicate volumes must not reach VSS");
}

void test_writer_failure_prevents_copy_and_deletes_snapshot() {
  MockBackend backend;
  backend.writers[0].state = ytec::vssrequester::WriterState::failed;
  backend.writers[0].status_code = E_FAIL;
  const auto result =
      ytec::vssrequester::execute_backup_workflow(valid_request(), backend);
  check(!result.has_value(), "Failed writer must stop backup");
  check(backend.copied_mapping_count == 0,
        "Writer failure must stop before snapshot data copy");
  check(backend.calls.back() == "delete",
        "Writer failure must still delete the snapshot set");
}

void test_writer_waiting_for_backup_complete_is_healthy() {
  MockBackend backend;
  backend.writers[0].state =
      ytec::vssrequester::WriterState::waiting_for_backup_complete;
  backend.writers[0].status_code = S_OK;
  const auto result =
      ytec::vssrequester::execute_backup_workflow(valid_request(), backend);
  check(result.has_value(),
        "Writer waiting for BackupComplete must remain a healthy state");
  check(backend.copied_mapping_count == valid_request().volumes.size(),
        "Healthy waiting writer must allow verified snapshot copy");
  check(result.value().backup_completed && result.value().snapshots_deleted,
        "Waiting writer path must still complete and clean up");
}

void test_empty_writer_state_is_not_ignored() {
  MockBackend backend;
  backend.writers.clear();
  const auto result =
      ytec::vssrequester::execute_backup_workflow(valid_request(), backend);
  check(!result.has_value(), "Missing writer state must fail closed");
  check(backend.calls.back() == "delete",
        "Missing writer state must clean up snapshots");
}

void test_wrong_snapshot_mapping_prevents_copy() {
  MockBackend backend;
  backend.mappings = {
      ytec::vssrequester::SnapshotMapping{
          .original_volume_guid_path = volume_path(L'1'),
          .snapshot_id = guid_value(L'1'),
          .snapshot_device_path =
              L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy1",
          .provider_id = L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}",
          .creation_timestamp = 1'001,
      },
  };
  const auto result =
      ytec::vssrequester::execute_backup_workflow(valid_request(), backend);
  check(!result.has_value(), "Incomplete mapping must fail closed");
  check(backend.copied_mapping_count == 0,
        "Unverified snapshot mapping must not be read");
  check(backend.calls.back() == "delete",
        "Mapping failure must delete the snapshot set");
}

void test_duplicate_snapshot_id_prevents_copy() {
  MockBackend backend;
  backend.mappings = {
      ytec::vssrequester::SnapshotMapping{
          .original_volume_guid_path = volume_path(L'1'),
          .snapshot_id = guid_value(L'9'),
          .snapshot_device_path =
              L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy1",
          .provider_id = L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}",
          .creation_timestamp = 1'001,
      },
      ytec::vssrequester::SnapshotMapping{
          .original_volume_guid_path = volume_path(L'2'),
          .snapshot_id = guid_value(L'9'),
          .snapshot_device_path =
              L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy2",
          .provider_id = L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}",
          .creation_timestamp = 1'002,
      },
  };
  const auto result =
      ytec::vssrequester::execute_backup_workflow(valid_request(), backend);
  check(!result.has_value() && backend.copied_mapping_count == 0,
        "Duplicate Snapshot IDs must fail before the copy callback");
  check(backend.calls.back() == "delete",
        "Duplicate Snapshot IDs must still delete the Snapshot set");
}

void test_failure_after_set_creation_always_attempts_cleanup() {
  for (const std::string step : {
           "add-volume", "prepare", "snapshot", "query-writers",
           "query-snapshot-devices", "copy", "backup-complete"}) {
    MockBackend backend;
    backend.fail_at = step;
    const auto result =
        ytec::vssrequester::execute_backup_workflow(valid_request(), backend);
    check(!result.has_value(), "Injected backend failure must fail workflow");
    check(backend.calls.back() == "delete",
          "Every post-creation failure must attempt cleanup");
  }
}

void test_delete_failure_is_never_reported_as_success() {
  MockBackend backend;
  backend.fail_at = "delete";
  const auto result =
      ytec::vssrequester::execute_backup_workflow(valid_request(), backend);
  check(!result.has_value(), "Snapshot deletion failure must fail the job");
  check(backend.copied_mapping_count == 2,
        "Deletion failure occurs only after verified copy");
}

void test_async_pending_then_finished_passes() {
  FakeAsyncOperation operation;
  operation.statuses = {VSS_S_ASYNC_PENDING, S_OK};
  const auto status = ytec::vssrequester::wait_for_vss_async(
      operation,
      ytec::vssrequester::AsyncWaitOptions{
          .timeout_ms = 100,
          .poll_interval_ms = 20,
      },
      L"モック非同期待機");
  check(status.has_value(), "Pending then S_OK must pass");
  check(
      operation.wait_slices == std::vector<DWORD>({20, 20}),
      "Wait must use bounded polling slices");
  check(operation.cancel_calls == 0, "Completed work must not be cancelled");
}

void test_async_explicit_cancel_stops_before_wait() {
  FakeAsyncOperation operation;
  const auto status = ytec::vssrequester::wait_for_vss_async(
      operation,
      ytec::vssrequester::AsyncWaitOptions{
          .timeout_ms = 100,
          .poll_interval_ms = 20,
          .cancellation_requested = []() { return true; },
      },
      L"モックキャンセル");
  check(!status.has_value(), "Cancellation request must fail the wait");
  check(operation.wait_slices.empty(), "Cancelled work must not wait again");
  check(operation.cancel_calls == 1, "Cancellation must call IVssAsync::Cancel");
  check(
      operation.status_index == 0,
      "Cancelled work must not query stale completion state");
}

void test_async_timeout_cancels_with_final_short_slice() {
  FakeAsyncOperation operation;
  operation.statuses = {VSS_S_ASYNC_PENDING};
  const auto status = ytec::vssrequester::wait_for_vss_async(
      operation,
      ytec::vssrequester::AsyncWaitOptions{
          .timeout_ms = 50,
          .poll_interval_ms = 20,
      },
      L"モックtimeout");
  check(!status.has_value(), "Permanent pending state must time out");
  check(
      operation.wait_slices == std::vector<DWORD>({20, 20, 10}),
      "Timeout budget must not be exceeded by polling");
  check(operation.cancel_calls == 1, "Timeout must cancel the VSS operation");
  check(
      operation.query_calls == 3,
      "Each bounded wait must be followed by one status query");
}

void test_async_query_failure_cancels_and_preserves_hresult() {
  FakeAsyncOperation operation;
  operation.query_result = E_ACCESSDENIED;
  const auto status = ytec::vssrequester::wait_for_vss_async(
      operation,
      ytec::vssrequester::AsyncWaitOptions{
          .timeout_ms = 100,
          .poll_interval_ms = 20,
      },
      L"モック状態取得");
  check(!status.has_value(), "QueryStatus failure must fail");
  check(
      status.error().native_code == static_cast<DWORD>(E_ACCESSDENIED),
      "QueryStatus HRESULT must be preserved");
  check(operation.cancel_calls == 1, "Query failure must attempt cancel");
}

void test_async_operation_failure_preserves_hresult() {
  FakeAsyncOperation operation;
  operation.statuses = {VSS_E_WRITERERROR_TIMEOUT};
  const auto status = ytec::vssrequester::wait_for_vss_async(
      operation,
      ytec::vssrequester::AsyncWaitOptions{
          .timeout_ms = 100,
          .poll_interval_ms = 20,
      },
      L"モックWriter失敗");
  check(!status.has_value(), "VSS operation HRESULT failure must fail");
  check(
      status.error().native_code ==
          static_cast<DWORD>(VSS_E_WRITERERROR_TIMEOUT),
      "Operation HRESULT must be preserved");
}

void test_async_invalid_options_fail_without_touching_operation() {
  FakeAsyncOperation operation;
  for (const auto options : {
           ytec::vssrequester::AsyncWaitOptions{
               .timeout_ms = 0,
               .poll_interval_ms = 20,
           },
           ytec::vssrequester::AsyncWaitOptions{
               .timeout_ms = INFINITE,
               .poll_interval_ms = 20,
           },
           ytec::vssrequester::AsyncWaitOptions{
               .timeout_ms = 20,
               .poll_interval_ms = INFINITE,
           },
       }) {
    const auto status = ytec::vssrequester::wait_for_vss_async(
        operation,
        options,
        L"モック不正設定");
    check(!status.has_value(), "Unbounded wait configuration must fail");
  }
  check(
      operation.wait_slices.empty() && operation.query_calls == 0 &&
          operation.cancel_calls == 0,
      "Invalid configuration must not touch the VSS operation");
}

void test_windows_backend_requires_snapshot_only_copy_callback() {
  ytec::vssrequester::WindowsVssBackend backend(
      ytec::vssrequester::WindowsVssBackendOptions{});
  const auto status = backend.initialize_components();
  check(!status.has_value(), "Missing snapshot callback must fail");
  check(
      status.error().code ==
          ytec::clonecore::ErrorCode::invalid_argument,
      "Missing callback must be reported as configuration error");
}

void test_snapshot_reader_rejects_live_path_before_backend() {
  MockSnapshotVolumeBackend backend;
  const auto result = ytec::vssrequester::open_snapshot_volume_reader(
      ytec::vssrequester::SnapshotVolumeOpenRequest{
          .snapshot_device_path =
              L"\\\\?\\Volume{00000000-0000-0000-0000-000000000001}\\",
          .expected_size_bytes = 1024ULL * 1024ULL,
          .logical_sector_size = 512,
      },
      backend);
  check(!result.has_value(), "Live Volume GUID must be rejected");
  check(
      backend.open_calls == 0,
      "Live path must not reach the Windows/open backend");
}

void test_snapshot_reader_rejects_invalid_geometry_before_backend() {
  for (const auto request : {
           ytec::vssrequester::SnapshotVolumeOpenRequest{
               .snapshot_device_path =
                   L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy1",
               .expected_size_bytes = 0,
               .logical_sector_size = 512,
           },
           ytec::vssrequester::SnapshotVolumeOpenRequest{
               .snapshot_device_path =
                   L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy1",
               .expected_size_bytes = 1025,
               .logical_sector_size = 512,
           },
           ytec::vssrequester::SnapshotVolumeOpenRequest{
               .snapshot_device_path =
                   L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy1",
               .expected_size_bytes = 1024ULL * 1024ULL,
               .logical_sector_size = 2048,
           },
       }) {
    MockSnapshotVolumeBackend backend;
    const auto result =
        ytec::vssrequester::open_snapshot_volume_reader(request, backend);
    check(!result.has_value(), "Invalid Snapshot geometry must fail");
    check(
        backend.open_calls == 0,
        "Invalid geometry must not reach the open backend");
  }
}

void test_snapshot_reader_valid_request_reaches_backend_once() {
  MockSnapshotVolumeBackend backend;
  const ytec::vssrequester::SnapshotVolumeOpenRequest request{
      .snapshot_device_path =
          L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy42",
      .expected_size_bytes = 1024ULL * 1024ULL,
      .logical_sector_size = 512,
  };
  const auto result =
      ytec::vssrequester::open_snapshot_volume_reader(request, backend);
  check(result.has_value(), "Valid Snapshot request must reach backend");
  check(backend.open_calls == 1, "Backend must be called exactly once");
  check(
      backend.received.snapshot_device_path ==
          request.snapshot_device_path,
      "Verified Snapshot identity must not change");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"success_uses_specified_order_and_cleans_up",
       test_success_uses_specified_order_and_cleans_up},
      {"non_administrator_stops_before_backend",
       test_non_administrator_stops_before_backend},
      {"invalid_or_duplicate_volume_stops_before_backend",
       test_invalid_or_duplicate_volume_stops_before_backend},
      {"writer_failure_prevents_copy_and_deletes_snapshot",
       test_writer_failure_prevents_copy_and_deletes_snapshot},
      {"writer_waiting_for_backup_complete_is_healthy",
       test_writer_waiting_for_backup_complete_is_healthy},
      {"empty_writer_state_is_not_ignored",
       test_empty_writer_state_is_not_ignored},
      {"wrong_snapshot_mapping_prevents_copy",
       test_wrong_snapshot_mapping_prevents_copy},
      {"duplicate_snapshot_id_prevents_copy",
       test_duplicate_snapshot_id_prevents_copy},
      {"failure_after_set_creation_always_attempts_cleanup",
       test_failure_after_set_creation_always_attempts_cleanup},
      {"delete_failure_is_never_reported_as_success",
       test_delete_failure_is_never_reported_as_success},
      {"async_pending_then_finished_passes",
       test_async_pending_then_finished_passes},
      {"async_explicit_cancel_stops_before_wait",
       test_async_explicit_cancel_stops_before_wait},
      {"async_timeout_cancels_with_final_short_slice",
       test_async_timeout_cancels_with_final_short_slice},
      {"async_query_failure_cancels_and_preserves_hresult",
       test_async_query_failure_cancels_and_preserves_hresult},
      {"async_operation_failure_preserves_hresult",
       test_async_operation_failure_preserves_hresult},
      {"async_invalid_options_fail_without_touching_operation",
       test_async_invalid_options_fail_without_touching_operation},
      {"windows_backend_requires_snapshot_only_copy_callback",
       test_windows_backend_requires_snapshot_only_copy_callback},
      {"snapshot_reader_rejects_live_path_before_backend",
       test_snapshot_reader_rejects_live_path_before_backend},
      {"snapshot_reader_rejects_invalid_geometry_before_backend",
       test_snapshot_reader_rejects_invalid_geometry_before_backend},
      {"snapshot_reader_valid_request_reaches_backend_once",
       test_snapshot_reader_valid_request_reaches_backend_once},
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

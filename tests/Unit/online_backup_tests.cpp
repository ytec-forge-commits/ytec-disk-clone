#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/vssrequester/online_backup.h"

#include <Windows.h>

#include <algorithm>
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

ytec::clonecore::Error injected_error(std::wstring operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::move(operation),
      .message = L"注入した失敗です",
  };
}

struct StagingState final {
  std::size_t begin_count{};
  std::size_t commit_count{};
  std::size_t abort_count{};
  bool begun{};
  std::vector<std::byte> bytes;
};

class MockStagingTarget final
    : public ytec::imageformat::IDcimgStagingTarget {
 public:
  explicit MockStagingTarget(StagingState& state) : state_(state) {}

  ytec::clonecore::Status begin(
      const std::uint64_t expected_length) override {
    ++state_.begin_count;
    state_.begun = true;
    state_.bytes.assign(
        static_cast<std::size_t>(expected_length), std::byte{0});
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (!state_.begun || offset > state_.bytes.size() ||
        bytes.size() > state_.bytes.size() - offset) {
      return ytec::clonecore::Status::failure(
          injected_error(L"モック遅延出力書込み"));
    }
    std::copy(
        bytes.begin(),
        bytes.end(),
        state_.bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!state_.begun || offset > state_.bytes.size() ||
        length > state_.bytes.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"モック遅延出力読戻し"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            state_.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            state_.bytes.begin() +
                static_cast<std::ptrdiff_t>(offset + length)));
  }

  ytec::clonecore::Status resize_before_verification(
      const std::uint64_t final_length) override {
    if (!state_.begun || final_length == 0U ||
        final_length > state_.bytes.size()) {
      return ytec::clonecore::Status::failure(
          injected_error(L"モック遅延出力最終長"));
    }
    state_.bytes.resize(static_cast<std::size_t>(final_length));
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status flush() override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status commit_verified() override {
    ++state_.commit_count;
    state_.begun = false;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status abort_incomplete() override {
    ++state_.abort_count;
    state_.begun = false;
    state_.bytes.clear();
    return ytec::clonecore::success_status();
  }

 private:
  StagingState& state_;
};

struct BackendState final {
  bool fail_backup_complete{};
  bool fail_delete{};
  bool saw_commit_before_backup_complete{};
  bool saw_commit_before_delete{};
  std::size_t copy_count{};
  StagingState* staging{};
};

class MockWorkflowBackend final
    : public ytec::vssrequester::IWorkflowBackend {
 public:
  MockWorkflowBackend(
      BackendState& state,
      ytec::vssrequester::SnapshotCopyCallback callback)
      : state_(state), callback_(std::move(callback)) {}

  ytec::clonecore::Status initialize_components() override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status set_backup_state() override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status gather_writer_metadata() override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::wstring>
  start_snapshot_set() override {
    return ytec::clonecore::Result<std::wstring>::success(L"snapshot-set");
  }

  ytec::clonecore::Status add_volume(
      const std::wstring&,
      const std::wstring&) override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status prepare_for_backup() override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status do_snapshot_set() override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<
      std::vector<ytec::vssrequester::WriterStatus>>
  query_writer_statuses() override {
    return ytec::clonecore::Result<
        std::vector<ytec::vssrequester::WriterStatus>>::success({
        ytec::vssrequester::WriterStatus{
            .name = L"Mock Writer",
            .state = ytec::vssrequester::WriterState::stable,
            .status_code = S_OK,
        },
    });
  }

  ytec::clonecore::Result<
      std::vector<ytec::vssrequester::SnapshotMapping>>
  query_snapshot_devices(
      const std::wstring&,
      const std::vector<ytec::vssrequester::VolumeRequest>& volumes)
      override {
    return ytec::clonecore::Result<
        std::vector<ytec::vssrequester::SnapshotMapping>>::success({
        ytec::vssrequester::SnapshotMapping{
            .original_volume_guid_path =
                volumes.front().volume_guid_path,
            .snapshot_id = L"{00000000-0000-0000-0000-000000000071}",
            .snapshot_device_path =
                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy71",
            .provider_id = L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}",
            .creation_timestamp = 1'071,
        },
    });
  }

  ytec::clonecore::Status copy_snapshot_data(
      const std::vector<ytec::vssrequester::SnapshotMapping>& mappings)
      override {
    ++state_.copy_count;
    return callback_(ytec::vssrequester::SnapshotCopyContext{
        .snapshot_set_id = L"snapshot-set",
        .mappings = mappings,
    });
  }

  ytec::clonecore::Status backup_complete() override {
    state_.saw_commit_before_backup_complete =
        state_.staging->commit_count != 0;
    if (state_.fail_backup_complete) {
      return ytec::clonecore::Status::failure(
          injected_error(L"モックBackupComplete"));
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status delete_snapshot_set(
      const std::wstring&) override {
    state_.saw_commit_before_delete =
        state_.staging->commit_count != 0;
    if (state_.fail_delete) {
      return ytec::clonecore::Status::failure(
          injected_error(L"モックSnapshot削除"));
    }
    return ytec::clonecore::success_status();
  }

 private:
  BackendState& state_;
  ytec::vssrequester::SnapshotCopyCallback callback_;
};

ytec::vssrequester::PreparedSnapshotImagePlan valid_plan() {
  constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
  ytec::clonecore::StableDiskIdentity source_identity{};
  source_identity.disk_number = 0;
  source_identity.model = L"ONLINE BACKUP FIXTURE";
  source_identity.size_bytes = 64ULL * kMiB;
  source_identity.logical_sector_size = 512;
  source_identity.serial_suffix = "ONLINE01";
  source_identity.device_instance_id = L"VIRTUAL\\ONLINE_BACKUP";
  source_identity.is_system_disk = true;
  const auto manifest =
      ytec::imageformat::build_backup_manifest_v1(
          ytec::imageformat::BackupImageManifest{
              .source = source_identity,
              .physical_sector_size = 4096,
              .partition_style =
                  ytec::imageformat::BackupPartitionStyle::mbr,
              .boot_mode =
                  ytec::imageformat::BackupBootMode::legacy_bios,
              .windows_major = 10,
              .windows_minor = 0,
              .windows_build = 19045,
              .windows_architecture = "AMD64",
              .bitlocker_fully_decrypted = true,
              .compression =
                  ytec::imageformat::DcimgCompression::none,
              .compression_version = 0,
              .chunk_size =
                  ytec::imageformat::kDcimgChunkSize16MiB,
              .created_utc = "2026-07-31T12:00:00Z",
              .app_version = "0.1.0",
              .partitions = {
                  ytec::imageformat::BackupManifestPartition{
                      .table_index = 2,
                      .offset_bytes = 16ULL * kMiB,
                      .length_bytes = 32ULL * kMiB,
                      .role =
                          ytec::imageformat::BackupPartitionRole::
                              windows_ntfs,
                      .file_system =
                          ytec::imageformat::BackupFileSystem::ntfs,
                      .cluster_size = 4096,
                      .name = L"Windows",
                  },
              },
          });
  check(manifest.has_value(), "Online fixture manifest should build");
  const auto partition_snapshot =
      ytec::imageformat::build_partition_snapshot_v1(
          ytec::imageformat::PartitionSnapshot{
              .style =
                  ytec::imageformat::PartitionTableStyle::mbr,
              .source_disk_size = 64ULL * kMiB,
              .logical_sector_size = 512,
              .regions = {
                  ytec::imageformat::PartitionTableRegion{
                      .disk_offset = 0,
                      .data =
                          std::vector<std::byte>(
                              512, std::byte{0}),
                  },
              },
          });
  check(
      partition_snapshot.has_value(),
      "Online fixture partition snapshot should build");
  ytec::vssrequester::PreparedSnapshotImagePlan plan;
  plan.workflow.administrator = true;
  plan.workflow.volumes.push_back(
      ytec::vssrequester::VolumeRequest{
          .volume_guid_path =
              L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
          .file_system = L"NTFS",
      });
  plan.image_copy.source_disk_size = 64ULL * kMiB;
  plan.image_copy.logical_sector_size = 512;
  plan.image_copy.physical_sector_size = 4096;
  plan.image_copy.manifest = manifest.value();
  plan.image_copy.partition_table_snapshot =
      partition_snapshot.value();
  plan.image_copy.volumes.push_back(
      ytec::vssrequester::SnapshotImageVolumePlan{
          .partition_entry_index = 2,
          .disk_offset = 16ULL * kMiB,
          .partition_length = 32ULL * kMiB,
      });
  plan.snapshot_partition_count = 1;
  return plan;
}

ytec::vssrequester::SnapshotImageCopyExecutor successful_copy() {
  return [](const ytec::vssrequester::SnapshotImageCopyRequest&,
            const std::span<const std::wstring> paths,
            ytec::imageformat::IDcimgStagingTarget& target) {
    check(paths.size() == 1, "Copy executor should receive one Snapshot");
    const auto begun = target.begin(4);
    if (!begun) {
      return ytec::clonecore::Result<
          ytec::imageformat::DcimgStreamBuildReport>::failure(
          begun.error());
    }
    const std::vector<std::byte> bytes{
        std::byte{'T'}, std::byte{'E'}, std::byte{'S'}, std::byte{'T'}};
    const auto written = target.write_at(0, bytes);
    if (!written) {
      return ytec::clonecore::Result<
          ytec::imageformat::DcimgStreamBuildReport>::failure(
          written.error());
    }
    const auto flushed = target.flush();
    if (!flushed) {
      return ytec::clonecore::Result<
          ytec::imageformat::DcimgStreamBuildReport>::failure(
          flushed.error());
    }
    const auto committed = target.commit_verified();
    if (!committed) {
      return ytec::clonecore::Result<
          ytec::imageformat::DcimgStreamBuildReport>::failure(
          committed.error());
    }
    return ytec::clonecore::Result<
        ytec::imageformat::DcimgStreamBuildReport>::success(
        ytec::imageformat::DcimgStreamBuildReport{
            .image_length = 4,
            .stored_data_bytes = 4,
            .chunk_count = 1,
            .all_chunks_read_back_verified = true,
            .global_hash_read_back_verified = true,
            .committed = true,
        });
  };
}

ytec::vssrequester::WorkflowBackendFactory factory(
    BackendState& state) {
  return [&](ytec::vssrequester::SnapshotCopyCallback callback) {
    std::unique_ptr<ytec::vssrequester::IWorkflowBackend> backend =
        std::make_unique<MockWorkflowBackend>(
            state, std::move(callback));
    return ytec::clonecore::Result<std::unique_ptr<
        ytec::vssrequester::IWorkflowBackend>>::success(
        std::move(backend));
  };
}

void test_final_file_commits_only_after_complete_and_delete() {
  StagingState staging;
  BackendState backend{.staging = &staging};
  const auto result =
      ytec::vssrequester::execute_prepared_snapshot_image_backup(
          valid_plan(),
          std::make_unique<MockStagingTarget>(staging),
          successful_copy(),
          factory(backend));
  check(result.has_value(), "Complete workflow should succeed");
  check(
      staging.commit_count == 1 && staging.abort_count == 0 &&
          !backend.saw_commit_before_backup_complete &&
          !backend.saw_commit_before_delete,
      "Final commit must occur only after BackupComplete and Snapshot delete");
  check(
      result.value().final_file_committed_after_vss &&
          result.value().workflow.backup_completed &&
          result.value().workflow.snapshots_deleted,
      "Report should prove VSS completion before final file");
}

void test_backup_complete_failure_aborts_without_final_file() {
  StagingState staging;
  BackendState backend{
      .fail_backup_complete = true,
      .staging = &staging,
  };
  const auto result =
      ytec::vssrequester::execute_prepared_snapshot_image_backup(
          valid_plan(),
          std::make_unique<MockStagingTarget>(staging),
          successful_copy(),
          factory(backend));
  check(!result.has_value(), "BackupComplete failure must fail");
  check(
      staging.commit_count == 0 && staging.abort_count == 1,
      "BackupComplete failure must discard partial output");
}

void test_snapshot_delete_failure_aborts_without_final_file() {
  StagingState staging;
  BackendState backend{
      .fail_delete = true,
      .staging = &staging,
  };
  const auto result =
      ytec::vssrequester::execute_prepared_snapshot_image_backup(
          valid_plan(),
          std::make_unique<MockStagingTarget>(staging),
          successful_copy(),
          factory(backend));
  check(!result.has_value(), "Snapshot delete failure must fail");
  check(
      staging.commit_count == 0 && staging.abort_count == 1,
      "Snapshot delete failure must discard partial output");
}

void test_invalid_non_admin_plan_stops_before_dependencies() {
  auto plan = valid_plan();
  plan.workflow.administrator = false;
  StagingState staging;
  std::size_t factory_calls = 0;
  const auto result =
      ytec::vssrequester::execute_prepared_snapshot_image_backup(
          plan,
          std::make_unique<MockStagingTarget>(staging),
          successful_copy(),
          [&](ytec::vssrequester::SnapshotCopyCallback) {
            ++factory_calls;
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::vssrequester::IWorkflowBackend>>::failure(
                injected_error(L"呼ばれてはいけないFactory"));
          });
  check(!result.has_value(), "Non-admin plan must fail");
  check(
      factory_calls == 0 && staging.begin_count == 0,
      "Invalid plan must stop before backend and output");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"final_file_commits_only_after_complete_and_delete",
       test_final_file_commits_only_after_complete_and_delete},
      {"backup_complete_failure_aborts_without_final_file",
       test_backup_complete_failure_aborts_without_final_file},
      {"snapshot_delete_failure_aborts_without_final_file",
       test_snapshot_delete_failure_aborts_without_final_file},
      {"invalid_non_admin_plan_stops_before_dependencies",
       test_invalid_non_admin_plan_stops_before_dependencies},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

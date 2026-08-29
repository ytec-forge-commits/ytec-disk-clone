#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/windowsapp/online_shrink_image.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kDiskBytes = 64ULL * kMiB;
constexpr std::uint64_t kWimBytes = 1003U;
constexpr std::uint64_t kRawBytes = 4096U;
constexpr wchar_t kVolume[] =
    L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
constexpr wchar_t kSnapshot[] =
    L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy91";
constexpr wchar_t kSnapshotId[] =
    L"{00000000-0000-0000-0000-000000000091}";
constexpr wchar_t kSnapshotSetId[] =
    L"{aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa}";
constexpr wchar_t kProviderId[] =
    L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}";

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool path_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

ytec::clonecore::Error injected_error(const std::wstring_view operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::wstring(operation),
      .message = L"合成失敗です",
  };
}

class TempDirectory final {
 public:
  TempDirectory() {
    std::array<wchar_t, MAX_PATH + 1U> root{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(root.size()), root.data());
    check(length != 0U && length < root.size(), "GetTempPathW failed");
    path_ = root.data();
    path_ += L"ytec-windows-shrink-image-";
    path_ += std::to_wstring(GetCurrentProcessId());
    path_ += L"-";
    path_ += std::to_wstring(GetTickCount64());
    check(CreateDirectoryW(path_.c_str(), nullptr) != FALSE,
          "CreateDirectoryW failed");
  }

  ~TempDirectory() {
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((path_ + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring_view name(found.cFileName);
        if (name != L"." && name != L".." &&
            (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
          const std::wstring child = path_ + L"\\" + found.cFileName;
          static_cast<void>(SetFileAttributesW(
              child.c_str(), FILE_ATTRIBUTE_NORMAL));
          static_cast<void>(DeleteFileW(child.c_str()));
        }
      } while (FindNextFileW(search, &found) != FALSE);
      FindClose(search);
    }
    static_cast<void>(RemoveDirectoryW(path_.c_str()));
  }

  [[nodiscard]] std::wstring file(const std::wstring_view name) const {
    return path_ + L"\\" + std::wstring(name);
  }

 private:
  std::wstring path_;
};

ytec::clonecore::StableDiskIdentity source_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 1U,
      .model = L"Synthetic source",
      .size_bytes = kDiskBytes,
      .logical_sector_size = 512U,
      .serial_suffix = "SRC1",
      .device_instance_id = L"SYNTHETIC\\SOURCE",
      .is_system_disk = false,
  };
}

ytec::clonecore::StableDiskIdentity work_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 2U,
      .model = L"Synthetic work",
      .size_bytes = 128ULL * kMiB,
      .logical_sector_size = 512U,
      .serial_suffix = "WRK2",
      .device_instance_id = L"SYNTHETIC\\WORK",
      .is_system_disk = false,
  };
}

ytec::imageformat::Sha256Digest fixture_source_model_hash() {
  const auto value = ytec::imageformat::hash_tsumugi_source_model_v1(
      source_identity().model);
  check(value.has_value(), "source model hash fixture should build");
  return value.value();
}

ytec::imageformat::Sha256Digest fixture_source_serial_hash() {
  const auto source = source_identity();
  const auto value = ytec::imageformat::hash_tsumugi_source_serial_v1(
      source.serial_suffix, source.device_instance_id);
  check(value.has_value(), "source serial hash fixture should build");
  return value.value();
}

std::vector<std::byte> partition_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  ytec::imageformat::PartitionTableRegion region{
      .disk_offset = 0U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  };
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));
  auto bytes = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(bytes.has_value(), "partition snapshot fixture should build");
  return bytes.take_value();
}

ytec::imageformat::TsumugiManifest manifest_template() {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::shrink,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-09T01:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = partition_snapshot(),
  };
  manifest.source_model_hash = fixture_source_model_hash();
  manifest.source_serial_hash = fixture_source_serial_hash();
  TsumugiManifestPartition ntfs{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected,
      .source_offset = 1ULL * kMiB,
      .source_size = 8ULL * kMiB,
      .used_bytes = 1ULL * kMiB,
      .minimum_target_bytes = 4ULL * kMiB,
      .planned_target_bytes = 4ULL * kMiB,
      .cluster_size = 4096U,
      .name_utf8 = "Data",
      .label_utf8 = "Data",
  };
  ntfs.type_id[0] = std::byte{0x07};
  TsumugiManifestPartition unknown{
      .source_table_index = 2U,
      .source_partition_number = 2U,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::unknown,
      .flags = TsumugiManifestPartitionFlags::selected,
      .source_offset = 10ULL * kMiB,
      .source_size = kRawBytes,
      .used_bytes = kRawBytes,
      .minimum_target_bytes = kRawBytes,
      .planned_target_bytes = kRawBytes,
      .name_utf8 = "Unknown",
      .label_utf8 = "Unknown",
  };
  unknown.type_id[0] = std::byte{0x83};
  manifest.partitions = {std::move(ntfs), std::move(unknown)};
  return manifest;
}

ytec::windowsapp::WindowsShrinkWorkPaths work_paths() {
  return {
      .scratch_directory = L"D:\\YTEC\\scratch",
      .checkpoint_path = L"D:\\YTEC\\checkpoint.bin",
      .log_path = L"D:\\YTEC\\operation.log",
  };
}

ytec::windowsapp::WindowsShrinkWorkPlacementObservation work_observation(
    const bool on_source = false) {
  const auto disk = on_source ? source_identity() : work_identity();
  return {
      .scratch = {
          .canonical_path = L"\\\\?\\D:\\YTEC\\scratch",
          .backing_disk = disk,
          .local_volume = true,
      },
      .checkpoint = {
          .canonical_path = L"\\\\?\\D:\\YTEC\\checkpoint.bin",
          .backing_disk = disk,
          .local_volume = true,
      },
      .log = {
          .canonical_path = L"\\\\?\\D:\\YTEC\\operation.log",
          .backing_disk = disk,
          .local_volume = true,
      },
  };
}

struct CaptureState final {
  std::size_t capture_calls{};
  std::size_t discard_calls{};
  bool wrong_snapshot{};
  bool wrong_source{};
  std::wstring received_scratch;
};

class CapturedSession final
    : public ytec::windowsapp::IWindowsShrinkCapturedSession {
 public:
  explicit CapturedSession(CaptureState& state)
      : state_(state),
        source_model_hash_(fixture_source_model_hash()),
        source_serial_hash_(fixture_source_serial_hash()) {
    if (state_.wrong_source) {
      observed_source_ = work_identity();
    }
    bytes_.resize(8192U);
    for (std::size_t index = 0U; index < bytes_.size(); ++index) {
      bytes_[index] = static_cast<std::byte>((index * 17U + 9U) & 0xFFU);
    }
    payloads_ = {
        ytec::windowsapp::WindowsShrinkCapturedPayload{
            .source_table_index = 1U,
            .kind = ytec::windowsapp::
                WindowsShrinkCapturedPayloadKind::vss_snapshot_wim,
            .session_source_offset = 0U,
            .length = kWimBytes,
            .original_volume_guid_path = kVolume,
            .snapshot_id = state_.wrong_snapshot
                ? L"{00000000-0000-0000-0000-000000000099}"
                : kSnapshotId,
            .snapshot_device_path = kSnapshot,
        },
        ytec::windowsapp::WindowsShrinkCapturedPayload{
            .source_table_index = 2U,
            .kind = ytec::windowsapp::
                WindowsShrinkCapturedPayloadKind::locked_read_only_exact_raw,
            .original_source_offset = 10ULL * kMiB,
            .session_source_offset = 4096U,
            .length = kRawBytes,
        },
    };
  }

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kDiskBytes;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    return source_model_hash_;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    return source_serial_hash_;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest value{};
    value[0] = std::byte{0x41};
    return value;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"captured session read"));
    }
    const auto first = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

  [[nodiscard]] std::span<const ytec::windowsapp::
      WindowsShrinkCapturedPayload> payloads() const noexcept override {
    return payloads_;
  }

  [[nodiscard]] const ytec::clonecore::StableDiskIdentity&
  observed_source_disk() const noexcept override {
    return observed_source_;
  }

  [[nodiscard]] ytec::clonecore::Status
  discard_owned_staging() noexcept override {
    ++state_.discard_calls;
    return ytec::clonecore::success_status();
  }

 private:
  CaptureState& state_;
  ytec::clonecore::StableDiskIdentity observed_source_{source_identity()};
  ytec::imageformat::Sha256Digest source_model_hash_{};
  ytec::imageformat::Sha256Digest source_serial_hash_{};
  std::vector<std::byte> bytes_;
  std::vector<ytec::windowsapp::WindowsShrinkCapturedPayload> payloads_;
};

struct BackendState final {
  bool fail_delete{};
};

class Backend final : public ytec::vssrequester::IWorkflowBackend {
 public:
  Backend(
      BackendState& state,
      ytec::vssrequester::SnapshotCopyCallback callback)
      : state_(state), callback_(std::move(callback)) {}

  [[nodiscard]] ytec::clonecore::Status initialize_components() override {
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status set_backup_state() override {
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status gather_writer_metadata() override {
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Result<std::wstring>
  start_snapshot_set() override {
    return ytec::clonecore::Result<std::wstring>::success(kSnapshotSetId);
  }
  [[nodiscard]] ytec::clonecore::Status add_volume(
      const std::wstring&,
      const std::wstring&) override {
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status prepare_for_backup() override {
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status do_snapshot_set() override {
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Result<
      std::vector<ytec::vssrequester::WriterStatus>>
  query_writer_statuses() override {
    return ytec::clonecore::Result<
        std::vector<ytec::vssrequester::WriterStatus>>::success({
        {.name = L"Synthetic writer",
         .state = ytec::vssrequester::WriterState::stable,
         .status_code = S_OK},
    });
  }
  [[nodiscard]] ytec::clonecore::Result<
      std::vector<ytec::vssrequester::SnapshotMapping>>
  query_snapshot_devices(
      const std::wstring&,
      const std::vector<ytec::vssrequester::VolumeRequest>&) override {
    return ytec::clonecore::Result<
        std::vector<ytec::vssrequester::SnapshotMapping>>::success({
        {.original_volume_guid_path = kVolume,
         .snapshot_id = kSnapshotId,
         .snapshot_device_path = kSnapshot,
         .provider_id = kProviderId,
         .creation_timestamp = 1'091},
    });
  }
  [[nodiscard]] ytec::clonecore::Status copy_snapshot_data(
      const std::vector<ytec::vssrequester::SnapshotMapping>& mappings)
      override {
    return callback_({
        .snapshot_set_id = kSnapshotSetId,
        .mappings = mappings,
    });
  }
  [[nodiscard]] ytec::clonecore::Status backup_complete() override {
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status delete_snapshot_set(
      const std::wstring&) override {
    return state_.fail_delete
        ? ytec::clonecore::Status::failure(injected_error(L"snapshot delete"))
        : ytec::clonecore::success_status();
  }

 private:
  BackendState& state_;
  ytec::vssrequester::SnapshotCopyCallback callback_;
};

ytec::windowsapp::WindowsOnlineShrinkImageCreateRequest request_for(
    const std::wstring& path) {
  using namespace ytec;
  return windowsapp::WindowsOnlineShrinkImageCreateRequest{
      .source_disk = source_identity(),
      .workflow = vssrequester::WorkflowRequest{
          .administrator = true,
          .volumes = {{.volume_guid_path = kVolume, .file_system = L"NTFS"}},
      },
      .image_template = imageformat::TsumugiImageCreateRequest{
          .final_path = path,
          .storage_file_system =
              imageformat::TsumugiImageStorageFileSystem::ntfs,
          .manifest = manifest_template(),
          .compression = imageformat::ImageCompression::zstandard,
          .chunk_size = imageformat::kImageChunkSize16MiB,
          .verification_block_bytes = 1024U,
      },
      .work_paths = work_paths(),
  };
}

ytec::windowsapp::WindowsOnlineShrinkImageCreateDependencies dependencies(
    CaptureState& capture,
    BackendState& backend,
    const bool work_on_source = false) {
  using namespace ytec;
  return {
      .observe_work_placement =
          [work_on_source](const windowsapp::WindowsShrinkWorkPaths&) {
            return clonecore::Result<windowsapp::
                WindowsShrinkWorkPlacementObservation>::success(
                work_observation(work_on_source));
          },
      .capture_snapshot_payloads =
          [&capture](
              const vssrequester::SnapshotCopyContext&,
              const std::span<const imageformat::TsumugiManifestPartition>,
              const windowsapp::WindowsShrinkWorkPaths& paths,
              const clonecore::DiskOperationCallbacks&) {
            ++capture.capture_calls;
            capture.received_scratch = paths.scratch_directory;
            std::unique_ptr<windowsapp::IWindowsShrinkCapturedSession> session =
                std::make_unique<CapturedSession>(capture);
            return clonecore::Result<std::unique_ptr<
                windowsapp::IWindowsShrinkCapturedSession>>::success(
                std::move(session));
          },
      .prepare_image =
          [](const imageformat::TsumugiImageCreateRequest& image,
             const clonecore::DiskOperationCallbacks& callbacks) {
            return imageformat::prepare_tsumugi_image_v1(image, callbacks);
          },
      .backend_factory =
          [&backend](vssrequester::SnapshotCopyCallback callback) {
            std::unique_ptr<vssrequester::IWorkflowBackend> value =
                std::make_unique<Backend>(backend, std::move(callback));
            return clonecore::Result<std::unique_ptr<
                vssrequester::IWorkflowBackend>>::success(std::move(value));
          },
      .revalidate_destination =
          [](const imageformat::TsumugiImageCreateReport*) {
            return clonecore::success_status();
          },
  };
}

void test_vss_wim_and_locked_raw_commit_one_tsumugi() {
  TempDirectory temp;
  const auto path = temp.file(L"mixed-shrink.tsumugi");
  CaptureState capture;
  BackendState backend;
  const auto result = ytec::windowsapp::
      execute_windows_online_shrink_image_create(
          request_for(path), dependencies(capture, backend));
  check(result.has_value(), "mixed shrink image should complete");
  check(
      result.value().wim_payload_count == 1U &&
          result.value().exact_raw_payload_count == 1U &&
          result.value().every_wim_bound_to_active_snapshot_set &&
          result.value().work_placement_revalidated_before_commit &&
          result.value().staging_discarded_before_commit &&
          result.value().final_file_committed_after_vss &&
          capture.capture_calls == 1U && capture.discard_calls == 1U &&
          capture.received_scratch == L"\\\\?\\D:\\YTEC\\scratch" &&
          path_exists(path),
      "report must prove the VSS, staging, and final-name ordering");
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  });
  check(verified.has_value(), "completed mixed image should fully verify");
  check(
      verified.value().manifest.partitions[0].payload_logical_length ==
              kWimBytes &&
          verified.value().manifest.partitions[1].payload_logical_offset %
                  512U ==
              0U &&
          verified.value().manifest.partitions[1].payload_logical_length ==
              kRawBytes,
      "WIM must retain byte length while exact RAW remains sector aligned");
}

void test_work_on_source_stops_before_vss() {
  TempDirectory temp;
  CaptureState capture;
  BackendState backend;
  const auto result = ytec::windowsapp::
      execute_windows_online_shrink_image_create(
          request_for(temp.file(L"blocked.tsumugi")),
          dependencies(capture, backend, true));
  check(!result.has_value(), "source-disk work placement must fail");
  check(capture.capture_calls == 0U,
        "unsafe work placement must stop before VSS capture");
}

void test_bounded_ram_log_needs_no_fabricated_path() {
  TempDirectory temp;
  CaptureState capture;
  BackendState backend;
  auto request = request_for(temp.file(L"ram-log.tsumugi"));
  request.work_paths.log_path.clear();
  request.work_paths.log_is_ram_only = true;
  auto adapters = dependencies(capture, backend);
  adapters.observe_work_placement =
      [](const ytec::windowsapp::WindowsShrinkWorkPaths&) {
        auto observation = work_observation();
        observation.log = {};
        return ytec::clonecore::Result<ytec::windowsapp::
            WindowsShrinkWorkPlacementObservation>::success(
            std::move(observation));
      };
  const auto result = ytec::windowsapp::
      execute_windows_online_shrink_image_create(request, adapters);
  check(result.has_value(),
        "bounded RAM logging must not require a fake filesystem path");

  auto contradictory = request_for(temp.file(L"bad-ram-log.tsumugi"));
  contradictory.work_paths.log_is_ram_only = true;
  CaptureState blocked_capture;
  BackendState blocked_backend;
  const auto blocked = ytec::windowsapp::
      execute_windows_online_shrink_image_create(
          contradictory,
          dependencies(blocked_capture, blocked_backend));
  check(!blocked.has_value() && blocked_capture.capture_calls == 0U,
        "RAM mode with a persistent path must stop before VSS");
}

void test_relative_work_path_and_source_hash_mismatch_stop_before_vss() {
  TempDirectory temp;
  CaptureState capture;
  BackendState backend;
  auto relative = request_for(temp.file(L"relative-blocked.tsumugi"));
  relative.work_paths.scratch_directory = L"scratch";
  const auto relative_result = ytec::windowsapp::
      execute_windows_online_shrink_image_create(
          relative, dependencies(capture, backend));
  check(!relative_result.has_value(), "relative work path must fail closed");

  auto mismatch = request_for(temp.file(L"hash-blocked.tsumugi"));
  mismatch.image_template.manifest.source_model_hash[0] ^= std::byte{0xFF};
  const auto mismatch_result = ytec::windowsapp::
      execute_windows_online_shrink_image_create(
          mismatch, dependencies(capture, backend));
  check(!mismatch_result.has_value(), "source identity hash mismatch must fail");
  check(capture.capture_calls == 0U,
        "work/source identity failures must stop before VSS capture");
}

void test_snapshot_evidence_mismatch_discards_staging() {
  TempDirectory temp;
  const auto path = temp.file(L"wrong-snapshot.tsumugi");
  CaptureState capture{.wrong_snapshot = true};
  BackendState backend;
  const auto result = ytec::windowsapp::
      execute_windows_online_shrink_image_create(
          request_for(path), dependencies(capture, backend));
  check(!result.has_value(), "wrong Snapshot ID must fail closed");
  check(
      capture.capture_calls == 1U && capture.discard_calls == 1U &&
          !path_exists(path) && !path_exists(path + L".partial"),
      "evidence failure must discard owned staging without a final image");
}

void test_capture_source_reidentity_mismatch_discards_staging() {
  TempDirectory temp;
  const auto path = temp.file(L"wrong-source.tsumugi");
  CaptureState capture{.wrong_source = true};
  BackendState backend;
  const auto result = ytec::windowsapp::
      execute_windows_online_shrink_image_create(
          request_for(path), dependencies(capture, backend));
  check(!result.has_value(), "changed source identity must fail closed");
  check(
      capture.capture_calls == 1U && capture.discard_calls == 1U &&
          !path_exists(path) && !path_exists(path + L".partial"),
      "changed source identity must discard staging before image creation");
}

void test_snapshot_delete_failure_aborts_verified_partial() {
  TempDirectory temp;
  const auto path = temp.file(L"delete-failure.tsumugi");
  CaptureState capture;
  BackendState backend{.fail_delete = true};
  const auto result = ytec::windowsapp::
      execute_windows_online_shrink_image_create(
          request_for(path), dependencies(capture, backend));
  check(!result.has_value(), "Snapshot delete failure must fail");
  check(
      capture.discard_calls == 1U && !path_exists(path) &&
          !path_exists(path + L".partial"),
      "Snapshot delete failure must abort the verified partial");
}

}  // namespace

int main() {
  try {
    test_vss_wim_and_locked_raw_commit_one_tsumugi();
    test_work_on_source_stops_before_vss();
    test_bounded_ram_log_needs_no_fabricated_path();
    test_relative_work_path_and_source_hash_mismatch_stop_before_vss();
    test_snapshot_evidence_mismatch_discards_staging();
    test_capture_source_reidentity_mismatch_discards_staging();
    test_snapshot_delete_failure_aborts_verified_partial();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All Windows online shrink image tests passed\n";
  return 0;
}

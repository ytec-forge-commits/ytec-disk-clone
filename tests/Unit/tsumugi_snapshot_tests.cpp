#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/vssrequester/online_tsumugi_backup.h"
#include "ytec/vssrequester/tsumugi_snapshot.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kDiskSize = 64ULL * kMiB;
constexpr std::uint64_t kRawOffset = 1ULL * kMiB;
constexpr std::uint64_t kMsrOffset = 2ULL * kMiB;
constexpr std::uint64_t kNtfsOffset = 3ULL * kMiB;
constexpr std::uint64_t kRawSize = 4096U;
constexpr std::uint64_t kMsrSize = 4096U;
constexpr std::uint64_t kNtfsSize = 16U * 1024U;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error test_error(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_READ_FAULT,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

class TempDirectory final {
 public:
  TempDirectory() {
    std::array<wchar_t, MAX_PATH + 1U> root{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(root.size()), root.data());
    check(length != 0U && length < root.size(), "GetTempPathW failed");
    path_ = root.data();
    path_ += L"ytec-tsumugi-snapshot-";
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
          static_cast<void>(DeleteFileW(
              (path_ + L"\\" + found.cFileName).c_str()));
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

bool path_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

class PatternReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  PatternReader(
      const std::uint64_t size,
      const bool ntfs,
      const std::uint8_t seed) noexcept
      : size_(size), ntfs_(ntfs), seed_(seed) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return size_;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > size_ || length > size_ - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"モックSnapshot範囲", L"範囲外です"));
    }
    const std::uint8_t active_seed =
        mutate_after_first_read && read_count != 0U
            ? static_cast<std::uint8_t>(seed_ + 1U)
            : seed_;
    ++read_count;
    std::vector<std::byte> bytes(length);
    for (std::size_t index = 0U; index < length; ++index) {
      bytes[index] = static_cast<std::byte>(
          (offset + index * 17U + active_seed) & 0xFFU);
    }
    if (ntfs_ && offset == 0U && length >= 512U) {
      constexpr char signature[] = "NTFS    ";
      std::memcpy(bytes.data() + 3U, signature, 8U);
      write_little<std::uint16_t>(bytes, 11U, 512U);
      bytes[13U] = std::byte{8};
      write_little<std::uint64_t>(bytes, 40U, size_ / 512U);
      bytes[510U] = std::byte{0x55};
      bytes[511U] = std::byte{0xAA};
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

  mutable std::size_t read_count{};
  bool mutate_after_first_read{};

 private:
  std::uint64_t size_{};
  bool ntfs_{};
  std::uint8_t seed_{};
};

class BitmapProvider final
    : public ytec::clonecore::INtfsUsedRangeProvider {
 public:
  [[nodiscard]] ytec::clonecore::Result<
      std::vector<ytec::clonecore::ByteRange>> query_used_ranges(
      const std::uint32_t partition_index,
      const ytec::clonecore::NtfsGeometry& geometry) override {
    ++query_count;
    check(partition_index == 3U, "unexpected bitmap partition index");
    check(geometry.cluster_size() == 4096U,
          "unexpected NTFS cluster size");
    return ytec::clonecore::Result<
        std::vector<ytec::clonecore::ByteRange>>::success({
        {.offset = 0U, .length = 4096U},
        {.offset = 8192U, .length = 4096U},
    });
  }

  std::size_t query_count{};
};

std::vector<std::byte> partition_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = kDiskSize,
      .logical_sector_size = 512U,
  };
  ytec::imageformat::PartitionTableRegion region{
      .disk_offset = 0U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  };
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));
  const auto built = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(built.has_value(), "partition snapshot should build");
  return built.value();
}

ytec::imageformat::TsumugiManifestPartition partition(
    const std::uint32_t index,
    const ytec::imageformat::TsumugiManifestPartitionRole role,
    const ytec::imageformat::TsumugiManifestFileSystem file_system,
    const std::uint64_t offset,
    const std::uint64_t size) {
  ytec::imageformat::TsumugiManifestPartition value{
      .source_table_index = index,
      .source_partition_number = index,
      .role = role,
      .file_system = file_system,
      .flags = ytec::imageformat::TsumugiManifestPartitionFlags::selected,
      .source_offset = offset,
      .source_size = size,
      .used_bytes = size,
      .minimum_target_bytes = size,
      .planned_target_bytes = size,
      .payload_logical_offset = offset,
      .payload_logical_length = size,
      .name_utf8 = "Synthetic",
      .label_utf8 = "Synthetic",
  };
  value.type_id[0] = std::byte{0x07};
  return value;
}

ytec::vssrequester::TsumugiSnapshotImageRequest request_for(
    const std::wstring& path,
    const PatternReader& raw,
    std::function<ytec::clonecore::Status()> layout_check) {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::exact,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .source_disk_size = kDiskSize,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-04T22:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = partition_snapshot(),
  };
  manifest.source_model_hash[0] = std::byte{0x21};
  manifest.source_serial_hash[0] = std::byte{0x32};
  manifest.partitions = {
      partition(1U, TsumugiManifestPartitionRole::efi_system,
                TsumugiManifestFileSystem::fat32, kRawOffset, kRawSize),
      partition(2U, TsumugiManifestPartitionRole::microsoft_reserved,
                TsumugiManifestFileSystem::none, kMsrOffset, kMsrSize),
      partition(3U, TsumugiManifestPartitionRole::data,
                TsumugiManifestFileSystem::ntfs, kNtfsOffset, kNtfsSize),
  };
  manifest.partitions[1].used_bytes = 0U;

  ytec::vssrequester::TsumugiSnapshotImageRequest request{
      .image = TsumugiImageCreateRequest{
          .final_path = path,
          .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
          .manifest = std::move(manifest),
          .compression = ImageCompression::zstandard,
          .chunk_size = kImageChunkSize16MiB,
          .verification_block_bytes = 1024U,
      },
      .volumes = {{
          .partition_entry_index = 3U,
          .disk_offset = kNtfsOffset,
          .partition_length = kNtfsSize,
          .original_volume_guid_path =
              L"\\\\?\\Volume{33333333-3333-3333-3333-333333333333}\\",
      }},
      .raw_regions = {{
          .partition_entry_index = 1U,
          .disk_offset = kRawOffset,
          .length = kRawSize,
          .source_offset = kRawOffset,
      }},
      .locked_raw_source = &raw,
      .revalidate_locked_layout = std::move(layout_check),
      .validate_destination_capacity =
          [](const std::uint64_t required_bytes) {
            check(required_bytes >= 64ULL * 1024ULL * 1024ULL,
                  "destination estimate must include safety margin");
            return ytec::clonecore::success_status();
          },
  };
  request.locked_source_state_hash[0] = std::byte{0x43};
  return request;
}

ytec::clonecore::Status ok_layout() {
  return ytec::clonecore::success_status();
}

ytec::vssrequester::SnapshotCopyContext snapshot_context(
    const std::wstring& snapshot_id,
    const std::wstring& device_path) {
  return ytec::vssrequester::SnapshotCopyContext{
      .snapshot_set_id = L"{snapshot-set-fixture}",
      .mappings = {
          ytec::vssrequester::SnapshotMapping{
              .original_volume_guid_path =
                  L"\\\\?\\Volume{33333333-3333-3333-3333-333333333333}\\",
              .snapshot_id = snapshot_id,
              .snapshot_device_path = device_path,
              .provider_id =
                  L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}",
              .creation_timestamp = 1'001,
          },
      },
  };
}

void test_snapshot_raw_and_zero_ranges_share_one_verified_image() {
  TempDirectory temp;
  PatternReader raw(kDiskSize, false, 7U);
  BitmapProvider bitmap;
  std::size_t layout_checks = 0U;
  const auto path = temp.file(L"composite.tsumugi");
  auto request = request_for(path, raw, [&]() {
    ++layout_checks;
    return ok_layout();
  });
  auto staged = ytec::vssrequester::prepare_tsumugi_snapshot_image_v1(
      request,
      snapshot_context(
          L"{snapshot-42}",
          L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy42"),
      [](const ytec::vssrequester::SnapshotVolumeOpenRequest& open_request) {
        return ytec::clonecore::Result<std::unique_ptr<
            ytec::clonecore::ISourceDiskReader>>::success(
            std::make_unique<PatternReader>(
                open_request.expected_size_bytes, true, std::uint8_t{19U}));
      },
      bitmap);
  check(staged.has_value() && staged.value().pending(),
        "composite Snapshot image should be fully staged");
  check(layout_checks == 2U && bitmap.query_count == 1U &&
            raw.read_count == 2U,
        "layout must be checked twice and raw bytes fingerprinted then copied");
  check(!path_exists(path) && path_exists(path + L".partial"),
        "VSS stage must not expose the final filename");
  check(staged.value().commit_verified().has_value(),
        "post-VSS explicit commit should succeed");

  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  });
  check(verified.has_value(), "committed composite image should verify");
  check(verified.value().container.records.size() == 6U,
        "raw, MSR zero, and four NTFS used/zero ranges should be explicit");
  check(verified.value().container.records[1].flags ==
            ytec::imageformat::TsumugiChunkFlags::zero_filled &&
            verified.value().container.records[3].flags ==
                ytec::imageformat::TsumugiChunkFlags::zero_filled &&
            verified.value().container.records[5].flags ==
                ytec::imageformat::TsumugiChunkFlags::zero_filled,
        "MSR and every NTFS bitmap complement must be explicit zero chunks");
  check(verified.value().manifest.source_state_hash !=
            ytec::imageformat::Sha256Digest{},
        "Snapshot/raw/layout evidence must bind the manifest source state");
}

void test_raw_change_between_fingerprint_and_copy_fails_closed() {
  TempDirectory temp;
  PatternReader raw(kDiskSize, false, 7U);
  raw.mutate_after_first_read = true;
  BitmapProvider bitmap;
  const auto path = temp.file(L"raw-change.tsumugi");
  auto request = request_for(path, raw, ok_layout);
  const auto staged = ytec::vssrequester::prepare_tsumugi_snapshot_image_v1(
      request,
      snapshot_context(
          L"{snapshot-43}",
          L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy43"),
      [](const ytec::vssrequester::SnapshotVolumeOpenRequest& open_request) {
        return ytec::clonecore::Result<std::unique_ptr<
            ytec::clonecore::ISourceDiskReader>>::success(
            std::make_unique<PatternReader>(
                open_request.expected_size_bytes, true, std::uint8_t{19U}));
      },
      bitmap);
  check(!staged.has_value() && !path_exists(path) &&
            !path_exists(path + L".partial"),
        "raw mutation must abort the owned partial and expose no final image");
}

void test_snapshot_identity_changes_manifest_source_state() {
  TempDirectory temp;
  PatternReader raw_first(kDiskSize, false, 7U);
  PatternReader raw_second(kDiskSize, false, 7U);
  const auto first_path = temp.file(L"identity-first.tsumugi");
  const auto second_path = temp.file(L"identity-second.tsumugi");
  auto build = [&](const std::wstring& path,
                   PatternReader& raw,
                   const std::wstring& snapshot_id,
                   const std::wstring& device) {
    auto request = request_for(path, raw, ok_layout);
    BitmapProvider bitmap;
    auto staged = ytec::vssrequester::prepare_tsumugi_snapshot_image_v1(
        request,
        snapshot_context(snapshot_id, device),
        [](const ytec::vssrequester::SnapshotVolumeOpenRequest& open) {
          return ytec::clonecore::Result<std::unique_ptr<
              ytec::clonecore::ISourceDiskReader>>::success(
              std::make_unique<PatternReader>(
                  open.expected_size_bytes, true, std::uint8_t{19U}));
        },
        bitmap);
    check(staged.has_value() && staged.value().commit_verified().has_value(),
          "identity fixture should build and commit");
    const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
        .image_path = path,
        .storage_file_system =
            ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
        .verification_block_bytes = 1024U,
    });
    check(verified.has_value(), "identity fixture should fully verify");
    return verified.value().manifest.source_state_hash;
  };
  const auto first = build(
      first_path,
      raw_first,
      L"{snapshot-101}",
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy101");
  const auto second = build(
      second_path,
      raw_second,
      L"{snapshot-102}",
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy102");
  check(first != second,
        "Snapshot ID and device identity must affect source_state_hash");
}

void test_final_layout_change_aborts_verified_stage() {
  TempDirectory temp;
  PatternReader raw(kDiskSize, false, 7U);
  BitmapProvider bitmap;
  std::size_t calls = 0U;
  const auto path = temp.file(L"layout-change.tsumugi");
  auto request = request_for(path, raw, [&]() {
    ++calls;
    if (calls == 2U) {
      return ytec::clonecore::Status::failure(test_error(
          L"モックレイアウト再検証", L"パーティション表が変化しました"));
    }
    return ok_layout();
  });
  const auto staged = ytec::vssrequester::prepare_tsumugi_snapshot_image_v1(
      request,
      snapshot_context(
          L"{snapshot-44}",
          L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy44"),
      [](const ytec::vssrequester::SnapshotVolumeOpenRequest& open_request) {
        return ytec::clonecore::Result<std::unique_ptr<
            ytec::clonecore::ISourceDiskReader>>::success(
            std::make_unique<PatternReader>(
                open_request.expected_size_bytes, true, std::uint8_t{19U}));
      },
      bitmap);
  check(!staged.has_value() && calls == 2U && !path_exists(path) &&
            !path_exists(path + L".partial"),
        "layout drift after verification must remove only the owned stage");
}

struct LifecycleState final {
  std::wstring final_path;
  bool fail_backup_complete{};
  bool fail_delete{};
  bool final_visible_during_backup_complete{};
  bool final_visible_during_delete{};
  std::size_t copy_count{};
  std::size_t delete_count{};
};

class LifecycleBackend final
    : public ytec::vssrequester::IWorkflowBackend {
 public:
  LifecycleBackend(
      LifecycleState& state,
      ytec::vssrequester::SnapshotCopyCallback callback)
      : state_(state), callback_(std::move(callback)) {}

  ytec::clonecore::Status initialize_components() override {
    return ok_layout();
  }
  ytec::clonecore::Status set_backup_state() override {
    return ok_layout();
  }
  ytec::clonecore::Status gather_writer_metadata() override {
    return ok_layout();
  }
  ytec::clonecore::Result<std::wstring> start_snapshot_set() override {
    return ytec::clonecore::Result<std::wstring>::success(
        L"{snapshot-set-fixture}");
  }
  ytec::clonecore::Status add_volume(
      const std::wstring&,
      const std::wstring&) override {
    return ok_layout();
  }
  ytec::clonecore::Status prepare_for_backup() override {
    return ok_layout();
  }
  ytec::clonecore::Status do_snapshot_set() override {
    return ok_layout();
  }
  ytec::clonecore::Result<std::vector<
      ytec::vssrequester::WriterStatus>> query_writer_statuses() override {
    return ytec::clonecore::Result<std::vector<
        ytec::vssrequester::WriterStatus>>::success({
        ytec::vssrequester::WriterStatus{
            .name = L"Synthetic Writer",
            .state = ytec::vssrequester::WriterState::stable,
            .status_code = S_OK,
        },
    });
  }
  ytec::clonecore::Result<std::vector<
      ytec::vssrequester::SnapshotMapping>> query_snapshot_devices(
      const std::wstring&,
      const std::vector<ytec::vssrequester::VolumeRequest>& volumes) override {
    return ytec::clonecore::Result<std::vector<
        ytec::vssrequester::SnapshotMapping>>::success({
        ytec::vssrequester::SnapshotMapping{
            .original_volume_guid_path = volumes.front().volume_guid_path,
            .snapshot_id = L"{00000000-0000-0000-0000-000000000055}",
            .snapshot_device_path =
                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy55",
            .provider_id = L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}",
            .creation_timestamp = 1'055,
        },
    });
  }
  ytec::clonecore::Status copy_snapshot_data(
      const std::vector<ytec::vssrequester::SnapshotMapping>& mappings)
      override {
    ++state_.copy_count;
    return callback_(ytec::vssrequester::SnapshotCopyContext{
        .snapshot_set_id = L"{snapshot-set-fixture}",
        .mappings = mappings,
    });
  }
  ytec::clonecore::Status backup_complete() override {
    state_.final_visible_during_backup_complete =
        path_exists(state_.final_path);
    if (state_.fail_backup_complete) {
      return ytec::clonecore::Status::failure(test_error(
          L"モックBackupComplete", L"注入失敗です"));
    }
    return ok_layout();
  }
  ytec::clonecore::Status delete_snapshot_set(
      const std::wstring&) override {
    ++state_.delete_count;
    state_.final_visible_during_delete = path_exists(state_.final_path);
    if (state_.fail_delete) {
      return ytec::clonecore::Status::failure(test_error(
          L"モックSnapshot削除", L"注入失敗です"));
    }
    return ok_layout();
  }

 private:
  LifecycleState& state_;
  ytec::vssrequester::SnapshotCopyCallback callback_;
};

ytec::clonecore::Result<ytec::vssrequester::OnlineTsumugiBackupReport>
run_lifecycle(
    LifecycleState& state,
    PatternReader& raw) {
  auto image = request_for(state.final_path, raw, ok_layout);
  ytec::vssrequester::PreparedOnlineTsumugiBackup prepared{
      .workflow = ytec::vssrequester::WorkflowRequest{
          .administrator = true,
          .volumes = {
              ytec::vssrequester::VolumeRequest{
                  .volume_guid_path =
                      image.volumes.front().original_volume_guid_path,
                  .file_system = L"NTFS",
              },
          },
      },
      .image = std::move(image),
      .revalidate_destination =
          [](const ytec::imageformat::TsumugiImageCreateReport*) {
            return ok_layout();
          },
  };
  return ytec::vssrequester::execute_prepared_online_tsumugi_backup(
      prepared,
      [](const ytec::vssrequester::TsumugiSnapshotImageRequest& request,
         const ytec::vssrequester::SnapshotCopyContext& context) {
        BitmapProvider bitmap;
        return ytec::vssrequester::prepare_tsumugi_snapshot_image_v1(
            request,
            context,
            [](const ytec::vssrequester::SnapshotVolumeOpenRequest& open) {
              return ytec::clonecore::Result<std::unique_ptr<
                  ytec::clonecore::ISourceDiskReader>>::success(
                  std::make_unique<PatternReader>(
                      open.expected_size_bytes,
                      true,
                      std::uint8_t{19U}));
            },
            bitmap);
      },
      [&](ytec::vssrequester::SnapshotCopyCallback callback) {
        std::unique_ptr<ytec::vssrequester::IWorkflowBackend> backend =
            std::make_unique<LifecycleBackend>(state, std::move(callback));
        return ytec::clonecore::Result<std::unique_ptr<
            ytec::vssrequester::IWorkflowBackend>>::success(
            std::move(backend));
      });
}

void test_online_lifecycle_commits_only_after_snapshot_cleanup() {
  TempDirectory temp;
  PatternReader raw(kDiskSize, false, 7U);
  LifecycleState state{
      .final_path = temp.file(L"online-success.tsumugi"),
  };
  const auto result = run_lifecycle(state, raw);
  check(result.has_value() &&
            result.value().final_file_committed_after_vss &&
            result.value().image.stream.committed,
        "successful VSS lifecycle should explicitly commit the staged image");
  check(!state.final_visible_during_backup_complete &&
            !state.final_visible_during_delete &&
            state.copy_count == 1U && state.delete_count == 1U &&
            path_exists(state.final_path) &&
            !path_exists(state.final_path + L".partial"),
        "completed image must remain invisible through exact Snapshot cleanup");
}

void test_online_lifecycle_failures_leave_no_completed_image() {
  for (const bool fail_backup_complete : {true, false}) {
    TempDirectory temp;
    PatternReader raw(kDiskSize, false, 7U);
    LifecycleState state{
        .final_path = temp.file(
            fail_backup_complete ? L"backup-fail.tsumugi"
                                 : L"delete-fail.tsumugi"),
        .fail_backup_complete = fail_backup_complete,
        .fail_delete = !fail_backup_complete,
    };
    const auto result = run_lifecycle(state, raw);
    check(!result.has_value() && !path_exists(state.final_path) &&
              !path_exists(state.final_path + L".partial"),
          "BackupComplete or Snapshot deletion failure must abort the stage");
    check(!state.final_visible_during_backup_complete &&
              !state.final_visible_during_delete,
          "a failed VSS lifecycle must never expose a completed image");
  }
}

}  // namespace

int main() {
  try {
    test_snapshot_raw_and_zero_ranges_share_one_verified_image();
    test_raw_change_between_fingerprint_and_copy_fails_closed();
    test_snapshot_identity_changes_manifest_source_state();
    test_final_layout_change_aborts_verified_stage();
    test_online_lifecycle_commits_only_after_snapshot_cleanup();
    test_online_lifecycle_failures_leave_no_completed_image();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All Tsumugi Snapshot tests passed\n";
  return 0;
}

#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/windowsapp/shrink_restore_transaction.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
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

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error injected_error(
    const ytec::clonecore::ErrorCode code,
    const std::wstring_view operation) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = static_cast<DWORD>(
          code == ytec::clonecore::ErrorCode::cancelled
              ? ERROR_CANCELLED
              : ERROR_GEN_FAILURE),
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
    path_ += L"ytec-windows-shrink-restore-";
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

class Source final : public ytec::imageformat::ITsumugiImageSourceSession {
 public:
  Source() {
    bytes_.resize(8192U);
    for (std::size_t index = 0U; index < bytes_.size(); ++index) {
      bytes_[index] = static_cast<std::byte>((index * 23U + 7U) & 0xFFU);
    }
    constexpr std::array<std::byte, 8U> magic{
        std::byte{'M'}, std::byte{'S'}, std::byte{'W'}, std::byte{'I'},
        std::byte{'M'}, std::byte{0}, std::byte{0}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes_.begin());
  }

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kDiskBytes;
  }
  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }
  [[nodiscard]] ytec::imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest value{};
    value[0] = std::byte{0x11};
    return value;
  }
  [[nodiscard]] ytec::imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest value{};
    value[0] = std::byte{0x22};
    return value;
  }
  [[nodiscard]] ytec::imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest value{};
    value[0] = std::byte{0x33};
    return value;
  }
  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(
              ytec::clonecore::ErrorCode::io_failed,
              L"合成縮小Source読取り"));
    }
    const auto first = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

 private:
  std::vector<std::byte> bytes_;
};

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

std::vector<std::byte> gpt_partition_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::gpt,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  snapshot.regions.push_back({
      .disk_offset = 0U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  });
  snapshot.regions.push_back({
      .disk_offset = kDiskBytes - 512U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  });
  auto bytes = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(bytes.has_value(), "GPT partition snapshot fixture should build");
  return bytes.take_value();
}

ytec::imageformat::TsumugiImageCreateRequest image_request(
    const std::wstring& path,
    const Source& source) {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::shrink,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-09T02:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = partition_snapshot(),
  };
  manifest.source_model_hash = source.source_model_hash();
  manifest.source_serial_hash = source.source_serial_hash();
  manifest.source_state_hash = source.source_state_hash();
  TsumugiManifestPartition wim{
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
      .payload_logical_offset = 0U,
      .payload_logical_length = kWimBytes,
      .payload_encoding =
          TsumugiManifestPayloadEncoding::microsoft_wim_single_image,
      .payload_format_version = kTsumugiWimPayloadFormatVersion,
      .cluster_size = 4096U,
      .name_utf8 = "Data",
      .label_utf8 = "Data",
  };
  wim.type_id[0] = std::byte{0x07};
  TsumugiManifestPartition raw{
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
      .payload_logical_offset = 1024U,
      .payload_logical_length = kRawBytes,
      .name_utf8 = "Unknown",
      .label_utf8 = "Unknown",
  };
  raw.type_id[0] = std::byte{0x83};
  manifest.partitions = {std::move(wim), std::move(raw)};
  return TsumugiImageCreateRequest{
      .final_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .manifest = std::move(manifest),
      .chunks = {
          TsumugiStreamBuildChunk{
              .logical_offset = 0U,
              .logical_length = kWimBytes,
              .source_offset = 0U,
              .source = &source,
          },
          TsumugiStreamBuildChunk{
              .logical_offset = 1024U,
              .logical_length = kRawBytes,
              .source_offset = 4096U,
              .source = &source,
          },
      },
      .compression = ImageCompression::zstandard,
      .chunk_size = kImageChunkSize16MiB,
      .verification_block_bytes = 1024U,
      .source_session = &source,
  };
}

ytec::imageformat::TsumugiRestoreDiskIdentity target_identity() {
  ytec::imageformat::TsumugiRestoreDiskIdentity identity{
      .disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  identity.stable_identity_hash[0] = std::byte{0xA5};
  return identity;
}

ytec::clonecore::StableDiskIdentity protected_source() {
  return {
      .disk_number = 4U,
      .model = L"Image storage",
      .size_bytes = 128ULL * kMiB,
      .logical_sector_size = 512U,
      .serial_suffix = "IMG4",
      .device_instance_id = L"SYNTHETIC\\IMAGE",
  };
}

ytec::clonecore::StableDiskIdentity work_disk() {
  return {
      .disk_number = 5U,
      .model = L"Work storage",
      .size_bytes = 128ULL * kMiB,
      .logical_sector_size = 512U,
      .serial_suffix = "WRK5",
      .device_instance_id = L"SYNTHETIC\\WORK",
  };
}

ytec::windowsapp::WindowsShrinkWorkPaths work_paths() {
  return {
      .scratch_directory = L"E:\\YTEC\\scratch",
      .checkpoint_path = L"E:\\YTEC\\checkpoint.bin",
      .log_path = L"E:\\YTEC\\operation.log",
  };
}

ytec::windowsapp::WindowsShrinkWorkPlacementObservation work_observation() {
  const auto disk = work_disk();
  return {
      .scratch = {
          .canonical_path = L"\\\\?\\E:\\YTEC\\scratch",
          .backing_disk = disk,
          .local_volume = true,
      },
      .checkpoint = {
          .canonical_path = L"\\\\?\\E:\\YTEC\\checkpoint.bin",
          .backing_disk = disk,
          .local_volume = true,
      },
      .log = {
          .canonical_path = L"\\\\?\\E:\\YTEC\\operation.log",
          .backing_disk = disk,
          .local_volume = true,
      },
  };
}

struct PlatformState final {
  std::vector<std::string> events;
  std::vector<std::byte> staged_wim;
  std::size_t abort_count{};
  bool target_offline{};
  bool target_incomplete{};
  bool committed{};
  bool fail_apply_as_cancel{};
  bool throw_create_file_system{};
  bool fail_commit{};
  std::wstring received_scratch;
};

class Platform final
    : public ytec::windowsapp::IWindowsTsumugiShrinkRestorePlatform {
 public:
  Platform(
      PlatformState& state,
      ytec::imageformat::TsumugiRestoreDiskIdentity target)
      : state_(state), target_(std::move(target)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::imageformat::TsumugiRestoreDiskIdentity>
  begin_offline_incomplete(
      const ytec::imageformat::TsumugiVerifiedImage&,
      const ytec::imageformat::TsumugiWholeDiskRestoreTarget&,
      const ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1&
          reviewed_layout,
      const ytec::windowsapp::WindowsShrinkWorkPaths& paths) override {
    state_.events.push_back("begin");
    state_.events.push_back(
        reviewed_layout.metadata.style ==
                ytec::imageformat::PartitionTableStyle::mbr
            ? "reviewed-mbr"
            : "reviewed-gpt");
    state_.received_scratch = paths.scratch_directory;
    state_.target_offline = true;
    state_.target_incomplete = true;
    return ytec::clonecore::Result<
        ytec::imageformat::TsumugiRestoreDiskIdentity>::success(target_);
  }

  [[nodiscard]] ytec::clonecore::Status create_target_file_system(
      const ytec::imageformat::TsumugiShrinkArchiveTarget&) override {
    state_.events.push_back("create-fs");
    if (state_.throw_create_file_system) {
      throw std::runtime_error("injected platform exception");
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status begin_staged_wim(
      const ytec::imageformat::TsumugiShrinkArchiveTarget& target,
      const std::wstring&) override {
    state_.events.push_back("begin-wim");
    state_.staged_wim.clear();
    state_.staged_wim.reserve(static_cast<std::size_t>(target.archive_length));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status append_staged_wim(
      const ytec::imageformat::TsumugiShrinkArchiveChunk&,
      const std::span<const std::byte> plaintext) override {
    state_.events.push_back("append-wim");
    state_.staged_wim.insert(
        state_.staged_wim.end(), plaintext.begin(), plaintext.end());
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status verify_staged_single_image_wim(
      const std::uint32_t) override {
    state_.events.push_back("verify-wim");
    const std::array<std::byte, 5U> expected{
        std::byte{'M'}, std::byte{'S'}, std::byte{'W'},
        std::byte{'I'}, std::byte{'M'}};
    return state_.staged_wim.size() == kWimBytes &&
            std::equal(
                expected.begin(), expected.end(), state_.staged_wim.begin())
        ? ytec::clonecore::success_status()
        : ytec::clonecore::Status::failure(injected_error(
              ytec::clonecore::ErrorCode::verification_failed,
              L"合成WIM検証"));
  }

  [[nodiscard]] ytec::clonecore::Status apply_staged_wim(
      const std::uint32_t) override {
    state_.events.push_back("apply-wim");
    return state_.fail_apply_as_cancel
        ? ytec::clonecore::Status::failure(injected_error(
              ytec::clonecore::ErrorCode::cancelled,
              L"合成WIM適用取消"))
        : ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status
  verify_applied_file_system_readback(
      const ytec::imageformat::TsumugiShrinkArchiveTarget&) override {
    state_.events.push_back("readback-fs");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status write_exact_raw_and_verify(
      const ytec::imageformat::TsumugiRestoreWrite&,
      const std::span<const std::byte> plaintext) override {
    state_.events.push_back("raw-readback");
    return plaintext.size() == kRawBytes
        ? ytec::clonecore::success_status()
        : ytec::clonecore::Status::failure(injected_error(
              ytec::clonecore::ErrorCode::verification_failed,
              L"合成RAW読戻し"));
  }

  [[nodiscard]] ytec::clonecore::Status commit_final_layout_last() override {
    state_.events.push_back("commit");
    if (state_.fail_commit) {
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成最終commit失敗"));
    }
    state_.committed = true;
    state_.target_incomplete = false;
    return ytec::clonecore::success_status();
  }

  void abort_keep_offline_incomplete() noexcept override {
    state_.events.push_back("abort");
    ++state_.abort_count;
    state_.target_offline = true;
    state_.target_incomplete = true;
    state_.committed = false;
  }

 private:
  PlatformState& state_;
  ytec::imageformat::TsumugiRestoreDiskIdentity target_;
};

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
reviewed_layout();

ytec::clonecore::Result<ytec::imageformat::TsumugiRestorePlan>
plan_for_layout(
    const std::wstring& path,
    const ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1&
        layout) {
  using namespace ytec::imageformat;
  TsumugiWholeDiskRestoreTarget target{
      .disk = target_identity(),
      .reviewed_shrink_layout = layout,
  };
  for (const auto& partition : layout.migration.target_partitions) {
    if (partition.source_table_index.has_value()) {
      target.shrink_placements.push_back({
          .source_table_index = partition.source_table_index.value(),
          .target_offset = partition.offset_bytes,
          .target_size = partition.size_bytes,
      });
    }
  }
  return prepare_tsumugi_restore_plan_v1({
      .image = {
          .image_path = path,
          .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
          .verification_block_bytes = 1024U,
      },
      .host = TsumugiRestoreHost::windows,
      .target = std::move(target),
  });
}

ytec::clonecore::Result<ytec::imageformat::TsumugiRestorePlan> plan_for(
    const std::wstring& path) {
  return plan_for_layout(path, reviewed_layout());
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
reviewed_layout() {
  using namespace ytec;
  return {
      .migration = {
          .target_style = migrationcore::MigrationPartitionStyle::mbr,
          .alignment_bytes = kMiB,
          .minimum_target_size_bytes = 9ULL * kMiB,
          .target_size_bytes = kDiskBytes,
          .unallocated_tail_bytes = kDiskBytes - 9ULL * kMiB - kRawBytes,
          .source_remains_unchanged = true,
          .target_partitions = {
              {
                  .target_number = 1U,
                  .source_table_index = 1U,
                  .role = migrationcore::MigrationPartitionRole::data,
                  .file_system = migrationcore::MigrationFileSystem::ntfs,
                  .action = migrationcore::MigrationPartitionAction::
                      apply_file_image,
                  .offset_bytes = 1ULL * kMiB,
                  .size_bytes = 4ULL * kMiB,
              },
              {
                  .target_number = 2U,
                  .source_table_index = 2U,
                  .role = migrationcore::MigrationPartitionRole::data,
                  .file_system =
                      migrationcore::MigrationFileSystem::unsupported,
                  .action = migrationcore::MigrationPartitionAction::
                      copy_exact_raw,
                  .offset_bytes = 8ULL * kMiB,
                  .size_bytes = kRawBytes,
              },
          },
      },
      .metadata = {
          .style = imageformat::PartitionTableStyle::mbr,
          .target_size_bytes = kDiskBytes,
          .logical_sector_size = 512U,
          .invalidation_ranges = {
              {.offset = 0U, .length = kMiB},
              {.offset = kDiskBytes - kMiB, .length = kMiB},
          },
          .commit_writes = {
              {
                  imageformat::TsumugiRestoreLayoutWriteKind::mbr_sector,
                  0U,
                  std::vector<std::byte>(512U, std::byte{0}),
              },
          },
          .target_layout = clonecore::MbrDisk{
              .logical_sector_size = 512U,
              .sector_count = kDiskBytes / 512U,
              .disk_signature = 0x13572468U,
              .partitions = {
                  {
                      .table_index = 0U,
                      .type = 0x07U,
                      .first_lba = static_cast<std::uint32_t>(
                          1ULL * kMiB / 512U),
                      .sector_count = static_cast<std::uint32_t>(
                          4ULL * kMiB / 512U),
                  },
                  {
                      .table_index = 1U,
                      .type = 0x83U,
                      .first_lba = static_cast<std::uint32_t>(
                          8ULL * kMiB / 512U),
                      .sector_count = static_cast<std::uint32_t>(
                          kRawBytes / 512U),
                  },
              },
          },
      },
  };
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
reviewed_gpt_layout() {
  auto layout = reviewed_layout();
  layout.migration.target_style =
      ytec::migrationcore::MigrationPartitionStyle::gpt;
  layout.metadata.style = ytec::imageformat::PartitionTableStyle::gpt;
  layout.metadata.staged_writes = {
      {
          ytec::imageformat::TsumugiRestoreLayoutWriteKind::
              gpt_primary_entries,
          2U * 512U,
          std::vector<std::byte>(512U, std::byte{0x11}),
      },
      {
          ytec::imageformat::TsumugiRestoreLayoutWriteKind::
              gpt_backup_entries,
          kDiskBytes - 2U * 512U,
          std::vector<std::byte>(512U, std::byte{0x22}),
      },
  };
  layout.metadata.commit_writes = {
      {
          ytec::imageformat::TsumugiRestoreLayoutWriteKind::
              gpt_backup_header,
          kDiskBytes - 512U,
          std::vector<std::byte>(512U, std::byte{0x33}),
      },
      {
          ytec::imageformat::TsumugiRestoreLayoutWriteKind::
              gpt_protective_mbr,
          0U,
          std::vector<std::byte>(512U, std::byte{0x44}),
      },
      {
          ytec::imageformat::TsumugiRestoreLayoutWriteKind::
              gpt_primary_header,
          512U,
          std::vector<std::byte>(512U, std::byte{0x55}),
      },
  };
  ytec::clonecore::GptGuid disk_guid{};
  ytec::clonecore::GptGuid first_guid{};
  ytec::clonecore::GptGuid second_guid{};
  disk_guid.bytes[0] = std::byte{0xD1};
  first_guid.bytes[0] = std::byte{0xA1};
  second_guid.bytes[0] = std::byte{0xA2};
  layout.metadata.target_layout = ytec::clonecore::GptDisk{
      .logical_sector_size = 512U,
      .sector_count = kDiskBytes / 512U,
      .disk_guid = disk_guid,
      .first_usable_lba = 2048U,
      .last_usable_lba = kDiskBytes / 512U - 2049U,
      .partition_entry_count = 128U,
      .partition_entry_size = 128U,
      .partitions = {
          {
              .entry_index = 0U,
              .type_guid = ytec::clonecore::gpt_type_basic_data(),
              .unique_guid = first_guid,
              .first_lba = 1ULL * kMiB / 512U,
              .last_lba = (5ULL * kMiB / 512U) - 1U,
          },
          {
              .entry_index = 1U,
              .type_guid = ytec::clonecore::gpt_type_basic_data(),
              .unique_guid = second_guid,
              .first_lba = 8ULL * kMiB / 512U,
              .last_lba = (8ULL * kMiB + kRawBytes) / 512U - 1U,
          },
      },
  };
  return layout;
}

std::unique_ptr<ytec::windowsapp::WindowsTsumugiShrinkRestoreTransaction>
transaction_for_layout(
    PlatformState& state,
    ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 layout) {
  std::unique_ptr<
      ytec::windowsapp::IWindowsTsumugiShrinkRestorePlatform> platform =
      std::make_unique<Platform>(state, target_identity());
  auto transaction = ytec::windowsapp::
      make_windows_tsumugi_shrink_restore_transaction(
          protected_source(),
          work_paths(),
          work_observation(),
          layout,
          std::move(platform));
  check(transaction.has_value(), "transaction factory should succeed");
  return transaction.take_value();
}

std::unique_ptr<ytec::windowsapp::WindowsTsumugiShrinkRestoreTransaction>
transaction_for(PlatformState& state) {
  return transaction_for_layout(state, reviewed_layout());
}

void test_restore_orders_fs_wim_readback_raw_and_commit() {
  TempDirectory temp;
  Source source;
  const auto path = temp.file(L"restore-order.tsumugi");
  check(
      ytec::imageformat::create_tsumugi_image_v1(
          image_request(path, source))
          .has_value(),
      "mixed shrink fixture should build");
  auto plan = plan_for(path);
  check(plan.has_value(), "mixed shrink restore should preflight");
  PlatformState state;
  auto transaction = transaction_for(state);
  const auto restored = ytec::imageformat::
      execute_tsumugi_shrink_restore_plan_v1(
          plan.value(), std::nullopt, *transaction);
  check(restored.has_value(), "mixed Windows shrink restore should succeed");
  const std::vector<std::string> expected{
      "begin", "reviewed-mbr", "create-fs", "begin-wim", "append-wim", "verify-wim",
      "apply-wim", "readback-fs", "raw-readback", "commit"};
  check(state.events == expected,
        "filesystem, WIM, readback, RAW, and commit order must be exact");
  check(
      state.target_offline && !state.target_incomplete && state.committed &&
          state.abort_count == 0U &&
          state.received_scratch == L"\\\\?\\E:\\YTEC\\scratch" &&
          restored.value().callbacks_started_after_complete_verification &&
          restored.value().all_payloads_verified_by_adapter &&
          restored.value().final_layout_committed,
      "success must remain offline and prove all verification gates");
}

void test_empty_file_system_payload_is_verified_then_recreated() {
  TempDirectory temp;
  Source source;
  const auto path = temp.file(L"restore-empty-fs.tsumugi");
  auto request = image_request(path, source);
  request.manifest.partitions.front().used_bytes = 0U;
  check(
      ytec::imageformat::create_tsumugi_image_v1(request).has_value(),
      "empty filesystem fixture should build");

  auto layout = reviewed_layout();
  layout.migration.target_partitions.front().action =
      ytec::migrationcore::MigrationPartitionAction::create_empty_ntfs;
  auto plan = plan_for_layout(path, layout);
  check(plan.has_value(), "empty filesystem restore should preflight");

  PlatformState state;
  auto transaction = transaction_for_layout(state, layout);
  const auto restored = ytec::imageformat::
      execute_tsumugi_shrink_restore_plan_v1(
          plan.value(), std::nullopt, *transaction);
  check(restored.has_value(), "empty filesystem restore should succeed");
  const std::vector<std::string> expected{
      "begin", "reviewed-mbr", "create-fs", "readback-fs",
      "raw-readback", "commit"};
  check(
      state.events == expected && state.staged_wim.empty() &&
          restored.value().completed_empty_file_system_partitions == 1U,
      "empty WIM must be fully authenticated by the service, discarded, then recreated and read back without DISM apply");
}

void test_apply_cancellation_aborts_offline_incomplete() {
  TempDirectory temp;
  Source source;
  const auto path = temp.file(L"restore-cancel.tsumugi");
  check(
      ytec::imageformat::create_tsumugi_image_v1(
          image_request(path, source))
          .has_value(),
      "cancel fixture should build");
  auto plan = plan_for(path);
  check(plan.has_value(), "cancel fixture should preflight");
  PlatformState state{.fail_apply_as_cancel = true};
  auto transaction = transaction_for(state);
  const auto restored = ytec::imageformat::
      execute_tsumugi_shrink_restore_plan_v1(
          plan.value(), std::nullopt, *transaction);
  check(!restored.has_value(), "injected apply cancellation must fail");
  check(
      state.target_offline && state.target_incomplete && !state.committed &&
          state.abort_count == 1U && !state.events.empty() &&
          state.events.back() == "abort",
      "cancelled restore must stay offline incomplete without final commit");
}

void test_source_disk_work_placement_fails_before_platform() {
  PlatformState state;
  std::unique_ptr<
      ytec::windowsapp::IWindowsTsumugiShrinkRestorePlatform> platform =
      std::make_unique<Platform>(state, target_identity());
  auto observation = work_observation();
  observation.scratch.backing_disk = protected_source();
  const auto result = ytec::windowsapp::
      make_windows_tsumugi_shrink_restore_transaction(
          protected_source(),
          work_paths(),
          observation,
          reviewed_layout(),
          std::move(platform));
  check(!result.has_value(), "source-disk scratch must fail closed");
  check(state.events.empty(),
        "unsafe work placement must stop before destructive platform begin");
}

void test_raw_source_alignment_and_target_shortage_fail_before_write() {
  TempDirectory temp;
  Source source;
  auto unaligned = image_request(
      temp.file(L"unaligned-raw.tsumugi"), source);
  unaligned.chunks[1].source_offset += 1U;
  check(
      !ytec::imageformat::create_tsumugi_image_v1(unaligned).has_value(),
      "exact RAW source offset must remain sector aligned");

  const auto path = temp.file(L"short-target.tsumugi");
  check(
      ytec::imageformat::create_tsumugi_image_v1(
          image_request(path, source))
          .has_value(),
      "short target fixture should build");
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  });
  check(verified.has_value(), "short target fixture should verify");
  auto short_layout = reviewed_layout();
  short_layout.migration.target_partitions.front().size_bytes = 512U;
  short_layout.migration.unallocated_tail_bytes += 4ULL * kMiB - 512U;
  std::get<ytec::clonecore::MbrDisk>(
      short_layout.metadata.target_layout)
      .partitions.front()
      .sector_count = 1U;
  PlatformState state;
  auto transaction = transaction_for_layout(state, short_layout);
  const ytec::imageformat::TsumugiWholeDiskRestoreTarget short_target{
      .disk = target_identity(),
      .reviewed_shrink_layout = std::move(short_layout),
      .shrink_placements = {
          {.source_table_index = 1U,
           .target_offset = 1ULL * kMiB,
           .target_size = 512U},
          {.source_table_index = 2U,
           .target_offset = 8ULL * kMiB,
           .target_size = kRawBytes},
      },
  };
  const auto begun = transaction->begin(
      verified.value(),
      ytec::imageformat::TsumugiRestoreTarget{short_target},
      ytec::imageformat::TsumugiRestoreHost::windows);
  check(!begun.has_value(), "target below minimum must fail closed");
  check(state.events.empty(),
        "target shortage must fail before destructive platform begin");
}

void test_reviewed_layout_rejects_changed_placement_before_platform() {
  TempDirectory temp;
  Source source;
  const auto path = temp.file(L"changed-placement.tsumugi");
  check(
      ytec::imageformat::create_tsumugi_image_v1(
          image_request(path, source))
          .has_value(),
      "changed placement fixture should build");
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  });
  check(verified.has_value(), "changed placement fixture should verify");
  const auto plan = plan_for(path);
  check(plan.has_value(), "changed placement fixture should preflight");
  auto changed_target = std::get<
      ytec::imageformat::TsumugiWholeDiskRestoreTarget>(
      plan.value().target());
  changed_target.shrink_placements.front().target_offset += kMiB;

  PlatformState state;
  auto transaction = transaction_for(state);
  const auto begun = transaction->begin(
      verified.value(),
      ytec::imageformat::TsumugiRestoreTarget{changed_target},
      ytec::imageformat::TsumugiRestoreHost::windows);
  check(!begun.has_value(), "reviewed placement drift must fail closed");
  check(state.events.empty(),
        "reviewed placement drift must stop before platform begin");

  auto tampered_layout = reviewed_layout();
  std::get<ytec::clonecore::MbrDisk>(
      tampered_layout.metadata.target_layout)
      .disk_signature ^= 1U;
  PlatformState layout_state;
  auto layout_transaction =
      transaction_for_layout(layout_state, std::move(tampered_layout));
  const auto layout_begun = layout_transaction->begin(
      verified.value(),
      plan.value().target(),
      ytec::imageformat::TsumugiRestoreHost::windows);
  check(!layout_begun.has_value(),
        "reviewed final-layout identity drift must fail closed");
  check(layout_state.events.empty(),
        "reviewed final-layout drift must stop before platform begin");
}

void test_platform_receives_exact_reviewed_gpt_layout() {
  TempDirectory temp;
  Source source;
  const auto path = temp.file(L"reviewed-gpt.tsumugi");
  auto request = image_request(path, source);
  request.manifest.partition_style =
      ytec::imageformat::TsumugiManifestPartitionStyle::gpt;
  request.manifest.partition_snapshot = gpt_partition_snapshot();
  check(
      ytec::imageformat::create_tsumugi_image_v1(
          request)
          .has_value(),
      "reviewed GPT fixture should build");
  auto plan = plan_for_layout(path, reviewed_gpt_layout());
  check(plan.has_value(), "reviewed GPT fixture should preflight");
  PlatformState state;
  auto transaction = transaction_for_layout(state, reviewed_gpt_layout());
  const auto restored = ytec::imageformat::
      execute_tsumugi_shrink_restore_plan_v1(
          plan.value(), std::nullopt, *transaction);
  check(restored.has_value(), "reviewed GPT layout should restore");
  check(state.events.size() >= 2U && state.events[0] == "begin" &&
            state.events[1] == "reviewed-gpt",
        "platform must receive the exact reviewed GPT layout");
}

void test_platform_exception_poison_aborts_transaction() {
  TempDirectory temp;
  Source source;
  const auto path = temp.file(L"platform-exception.tsumugi");
  check(
      ytec::imageformat::create_tsumugi_image_v1(
          image_request(path, source))
          .has_value(),
      "platform exception fixture should build");
  auto plan = plan_for(path);
  check(plan.has_value(), "platform exception fixture should preflight");
  PlatformState state{.throw_create_file_system = true};
  auto transaction = transaction_for(state);
  const auto restored = ytec::imageformat::
      execute_tsumugi_shrink_restore_plan_v1(
          plan.value(), std::nullopt, *transaction);
  check(!restored.has_value(), "platform exception must become a failure");
  check(
      state.abort_count == 1U && state.target_offline &&
          state.target_incomplete && !state.committed,
      "platform exception must poison and abort offline incomplete exactly once");
}

void test_failed_commit_poison_cannot_retry() {
  TempDirectory temp;
  Source source;
  const auto path = temp.file(L"commit-failure.tsumugi");
  check(
      ytec::imageformat::create_tsumugi_image_v1(
          image_request(path, source))
          .has_value(),
      "commit failure fixture should build");
  auto plan = plan_for(path);
  check(plan.has_value(), "commit failure fixture should preflight");
  PlatformState state{.fail_commit = true};
  auto transaction = transaction_for(state);
  const auto restored = ytec::imageformat::
      execute_tsumugi_shrink_restore_plan_v1(
          plan.value(), std::nullopt, *transaction);
  check(!restored.has_value(), "injected final commit failure must fail");
  const auto retried = transaction->commit();
  check(!retried.has_value(), "poisoned transaction must reject commit retry");
  check(
      state.abort_count == 1U && state.target_offline &&
          state.target_incomplete && !state.committed &&
          std::count(state.events.begin(), state.events.end(), "commit") == 1,
      "failed final commit must abort once and never reach platform again");
}

}  // namespace

int main() {
  try {
    test_restore_orders_fs_wim_readback_raw_and_commit();
    test_empty_file_system_payload_is_verified_then_recreated();
    test_apply_cancellation_aborts_offline_incomplete();
    test_source_disk_work_placement_fails_before_platform();
    test_raw_source_alignment_and_target_shortage_fail_before_write();
    test_reviewed_layout_rejects_changed_placement_before_platform();
    test_platform_receives_exact_reviewed_gpt_layout();
    test_platform_exception_poison_aborts_transaction();
    test_failed_commit_poison_cannot_retry();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All Windows shrink restore transaction tests passed\n";
  return 0;
}

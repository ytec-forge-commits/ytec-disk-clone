#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_image_service.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kDiskBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionOffset = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionBytes = 8ULL * 1024ULL;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

bool path_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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

class TempDirectory final {
 public:
  TempDirectory() {
    std::array<wchar_t, MAX_PATH + 1U> root{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(root.size()), root.data());
    check(length != 0U && length < root.size(), "GetTempPathW failed");
    path_ = root.data();
    path_ += L"ytec-tsumugi-service-";
    path_ += std::to_wstring(GetCurrentProcessId());
    path_ += L"-";
    path_ += std::to_wstring(GetTickCount64());
    check(CreateDirectoryW(path_.c_str(), nullptr) != FALSE,
          "CreateDirectoryW failed");
  }

  ~TempDirectory() {
    const std::wstring pattern = path_ + L"\\*";
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &found);
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

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] std::wstring file(const std::wstring_view name) const {
    return path_ + L"\\" + std::wstring(name);
  }

 private:
  std::wstring path_;
};

class MemorySource final
    : public ytec::imageformat::ITsumugiImageSourceSession {
 public:
  explicit MemorySource(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kDiskBytes;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x21};
    return result;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x34};
    return result;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x55};
    return result;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++read_count;
    if (fail_read || offset > bytes_.size() ||
        length > bytes_.size() - static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"合成画像Source読取り", L"注入した読取り失敗です"));
    }
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(begin, begin +
            static_cast<std::ptrdiff_t>(length)));
  }

  mutable std::size_t read_count{};
  bool fail_read{};

 private:
  std::vector<std::byte> bytes_;
};

class RescueStagingSource final
    : public ytec::imageformat::ITsumugiImageSourceSession {
 public:
  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kDiskBytes;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x21};
    return result;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x34};
    return result;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x55};
    return result;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++read_count;
    if (offset > kDiskBytes || length > kDiskBytes - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"合成救出一時Source読取り", L"範囲外読取りです"));
    }
    std::vector<std::byte> bytes(length);
    for (std::size_t index = 0U; index < length; ++index) {
      bytes[index] = static_cast<std::byte>(
          ((offset + index) * 17U + 9U) & 0xFFU);
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

  mutable std::size_t read_count{};
};

class FaultingRescueSource final
    : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit FaultingRescueSource(const std::uint64_t fault_offset)
      : fault_offset_(fault_offset) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kDiskBytes;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++read_count;
    if (offset > kDiskBytes || length > kDiskBytes - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"合成故障Source読取り", L"範囲外読取りです"));
    }
    if (offset <= fault_offset_ && fault_offset_ < offset + length) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"合成故障Source読取り", L"注入したsector故障です"));
    }
    std::vector<std::byte> bytes(length);
    for (std::size_t index = 0U; index < length; ++index) {
      bytes[index] = static_cast<std::byte>(
          ((offset + index) * 17U + 9U) & 0xFFU);
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  }

  mutable std::size_t read_count{};

 private:
  std::uint64_t fault_offset_{};
};

class MemoryRescueStagingSession final
    : public ytec::imageformat::ITsumugiRescueStagingSession {
 public:
  MemoryRescueStagingSession()
      : bytes_(static_cast<std::size_t>(kDiskBytes), std::byte{0}) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kDiskBytes;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x21};
    return result;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x34};
    return result;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest result{};
    result[0] = std::byte{0x55};
    return result;
  }

  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (sealed_ || discarded_ || offset > kDiskBytes ||
        bytes.size() > kDiskBytes - offset) {
      return ytec::clonecore::Status::failure(
          test_error(L"合成救出一時領域書込み", L"書込み状態または範囲が不正です"));
    }
    std::copy(
        bytes.begin(), bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (discarded_ || offset > kDiskBytes || length > kDiskBytes - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"合成救出一時領域読戻し", L"読戻し状態または範囲が不正です"));
    }
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            begin, begin + static_cast<std::ptrdiff_t>(length)));
  }

  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    if (discarded_) {
      return ytec::clonecore::Status::failure(
          test_error(L"合成救出一時領域flush", L"破棄後のflushです"));
    }
    ++flush_count;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status seal_for_image_read() override {
    if (discarded_ || flush_count == 0U) {
      return ytec::clonecore::Status::failure(
          test_error(L"合成救出一時領域封印", L"flush前または破棄後です"));
    }
    sealed_ = true;
    ++seal_count;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] bool sealed_for_image_read() const noexcept override {
    return sealed_;
  }

  [[nodiscard]] ytec::clonecore::Status
  discard_owned_staging() noexcept override {
    ++discard_count;
    if (fail_discard) {
      return ytec::clonecore::Status::failure(
          test_error(L"合成救出一時領域破棄", L"注入した破棄失敗です"));
    }
    if (!discarded_) {
      bytes_.clear();
      bytes_.shrink_to_fit();
      discarded_ = true;
      sealed_ = false;
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status
  validate_image_destination_before_commit(
      const std::uint64_t expected_owned_partial_bytes) override {
    ++destination_validation_count;
    if (fail_destination_validation || !discarded_ ||
        expected_owned_partial_bytes == 0U) {
      return ytec::clonecore::Status::failure(test_error(
          L"合成救出画像保存先再識別",
          L"注入した保存先再識別失敗または一時領域未破棄です"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!sealed_ || discarded_ || offset > kDiskBytes ||
        length > kDiskBytes - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"合成救出封印Source読取り", L"未封印、破棄済み、または範囲外です"));
    }
    ++image_read_count;
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            begin, begin + static_cast<std::ptrdiff_t>(length)));
  }

  std::size_t flush_count{};
  std::size_t seal_count{};
  std::size_t discard_count{};
  std::size_t destination_validation_count{};
  mutable std::size_t image_read_count{};
  bool fail_discard{};
  bool fail_destination_validation{};
  bool discarded_{};

 private:
  std::vector<std::byte> bytes_;
  bool sealed_{};
};

std::vector<std::byte> source_bytes(const std::uint8_t seed = 7U) {
  std::vector<std::byte> bytes(static_cast<std::size_t>(kPartitionBytes));
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>((index * 29U + seed) & 0xFFU);
  }
  return bytes;
}

std::vector<std::byte> mbr_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  ytec::imageformat::PartitionTableRegion region;
  region.disk_offset = 0U;
  region.data.assign(512U, std::byte{0});
  region.data[446U + 4U] = std::byte{0x07};
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));
  const auto built = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(built.has_value(), "synthetic MBR snapshot should build");
  return built.value();
}

std::vector<std::byte> gpt_snapshot() {
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
  const auto built = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(built.has_value(), "synthetic GPT snapshot should build");
  return built.value();
}

bool is_selected(
    const ytec::imageformat::TsumugiManifestPartition& partition) {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              ytec::imageformat::TsumugiManifestPartitionFlags::selected)) !=
      0U;
}

ytec::imageformat::TsumugiManifestPartition shrink_partition(
    const std::uint32_t table_index,
    const ytec::imageformat::TsumugiManifestPartitionRole role,
    const ytec::imageformat::TsumugiManifestFileSystem file_system,
    const std::uint64_t source_offset,
    const std::uint64_t payload_offset,
    const bool archive,
    const ytec::imageformat::TsumugiManifestPartitionFlags flags) {
  using namespace ytec::imageformat;
  constexpr std::uint64_t source_size = 8192U;
  TsumugiManifestPartition partition{
      .source_table_index = table_index,
      .source_partition_number = table_index,
      .role = role,
      .file_system = file_system,
      .flags = flags,
      .source_offset = source_offset,
      .source_size = source_size,
      .used_bytes = archive ? 4096U : source_size,
      .minimum_target_bytes = archive ? 4096U : source_size,
      .planned_target_bytes = archive ? 4096U : source_size,
      .payload_logical_offset = payload_offset,
      .payload_logical_length = archive ? 4096U : source_size,
      .payload_encoding = archive
          ? TsumugiManifestPayloadEncoding::microsoft_wim_single_image
          : TsumugiManifestPayloadEncoding::exact_raw,
      .payload_format_version = archive
          ? kTsumugiWimPayloadFormatVersion
          : 0U,
      .cluster_size = archive ? 4096U : 0U,
      .name_utf8 = "Synthetic",
      .label_utf8 = "Synthetic",
  };
  partition.type_id[0] = std::byte{0x07};
  return partition;
}

ytec::imageformat::TsumugiManifest shrink_binding_manifest(
    const ytec::imageformat::TsumugiManifestPartitionStyle source_style,
    const bool windows,
    std::vector<ytec::imageformat::TsumugiManifestPartition> partitions) {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::shrink,
      .partition_style = source_style,
      .flags = windows
          ? TsumugiManifestFlags::source_contains_windows
          : TsumugiManifestFlags::none,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-09T00:00:00Z",
      .app_version = "1.0.0",
      .partitions = std::move(partitions),
      .partition_snapshot = source_style == TsumugiManifestPartitionStyle::gpt
          ? gpt_snapshot()
          : mbr_snapshot(),
  };
  manifest.source_model_hash[0] = std::byte{0x21};
  manifest.source_serial_hash[0] = std::byte{0x34};
  manifest.source_state_hash[0] = std::byte{0x55};
  check(
      std::all_of(
          manifest.partitions.begin(),
          manifest.partitions.end(),
          is_selected),
      "binding fixtures should select every partition");
  check(
      build_tsumugi_manifest_v1(manifest).has_value(),
      "synthetic binding manifest should be canonical");
  return manifest;
}

ytec::migrationcore::ShrinkPlannedPartition generated_partition(
    const std::uint32_t number,
    const ytec::migrationcore::MigrationPartitionRole role,
    const ytec::migrationcore::MigrationFileSystem file_system,
    const ytec::migrationcore::MigrationPartitionAction action,
    const std::uint64_t offset,
    const std::uint64_t size) {
  return {
      .target_number = number,
      .source_table_index = std::nullopt,
      .role = role,
      .file_system = file_system,
      .action = action,
      .offset_bytes = offset,
      .size_bytes = size,
  };
}

ytec::migrationcore::ShrinkPlannedPartition mapped_partition(
    const std::uint32_t number,
    const std::uint32_t source_index,
    const ytec::migrationcore::MigrationPartitionRole role,
    const ytec::migrationcore::MigrationFileSystem file_system,
    const ytec::migrationcore::MigrationPartitionAction action,
    const std::uint64_t offset,
    const std::uint64_t size) {
  return {
      .target_number = number,
      .source_table_index = source_index,
      .role = role,
      .file_system = file_system,
      .action = action,
      .offset_bytes = offset,
      .size_bytes = size,
      .source_size_bytes = size,
      .source_used_bytes = 4096U,
  };
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
reviewed_layout(
    const ytec::migrationcore::MigrationPartitionStyle style,
    std::vector<ytec::migrationcore::ShrinkPlannedPartition> partitions) {
  using namespace ytec;
  imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 result{
      .migration = migrationcore::ShrinkMigrationPlan{
          .target_style = style,
          .alignment_bytes = 1ULL * 1024ULL * 1024ULL,
          .minimum_target_size_bytes = 16ULL * 1024ULL * 1024ULL,
          .target_size_bytes = kDiskBytes,
          .source_remains_unchanged = true,
          .target_partitions = std::move(partitions),
      },
      .metadata = imageformat::TsumugiWholeDiskRestoreLayoutPlan{
          .style = style == migrationcore::MigrationPartitionStyle::gpt
              ? imageformat::PartitionTableStyle::gpt
              : imageformat::PartitionTableStyle::mbr,
          .target_size_bytes = kDiskBytes,
          .logical_sector_size = 512U,
      },
  };
  if (style == migrationcore::MigrationPartitionStyle::gpt) {
    clonecore::GptDisk disk{
        .logical_sector_size = 512U,
        .sector_count = kDiskBytes / 512U,
        .first_usable_lba = 34U,
        .last_usable_lba = kDiskBytes / 512U - 34U,
        .partition_entry_count = 128U,
        .partition_entry_size = 128U,
    };
    for (const auto& partition : result.migration.target_partitions) {
      disk.partitions.push_back(clonecore::GptPartition{
          .entry_index = partition.target_number - 1U,
          .first_lba = partition.offset_bytes / 512U,
          .last_lba =
              (partition.offset_bytes + partition.size_bytes) / 512U - 1U,
      });
    }
    result.metadata.target_layout = std::move(disk);
  } else {
    clonecore::MbrDisk disk{
        .logical_sector_size = 512U,
        .sector_count = kDiskBytes / 512U,
    };
    for (const auto& partition : result.migration.target_partitions) {
      disk.partitions.push_back(clonecore::MbrPartition{
          .table_index = static_cast<std::uint8_t>(
              partition.target_number - 1U),
          .type = 0x07U,
          .first_lba = static_cast<std::uint32_t>(
              partition.offset_bytes / 512U),
          .sector_count = static_cast<std::uint32_t>(
              partition.size_bytes / 512U),
      });
    }
    result.metadata.target_layout = std::move(disk);
  }
  return result;
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
simple_data_mbr_layout() {
  using namespace ytec::migrationcore;
  return reviewed_layout(
      MigrationPartitionStyle::mbr,
      {mapped_partition(
          1U,
          1U,
          MigrationPartitionRole::data,
          MigrationFileSystem::ntfs,
          MigrationPartitionAction::apply_file_image,
          kPartitionOffset,
          kPartitionBytes / 2U)});
}

ytec::imageformat::TsumugiManifest exact_manifest(
    const bool contains_windows = true) {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::exact,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .flags = contains_windows
          ? TsumugiManifestFlags::source_contains_windows
          : TsumugiManifestFlags::none,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-04T20:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = mbr_snapshot(),
  };
  manifest.source_model_hash[0] = std::byte{0x21};
  manifest.source_serial_hash[0] = std::byte{0x34};
  manifest.source_state_hash[0] = std::byte{0x55};
  TsumugiManifestPartition partition{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = contains_windows
          ? TsumugiManifestPartitionRole::windows
          : TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected |
          (contains_windows
              ? TsumugiManifestPartitionFlags::contains_windows
              : TsumugiManifestPartitionFlags::none),
      .source_offset = kPartitionOffset,
      .source_size = kPartitionBytes,
      .used_bytes = kPartitionBytes,
      .minimum_target_bytes = kPartitionBytes,
      .planned_target_bytes = kPartitionBytes,
      .payload_logical_offset = kPartitionOffset,
      .payload_logical_length = kPartitionBytes,
      .name_utf8 = contains_windows ? "Windows" : "Data",
      .label_utf8 = contains_windows ? "System" : "Data",
  };
  partition.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(partition));
  return manifest;
}

ytec::imageformat::TsumugiImageCreateRequest create_request(
    const std::wstring& path,
    const MemorySource& source,
    const bool contains_windows = true) {
  using namespace ytec::imageformat;
  TsumugiImageCreateRequest request{
      .final_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .manifest = exact_manifest(contains_windows),
      .compression = ImageCompression::zstandard,
      .chunk_size = kImageChunkSize16MiB,
      .verification_block_bytes = 1024U,
  };
  request.chunks = {
      TsumugiStreamBuildChunk{
          .logical_offset = kPartitionOffset,
          .logical_length = kPartitionBytes / 2U,
          .source_offset = 0U,
          .flags = TsumugiChunkFlags::none,
          .source = &source,
      },
      TsumugiStreamBuildChunk{
          .logical_offset = kPartitionOffset + kPartitionBytes / 2U,
          .logical_length = kPartitionBytes / 2U,
          .source_offset = kPartitionBytes / 2U,
          .flags = TsumugiChunkFlags::none,
          .source = &source,
      },
  };
  request.source_session = &source;
  return request;
}

ytec::imageformat::TsumugiImageCreateRequest shrink_create_request(
    const std::wstring& path,
    const MemorySource& source) {
  using namespace ytec::imageformat;
  auto manifest = exact_manifest(false);
  manifest.mode = TsumugiManifestMode::shrink;
  auto& partition = manifest.partitions[0];
  partition.used_bytes = kPartitionBytes / 4U;
  partition.minimum_target_bytes = kPartitionBytes / 2U;
  partition.planned_target_bytes = kPartitionBytes / 2U;
  partition.payload_logical_offset = 0U;
  partition.payload_logical_length = kPartitionBytes / 2U;
  partition.payload_encoding =
      TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
  partition.payload_format_version = kTsumugiWimPayloadFormatVersion;
  partition.cluster_size = 4096U;

  TsumugiImageCreateRequest request{
      .final_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .manifest = std::move(manifest),
      .compression = ImageCompression::zstandard,
      .chunk_size = kImageChunkSize16MiB,
      .verification_block_bytes = 1024U,
  };
  request.chunks = {
      TsumugiStreamBuildChunk{
          .logical_offset = 0U,
          .logical_length = kPartitionBytes / 4U,
          .source_offset = 0U,
          .flags = TsumugiChunkFlags::none,
          .source = &source,
      },
      TsumugiStreamBuildChunk{
          .logical_offset = kPartitionBytes / 4U,
          .logical_length = kPartitionBytes / 4U,
          .source_offset = kPartitionBytes / 4U,
          .flags = TsumugiChunkFlags::none,
          .source = &source,
      },
  };
  request.source_session = &source;
  return request;
}

ytec::imageformat::TsumugiRestoreDiskIdentity target_identity(
    const bool running_windows = false) {
  ytec::imageformat::TsumugiRestoreDiskIdentity identity{
      .disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .is_running_windows_system_disk = running_windows,
  };
  identity.stable_identity_hash[0] = std::byte{0xA5};
  return identity;
}

class MemoryRestoreTransaction final
    : public ytec::imageformat::ITsumugiRestoreTransaction {
 public:
  explicit MemoryRestoreTransaction(
      ytec::imageformat::TsumugiRestoreDiskIdentity current)
      : current_(std::move(current)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::imageformat::TsumugiRestoreDiskIdentity> begin(
      const ytec::imageformat::TsumugiVerifiedImage&,
      const ytec::imageformat::TsumugiRestoreTarget&,
      const ytec::imageformat::TsumugiRestoreHost) override {
    ++begin_count;
    begun = true;
    return ytec::clonecore::Result<
        ytec::imageformat::TsumugiRestoreDiskIdentity>::success(current_);
  }

  [[nodiscard]] ytec::clonecore::Status write_and_verify(
      const ytec::imageformat::TsumugiRestoreWrite& write,
      const std::span<const std::byte> plaintext) override {
    ++write_count;
    if (!begun || fail_write) {
      return ytec::clonecore::Status::failure(test_error(
          L"合成復元トランザクション書込み・読戻し",
          L"注入した書込みまたは読戻し失敗です"));
    }
    if (on_write) {
      return on_write(write, plaintext);
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status commit() override {
    ++commit_count;
    if (!begun || fail_commit) {
      return ytec::clonecore::Status::failure(test_error(
          L"合成復元トランザクション最終commit",
          L"注入したflushまたはパーティション表commit失敗です"));
    }
    committed = true;
    return ytec::clonecore::success_status();
  }

  void abort() noexcept override {
    ++abort_count;
    aborted = true;
  }

  std::function<ytec::clonecore::Status(
      const ytec::imageformat::TsumugiRestoreWrite&,
      std::span<const std::byte>)> on_write;
  bool fail_write{};
  bool fail_commit{};
  bool begun{};
  bool committed{};
  bool aborted{};
  std::size_t begin_count{};
  std::size_t write_count{};
  std::size_t commit_count{};
  std::size_t abort_count{};

 private:
  ytec::imageformat::TsumugiRestoreDiskIdentity current_;
};

class MemoryShrinkRestoreTransaction final
    : public ytec::imageformat::ITsumugiShrinkRestoreTransaction {
 public:
  explicit MemoryShrinkRestoreTransaction(
      ytec::imageformat::TsumugiRestoreDiskIdentity current)
      : current_(std::move(current)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::imageformat::TsumugiRestoreDiskIdentity> begin(
      const ytec::imageformat::TsumugiVerifiedImage&,
      const ytec::imageformat::TsumugiRestoreTarget&,
      ytec::imageformat::TsumugiRestoreHost) override {
    ++begin_count;
    begun = true;
    return ytec::clonecore::Result<
        ytec::imageformat::TsumugiRestoreDiskIdentity>::success(current_);
  }

  [[nodiscard]] ytec::clonecore::Status write_exact_raw_and_verify(
      const ytec::imageformat::TsumugiRestoreWrite&,
      std::span<const std::byte>) override {
    ++raw_write_count;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status
  recreate_empty_file_system_and_verify(
      const ytec::imageformat::TsumugiShrinkArchiveTarget& target) override {
    ++empty_file_system_count;
    empty_file_system_target = target;
    return begun && !fail_empty_file_system
        ? ytec::clonecore::success_status()
        : ytec::clonecore::Status::failure(test_error(
              L"合成空ファイルシステム再作成",
              begun ? L"注入した作成または読戻し失敗です"
                    : L"target beginより前に呼ばれました"));
  }

  [[nodiscard]] ytec::clonecore::Status begin_wim_archive(
      const ytec::imageformat::TsumugiShrinkArchiveTarget& target) override {
    ++archive_begin_count;
    archive_target = target;
    archive.assign(
        static_cast<std::size_t>(target.archive_length), std::byte{0});
    return begun ? ytec::clonecore::success_status()
                 : ytec::clonecore::Status::failure(test_error(
                       L"合成WIM開始", L"target beginより前に呼ばれました"));
  }

  [[nodiscard]] ytec::clonecore::Status append_wim_archive(
      const ytec::imageformat::TsumugiShrinkArchiveChunk& chunk,
      const std::span<const std::byte> plaintext) override {
    ++archive_append_count;
    if (chunk.archive_offset > archive.size() ||
        chunk.length > archive.size() -
            static_cast<std::size_t>(chunk.archive_offset) ||
        (!chunk.zero_fill && plaintext.size() != chunk.length)) {
      return ytec::clonecore::Status::failure(test_error(
          L"合成WIM追記", L"archive範囲または平文長が不正です"));
    }
    const auto begin = archive.begin() +
        static_cast<std::ptrdiff_t>(chunk.archive_offset);
    if (chunk.zero_fill) {
      std::fill_n(begin, static_cast<std::size_t>(chunk.length), std::byte{0});
    } else {
      std::copy(plaintext.begin(), plaintext.end(), begin);
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status
  complete_wim_archive_and_verify(
      const std::uint32_t source_table_index) override {
    ++archive_complete_count;
    if (fail_complete ||
        source_table_index != archive_target.source_table_index) {
      return ytec::clonecore::Status::failure(test_error(
          L"合成WIM適用・読戻し", L"注入したWIM検証失敗です"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status commit() override {
    ++commit_count;
    committed = true;
    return ytec::clonecore::success_status();
  }

  void abort() noexcept override {
    ++abort_count;
    aborted = true;
  }

  bool fail_complete{};
  bool fail_empty_file_system{};
  bool begun{};
  bool committed{};
  bool aborted{};
  std::size_t begin_count{};
  std::size_t raw_write_count{};
  std::size_t empty_file_system_count{};
  std::size_t archive_begin_count{};
  std::size_t archive_append_count{};
  std::size_t archive_complete_count{};
  std::size_t commit_count{};
  std::size_t abort_count{};
  ytec::imageformat::TsumugiShrinkArchiveTarget archive_target;
  ytec::imageformat::TsumugiShrinkArchiveTarget empty_file_system_target;
  std::vector<std::byte> archive;

 private:
  ytec::imageformat::TsumugiRestoreDiskIdentity current_;
};

ytec::imageformat::TsumugiImageVerifyRequest verify_request(
    const std::wstring& path) {
  return ytec::imageformat::TsumugiImageVerifyRequest{
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  };
}

void test_create_verify_and_whole_restore() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  const auto expected = source_bytes();
  MemorySource source(expected);
  const auto path = temp.file(L"whole.tsumugi");
  const auto created = create_tsumugi_image_v1(
      create_request(path, source, false));
  check(created.has_value(), "valid image creation should pass");
  check(created.value().complete_verification_passed &&
            created.value().stream.committed,
        "creation must commit only after complete verification");
  check(source.read_count == 2U,
        "each non-zero payload chunk should be read once");

  const auto verified = verify_tsumugi_image_v1(TsumugiImageVerifyRequest{
      .image_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  });
  if (!verified.has_value()) {
    std::cerr << "verify error code="
              << static_cast<unsigned int>(verified.error().code)
              << " native=" << verified.error().native_code
              << " operation_chars=" << verified.error().operation.size()
              << " message_chars=" << verified.error().message.size()
              << '\n';
  }
  check(verified.has_value() && !verified.value().partial_loss,
        "created image should fully verify without loss");

  auto prepared = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::windows,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
      },
  });
  check(prepared.has_value(), "whole-disk restore plan should prepare");
  check(prepared.value().planned_payload_bytes() == kPartitionBytes,
        "whole restore byte count should equal selected payload");

  std::vector<std::byte> restored(expected.size(), std::byte{0});
  std::size_t callbacks = 0U;
  const auto current_target = target_identity();
  MemoryRestoreTransaction transaction(current_target);
  transaction.on_write =
      [&](const TsumugiRestoreWrite& write,
          const std::span<const std::byte> bytes) {
        check(write.target_offset >= kPartitionOffset,
              "exact restore must retain source partition offset");
        const auto relative = static_cast<std::size_t>(
            write.target_offset - kPartitionOffset);
        check(relative <= restored.size() &&
                  bytes.size() <= restored.size() - relative,
              "restore callback must remain within the target partition");
        std::copy(bytes.begin(), bytes.end(), restored.begin() +
            static_cast<std::ptrdiff_t>(relative));
        ++callbacks;
        return ytec::clonecore::success_status();
      };
  const auto executed = execute_tsumugi_restore_plan_v1(
      prepared.value(),
      std::nullopt,
      transaction);
  check(executed.has_value(), "whole restore execution should pass");
  check(callbacks == 2U && restored == expected,
        "verified callbacks should restore every source byte exactly");
  check(executed.value().callbacks_started_after_complete_verification &&
            executed.value().image_matched_prepared_plan &&
            executed.value().target_reidentified_before_write,
        "callbacks must begin only after image and target re-identification");
  check(executed.value().all_writes_read_back_verified &&
            executed.value().final_layout_committed &&
            transaction.committed && !transaction.aborted,
        "success requires read-back writes and final layout commit");
}

void test_fast_create_reports_selected_verification_without_complete_claim() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes(27U));
  const auto path = temp.file(L"fast-service.tsumugi");
  auto request = create_request(path, source, false);
  request.verification_mode = TsumugiCreateVerificationMode::fast;
  const auto created = create_tsumugi_image_v1(request);
  check(
      created.has_value() && created.value().selected_verification_passed &&
          !created.value().complete_verification_passed &&
          created.value().stream.verification_mode ==
              TsumugiCreateVerificationMode::fast &&
          created.value().stream.all_chunks_authenticated_and_hashed &&
          created.value().stream.final_metadata_read_back_verified &&
          !created.value().stream.final_complete_scan_performed &&
          created.value().stream.committed,
      "fast service creation must pass the selected gates without claiming a complete scan");
  check(
      selected_tsumugi_creation_verification_passed(created.value()),
      "the shared product evidence gate must accept a coherent fast report");
  auto inconsistent = created.value();
  inconsistent.complete_verification_passed = true;
  check(
      !selected_tsumugi_creation_verification_passed(inconsistent),
      "the shared gate must reject a fast report that claims an omitted complete scan");
  auto strengthened = created.value();
  strengthened.complete_verification_passed = true;
  strengthened.stream.final_complete_scan_performed = true;
  check(
      selected_tsumugi_creation_verification_passed(strengthened),
      "the shared gate must accept a fast selection strengthened by persistent final complete verification");
  inconsistent = created.value();
  inconsistent.stream.all_chunks_authenticated_and_hashed = false;
  check(
      !selected_tsumugi_creation_verification_passed(inconsistent),
      "the shared gate must reject fast mode without immediate authentication and plaintext hashes");
  inconsistent = created.value();
  inconsistent.stream.final_metadata_read_back_verified = false;
  check(
      !selected_tsumugi_creation_verification_passed(inconsistent),
      "the shared gate must reject fast mode without final metadata read-back");

  const auto independently_verified =
      verify_tsumugi_image_v1(verify_request(path));
  check(
      independently_verified.has_value() &&
          independently_verified.value().container.all_chunks_verified &&
          independently_verified.value().container.global_hash_verified,
      "a fast-created service image must remain valid under mandatory complete verification");
}

void test_rescue_report_plans_authenticated_chunks_without_source_reread() {
  using namespace ytec;
  using namespace ytec::imageformat;

  auto manifest = exact_manifest(false);
  manifest.mode = TsumugiManifestMode::rescue;
  RescueStagingSource staging;
  clonecore::RescueRawCopyReport report{
      .source_extent_bytes = kDiskBytes,
      .copied_source_bytes = kDiskBytes - 512U,
      .zero_filled_bytes = 512U,
      .written_and_read_back_verified_bytes = kDiskBytes,
      .exhausted_sector_count = 1U,
      .missing_ranges = {
          clonecore::RescueMissingRange{
              .bytes = clonecore::ByteRange{
                  .offset = kPartitionOffset + 2048U,
                  .length = 512U,
              },
              .first_lba = (kPartitionOffset + 2048U) / 512U,
              .sector_count = 1U,
              .forward_attempts = 1U,
              .reverse_attempts = 1U,
              .sector_attempts = 1U,
              .forward_native_error = ERROR_CRC,
              .reverse_native_error = ERROR_SECTOR_NOT_FOUND,
              .sector_native_error = ERROR_READ_FAULT,
              .zero_fill_read_back_verified = true,
          },
      },
      .layout_preserved_without_conversion = true,
      .byte_exact_copy = false,
      .target_flushed = true,
      .all_writes_read_back_verified = true,
      .partial_data_loss = true,
  };

  const auto planned = make_tsumugi_rescue_chunks_v1(
      manifest, report, staging, kImageChunkSize16MiB);
  check(planned.has_value() && planned.value().size() == 3U,
        "verified rescue staging should split around one missing sector");
  check(planned.value()[0].logical_offset == kPartitionOffset &&
            planned.value()[0].logical_length == 2048U &&
            planned.value()[0].source == &staging &&
            planned.value()[1].logical_offset ==
                kPartitionOffset + 2048U &&
            planned.value()[1].logical_length == 512U &&
            planned.value()[1].flags ==
                TsumugiChunkFlags::unreadable_zero_filled &&
            planned.value()[1].source == nullptr &&
            planned.value()[1].rescue_read_evidence.has_value() &&
            planned.value()[1].rescue_read_evidence->sector_native_error ==
                ERROR_READ_FAULT &&
            planned.value()[2].logical_offset ==
                kPartitionOffset + 2560U &&
            planned.value()[2].source == &staging,
        "rescue chunks must preserve disk coordinates and exact retry evidence");
  check(staging.read_count == 0U,
        "rescue planning must not reopen or read the failing source");

  TempDirectory temp;
  const auto path = temp.file(L"rescue-service.tsumugi");
  TsumugiImageCreateRequest request{
      .final_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .manifest = manifest,
      .chunks = planned.value(),
      .compression = ImageCompression::zstandard,
      .chunk_size = kImageChunkSize16MiB,
      .verification_block_bytes = 1024U,
      .source_session = &staging,
  };
  const auto created = create_tsumugi_image_v1(request);
  check(created.has_value() &&
            created.value().unreadable_ranges.size() == 1U &&
            created.value().unreadable_ranges[0].offset ==
                kPartitionOffset + 2048U &&
            created.value().unreadable_ranges[0].length == 512U,
        "the image service must commit the planned missing map");
  const auto verified = verify_tsumugi_image_v1(verify_request(path));
  check(verified.has_value() && verified.value().partial_loss &&
            verified.value().manifest.mode == TsumugiManifestMode::rescue &&
            verified.value().container.records.size() == 3U &&
            verified.value().container.records[1]
                .rescue_read_evidence.has_value(),
        "the final rescue container must authenticate its retry evidence");

  auto lossless = report;
  lossless.copied_source_bytes = kDiskBytes;
  lossless.zero_filled_bytes = 0U;
  lossless.exhausted_sector_count = 0U;
  lossless.missing_ranges.clear();
  lossless.byte_exact_copy = true;
  lossless.partial_data_loss = false;
  const auto lossless_chunks = make_tsumugi_rescue_chunks_v1(
      manifest, lossless, staging, kImageChunkSize16MiB);
  check(lossless_chunks.has_value() && lossless_chunks.value().size() == 1U &&
            lossless_chunks.value()[0].source == &staging,
        "a fully recovered run must remain a valid rescue image plan");
  const auto lossless_path = temp.file(L"lossless-rescue-service.tsumugi");
  request.final_path = lossless_path;
  request.chunks = lossless_chunks.value();
  check(create_tsumugi_image_v1(request).has_value(),
        "a lossless rescue classification should create successfully");
  const auto lossless_verified =
      verify_tsumugi_image_v1(TsumugiImageVerifyRequest{
          .image_path = lossless_path,
          .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
          .verification_block_bytes = 1024U,
      });
  check(lossless_verified.has_value() &&
            !lossless_verified.value().partial_loss &&
            lossless_verified.value().manifest.mode ==
                TsumugiManifestMode::rescue,
        "lossless rescue must remain rescue-classified without a false gap");

  const std::size_t reads_before_rejections = staging.read_count;
  auto not_flushed = report;
  not_flushed.target_flushed = false;
  check(!make_tsumugi_rescue_chunks_v1(
             manifest, not_flushed, staging, kImageChunkSize16MiB)
             .has_value(),
        "an unflushed rescue staging result must fail closed");

  auto outside_payload = report;
  outside_payload.missing_ranges[0].bytes.offset = 0U;
  outside_payload.missing_ranges[0].first_lba = 0U;
  check(!make_tsumugi_rescue_chunks_v1(
             manifest, outside_payload, staging, kImageChunkSize16MiB)
             .has_value(),
        "a missing range outside selected payloads must not be discarded");

  auto missing_attempt = report;
  missing_attempt.missing_ranges[0].forward_attempts = 0U;
  check(!make_tsumugi_rescue_chunks_v1(
             manifest, missing_attempt, staging, kImageChunkSize16MiB)
             .has_value() &&
            staging.read_count == reads_before_rejections,
        "invalid rescue evidence must be rejected before any staging read");
}

void test_rescue_image_service_discards_staging_before_final_commit() {
  using namespace ytec;
  using namespace ytec::imageformat;

  auto manifest = exact_manifest(false);
  manifest.mode = TsumugiManifestMode::rescue;
  TempDirectory temp;
  const auto final_path = temp.file(L"orchestrated-rescue.tsumugi");
  FaultingRescueSource source(kPartitionOffset + 2048U);
  MemoryRescueStagingSession staging;
  TsumugiRescueImageCreateRequest request{
      .image = TsumugiImageCreateRequest{
          .final_path = final_path,
          .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
          .manifest = manifest,
          .compression = ImageCompression::zstandard,
          .chunk_size = kImageChunkSize16MiB,
          .verification_block_bytes = 1024U,
      },
      .rescue_copy = clonecore::RescueRawCopyRequest{
          .environment = clonecore::RescueExecutionEnvironment::windows,
          .source_kind = clonecore::RescueSourceKind::data_disk,
          .rescue_mode_explicitly_confirmed = true,
          .large_block_bytes = 1U * 1024U * 1024U,
      },
      .failing_source = &source,
      .staging = &staging,
  };

  const auto created = create_tsumugi_rescue_image_v1(request);
  check(created.has_value() && created.value().rescue.partial_data_loss &&
            created.value().rescue.missing_ranges.size() == 1U &&
            created.value().rescue.missing_ranges[0].bytes.offset ==
                kPartitionOffset + 2048U &&
            created.value().staging_sealed_for_image_read &&
            created.value().staging_discarded_before_final_commit &&
            created.value()
                .staging_destination_revalidated_before_final_commit &&
            created.value().image.stream.committed &&
            staging.flush_count >= 1U && staging.seal_count == 1U &&
            staging.discard_count == 1U &&
            staging.destination_validation_count == 1U &&
            staging.discarded_ &&
            staging.image_read_count == 2U && path_exists(final_path) &&
            !path_exists(final_path + L".partial"),
        "rescue orchestration must seal, image from staging, discard it, then publish final");
  const auto verified = verify_tsumugi_image_v1(TsumugiImageVerifyRequest{
      .image_path = final_path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  });
  check(verified.has_value() && verified.value().partial_loss &&
            verified.value().container.records.size() == 3U &&
            verified.value().container.records[1]
                .rescue_read_evidence.has_value(),
        "orchestrated final image must authenticate the rescue retry evidence");

  const auto outside_path = temp.file(L"outside-gap-rescue.tsumugi");
  FaultingRescueSource outside_source(0U);
  MemoryRescueStagingSession outside_staging;
  request.image.final_path = outside_path;
  request.failing_source = &outside_source;
  request.staging = &outside_staging;
  const auto outside = create_tsumugi_rescue_image_v1(request);
  check(!outside.has_value() && outside_staging.seal_count == 1U &&
            outside_staging.discard_count == 1U &&
            outside_staging.discarded_ &&
            !path_exists(outside_path) &&
            !path_exists(outside_path + L".partial"),
        "an out-of-payload loss must discard owned staging without publishing an image");

  const auto changed_destination_path =
      temp.file(L"changed-destination-rescue.tsumugi");
  FaultingRescueSource changed_destination_source(
      kPartitionOffset + 2048U);
  MemoryRescueStagingSession changed_destination_staging;
  changed_destination_staging.fail_destination_validation = true;
  request.image.final_path = changed_destination_path;
  request.failing_source = &changed_destination_source;
  request.staging = &changed_destination_staging;
  const auto changed_destination = create_tsumugi_rescue_image_v1(request);
  check(!changed_destination.has_value() &&
            changed_destination_staging.discard_count == 1U &&
            changed_destination_staging.destination_validation_count == 1U &&
            !path_exists(changed_destination_path) &&
            !path_exists(changed_destination_path + L".partial"),
        "destination replacement after staging discard must abort the verified partial without publishing final");

  const auto invalid_path = temp.file(L"invalid-storage-rescue.tsumugi");
  FaultingRescueSource untouched_source(kPartitionOffset + 2048U);
  MemoryRescueStagingSession untouched_staging;
  request.image.final_path = invalid_path;
  request.image.storage_file_system = TsumugiImageStorageFileSystem::fat32;
  request.failing_source = &untouched_source;
  request.staging = &untouched_staging;
  const auto invalid = create_tsumugi_rescue_image_v1(request);
  check(!invalid.has_value() && untouched_source.read_count == 0U &&
            untouched_staging.flush_count == 0U &&
            untouched_staging.seal_count == 0U &&
            untouched_staging.discard_count == 0U &&
            !path_exists(invalid_path),
        "invalid image storage must be rejected before rescue I/O begins");
}

void test_fat32_rejected_before_source_read() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes());
  source.fail_read = true;
  auto request = create_request(temp.file(L"fat32.tsumugi"), source);
  request.storage_file_system = TsumugiImageStorageFileSystem::fat32;
  const auto result = create_tsumugi_image_v1(request);
  check(!result.has_value(), "FAT32 image destination must fail closed");
  check(source.read_count == 0U,
        "FAT32 preflight must reject before reading the source");
}

void test_service_stages_complete_image_until_explicit_commit() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes());
  const auto path = temp.file(L"service-staged.tsumugi");
  auto staged = prepare_tsumugi_image_v1(create_request(path, source));
  check(staged.has_value() && staged.value().pending(),
        "service should return a pending completely verified image");
  check(staged.value().report().complete_verification_passed &&
            !staged.value().report().stream.committed &&
            !path_exists(path) && path_exists(path + L".partial"),
        "service preparation must not expose the final filename");

  const auto committed = staged.value().commit_verified();
  check(committed.has_value() && committed.value().stream.committed &&
            !staged.value().pending(),
        "service commit should complete the underlying stream transaction");
  check(path_exists(path) && !path_exists(path + L".partial") &&
            verify_tsumugi_image_v1(verify_request(path)).has_value(),
        "service commit should expose a fully verifiable image only once");
  check(!staged.value().commit_verified().has_value(),
        "committed service transaction must reject reuse");
}

void test_mixed_source_sessions_are_rejected_before_read() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource first(source_bytes(1U));
  MemorySource second(source_bytes(2U));
  auto request = create_request(temp.file(L"mixed-source.tsumugi"), first);
  request.chunks[1].source = &second;
  const auto result = create_tsumugi_image_v1(request);
  check(!result.has_value(),
        "chunks from different source sessions must fail closed");
  check(first.read_count == 0U && second.read_count == 0U,
        "mixed source sessions must be rejected before any source read");
}

void test_encrypted_image_requires_the_password() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes());
  const auto path = temp.file(L"encrypted.tsumugi");
  auto request = create_request(path, source);
  request.encryption = TsumugiImageEncryptionRequest{
      .password = "abcdefgh",
  };
  const auto created = create_tsumugi_image_v1(request);
  check(created.has_value() && created.value().encrypted,
        "encrypted service creation should pass");
  check(created.value().password_was_weak,
        "accepted weak password should be surfaced as a warning");

  auto wrong = verify_request(path);
  wrong.password = std::string_view("wrong-pass");
  check(!verify_tsumugi_image_v1(wrong).has_value(),
        "wrong password must fail authentication");
  auto correct = verify_request(path);
  correct.password = std::string_view("abcdefgh");
  check(verify_tsumugi_image_v1(correct).has_value(),
        "correct password should verify the whole image");
}

void test_running_windows_target_is_rejected() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes());
  const auto path = temp.file(L"running-windows.tsumugi");
  check(create_tsumugi_image_v1(create_request(path, source)).has_value(),
        "fixture image creation should pass");
  const auto plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::windows,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(true),
      },
  });
  check(!plan.has_value(),
        "Windows host must reject its running system disk as restore target");
}

void test_replaced_image_is_rejected_before_first_write() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  const auto path = temp.file(L"replace.tsumugi");
  MemorySource first(source_bytes(3U));
  check(create_tsumugi_image_v1(create_request(path, first)).has_value(),
        "first fixture image should build");
  auto prepared = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
      },
  });
  check(prepared.has_value(), "restore plan should prepare for first image");

  MemorySource second(source_bytes(91U));
  auto replacement = create_request(path, second);
  replacement.replace_existing = true;
  check(create_tsumugi_image_v1(replacement).has_value(),
        "second valid image should replace the first transactionally");

  MemoryRestoreTransaction transaction(target_identity());
  const auto executed = execute_tsumugi_restore_plan_v1(
      prepared.value(),
      std::nullopt,
      transaction);
  check(!executed.has_value(),
        "image changed after planning must fail same-handle binding");
  check(transaction.begin_count == 0U && transaction.write_count == 0U,
        "changed image must be rejected before the first target write");
}

void test_target_identity_drift_is_rejected_before_first_write() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes());
  const auto path = temp.file(L"target-drift.tsumugi");
  check(create_tsumugi_image_v1(create_request(path, source)).has_value(),
        "target drift fixture should build");
  auto prepared = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
      },
  });
  check(prepared.has_value(), "target drift plan should prepare");

  auto changed = target_identity();
  ++changed.disk_size;
  MemoryRestoreTransaction transaction(changed);
  const auto executed = execute_tsumugi_restore_plan_v1(
      prepared.value(),
      std::nullopt,
      transaction);
  check(!executed.has_value(),
        "changed target geometry must fail final re-identification");
  check(transaction.write_count == 0U && transaction.abort_count == 1U,
        "target drift must be rejected before the first write");
}

void test_transaction_failures_abort_and_plan_is_single_use() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes());
  const auto path = temp.file(L"transaction-failure.tsumugi");
  check(create_tsumugi_image_v1(create_request(path, source)).has_value(),
        "transaction failure fixture should build");

  auto write_plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
      },
  });
  check(write_plan.has_value(), "write failure plan should prepare");
  MemoryRestoreTransaction write_failure(target_identity());
  write_failure.fail_write = true;
  check(!execute_tsumugi_restore_plan_v1(
             write_plan.value(), std::nullopt, write_failure)
             .has_value(),
        "write/read-back failure must fail the restore");
  check(write_failure.begin_count == 1U &&
            write_failure.write_count == 1U &&
            write_failure.commit_count == 0U &&
            write_failure.abort_count == 1U,
        "write failure must abort without final layout commit");

  MemoryRestoreTransaction repeated(target_identity());
  check(!execute_tsumugi_restore_plan_v1(
             write_plan.value(), std::nullopt, repeated)
             .has_value(),
        "the same prepared plan must not execute twice");
  check(repeated.begin_count == 0U && repeated.write_count == 0U,
        "second execution must stop before target acquisition");

  auto commit_plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
      },
  });
  check(commit_plan.has_value(), "commit failure plan should prepare");
  MemoryRestoreTransaction commit_failure(target_identity());
  commit_failure.fail_commit = true;
  check(!execute_tsumugi_restore_plan_v1(
             commit_plan.value(), std::nullopt, commit_failure)
             .has_value(),
        "flush/final layout commit failure must fail the restore");
  check(commit_failure.write_count == 2U &&
            commit_failure.commit_count == 1U &&
            commit_failure.abort_count == 1U,
        "commit failure must keep the transaction incomplete and abort it");
}

void test_forbidden_restore_media_are_rejected() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes());
  const auto path = temp.file(L"forbidden-target.tsumugi");
  check(create_tsumugi_image_v1(create_request(path, source)).has_value(),
        "forbidden target fixture should build");

  auto usb_memory = target_identity();
  usb_memory.is_usb_attached = true;
  usb_memory.is_usb_memory = true;
  usb_memory.connection_instance_hash[0] = std::byte{0x44};
  const auto plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = usb_memory,
      },
  });
  check(!plan.has_value(), "USB memory must never be a restore target");

  auto reconnected_usb_disk = target_identity();
  reconnected_usb_disk.is_usb_attached = true;
  check(!prepare_tsumugi_restore_plan_v1({
             .image = verify_request(path),
             .host = TsumugiRestoreHost::winpe,
             .target = TsumugiWholeDiskRestoreTarget{
                 .disk = reconnected_usb_disk,
             },
         }).has_value(),
        "USB disk without a connection-session identity must be reselected");
}

void test_individual_windows_restore_offers_boot_repair() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes());
  const auto path = temp.file(L"individual.tsumugi");
  check(create_tsumugi_image_v1(create_request(path, source)).has_value(),
        "Windows fixture image should build");
  const auto plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiIndividualPartitionRestoreTarget{
          .source_table_index = 1U,
          .target = TsumugiUnallocatedRestoreTarget{
              .disk = target_identity(),
              .target_offset = 2ULL * 1024ULL * 1024ULL,
              .target_size = kPartitionBytes,
          },
      },
  });
  check(plan.has_value() && plan.value().requires_boot_repair_offer(),
        "Windows-only partition restore should offer boot repair");
}

void test_shrink_wim_requires_dedicated_verified_adapter() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  const auto bytes = source_bytes(73U);
  MemorySource source(bytes);
  const auto path = temp.file(L"shrink-wim.tsumugi");
  check(
      create_tsumugi_image_v1(shrink_create_request(path, source)).has_value(),
      "single-WIM shrink payload should be representable in one .tsumugi");
  const auto verified = verify_tsumugi_image_v1(verify_request(path));
  check(
      verified.has_value() &&
          tsumugi_manifest_requires_shrink_archive_adapter(
              verified.value().manifest),
      "verified shrink image must retain its dedicated-adapter requirement");

  auto block_plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
          .reviewed_shrink_layout = simple_data_mbr_layout(),
      },
  });
  check(block_plan.has_value(), "shrink target geometry should preflight");
  MemoryRestoreTransaction block_transaction(target_identity());
  const auto blocked = execute_tsumugi_restore_plan_v1(
      block_plan.value(), std::nullopt, block_transaction);
  check(
      !blocked.has_value() && block_transaction.begin_count == 0U &&
          block_transaction.write_count == 0U,
      "normal block restore must never write WIM archive bytes as sectors");

  auto shrink_plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
          .reviewed_shrink_layout = simple_data_mbr_layout(),
      },
  });
  check(shrink_plan.has_value(), "dedicated shrink plan should preflight");
  MemoryShrinkRestoreTransaction shrink_transaction(target_identity());
  const auto restored = execute_tsumugi_shrink_restore_plan_v1(
      shrink_plan.value(), std::nullopt, shrink_transaction);
  check(restored.has_value(), "dedicated synthetic shrink adapter should run");
  check(
      restored.value().callbacks_started_after_complete_verification &&
          restored.value().image_matched_prepared_plan &&
          restored.value().target_reidentified_before_write &&
          restored.value().archive_logical_bytes == kPartitionBytes / 2U &&
          restored.value().archive_chunk_count == 2U &&
          restored.value().completed_archive_partitions == 1U &&
          restored.value().all_payloads_verified_by_adapter &&
          restored.value().final_layout_committed,
      "success must prove full verification, re-identification, WIM apply verification, and commit");
  check(
      shrink_transaction.archive_begin_count == 1U &&
          shrink_transaction.archive_append_count == 2U &&
          shrink_transaction.archive_complete_count == 1U &&
          shrink_transaction.raw_write_count == 0U &&
          shrink_transaction.commit_count == 1U &&
          !shrink_transaction.aborted &&
          std::equal(
              shrink_transaction.archive.begin(),
              shrink_transaction.archive.end(),
              bytes.begin()),
      "the adapter should receive the authenticated WIM bytes in canonical order");
}

void test_shrink_wim_apply_failure_aborts_before_commit() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  MemorySource source(source_bytes(19U));
  const auto path = temp.file(L"shrink-wim-failure.tsumugi");
  check(
      create_tsumugi_image_v1(shrink_create_request(path, source)).has_value(),
      "shrink failure fixture should build");
  auto plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
          .reviewed_shrink_layout = simple_data_mbr_layout(),
      },
  });
  check(plan.has_value(), "shrink failure plan should preflight");
  MemoryShrinkRestoreTransaction transaction(target_identity());
  transaction.fail_complete = true;
  check(
      !execute_tsumugi_shrink_restore_plan_v1(
           plan.value(), std::nullopt, transaction)
           .has_value(),
      "WIM apply/read-back failure must fail the transaction");
  check(
      transaction.begin_count == 1U && transaction.commit_count == 0U &&
          transaction.abort_count == 1U,
      "WIM apply/read-back failure must abort without final layout commit");
}

void test_changed_shrink_image_is_rejected_before_target_begin() {
  using namespace ytec::imageformat;
  TempDirectory temp;
  const auto path = temp.file(L"shrink-wim-replaced.tsumugi");
  MemorySource first(source_bytes(31U));
  check(
      create_tsumugi_image_v1(shrink_create_request(path, first)).has_value(),
      "first shrink fixture should build");
  auto plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
          .reviewed_shrink_layout = simple_data_mbr_layout(),
      },
  });
  check(plan.has_value(), "first shrink plan should prepare");

  MemorySource second(source_bytes(109U));
  auto replacement = shrink_create_request(path, second);
  replacement.replace_existing = true;
  check(
      create_tsumugi_image_v1(replacement).has_value(),
      "second shrink image should replace the first transactionally");
  MemoryShrinkRestoreTransaction transaction(target_identity());
  check(
      !execute_tsumugi_shrink_restore_plan_v1(
           plan.value(), std::nullopt, transaction)
           .has_value(),
      "a shrink image changed after review must fail execution");
  check(
      transaction.begin_count == 0U &&
          transaction.archive_begin_count == 0U &&
          transaction.raw_write_count == 0U,
      "changed shrink image must fail after full verify but before target begin");
}

const ytec::imageformat::TsumugiShrinkPayloadBindingV1* find_binding(
    const std::vector<ytec::imageformat::TsumugiShrinkPayloadBindingV1>&
        bindings,
    const std::uint32_t source_table_index) {
  const auto found = std::find_if(
      bindings.begin(), bindings.end(),
      [source_table_index](const auto& binding) {
        return binding.source_table_index == source_table_index;
      });
  return found == bindings.end() ? nullptr : &*found;
}

void test_reviewed_layout_derives_only_authenticated_payload_bindings() {
  using namespace ytec::imageformat;
  using namespace ytec::migrationcore;
  constexpr auto selected = TsumugiManifestPartitionFlags::selected;
  constexpr auto required = TsumugiManifestPartitionFlags::required;
  constexpr auto contains_windows =
      TsumugiManifestPartitionFlags::contains_windows;
  constexpr auto active = TsumugiManifestPartitionFlags::active;

  auto gpt_windows = shrink_binding_manifest(
      TsumugiManifestPartitionStyle::gpt,
      true,
      {
          shrink_partition(
              1U,
              TsumugiManifestPartitionRole::efi_system,
              TsumugiManifestFileSystem::fat32,
              1ULL * 1024ULL * 1024ULL,
              0U,
              false,
              selected | required),
          shrink_partition(
              2U,
              TsumugiManifestPartitionRole::microsoft_reserved,
              TsumugiManifestFileSystem::none,
              2ULL * 1024ULL * 1024ULL,
              8192U,
              false,
              selected | required),
          shrink_partition(
              3U,
              TsumugiManifestPartitionRole::windows,
              TsumugiManifestFileSystem::ntfs,
              3ULL * 1024ULL * 1024ULL,
              16384U,
              true,
              selected | required | contains_windows),
      });
  auto gpt_windows_layout = reviewed_layout(
      MigrationPartitionStyle::gpt,
      {
          generated_partition(
              1U,
              MigrationPartitionRole::efi_system,
              MigrationFileSystem::fat32,
              MigrationPartitionAction::create_fat32,
              1ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          generated_partition(
              2U,
              MigrationPartitionRole::microsoft_reserved,
              MigrationFileSystem::none,
              MigrationPartitionAction::create_reserved,
              2ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          mapped_partition(
              3U,
              3U,
              MigrationPartitionRole::windows,
              MigrationFileSystem::ntfs,
              MigrationPartitionAction::apply_file_image,
              3ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
      });
  const auto gpt_bindings = make_tsumugi_shrink_payload_bindings_v1(
      gpt_windows, gpt_windows_layout);
  check(gpt_bindings.has_value() && gpt_bindings.value().size() == 3U,
        "GPT-to-GPT should bind all selected payloads");
  check(
      find_binding(gpt_bindings.value(), 1U)->disposition ==
              TsumugiShrinkPayloadDispositionV1::regenerate_efi_system &&
          find_binding(gpt_bindings.value(), 2U)->disposition ==
              TsumugiShrinkPayloadDispositionV1::
                  regenerate_microsoft_reserved &&
          find_binding(gpt_bindings.value(), 3U)->target_number == 3U,
      "GPT ESP/MSR may only be omitted for their explicit regenerated targets");

  auto mbr_windows = shrink_binding_manifest(
      TsumugiManifestPartitionStyle::mbr,
      true,
      {
          shrink_partition(
              1U,
              TsumugiManifestPartitionRole::bios_system,
              TsumugiManifestFileSystem::ntfs,
              1ULL * 1024ULL * 1024ULL,
              0U,
              true,
              selected | required | active),
          shrink_partition(
              2U,
              TsumugiManifestPartitionRole::windows,
              TsumugiManifestFileSystem::ntfs,
              2ULL * 1024ULL * 1024ULL,
              4096U,
              true,
              selected | required | contains_windows),
      });
  auto mbr_to_gpt_layout = reviewed_layout(
      MigrationPartitionStyle::gpt,
      {
          generated_partition(
              1U,
              MigrationPartitionRole::efi_system,
              MigrationFileSystem::fat32,
              MigrationPartitionAction::create_fat32,
              1ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          generated_partition(
              2U,
              MigrationPartitionRole::microsoft_reserved,
              MigrationFileSystem::none,
              MigrationPartitionAction::create_reserved,
              2ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          mapped_partition(
              3U,
              2U,
              MigrationPartitionRole::windows,
              MigrationFileSystem::ntfs,
              MigrationPartitionAction::apply_file_image,
              3ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
      });
  const auto converted_bindings = make_tsumugi_shrink_payload_bindings_v1(
      mbr_windows, mbr_to_gpt_layout);
  check(
      converted_bindings.has_value() &&
          find_binding(converted_bindings.value(), 1U)->disposition ==
              TsumugiShrinkPayloadDispositionV1::
                  replace_bios_system_with_uefi &&
          find_binding(converted_bindings.value(), 2U)->target_number == 3U,
      "MBR-to-GPT may replace only the BIOS system payload with generated UEFI infrastructure");

  auto mbr_layout = reviewed_layout(
      MigrationPartitionStyle::mbr,
      {
          mapped_partition(
              1U,
              1U,
              MigrationPartitionRole::bios_system,
              MigrationFileSystem::ntfs,
              MigrationPartitionAction::apply_file_image,
              1ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          mapped_partition(
              2U,
              2U,
              MigrationPartitionRole::windows,
              MigrationFileSystem::ntfs,
              MigrationPartitionAction::apply_file_image,
              2ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
      });
  const auto preserved_bindings = make_tsumugi_shrink_payload_bindings_v1(
      mbr_windows, mbr_layout);
  check(
      preserved_bindings.has_value() &&
          std::all_of(
              preserved_bindings.value().begin(),
              preserved_bindings.value().end(),
              [](const auto& binding) {
                return binding.disposition ==
                    TsumugiShrinkPayloadDispositionV1::
                        restore_to_reviewed_partition;
              }),
      "MBR-to-MBR must preserve both BIOS system and Windows payloads");

  auto gpt_data = shrink_binding_manifest(
      TsumugiManifestPartitionStyle::gpt,
      false,
      {
          shrink_partition(
              1U,
              TsumugiManifestPartitionRole::microsoft_reserved,
              TsumugiManifestFileSystem::none,
              1ULL * 1024ULL * 1024ULL,
              0U,
              false,
              selected | required),
          shrink_partition(
              2U,
              TsumugiManifestPartitionRole::data,
              TsumugiManifestFileSystem::ntfs,
              2ULL * 1024ULL * 1024ULL,
              8192U,
              true,
              selected),
      });
  auto gpt_data_layout = reviewed_layout(
      MigrationPartitionStyle::gpt,
      {
          generated_partition(
              1U,
              MigrationPartitionRole::microsoft_reserved,
              MigrationFileSystem::none,
              MigrationPartitionAction::create_reserved,
              1ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          mapped_partition(
              2U,
              2U,
              MigrationPartitionRole::data,
              MigrationFileSystem::ntfs,
              MigrationPartitionAction::apply_file_image,
              2ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
      });
  const auto data_bindings = make_tsumugi_shrink_payload_bindings_v1(
      gpt_data, gpt_data_layout);
  check(
      data_bindings.has_value() &&
          find_binding(data_bindings.value(), 1U)->disposition ==
              TsumugiShrinkPayloadDispositionV1::
                  regenerate_microsoft_reserved &&
          find_binding(data_bindings.value(), 2U)->target_number == 2U,
      "a GPT data disk should regenerate its filesystem-none MSR and restore data");

  gpt_data_layout.migration.target_partitions.pop_back();
  std::get<ytec::clonecore::GptDisk>(gpt_data_layout.metadata.target_layout)
      .partitions.pop_back();
  check(
      !make_tsumugi_shrink_payload_bindings_v1(
           gpt_data, gpt_data_layout)
           .has_value(),
      "an arbitrary selected data-payload skip must fail closed");
}

void test_regenerated_payload_is_verified_but_not_written() {
  using namespace ytec::imageformat;
  using namespace ytec::migrationcore;
  constexpr auto selected = TsumugiManifestPartitionFlags::selected;
  constexpr auto required = TsumugiManifestPartitionFlags::required;
  auto manifest = shrink_binding_manifest(
      TsumugiManifestPartitionStyle::mbr,
      true,
      {
          shrink_partition(
              1U,
              TsumugiManifestPartitionRole::bios_system,
              TsumugiManifestFileSystem::ntfs,
              1ULL * 1024ULL * 1024ULL,
              0U,
              true,
              selected | required |
                  TsumugiManifestPartitionFlags::active),
          shrink_partition(
              2U,
              TsumugiManifestPartitionRole::windows,
              TsumugiManifestFileSystem::ntfs,
              2ULL * 1024ULL * 1024ULL,
              4096U,
              true,
              selected | required |
                  TsumugiManifestPartitionFlags::contains_windows),
          shrink_partition(
              3U,
              TsumugiManifestPartitionRole::data,
              TsumugiManifestFileSystem::ntfs,
              3ULL * 1024ULL * 1024ULL,
              8192U,
              true,
              selected),
      });
  manifest.partitions[2].used_bytes = 0U;
  check(
      build_tsumugi_manifest_v1(manifest).has_value(),
      "empty filesystem payload should remain canonical");
  auto layout = reviewed_layout(
      MigrationPartitionStyle::gpt,
      {
          generated_partition(
              1U,
              MigrationPartitionRole::efi_system,
              MigrationFileSystem::fat32,
              MigrationPartitionAction::create_fat32,
              1ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          generated_partition(
              2U,
              MigrationPartitionRole::microsoft_reserved,
              MigrationFileSystem::none,
              MigrationPartitionAction::create_reserved,
              2ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          mapped_partition(
              3U,
              2U,
              MigrationPartitionRole::windows,
              MigrationFileSystem::ntfs,
              MigrationPartitionAction::apply_file_image,
              3ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
          mapped_partition(
              4U,
              3U,
              MigrationPartitionRole::data,
              MigrationFileSystem::ntfs,
              MigrationPartitionAction::create_empty_ntfs,
              4ULL * 1024ULL * 1024ULL,
              1ULL * 1024ULL * 1024ULL),
      });

  TempDirectory temp;
  auto bytes = source_bytes(141U);
  bytes.resize(3U * 4096U, std::byte{0x6D});
  MemorySource source(bytes);
  const auto path = temp.file(L"regenerated-payload.tsumugi");
  TsumugiImageCreateRequest request{
      .final_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .manifest = manifest,
      .chunks = {
          TsumugiStreamBuildChunk{
              .logical_offset = 0U,
              .logical_length = 4096U,
              .source_offset = 0U,
              .source = &source,
          },
          TsumugiStreamBuildChunk{
              .logical_offset = 4096U,
              .logical_length = 4096U,
              .source_offset = 4096U,
              .source = &source,
          },
          TsumugiStreamBuildChunk{
              .logical_offset = 8192U,
              .logical_length = 4096U,
              .source_offset = 8192U,
              .source = &source,
          },
      },
      .compression = ImageCompression::zstandard,
      .chunk_size = kImageChunkSize16MiB,
      .verification_block_bytes = 1024U,
      .source_session = &source,
  };
  check(create_tsumugi_image_v1(request).has_value(),
        "regenerated-payload image should build and fully verify");

  const auto no_review = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
          .shrink_placements = {
              TsumugiRestorePartitionPlacement{
                  .source_table_index = 2U,
                  .target_offset = 3ULL * 1024ULL * 1024ULL,
                  .target_size = 1ULL * 1024ULL * 1024ULL,
              },
          },
      },
  });
  check(!no_review.has_value(),
        "caller placements without a reviewed final layout must be rejected");

  const auto mismatched_placement = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
          .reviewed_shrink_layout = layout,
          .shrink_placements = {
              TsumugiRestorePartitionPlacement{
                  .source_table_index = 2U,
                  .target_offset = 5ULL * 1024ULL * 1024ULL,
                  .target_size = 1ULL * 1024ULL * 1024ULL,
              },
          },
      },
  });
  check(
      !mismatched_placement.has_value(),
      "caller placement drift from the reviewed final layout must be rejected");

  auto plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
          .reviewed_shrink_layout = layout,
      },
  });
  check(
      plan.has_value() && plan.value().shrink_payload_bindings().size() == 3U,
      "reviewed layout should seal regenerated, restored, and empty-filesystem bindings");
  MemoryShrinkRestoreTransaction transaction(target_identity());
  const auto restored = execute_tsumugi_shrink_restore_plan_v1(
      plan.value(), std::nullopt, transaction);
  check(restored.has_value(),
        "reviewed MBR-to-GPT payload execution should succeed");
  check(
      restored.value().archive_logical_bytes == 4096U &&
          restored.value().archive_chunk_count == 1U &&
          restored.value().intentionally_omitted_logical_bytes == 8192U &&
          restored.value().intentionally_omitted_chunk_count == 2U &&
          restored.value().intentionally_regenerated_partitions == 2U &&
          restored.value().completed_empty_file_system_partitions == 1U &&
          restored.value().callbacks_started_after_complete_verification,
      "regenerated payloads must be fully verified and explicitly counted without archive writes");
  check(
      transaction.archive_begin_count == 1U &&
          transaction.archive_append_count == 1U &&
          transaction.archive_complete_count == 1U &&
          transaction.empty_file_system_count == 1U &&
          transaction.empty_file_system_target.source_table_index == 3U &&
          transaction.raw_write_count == 0U &&
          transaction.archive.size() == 4096U &&
          std::equal(
              transaction.archive.begin(),
              transaction.archive.end(),
              bytes.begin() + 4096),
      "only Windows archive bytes may reach WIM apply while empty NTFS receives its verified recreate callback");

  auto failure_plan = prepare_tsumugi_restore_plan_v1({
      .image = verify_request(path),
      .host = TsumugiRestoreHost::winpe,
      .target = TsumugiWholeDiskRestoreTarget{
          .disk = target_identity(),
          .reviewed_shrink_layout = layout,
      },
  });
  check(failure_plan.has_value(),
        "empty-filesystem failure plan should prepare independently");
  MemoryShrinkRestoreTransaction failed_transaction(target_identity());
  failed_transaction.fail_empty_file_system = true;
  check(
      !execute_tsumugi_shrink_restore_plan_v1(
           failure_plan.value(), std::nullopt, failed_transaction)
           .has_value() &&
          failed_transaction.begin_count == 1U &&
          failed_transaction.empty_file_system_count == 1U &&
          failed_transaction.archive_begin_count == 0U &&
          failed_transaction.commit_count == 0U &&
          failed_transaction.abort_count == 1U,
      "empty-filesystem create/readback failure must abort before payload delivery and final commit");
}

}  // namespace

int main() {
  try {
    test_create_verify_and_whole_restore();
    test_fast_create_reports_selected_verification_without_complete_claim();
    test_rescue_report_plans_authenticated_chunks_without_source_reread();
    test_rescue_image_service_discards_staging_before_final_commit();
    test_fat32_rejected_before_source_read();
    test_service_stages_complete_image_until_explicit_commit();
    test_mixed_source_sessions_are_rejected_before_read();
    test_encrypted_image_requires_the_password();
    test_running_windows_target_is_rejected();
    test_replaced_image_is_rejected_before_first_write();
    test_target_identity_drift_is_rejected_before_first_write();
    test_transaction_failures_abort_and_plan_is_single_use();
    test_forbidden_restore_media_are_rejected();
    test_individual_windows_restore_offers_boot_repair();
    test_shrink_wim_requires_dedicated_verified_adapter();
    test_shrink_wim_apply_failure_aborts_before_commit();
    test_changed_shrink_image_is_rejected_before_target_begin();
    test_reviewed_layout_derives_only_authenticated_payload_bindings();
    test_regenerated_payload_is_verified_but_not_written();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All Tsumugi image service tests passed\n";
  return 0;
}

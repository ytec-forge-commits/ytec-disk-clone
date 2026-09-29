#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/clonecore/gpt.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kSourceDiskBytes = 8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetDiskBytes = 12ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionOffset = 2ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionBytes = 8ULL * 1024ULL;
constexpr std::uint64_t kExistingTargetOffset =
    4ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kSectorSize = 512U;
constexpr std::uint32_t kSourceSignature = 0x1234ABCDU;

ytec::clonecore::GptGuid guid(const std::byte seed) {
  ytec::clonecore::GptGuid value;
  value.bytes.fill(seed);
  return value;
}

class SequenceGuidGenerator final
    : public ytec::clonecore::IGuidGenerator {
 public:
  explicit SequenceGuidGenerator(
      std::vector<ytec::clonecore::GptGuid> values)
      : values_(std::move(values)) {}

  [[nodiscard]] ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    if (next_ >= values_.size()) {
      return ytec::clonecore::Result<ytec::clonecore::GptGuid>::failure({
          .code = ytec::clonecore::ErrorCode::internal_error,
          .native_code = ERROR_NO_MORE_ITEMS,
          .operation = L"合成GPT GUID",
          .message = L"GUIDが不足しています",
      });
    }
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(
        values_[next_++]);
  }

 private:
  std::vector<ytec::clonecore::GptGuid> values_;
  std::size_t next_{};
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
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
    check(length != 0U && length < root.size(), "temp path is required");
    path_ = root.data();
    path_ += L"ytec-physical-restore-";
    path_ += std::to_wstring(GetCurrentProcessId());
    path_ += L"-";
    path_ += std::to_wstring(GetTickCount64());
    check(CreateDirectoryW(path_.c_str(), nullptr) != FALSE,
          "temporary directory must be created");
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

  [[nodiscard]] std::wstring file(const std::wstring_view name) const {
    return path_ + L"\\" + std::wstring(name);
  }

 private:
  std::wstring path_;
};

class SourceSession final
    : public ytec::imageformat::ITsumugiImageSourceSession {
 public:
  explicit SourceSession(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return kSourceDiskBytes;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest value{};
    value[0] = std::byte{0x21};
    return value;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest value{};
    value[0] = std::byte{0x34};
    return value;
  }

  [[nodiscard]] ytec::imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    ytec::imageformat::Sha256Digest value{};
    value[0] = std::byte{0x55};
    return value;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"合成Tsumugiコピー元読取り",
          .message = L"範囲外です",
      });
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length)));
  }

 private:
  std::vector<std::byte> bytes_;
};

struct WriterState final {
  std::vector<std::byte> bytes{
      static_cast<std::size_t>(kTargetDiskBytes), std::byte{0xCC}};
  std::size_t writes{};
  std::size_t reads{};
  std::size_t flushes{};
  std::optional<std::uint64_t> fail_write_once_at;
};

class SharedMemoryWriter final : public ytec::clonecore::ITargetDiskWriter {
 public:
  explicit SharedMemoryWriter(std::shared_ptr<WriterState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return state_->bytes.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (offset > state_->bytes.size() ||
        bytes.size() > state_->bytes.size() - offset) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_WRITE_FAULT,
          .operation = L"合成物理復元書込み",
          .message = L"範囲外です",
      });
    }
    ++state_->writes;
    std::copy(
        bytes.begin(), bytes.end(),
        state_->bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    if (state_->fail_write_once_at.has_value() &&
        *state_->fail_write_once_at == offset) {
      state_->fail_write_once_at.reset();
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_WRITE_FAULT,
          .operation = L"合成保持型metadata書込み",
          .message = L"書込み後の注入失敗です",
      });
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > state_->bytes.size() ||
        length > state_->bytes.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"合成物理復元読戻し",
          .message = L"範囲外です",
      });
    }
    ++state_->reads;
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            state_->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            state_->bytes.begin() +
                static_cast<std::ptrdiff_t>(offset + length)));
  }

  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    ++state_->flushes;
    return ytec::clonecore::success_status();
  }

 private:
  std::shared_ptr<WriterState> state_;
};

class StateReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit StateReader(std::shared_ptr<WriterState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return state_->bytes.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > state_->bytes.size() ||
        length > state_->bytes.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"合成GPT対象読取り",
          .message = L"範囲外です",
      });
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            state_->bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            state_->bytes.begin() +
                static_cast<std::ptrdiff_t>(offset + length)));
  }

 private:
  std::shared_ptr<WriterState> state_;
};

std::vector<std::byte> payload() {
  std::vector<std::byte> result(static_cast<std::size_t>(kPartitionBytes));
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] = static_cast<std::byte>((index * 31U + 7U) & 0xffU);
  }
  return result;
}

std::vector<std::byte> partition_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = kSourceDiskBytes,
      .logical_sector_size = kSectorSize,
  };
  ytec::imageformat::PartitionTableRegion region;
  region.disk_offset = 0U;
  region.data.assign(kSectorSize, std::byte{0});
  write_little(region.data, 440U, kSourceSignature);
  region.data[446U] = std::byte{0x80};
  region.data[450U] = std::byte{0x07};
  write_little(
      region.data,
      454U,
      static_cast<std::uint32_t>(kPartitionOffset / kSectorSize));
  write_little(
      region.data,
      458U,
      static_cast<std::uint32_t>(kPartitionBytes / kSectorSize));
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));
  auto built = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(built.has_value(), "canonical MBR snapshot must build");
  return built.take_value();
}

ytec::imageformat::TsumugiManifest manifest() {
  using namespace ytec::imageformat;
  TsumugiManifest result{
      .mode = TsumugiManifestMode::exact,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .flags = TsumugiManifestFlags::none,
      .source_disk_size = kSourceDiskBytes,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-08T12:00:00Z",
      .app_version = "1.0.0-test",
      .partition_snapshot = partition_snapshot(),
  };
  result.source_model_hash[0] = std::byte{0x21};
  result.source_serial_hash[0] = std::byte{0x34};
  result.source_state_hash[0] = std::byte{0x55};
  TsumugiManifestPartition partition{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected |
          TsumugiManifestPartitionFlags::active,
      .source_offset = kPartitionOffset,
      .source_size = kPartitionBytes,
      .used_bytes = kPartitionBytes,
      .minimum_target_bytes = kPartitionBytes,
      .planned_target_bytes = kPartitionBytes,
      .payload_logical_offset = kPartitionOffset,
      .payload_logical_length = kPartitionBytes,
      .name_utf8 = "Data",
      .label_utf8 = "Data",
  };
  partition.type_id[0] = std::byte{0x07};
  result.partitions.push_back(std::move(partition));
  return result;
}

ytec::imageformat::TsumugiManifest gpt_manifest() {
  using namespace ytec;
  clonecore::GptDisk source{
      .logical_sector_size = kSectorSize,
      .sector_count = kSourceDiskBytes / kSectorSize,
      .disk_guid = guid(std::byte{0x10}),
      .first_usable_lba = 34U,
      .last_usable_lba = kSourceDiskBytes / kSectorSize - 34U,
      .partition_entry_count = 128U,
      .partition_entry_size = 128U,
      .partitions = {
          clonecore::GptPartition{
              .entry_index = 0U,
              .type_guid = clonecore::gpt_type_basic_data(),
              .unique_guid = guid(std::byte{0x11}),
              .first_lba = kPartitionOffset / kSectorSize,
              .last_lba =
                  (kPartitionOffset + kPartitionBytes) / kSectorSize - 1U,
              .name = u"Data",
          },
      },
  };
  SequenceGuidGenerator ids({
      guid(std::byte{0x20}),
      guid(std::byte{0x21}),
  });
  auto generated = clonecore::make_gpt_write_plan(
      source, kSourceDiskBytes, kSectorSize, ids);
  check(generated.has_value(), "source GPT snapshot plan must build");
  imageformat::PartitionSnapshot snapshot{
      .style = imageformat::PartitionTableStyle::gpt,
      .source_disk_size = kSourceDiskBytes,
      .logical_sector_size = kSectorSize,
  };
  std::vector<std::byte> disk(
      static_cast<std::size_t>(kSourceDiskBytes), std::byte{0});
  for (const auto& write : generated.value().writes) {
    std::copy(
        write.bytes.begin(), write.bytes.end(),
        disk.begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
  const auto primary_entries = std::find_if(
      generated.value().writes.begin(),
      generated.value().writes.end(),
      [](const clonecore::GptMetadataWrite& write) {
        return write.kind == clonecore::GptMetadataKind::primary_entries;
      });
  const auto backup_entries = std::find_if(
      generated.value().writes.begin(),
      generated.value().writes.end(),
      [](const clonecore::GptMetadataWrite& write) {
        return write.kind == clonecore::GptMetadataKind::backup_entries;
      });
  check(primary_entries != generated.value().writes.end() &&
            backup_entries != generated.value().writes.end(),
        "source GPT metadata ranges must exist");
  const auto leading_length =
      primary_entries->offset + primary_entries->bytes.size();
  snapshot.regions.push_back({
      .disk_offset = 0U,
      .data = std::vector<std::byte>(
          disk.begin(),
          disk.begin() + static_cast<std::ptrdiff_t>(leading_length)),
  });
  snapshot.regions.push_back({
      .disk_offset = backup_entries->offset,
      .data = std::vector<std::byte>(
          disk.begin() +
              static_cast<std::ptrdiff_t>(backup_entries->offset),
          disk.end()),
  });
  auto built_snapshot = imageformat::build_partition_snapshot_v1(snapshot);
  check(built_snapshot.has_value(), "source GPT snapshot must canonicalize");
  imageformat::TsumugiManifest result{
      .mode = imageformat::TsumugiManifestMode::exact,
      .partition_style = imageformat::TsumugiManifestPartitionStyle::gpt,
      .flags = imageformat::TsumugiManifestFlags::none,
      .source_disk_size = kSourceDiskBytes,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-10T12:00:00Z",
      .app_version = "1.0.0-test",
      .partition_snapshot = built_snapshot.take_value(),
  };
  result.source_model_hash[0] = std::byte{0x21};
  result.source_serial_hash[0] = std::byte{0x34};
  result.source_state_hash[0] = std::byte{0x55};
  const auto& partition = generated.value().target_disk.partitions.front();
  imageformat::TsumugiManifestPartition record{
      .source_table_index = partition.entry_index + 1U,
      .source_partition_number = 1U,
      .role = imageformat::TsumugiManifestPartitionRole::data,
      .file_system = imageformat::TsumugiManifestFileSystem::ntfs,
      .flags = imageformat::TsumugiManifestPartitionFlags::selected,
      .source_offset = kPartitionOffset,
      .source_size = kPartitionBytes,
      .used_bytes = kPartitionBytes,
      .minimum_target_bytes = kPartitionBytes,
      .planned_target_bytes = kPartitionBytes,
      .payload_logical_offset = kPartitionOffset,
      .payload_logical_length = kPartitionBytes,
      .name_utf8 = "Data",
      .label_utf8 = "Data",
  };
  record.type_id = partition.type_guid.bytes;
  record.unique_id = partition.unique_guid.bytes;
  result.partitions.push_back(std::move(record));
  return result;
}

ytec::imageformat::TsumugiImageVerifyRequest create_image(
    const std::wstring& path,
    const SourceSession& source) {
  using namespace ytec::imageformat;
  TsumugiImageCreateRequest request{
      .final_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .manifest = manifest(),
      .compression = ImageCompression::zstandard,
      .chunk_size = kImageChunkSize16MiB,
      .verification_block_bytes = 1024U,
      .source_session = &source,
  };
  request.chunks.push_back(TsumugiStreamBuildChunk{
      .logical_offset = kPartitionOffset,
      .logical_length = kPartitionBytes,
      .source_offset = 0U,
      .flags = TsumugiChunkFlags::none,
      .source = &source,
  });
  const auto created = create_tsumugi_image_v1(request);
  check(created.has_value() && created.value().complete_verification_passed,
        "synthetic Tsumugi image must fully verify during creation");
  return TsumugiImageVerifyRequest{
      .image_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  };
}

ytec::imageformat::TsumugiImageVerifyRequest create_gpt_image(
    const std::wstring& path,
    const SourceSession& source) {
  using namespace ytec::imageformat;
  TsumugiImageCreateRequest request{
      .final_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .manifest = gpt_manifest(),
      .compression = ImageCompression::zstandard,
      .chunk_size = kImageChunkSize16MiB,
      .verification_block_bytes = 1024U,
      .source_session = &source,
  };
  request.chunks.push_back(TsumugiStreamBuildChunk{
      .logical_offset = kPartitionOffset,
      .logical_length = kPartitionBytes,
      .source_offset = 0U,
      .flags = TsumugiChunkFlags::none,
      .source = &source,
  });
  const auto created = create_tsumugi_image_v1(request);
  check(created.has_value() && created.value().complete_verification_passed,
        "synthetic GPT Tsumugi image must fully verify");
  return TsumugiImageVerifyRequest{
      .image_path = path,
      .storage_file_system = TsumugiImageStorageFileSystem::ntfs,
      .verification_block_bytes = 1024U,
  };
}

ytec::imageformat::TsumugiRestoreDiskIdentity target_identity() {
  ytec::imageformat::TsumugiRestoreDiskIdentity result{
      .disk_size = kTargetDiskBytes,
      .logical_sector_size = kSectorSize,
  };
  result.stable_identity_hash[0] = std::byte{0xA5};
  return result;
}

std::array<std::byte, kSectorSize> seed_target_mbr(
    const std::shared_ptr<WriterState>& state) {
  std::array<std::byte, kSectorSize> sector{};
  constexpr std::uint32_t signature = 0x78563412U;
  std::memcpy(sector.data() + 440U, &signature, sizeof(signature));
  sector[450U] = std::byte{0x07};
  const auto first_lba = static_cast<std::uint32_t>(
      (1ULL * 1024ULL * 1024ULL) / kSectorSize);
  const auto sector_count = static_cast<std::uint32_t>(
      (1ULL * 1024ULL * 1024ULL) / kSectorSize);
  std::memcpy(sector.data() + 454U, &first_lba, sizeof(first_lba));
  std::memcpy(sector.data() + 458U, &sector_count, sizeof(sector_count));
  sector[510U] = std::byte{0x55};
  sector[511U] = std::byte{0xAA};
  std::copy(sector.begin(), sector.end(), state->bytes.begin());
  return sector;
}

ytec::clonecore::GptDisk seed_target_gpt(
    const std::shared_ptr<WriterState>& state) {
  using namespace ytec::clonecore;
  GptDisk source{
      .logical_sector_size = kSectorSize,
      .sector_count = kTargetDiskBytes / kSectorSize,
      .disk_guid = guid(std::byte{0x40}),
      .first_usable_lba = 34U,
      .last_usable_lba = kTargetDiskBytes / kSectorSize - 34U,
      .partition_entry_count = 128U,
      .partition_entry_size = 128U,
      .partitions = {
          GptPartition{
              .entry_index = 0U,
              .type_guid = gpt_type_basic_data(),
              .unique_guid = guid(std::byte{0x41}),
              .first_lba =
                  (1ULL * 1024ULL * 1024ULL) / kSectorSize,
              .last_lba =
                  (2ULL * 1024ULL * 1024ULL) / kSectorSize - 1U,
              .name = u"Existing",
          },
      },
  };
  SequenceGuidGenerator ids({
      guid(std::byte{0x50}),
      guid(std::byte{0x51}),
  });
  auto generated = make_gpt_write_plan(
      source, kTargetDiskBytes, kSectorSize, ids);
  check(generated.has_value(), "target GPT fixture must build");
  for (const auto& write : generated.value().writes) {
    check(
        write.offset <= state->bytes.size() &&
            write.bytes.size() <= state->bytes.size() - write.offset,
        "target GPT metadata must fit");
    std::copy(
        write.bytes.begin(), write.bytes.end(),
        state->bytes.begin() +
            static_cast<std::ptrdiff_t>(write.offset));
  }
  return generated.take_value().target_disk;
}

void complete_engine_writes_reads_back_and_commits_mbr_last() {
  TempDirectory temporary;
  const auto expected = payload();
  SourceSession source(expected);
  const auto request = create_image(
      temporary.file(L"physical-success.tsumugi"), source);
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1(request);
  check(verified.has_value(), "initial complete verification must pass");
  auto state = std::make_shared<WriterState>();
  std::unique_ptr<ytec::clonecore::ITargetDiskWriter> writer =
      std::make_unique<SharedMemoryWriter>(state);
  const std::array disallowed{kSourceSignature};
  bool revalidated_immediately_before_write = false;
  std::vector<ytec::clonecore::DiskOperationProgress> progress;
  std::vector<ytec::clonecore::DiskOperationSafeBoundary> boundaries;
  const auto restored = ytec::imageformat::
      execute_tsumugi_physical_whole_disk_restore_engine_v1(
          request,
          verified.value(),
          target_identity(),
          std::move(writer),
          disallowed,
          ytec::imageformat::TsumugiRestoreHost::winpe,
          [&] {
            revalidated_immediately_before_write = true;
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiRestoreDiskIdentity>::success(
                target_identity());
          },
          ytec::clonecore::DiskOperationCallbacks{
              .progress = [&](const auto& value) {
                progress.push_back(value);
              },
              .safe_boundary = [&](const auto& boundary) {
                boundaries.push_back(boundary);
                return ytec::clonecore::DiskOperationControlDecision::
                    continue_operation;
              },
          });
  check(restored.has_value() && revalidated_immediately_before_write,
        "shared PE physical engine must revalidate and succeed");
  check(restored.value().written_logical_bytes == kPartitionBytes &&
            restored.value().callbacks_started_after_complete_verification &&
            restored.value().image_matched_prepared_plan &&
            restored.value().target_reidentified_before_write &&
            restored.value().all_writes_read_back_verified &&
            restored.value().final_layout_committed,
        "engine must report complete verification and final commit evidence");
  check(std::equal(
            expected.begin(), expected.end(),
            state->bytes.begin() +
                static_cast<std::ptrdiff_t>(kPartitionOffset)) &&
            state->bytes[510U] == std::byte{0x55} &&
            state->bytes[511U] == std::byte{0xAA} &&
            state->writes >= 4U && state->reads >= state->writes &&
            state->flushes >= state->writes,
        "payload and final MBR must be written, flushed and read back");
  check(
      std::count_if(
          boundaries.begin(), boundaries.end(), [](const auto& boundary) {
            return boundary.stage ==
                       ytec::clonecore::DiskOperationStage::copying_data &&
                boundary.kind ==
                ytec::clonecore::DiskOperationSafeBoundaryKind::
                    verified_chunk;
          }) == 1,
      "physical restore must expose its payload only after read-back verify");
  check(
      std::all_of(
          progress.begin(), progress.end(), [](const auto& value) {
            const bool metadata =
                value.stage ==
                    ytec::clonecore::DiskOperationStage::
                        invalidating_target ||
                value.stage ==
                    ytec::clonecore::DiskOperationStage::
                        staging_partition_table ||
                value.stage ==
                    ytec::clonecore::DiskOperationStage::
                        committing_partition_table;
            return !metadata || !value.pause_allowed;
          }),
      "physical restore metadata transaction must never advertise pause");
  std::uint32_t target_signature{};
  std::memcpy(
      &target_signature, state->bytes.data() + 440U,
      sizeof(target_signature));
  check(target_signature != 0U && target_signature != kSourceSignature,
        "restored MBR must receive a fresh non-colliding signature");
}

void changed_image_binding_stops_before_first_target_write() {
  TempDirectory temporary;
  const auto expected = payload();
  SourceSession source(expected);
  const auto request = create_image(
      temporary.file(L"physical-drift.tsumugi"), source);
  auto initially_verified =
      ytec::imageformat::verify_tsumugi_image_v1(request);
  check(initially_verified.has_value(), "initial image must verify");
  initially_verified.value().container.global_hash[0] ^= std::byte{0x01};
  auto state = std::make_shared<WriterState>();
  std::unique_ptr<ytec::clonecore::ITargetDiskWriter> writer =
      std::make_unique<SharedMemoryWriter>(state);
  const auto restored = ytec::imageformat::
      execute_tsumugi_physical_whole_disk_restore_engine_v1(
          request,
          initially_verified.value(),
          target_identity(),
          std::move(writer),
          {},
          ytec::imageformat::TsumugiRestoreHost::winpe,
          [] {
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiRestoreDiskIdentity>::success(
                target_identity());
          });
  check(!restored.has_value() && state->writes == 0U &&
            state->reads == 0U && state->flushes == 0U &&
            std::all_of(
                state->bytes.begin(), state->bytes.end(),
                [](const std::byte value) {
                  return value == std::byte{0xCC};
                }),
        "second complete verification drift must stop before target mutation");
}

void final_target_revalidation_failure_stops_before_layout_invalidation() {
  TempDirectory temporary;
  const auto expected = payload();
  SourceSession source(expected);
  const auto request = create_image(
      temporary.file(L"physical-target-drift.tsumugi"), source);
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1(request);
  check(verified.has_value(), "initial image must verify");
  auto state = std::make_shared<WriterState>();
  std::unique_ptr<ytec::clonecore::ITargetDiskWriter> writer =
      std::make_unique<SharedMemoryWriter>(state);
  bool final_revalidation_called = false;
  const auto restored = ytec::imageformat::
      execute_tsumugi_physical_whole_disk_restore_engine_v1(
          request,
          verified.value(),
          target_identity(),
          std::move(writer),
          {},
          ytec::imageformat::TsumugiRestoreHost::winpe,
          [&] {
            final_revalidation_called = true;
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiRestoreDiskIdentity>::failure({
                .code = ytec::clonecore::ErrorCode::identity_mismatch,
                .native_code = ERROR_DEVICE_REINITIALIZATION_NEEDED,
                .operation = L"合成書込み直前対象再照合",
                .message = L"レイアウトが変化しました",
            });
          });
  check(!restored.has_value() && final_revalidation_called &&
            state->writes == 0U && state->reads == 0U &&
            state->flushes == 0U &&
            std::all_of(
                state->bytes.begin(), state->bytes.end(),
                [](const std::byte value) {
                  return value == std::byte{0xCC};
                }),
        "post-verification target drift must stop before table invalidation");
}

void existing_partition_engine_writes_only_reviewed_extent() {
  TempDirectory temporary;
  const auto expected = payload();
  SourceSession source(expected);
  const auto request = create_image(
      temporary.file(L"physical-individual-existing.tsumugi"), source);
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1(request);
  check(verified.has_value(), "initial individual image must verify");
  auto state = std::make_shared<WriterState>();
  std::unique_ptr<ytec::clonecore::ITargetDiskWriter> writer =
      std::make_unique<SharedMemoryWriter>(state);
  const ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection selection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalExistingPartitionRestoreSelection{
                  .target_table_index = 2U,
                  .target_partition_number = 7U,
                  .target_offset = kExistingTargetOffset,
                  .target_size = 2U * kPartitionBytes,
              },
      };
  std::size_t revalidations = 0U;
  const auto restored = ytec::imageformat::
      execute_tsumugi_physical_existing_partition_restore_engine_v1(
          request,
          verified.value(),
          target_identity(),
          selection,
          std::move(writer),
          ytec::imageformat::TsumugiRestoreHost::winpe,
          [&] {
            ++revalidations;
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiRestoreDiskIdentity>::success(
                target_identity());
          });
  check(restored.has_value() &&
            restored.value().written_logical_bytes == kPartitionBytes &&
            restored.value().written_chunk_count == 1U &&
            restored.value().final_layout_committed &&
            revalidations == 2U,
        "individual engine must verify target before write and at commit");
  check(std::equal(
            expected.begin(), expected.end(),
            state->bytes.begin() +
                static_cast<std::ptrdiff_t>(kExistingTargetOffset)),
        "payload must be rebased into the reviewed existing partition");
  check(std::all_of(
            state->bytes.begin(),
            state->bytes.begin() +
                static_cast<std::ptrdiff_t>(kExistingTargetOffset),
            [](const std::byte value) { return value == std::byte{0xCC}; }) &&
            std::all_of(
                state->bytes.begin() +
                    static_cast<std::ptrdiff_t>(
                        kExistingTargetOffset + kPartitionBytes),
                state->bytes.end(),
                [](const std::byte value) {
                  return value == std::byte{0xCC};
                }),
        "individual restore must not mutate partition metadata or other extents");
  check(state->writes == 1U && state->reads == 1U &&
            state->flushes >= 3U,
        "individual payload must be flushed/read back without metadata writes");
}

void unallocated_partition_engine_preserves_mbr_and_publishes_last() {
  TempDirectory temporary;
  const auto expected = payload();
  SourceSession source(expected);
  const auto request = create_image(
      temporary.file(L"physical-individual-unallocated.tsumugi"), source);
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1(request);
  check(verified.has_value(), "initial unallocated image must verify");
  auto state = std::make_shared<WriterState>();
  const auto original_mbr = seed_target_mbr(state);
  std::unique_ptr<ytec::clonecore::ITargetDiskWriter> writer =
      std::make_unique<SharedMemoryWriter>(state);
  const ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection selection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalUnallocatedRestoreSelection{
                  .target_offset = kExistingTargetOffset,
                  .target_size = kPartitionBytes,
              },
      };
  std::size_t revalidations = 0U;
  const auto restored = ytec::imageformat::
      execute_tsumugi_physical_individual_partition_restore_engine_v1(
          request,
          verified.value(),
          target_identity(),
          selection,
          std::move(writer),
          ytec::imageformat::TsumugiRestoreHost::winpe,
          [&] {
            ++revalidations;
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiRestoreDiskIdentity>::success(
                target_identity());
          });
  check(restored.has_value() &&
            restored.value().written_logical_bytes == kPartitionBytes &&
            restored.value().final_layout_committed && revalidations == 1U,
        "unallocated engine must publish one partition after payload verify");
  check(std::equal(
            expected.begin(), expected.end(),
            state->bytes.begin() +
                static_cast<std::ptrdiff_t>(kExistingTargetOffset)),
        "unallocated payload must be rebased into the reviewed gap");
  check(std::equal(
            original_mbr.begin(), original_mbr.begin() + 462,
            state->bytes.begin()) &&
            std::equal(
                original_mbr.begin() + 478,
                original_mbr.end(),
                state->bytes.begin() + 478),
        "MBR bootstrap, signature, existing entry and other empty entries must remain exact");
  std::uint32_t new_first_lba{};
  std::uint32_t new_sector_count{};
  std::memcpy(&new_first_lba, state->bytes.data() + 470U, sizeof(new_first_lba));
  std::memcpy(
      &new_sector_count, state->bytes.data() + 474U,
      sizeof(new_sector_count));
  check(state->bytes[466U] == std::byte{0x07} &&
            new_first_lba == kExistingTargetOffset / kSectorSize &&
            new_sector_count == kPartitionBytes / kSectorSize &&
            state->writes == 2U,
        "the first empty MBR entry must be the only metadata addition");
}

void unallocated_metadata_failure_rolls_back_exact_mbr() {
  TempDirectory temporary;
  const auto expected = payload();
  SourceSession source(expected);
  const auto request = create_image(
      temporary.file(L"physical-unallocated-rollback.tsumugi"), source);
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1(request);
  check(verified.has_value(), "rollback image must verify");
  auto state = std::make_shared<WriterState>();
  const auto original_mbr = seed_target_mbr(state);
  state->fail_write_once_at = 0U;
  std::unique_ptr<ytec::clonecore::ITargetDiskWriter> writer =
      std::make_unique<SharedMemoryWriter>(state);
  const ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection selection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalUnallocatedRestoreSelection{
                  .target_offset = kExistingTargetOffset,
                  .target_size = kPartitionBytes,
              },
      };
  const auto restored = ytec::imageformat::
      execute_tsumugi_physical_individual_partition_restore_engine_v1(
          request,
          verified.value(),
          target_identity(),
          selection,
          std::move(writer),
          ytec::imageformat::TsumugiRestoreHost::winpe,
          [] {
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiRestoreDiskIdentity>::success(
                target_identity());
          });
  check(!restored.has_value() &&
            std::equal(
                original_mbr.begin(), original_mbr.end(),
                state->bytes.begin()),
        "metadata publication failure must restore the exact original MBR");
  check(std::equal(
            expected.begin(), expected.end(),
            state->bytes.begin() +
                static_cast<std::ptrdiff_t>(kExistingTargetOffset)),
        "verified bytes may remain unreferenced in the gap after metadata rollback");
}

void unallocated_partition_engine_preserves_gpt_identity() {
  TempDirectory temporary;
  const auto expected = payload();
  SourceSession source(expected);
  const auto request = create_gpt_image(
      temporary.file(L"physical-unallocated-gpt.tsumugi"), source);
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1(request);
  check(verified.has_value(), "initial GPT individual image must verify");
  auto state = std::make_shared<WriterState>();
  const auto original = seed_target_gpt(state);
  const std::vector<std::byte> protective_mbr(
      state->bytes.begin(), state->bytes.begin() + kSectorSize);
  std::unique_ptr<ytec::clonecore::ITargetDiskWriter> writer =
      std::make_unique<SharedMemoryWriter>(state);
  const ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection selection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalUnallocatedRestoreSelection{
                  .target_offset = kExistingTargetOffset,
                  .target_size = kPartitionBytes,
              },
      };
  const auto restored = ytec::imageformat::
      execute_tsumugi_physical_individual_partition_restore_engine_v1(
          request,
          verified.value(),
          target_identity(),
          selection,
          std::move(writer),
          ytec::imageformat::TsumugiRestoreHost::winpe,
          [] {
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiRestoreDiskIdentity>::success(
                target_identity());
          });
  check(restored.has_value() &&
            restored.value().final_layout_committed &&
            std::equal(
                protective_mbr.begin(), protective_mbr.end(),
                state->bytes.begin()),
        "GPT individual restore must commit without rewriting protective MBR");
  StateReader reader(state);
  const auto parsed = ytec::clonecore::parse_gpt(reader);
  check(parsed.has_value() && parsed.value().partitions.size() == 2U &&
            parsed.value().disk_guid == original.disk_guid,
        "final GPT must preserve disk identity and add exactly one entry");
  const auto preserved = std::find_if(
      parsed.value().partitions.begin(),
      parsed.value().partitions.end(),
      [&](const ytec::clonecore::GptPartition& partition) {
        return partition.entry_index ==
                original.partitions.front().entry_index &&
            partition.unique_guid ==
                original.partitions.front().unique_guid &&
            partition.first_lba == original.partitions.front().first_lba &&
            partition.last_lba == original.partitions.front().last_lba;
      });
  const auto added = std::find_if(
      parsed.value().partitions.begin(),
      parsed.value().partitions.end(),
      [](const ytec::clonecore::GptPartition& partition) {
        return partition.first_lba ==
                kExistingTargetOffset / kSectorSize &&
            partition.last_lba - partition.first_lba + 1U ==
                kPartitionBytes / kSectorSize;
      });
  check(preserved != parsed.value().partitions.end() &&
            added != parsed.value().partitions.end(),
        "existing GUID/range and reviewed new GPT range must both be exact");
}

void unallocated_candidates_are_fixed_size_and_aligned() {
  auto source_manifest = manifest();
  ytec::diskmodel::DiskInfo target{
      .disk_number = 8U,
      .size_bytes = kTargetDiskBytes,
      .sector_count = kTargetDiskBytes / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .partition_style = ytec::diskmodel::PartitionStyle::mbr,
  };
  target.partitions.push_back({
      .number = 1U,
      .offset_bytes = 1ULL * 1024ULL * 1024ULL,
      .size_bytes = 1ULL * 1024ULL * 1024ULL,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
  });
  const auto candidates = ytec::imageformat::
      find_tsumugi_physical_unallocated_restore_candidates_v1(
          source_manifest, target, 1U);
  check(candidates.has_value() && candidates.value().size() == 1U,
        "one trailing safe MBR gap must produce one candidate");
  const auto& selected = std::get<ytec::imageformat::
      TsumugiPhysicalUnallocatedRestoreSelection>(
      candidates.value().front().target);
  check(selected.target_offset == 2ULL * 1024ULL * 1024ULL &&
            selected.target_size == kPartitionBytes,
        "candidate must use the first 1 MiB-aligned address and exact source size");
}

void individual_selection_requires_exact_reviewed_partition() {
  auto source_manifest = manifest();
  ytec::diskmodel::DiskInfo target{
      .disk_number = 8U,
      .size_bytes = kTargetDiskBytes,
      .sector_count = kTargetDiskBytes / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .partition_style = ytec::diskmodel::PartitionStyle::mbr,
  };
  target.partitions.push_back({
      .number = 7U,
      .offset_bytes = kExistingTargetOffset,
      .size_bytes = 2U * kPartitionBytes,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
  });
  ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection selection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalExistingPartitionRestoreSelection{
                  .target_table_index = 1U,
                  .target_partition_number = 7U,
                  .target_offset = kExistingTargetOffset,
                  .target_size = 2U * kPartitionBytes,
              },
      };
  const auto accepted = ytec::imageformat::
      validate_tsumugi_physical_individual_partition_selection_v1(
          source_manifest, target, selection);
  check(accepted.has_value(),
        "exact reviewed existing partition must pass read-only validation");
  std::get<ytec::imageformat::
      TsumugiPhysicalExistingPartitionRestoreSelection>(selection.target)
      .target_partition_number = 8U;
  const auto drifted = ytec::imageformat::
      validate_tsumugi_physical_individual_partition_selection_v1(
          source_manifest, target, selection);
  check(!drifted.has_value(),
        "partition-number drift must fail before destructive execution");
}

void abnormal_target_health_is_not_a_restore_target() {
  ytec::diskmodel::DiskInfo target{
      .disk_number = 4,
      .device_path = L"\\\\.\\PhysicalDrive4",
      .device_instance_id = L"VIRTUAL\\HEALTH_TARGET",
      .model = L"SYNTHETIC FAILING TARGET",
      .size_bytes = kTargetDiskBytes,
      .sector_count = kTargetDiskBytes / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "HEALTH04",
      .partition_style = ytec::diskmodel::PartitionStyle::raw,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  target.health.state = ytec::diskmodel::DiskHealthState::caution;
  const auto target_class = ytec::imageformat::
      classify_tsumugi_physical_restore_target(target);
  const auto status = ytec::imageformat::
      validate_tsumugi_physical_restore_target(
          target, target_class, false);
  check(!status.has_value(),
        "SMART/NVMe caution must reject a restore target");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"complete_engine_writes_reads_back_and_commits_mbr_last",
       complete_engine_writes_reads_back_and_commits_mbr_last},
      {"changed_image_binding_stops_before_first_target_write",
       changed_image_binding_stops_before_first_target_write},
      {"final_target_revalidation_failure_stops_before_layout_invalidation",
       final_target_revalidation_failure_stops_before_layout_invalidation},
      {"existing_partition_engine_writes_only_reviewed_extent",
       existing_partition_engine_writes_only_reviewed_extent},
      {"unallocated_partition_engine_preserves_mbr_and_publishes_last",
       unallocated_partition_engine_preserves_mbr_and_publishes_last},
      {"unallocated_metadata_failure_rolls_back_exact_mbr",
       unallocated_metadata_failure_rolls_back_exact_mbr},
      {"unallocated_partition_engine_preserves_gpt_identity",
       unallocated_partition_engine_preserves_gpt_identity},
      {"unallocated_candidates_are_fixed_size_and_aligned",
       unallocated_candidates_are_fixed_size_and_aligned},
      {"individual_selection_requires_exact_reviewed_partition",
       individual_selection_requires_exact_reviewed_partition},
      {"abnormal_target_health_is_not_a_restore_target",
       abnormal_target_health_is_not_a_restore_target},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

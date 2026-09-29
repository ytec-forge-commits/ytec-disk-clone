#include "ytec/windowsapp/online_image_create.h"

#include "ytec/imageformat/tsumugi_manifest.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSectorSize = 512U;
constexpr std::uint64_t kDiskSize = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kFirstLba = 2048U;
constexpr std::uint32_t kPartitionSectors = 16384U;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void write_ntfs_boot(
    std::vector<std::byte>& bytes,
    std::uint64_t first_lba,
    std::uint64_t sector_count);

std::vector<std::byte> make_mbr_disk(const bool fat32) {
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  write_little<std::uint32_t>(bytes, 440U, 0x10203040U);
  constexpr std::size_t entry = 446U;
  bytes[entry] = fat32 ? std::byte{0} : std::byte{0x80};
  bytes[entry + 4U] = fat32 ? std::byte{0x0C} : std::byte{0x07};
  write_little(bytes, entry + 8U, kFirstLba);
  write_little(bytes, entry + 12U, kPartitionSectors);
  bytes[510U] = std::byte{0x55};
  bytes[511U] = std::byte{0xAA};

  const std::size_t boot =
      static_cast<std::size_t>(kFirstLba) * kSectorSize;
  write_little<std::uint16_t>(bytes, boot + 11U, kSectorSize);
  bytes[boot + 13U] = std::byte{8};
  if (fat32) {
    constexpr char signature[] = "FAT32   ";
    write_little<std::uint32_t>(
        bytes, boot + 32U, kPartitionSectors);
    std::memcpy(bytes.data() + boot + 82U, signature, 8U);
  } else {
    constexpr char signature[] = "NTFS    ";
    std::memcpy(bytes.data() + boot + 3U, signature, 8U);
    write_little<std::uint64_t>(
        bytes, boot + 40U, kPartitionSectors);
  }
  bytes[boot + 510U] = std::byte{0x55};
  bytes[boot + 511U] = std::byte{0xAA};
  return bytes;
}

std::vector<std::byte> make_two_partition_mbr_disk() {
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  write_little<std::uint32_t>(bytes, 440U, 0x50607080U);
  constexpr std::uint32_t first_lba = 2048U;
  constexpr std::uint32_t partition_sectors = 4096U;
  constexpr std::uint32_t second_lba = 8192U;
  for (const auto [entry, start] : {
           std::pair<std::size_t, std::uint32_t>{446U, first_lba},
           std::pair<std::size_t, std::uint32_t>{462U, second_lba}}) {
    bytes[entry + 4U] = std::byte{0x07};
    write_little(bytes, entry + 8U, start);
    write_little(bytes, entry + 12U, partition_sectors);
    write_ntfs_boot(bytes, start, partition_sectors);
  }
  bytes[510U] = std::byte{0x55};
  bytes[511U] = std::byte{0xAA};
  return bytes;
}

void write_ntfs_boot(
    std::vector<std::byte>& bytes,
    const std::uint64_t first_lba,
    const std::uint64_t sector_count) {
  const auto offset = static_cast<std::size_t>(first_lba * kSectorSize);
  constexpr char signature[] = "NTFS    ";
  std::memcpy(bytes.data() + offset + 3U, signature, 8U);
  write_little<std::uint16_t>(bytes, offset + 11U, kSectorSize);
  bytes[offset + 13U] = std::byte{8};
  write_little<std::uint64_t>(bytes, offset + 40U, sector_count);
  bytes[offset + 510U] = std::byte{0x55};
  bytes[offset + 511U] = std::byte{0xAA};
}

void write_fat32_boot(
    std::vector<std::byte>& bytes,
    const std::uint64_t first_lba,
    const std::uint32_t sector_count) {
  const auto offset = static_cast<std::size_t>(first_lba * kSectorSize);
  constexpr char signature[] = "FAT32   ";
  write_little<std::uint16_t>(bytes, offset + 11U, kSectorSize);
  bytes[offset + 13U] = std::byte{8};
  write_little<std::uint32_t>(bytes, offset + 32U, sector_count);
  std::memcpy(bytes.data() + offset + 82U, signature, 8U);
  bytes[offset + 510U] = std::byte{0x55};
  bytes[offset + 511U] = std::byte{0xAA};
}

class GuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid> next_guid() override {
    ytec::clonecore::GptGuid result;
    result.bytes[0] = static_cast<std::byte>(next_++);
    result.bytes[15] = std::byte{0xA5};
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(result);
  }

 private:
  std::uint8_t next_{1U};
};

ytec::clonecore::GptGuid guid(const std::uint8_t value) {
  ytec::clonecore::GptGuid result;
  result.bytes[0] = static_cast<std::byte>(value);
  result.bytes[15] = std::byte{0x5A};
  return result;
}

struct GptFixture final {
  std::vector<std::byte> bytes;
  ytec::clonecore::GptDisk layout;
};

GptFixture make_gpt_disk() {
  constexpr std::uint64_t sector_count = kDiskSize / kSectorSize;
  ytec::clonecore::GptDisk source{
      .logical_sector_size = kSectorSize,
      .sector_count = sector_count,
      .disk_guid = guid(0x10U),
      .first_usable_lba = 34U,
      .last_usable_lba = sector_count - 34U,
      .partition_entry_count = 128U,
      .partition_entry_size = 128U,
      .partitions = {
          ytec::clonecore::GptPartition{
              .entry_index = 0U,
              .type_guid = ytec::clonecore::gpt_type_efi_system(),
              .unique_guid = guid(0x20U),
              .first_lba = 2048U,
              .last_lba = 3071U,
              .name = u"EFI",
          },
          ytec::clonecore::GptPartition{
              .entry_index = 1U,
              .type_guid = ytec::clonecore::gpt_type_microsoft_reserved(),
              .unique_guid = guid(0x21U),
              .first_lba = 3072U,
              .last_lba = 3327U,
              .name = u"MSR",
          },
          ytec::clonecore::GptPartition{
              .entry_index = 2U,
              .type_guid = ytec::clonecore::gpt_type_basic_data(),
              .unique_guid = guid(0x22U),
              .first_lba = 4096U,
              .last_lba = 16383U,
              .name = u"Windows",
          },
          ytec::clonecore::GptPartition{
              .entry_index = 3U,
              .type_guid = ytec::clonecore::gpt_type_windows_recovery(),
              .unique_guid = guid(0x23U),
              .first_lba = 18432U,
              .last_lba = 20479U,
              .name = u"Recovery",
          },
      },
  };
  GuidGenerator generator;
  const auto plan = ytec::clonecore::make_gpt_write_plan(
      source, kDiskSize, kSectorSize, generator);
  check(plan.has_value(), "Synthetic GPT write plan should build");
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  for (const auto& write : plan.value().writes) {
    std::copy(
        write.bytes.begin(),
        write.bytes.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
  write_fat32_boot(bytes, 2048U, 1024U);
  write_ntfs_boot(bytes, 4096U, 12288U);
  write_ntfs_boot(bytes, 18432U, 2048U);
  return GptFixture{
      .bytes = std::move(bytes),
      .layout = plan.value().target_disk,
  };
}

class MemoryReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit MemoryReader(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_READ_FAULT,
              .operation = L"合成Tsumugi Source読取り",
              .message = L"範囲外です",
          });
    }
    const auto first = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

  void mutate_partition_table() {
    bytes_[440U] ^= std::byte{1};
  }

 private:
  std::vector<std::byte> bytes_;
};

class SharedRescueReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  SharedRescueReader(
      std::shared_ptr<std::vector<std::byte>> bytes,
      std::shared_ptr<bool> source_mutated)
      : bytes_(std::move(bytes)),
        source_mutated_(std::move(source_mutated)) {}

  std::uint64_t size_bytes() const noexcept override {
    return bytes_->size();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_->size() || length > bytes_->size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"合成Windows救出Source読取り",
          .message = L"範囲外です",
      });
    }
    const auto first =
        bytes_->begin() + static_cast<std::ptrdiff_t>(offset);
    std::vector<std::byte> result(
        first, first + static_cast<std::ptrdiff_t>(length));
    if (!*source_mutated_ && offset == 0U &&
        length >= 4U * 1024U * 1024U) {
      (*bytes_)[440U] ^= std::byte{0x01};
      *source_mutated_ = true;
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }

 private:
  std::shared_ptr<std::vector<std::byte>> bytes_;
  std::shared_ptr<bool> source_mutated_;
};

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::vector<wchar_t> buffer(32768U, L'\0');
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(buffer.size()), buffer.data());
    check(length != 0U && length < buffer.size(), "temp path is required");
    path_ = std::filesystem::path(buffer.data()) /
        (L"ytec-windows-rescue-image-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::error_code error;
    check(
        std::filesystem::create_directory(path_, error) && !error,
        "temporary directory must be created");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] std::wstring image(const std::wstring& name) const {
    return (path_ / name).wstring();
  }

 private:
  std::filesystem::path path_;
};

struct RescueStagingState final {
  std::size_t factory_calls{};
  std::size_t seal_calls{};
  std::size_t discard_calls{};
  std::size_t destination_validation_calls{};
  ytec::imageformat::WindowsTsumugiRescueStagingRequest request;
};

class MemoryRescueStaging final
    : public ytec::imageformat::ITsumugiRescueStagingSession {
 public:
  MemoryRescueStaging(
      ytec::imageformat::WindowsTsumugiRescueStagingRequest request,
      std::shared_ptr<RescueStagingState> state)
      : request_(std::move(request)),
        state_(std::move(state)),
        bytes_(static_cast<std::size_t>(request_.source_disk_size),
               std::byte{0}) {}

  std::uint64_t size_bytes() const noexcept override {
    return request_.source_disk_size;
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return request_.logical_sector_size;
  }

  ytec::imageformat::Sha256Digest source_model_hash()
      const noexcept override {
    return request_.source_model_hash;
  }

  ytec::imageformat::Sha256Digest source_serial_hash()
      const noexcept override {
    return request_.source_serial_hash;
  }

  ytec::imageformat::Sha256Digest source_state_hash()
      const noexcept override {
    return request_.source_state_hash;
  }

  ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (sealed_ || discarded_ || offset > bytes_.size() ||
        bytes.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_WRITE_FAULT,
          .operation = L"合成Windows救出一時領域write",
          .message = L"状態または範囲が不正です",
      });
    }
    std::copy(
        bytes.begin(), bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    flushed_ = false;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    return read_range(offset, length, false);
  }

  ytec::clonecore::Status flush_target() override {
    if (sealed_ || discarded_) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_INVALID_STATE,
          .operation = L"合成Windows救出一時領域flush",
          .message = L"状態が不正です",
      });
    }
    flushed_ = true;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status seal_for_image_read() override {
    ++state_->seal_calls;
    if (!flushed_ || discarded_ || sealed_) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::verification_failed,
          .native_code = ERROR_INVALID_STATE,
          .operation = L"合成Windows救出一時領域seal",
          .message = L"状態が不正です",
      });
    }
    sealed_ = true;
    return ytec::clonecore::success_status();
  }

  bool sealed_for_image_read() const noexcept override { return sealed_; }

  ytec::clonecore::Status discard_owned_staging() noexcept override {
    ++state_->discard_calls;
    if (!discarded_) {
      bytes_.clear();
      discarded_ = true;
      sealed_ = false;
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status validate_image_destination_before_commit(
      const std::uint64_t expected_owned_partial_bytes) override {
    ++state_->destination_validation_calls;
    if (!discarded_ || expected_owned_partial_bytes == 0U) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::identity_mismatch,
          .native_code = ERROR_FILE_INVALID,
          .operation = L"合成Windows救出保存先再識別",
          .message = L"一時領域未破棄またはpartial長不正です",
      });
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    return read_range(offset, length, true);
  }

 private:
  ytec::clonecore::Result<std::vector<std::byte>> read_range(
      const std::uint64_t offset,
      const std::size_t length,
      const bool require_sealed) const {
    if (discarded_ || (require_sealed && !sealed_) ||
        (!require_sealed && sealed_) || offset > bytes_.size() ||
        length > bytes_.size() - static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"合成Windows救出一時領域read",
          .message = L"状態または範囲が不正です",
      });
    }
    const auto begin =
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            begin, begin + static_cast<std::ptrdiff_t>(length)));
  }

  ytec::imageformat::WindowsTsumugiRescueStagingRequest request_;
  std::shared_ptr<RescueStagingState> state_;
  std::vector<std::byte> bytes_;
  bool flushed_{};
  bool sealed_{};
  bool discarded_{};
};

ytec::diskmodel::DiskInfo source_disk(
    const bool system = true,
    const bool fat32 = false) {
  return ytec::diskmodel::DiskInfo{
      .disk_number = 0U,
      .device_path = L"\\\\.\\PhysicalDrive0",
      .device_instance_id = L"VIRTUAL\\TSUMUGI_SOURCE",
      .model = L"TSUMUGI SOURCE FIXTURE",
      .size_bytes = kDiskSize,
      .sector_count = kDiskSize / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"Virtual",
      .serial_suffix = "TSUM0001",
      .partition_style = ytec::diskmodel::PartitionStyle::mbr,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = system,
      .partitions = {
          ytec::diskmodel::PartitionInfo{
              .number = 1U,
              .offset_bytes =
                  static_cast<std::uint64_t>(kFirstLba) * kSectorSize,
              .size_bytes =
                  static_cast<std::uint64_t>(kPartitionSectors) * kSectorSize,
              .style = ytec::diskmodel::PartitionStyle::mbr,
              .type = fat32 ? L"0x0C" : L"0x07",
              .identifier = L"MBR-PARTITION-1",
              .name = fat32 ? L"Data" : L"Windows",
              .bootable = !fat32,
          },
      },
  };
}

ytec::diskmodel::DiskInfo source_gpt_disk() {
  auto result = source_disk(true);
  result.device_instance_id = L"VIRTUAL\\TSUMUGI_GPT_SOURCE";
  result.model = L"TSUMUGI GPT SOURCE FIXTURE";
  result.serial_suffix = "TSUMGPT1";
  result.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  result.partitions = {
      ytec::diskmodel::PartitionInfo{
          .number = 1U,
          .offset_bytes = 2048ULL * kSectorSize,
          .size_bytes = 1024ULL * kSectorSize,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
          .identifier = L"GPT-PARTITION-1",
          .name = L"EFI",
      },
      ytec::diskmodel::PartitionInfo{
          .number = 2U,
          .offset_bytes = 3072ULL * kSectorSize,
          .size_bytes = 256ULL * kSectorSize,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}",
          .identifier = L"GPT-PARTITION-2",
          .name = L"MSR",
      },
      ytec::diskmodel::PartitionInfo{
          .number = 3U,
          .offset_bytes = 4096ULL * kSectorSize,
          .size_bytes = 12288ULL * kSectorSize,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
          .identifier = L"GPT-PARTITION-3",
          .name = L"Windows",
      },
      ytec::diskmodel::PartitionInfo{
          .number = 4U,
          .offset_bytes = 18432ULL * kSectorSize,
          .size_bytes = 2048ULL * kSectorSize,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}",
          .identifier = L"GPT-PARTITION-4",
          .name = L"Recovery",
      },
  };
  return result;
}

ytec::diskmodel::DiskInfo source_two_partition_mbr_disk() {
  auto result = source_disk(false);
  result.device_instance_id = L"VIRTUAL\\TSUMUGI_TWO_PARTITION_SOURCE";
  result.model = L"TSUMUGI TWO PARTITION SOURCE FIXTURE";
  result.serial_suffix = "TSUM0002";
  result.partitions = {
      ytec::diskmodel::PartitionInfo{
          .number = 1U,
          .offset_bytes = 2048ULL * kSectorSize,
          .size_bytes = 4096ULL * kSectorSize,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
          .identifier = L"MBR-PARTITION-1",
          .name = L"Data 1",
      },
      ytec::diskmodel::PartitionInfo{
          .number = 2U,
          .offset_bytes = 8192ULL * kSectorSize,
          .size_bytes = 4096ULL * kSectorSize,
          .style = ytec::diskmodel::PartitionStyle::mbr,
          .type = L"0x07",
          .identifier = L"MBR-PARTITION-2",
          .name = L"Data 2",
      },
  };
  return result;
}

ytec::windowsapp::OnlineImageCreateRequest request() {
  return ytec::windowsapp::OnlineImageCreateRequest{
      .selected_source = source_disk(),
      .final_path = L"D:\\backup\\system.tsumugi",
      .administrator = true,
      .windows_major = 10U,
      .windows_minor = 0U,
      .windows_build = 19045U,
      .windows_architecture = "AMD64",
      .created_utc = "2026-08-04T00:00:00Z",
      .app_version = "1.0.0",
      .encryption_password = std::string_view("fixture-password"),
      .replace_existing = true,
      .callbacks = ytec::clonecore::DiskOperationCallbacks{
          .progress = [](const ytec::clonecore::DiskOperationProgress&) {},
          .cancellation_requested = []() { return false; },
      },
  };
}

ytec::clonecore::Result<ytec::diskmodel::ReadOnlyPhysicalDiskHandle>
open_fixture(
    const ytec::clonecore::StableDiskIdentity& expected,
    const bool fat32 = false) {
  auto observed = source_disk(expected.is_system_disk, fat32);
  return ytec::clonecore::Result<
      ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success(
      ytec::diskmodel::ReadOnlyPhysicalDiskHandle{
          .observed = ytec::diskmodel::ReidentifiedReadOnlyDisk{
              .observed = std::move(observed),
              .identity = expected,
          },
          .reader = std::make_unique<MemoryReader>(make_mbr_disk(fat32)),
      });
}

bool flag_set(
    const ytec::imageformat::TsumugiManifestPartitionFlags value,
    const ytec::imageformat::TsumugiManifestPartitionFlags flag) {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

struct Observations final {
  bool opened{};
  bool bindings{};
  bool executed{};
  std::optional<ytec::imageformat::TsumugiCreateVerificationMode>
      verification_mode;
  MemoryReader* reader{};
  std::vector<std::uint64_t> destination_requirements;
  std::vector<
      ytec::imageformat::WindowsTsumugiDestinationGuardPhase>
      destination_phases;
  std::vector<std::uint64_t> expected_partial_bytes;
};

ytec::windowsapp::OnlineImageCreateDependencies dependencies(
    Observations& observed,
    const bool fat32 = false) {
  return ytec::windowsapp::OnlineImageCreateDependencies{
      .open_read_only_disk =
          [&observed, fat32](
              const ytec::clonecore::StableDiskIdentity& expected) {
            observed.opened = true;
            auto result = open_fixture(expected, fat32);
            observed.reader =
                static_cast<MemoryReader*>(result.value().reader.get());
            return result;
          },
      .query_gpt_bindings =
          [](const ytec::diskmodel::DiskInfo&,
             const ytec::clonecore::GptDisk&,
             const std::span<const std::uint32_t>) {
            return ytec::clonecore::Result<std::vector<
                ytec::clonecore::VolumeBitmapBinding>>::failure(
                ytec::clonecore::Error{
                    .code = ytec::clonecore::ErrorCode::unsupported_layout,
                    .native_code = ERROR_NOT_SUPPORTED,
                    .operation = L"予期しないGPT",
                    .message = L"MBRテストです",
                });
          },
      .query_mbr_bindings =
          [&](const ytec::diskmodel::DiskInfo&,
              const ytec::clonecore::MbrDisk& layout,
              const std::span<const std::uint32_t> selected_entries) {
            observed.bindings = true;
            check(layout.partitions.size() == 1U, "Expected one MBR partition");
            check(
                selected_entries.size() == 1U && selected_entries[0] == 0U,
                "Whole MBR selection must bind its only table entry");
            return ytec::clonecore::Result<std::vector<
                ytec::clonecore::VolumeBitmapBinding>>::success({
                ytec::clonecore::VolumeBitmapBinding{
                    .partition_entry_index = 0U,
                    .volume_device_path =
                        L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
                },
            });
          },
      .query_destination_file_system =
          [](const std::wstring&) {
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiImageStorageFileSystem>::success(
                ytec::imageformat::TsumugiImageStorageFileSystem::ntfs);
          },
      .validate_destination =
          [&](const ytec::imageformat::WindowsTsumugiDestinationGuardRequest&
                  guard) {
            observed.destination_requirements.push_back(
                guard.required_available_bytes);
            observed.destination_phases.push_back(guard.phase);
            observed.expected_partial_bytes.push_back(
                guard.expected_owned_partial_bytes);
            check(
                guard.final_path == L"D:\\backup\\system.tsumugi" &&
                    guard.replace_existing,
                "Every destination guard must preserve the approved path and replacement policy");
            return ytec::clonecore::success_status();
          },
      .execute_backup =
          [&](const ytec::vssrequester::WindowsOnlineTsumugiBackupRequest&
                  execution) {
            observed.executed = true;
            check(
                execution.prepared.workflow.administrator &&
                    execution.prepared.workflow.volumes.size() == 1U &&
                    execution.prepared.image.volumes.size() == 1U &&
                    execution.prepared.image.raw_regions.empty(),
                "Exact NTFS source must produce one matched VSS volume and no raw data range");
            const auto& image = execution.prepared.image.image;
            observed.verification_mode = image.verification_mode;
            check(
                image.final_path == L"D:\\backup\\system.tsumugi" &&
                    image.manifest.mode ==
                        ytec::imageformat::TsumugiManifestMode::exact &&
                    image.storage_file_system ==
                        ytec::imageformat::TsumugiImageStorageFileSystem::ntfs &&
                    image.encryption.has_value() && image.replace_existing &&
                    image.chunks.empty() && image.source_session == nullptr,
                "Executor must receive an unmaterialized exact .tsumugi request");
            check(
                image.manifest.partitions.size() == 1U &&
                    image.manifest.partitions.front().source_table_index == 1U &&
                    image.manifest.partitions.front().source_partition_number ==
                        1U &&
                    image.manifest.partitions.front().role ==
                        ytec::imageformat::TsumugiManifestPartitionRole::windows &&
                    flag_set(
                        image.manifest.partitions.front().flags,
                        ytec::imageformat::
                            TsumugiManifestPartitionFlags::selected) &&
                    flag_set(
                        image.manifest.partitions.front().flags,
                        ytec::imageformat::
                            TsumugiManifestPartitionFlags::required) &&
                    flag_set(
                        image.manifest.partitions.front().flags,
                        ytec::imageformat::
                            TsumugiManifestPartitionFlags::contains_windows) &&
                    flag_set(
                        image.manifest.partitions.front().flags,
                        ytec::imageformat::
                            TsumugiManifestPartitionFlags::active),
                "Legacy zero-based MBR index and flags must be normalized into the typed manifest");
            const auto manifest =
                ytec::imageformat::build_tsumugi_manifest_v1(image.manifest);
            check(
                manifest.has_value(),
                "Controller must produce a canonical valid typed manifest");
            check(
                execution.prepared.image.revalidate_locked_layout().has_value(),
                "Same locked read-only layout must revalidate");
            check(
                execution.prepared.image
                    .validate_destination_capacity(64U * 1024U * 1024U)
                    .has_value(),
                "Capacity estimate must run the same destination guard");
            const ytec::imageformat::TsumugiImageCreateReport staged_report{
                .stream = ytec::imageformat::TsumugiStreamBuildReport{
                     .image_length = 32U * 1024U * 1024U,
                     .all_chunks_read_back_verified = true,
                     .all_chunks_authenticated_and_hashed = true,
                     .global_hash_read_back_verified = true,
                    .final_metadata_read_back_verified = true,
                    .final_complete_scan_performed = true,
                },
                .selected_verification_passed = true,
                .complete_verification_passed = true,
            };
            check(
                execution.prepared.revalidate_destination(nullptr)
                        .has_value() &&
                    execution.prepared
                        .revalidate_destination(&staged_report)
                        .has_value(),
                "VSS lifecycle must distinguish the pre-stage guard from the owned-partial final guard");
            check(
                static_cast<bool>(execution.callbacks.progress) &&
                    static_cast<bool>(
                        execution.callbacks.cancellation_requested),
                "Progress and cancellation callbacks must reach the executor");
            return ytec::clonecore::Result<
                ytec::vssrequester::OnlineTsumugiBackupReport>::success(
                ytec::vssrequester::OnlineTsumugiBackupReport{
                    .workflow = ytec::vssrequester::WorkflowReport{
                        .snapshot_data_copied = true,
                        .backup_completed = true,
                        .snapshots_deleted = true,
                    },
                    .image = ytec::imageformat::TsumugiImageCreateReport{
                        .stream = ytec::imageformat::TsumugiStreamBuildReport{
                             .all_chunks_read_back_verified = true,
                             .all_chunks_authenticated_and_hashed = true,
                             .global_hash_read_back_verified = true,
                            .final_metadata_read_back_verified = true,
                            .final_complete_scan_performed = true,
                        },
                        .selected_verification_passed = true,
                        .complete_verification_passed = true,
                    },
                    .final_file_committed_after_vss = true,
                });
          },
  };
}

struct RescueObservations final {
  bool opened{};
  std::vector<ytec::imageformat::WindowsTsumugiDestinationGuardRequest>
      guards;
  std::shared_ptr<std::vector<std::byte>> source_bytes{
      std::make_shared<std::vector<std::byte>>(make_mbr_disk(false))};
  std::shared_ptr<bool> source_mutated{std::make_shared<bool>(false)};
  std::shared_ptr<RescueStagingState> staging{
      std::make_shared<RescueStagingState>()};
};

ytec::windowsapp::WindowsDataRescueImageCreateDependencies
rescue_dependencies(
    const ytec::diskmodel::DiskInfo& reviewed,
    const std::wstring& expected_path,
    RescueObservations& observed) {
  return {
      .open_read_only_disk =
          [&observed, reviewed](
              const ytec::clonecore::StableDiskIdentity& expected) {
            observed.opened = true;
            auto current = reviewed;
            auto identity = ytec::diskmodel::make_stable_disk_identity(
                current, false);
            if (!identity || identity.value().device_instance_id !=
                    expected.device_instance_id) {
              return ytec::clonecore::Result<
                  ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::failure({
                  .code = ytec::clonecore::ErrorCode::identity_mismatch,
                  .native_code = ERROR_INVALID_DATA,
                  .operation = L"合成Windows救出Source再識別",
                  .message = L"対象不一致",
              });
            }
            return ytec::clonecore::Result<
                ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success({
                .observed = {
                    .observed = std::move(current),
                    .identity = identity.take_value(),
                },
                .reader = std::make_unique<SharedRescueReader>(
                    observed.source_bytes, observed.source_mutated),
            });
          },
      .query_destination_file_system =
          [](const std::wstring&) {
            return ytec::clonecore::Result<
                ytec::imageformat::TsumugiImageStorageFileSystem>::success(
                ytec::imageformat::TsumugiImageStorageFileSystem::ntfs);
          },
      .validate_destination =
          [&observed, expected_path](const auto& guard) {
            check(
                guard.final_path == expected_path,
                "Rescue destination guard must retain the selected path");
            observed.guards.push_back(guard);
            return ytec::clonecore::success_status();
          },
      .make_rescue_staging =
          [&observed](const auto& staging_request) {
            ++observed.staging->factory_calls;
            observed.staging->request = staging_request;
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::imageformat::ITsumugiRescueStagingSession>>::success(
                std::make_unique<MemoryRescueStaging>(
                    staging_request, observed.staging));
          },
  };
}

void test_windows_data_rescue_image_uses_owned_staging_without_source_reread() {
  TemporaryDirectory temporary;
  const auto path = temporary.image(L"windows-data-rescue.tsumugi");
  auto source = source_disk(false);
  source.read_only = true;
  source.offline = false;
  auto value = request();
  value.selected_source = source;
  value.final_path = path;
  value.encryption_password.reset();
  value.replace_existing = false;
  RescueObservations observed;
  const auto result =
      ytec::windowsapp::execute_windows_data_rescue_image_create(
          value, rescue_dependencies(source, path, observed));
  check(result.has_value(), "Windows data rescue image must succeed");
  check(
      result.value().source_was_read_only_or_offline &&
          result.value().source_partition_style ==
              ytec::diskmodel::PartitionStyle::mbr &&
          result.value().imaged_partition_count == 1U &&
          result.value().logical_payload_bytes ==
              static_cast<std::uint64_t>(kPartitionSectors) * kSectorSize,
      "Windows rescue report must retain protected source and payload facts");
  check(
      *observed.source_mutated && observed.opened &&
          observed.guards.size() == 2U &&
          observed.guards[0].required_available_bytes == 1U &&
          observed.guards[1].required_available_bytes >
              kDiskSize + result.value().logical_payload_bytes &&
          observed.staging->factory_calls == 1U &&
          observed.staging->seal_calls == 1U &&
          observed.staging->discard_calls == 1U &&
          observed.staging->destination_validation_calls == 1U,
      "Windows rescue must reserve stage plus image and complete owned lifecycle after source drift");
  check(
      result.value().rescue.rescue.byte_exact_copy &&
          !result.value().rescue.rescue.partial_data_loss &&
          result.value().rescue.image.complete_verification_passed &&
          result.value().rescue.image.stream.committed,
      "Lossless Windows rescue must remain rescue-classified and fully verified");
  const auto verified = ytec::imageformat::verify_tsumugi_image_v1({
      .image_path = path,
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
  });
  check(
      verified.has_value() &&
          verified.value().manifest.mode ==
              ytec::imageformat::TsumugiManifestMode::rescue &&
          !verified.value().partial_loss,
      "Committed Windows rescue must reopen as rescue, not exact");
}

void test_windows_data_rescue_rejects_unsafe_sources_before_environment_io() {
  TemporaryDirectory temporary;
  const auto path = temporary.image(L"unsafe-rescue.tsumugi");

  auto system_source = source_disk(true);
  system_source.read_only = true;
  RescueObservations system_observed;
  auto value = request();
  value.selected_source = system_source;
  value.final_path = path;
  auto result = ytec::windowsapp::execute_windows_data_rescue_image_create(
      value,
      rescue_dependencies(system_source, path, system_observed));
  check(
      !result.has_value() && !system_observed.opened &&
          system_observed.guards.empty(),
      "Windows system rescue image must stop before every environment call");

  auto unprotected = source_disk(false);
  RescueObservations unprotected_observed;
  value.selected_source = unprotected;
  result = ytec::windowsapp::execute_windows_data_rescue_image_create(
      value,
      rescue_dependencies(unprotected, path, unprotected_observed));
  check(
      !result.has_value() && !unprotected_observed.opened &&
          unprotected_observed.guards.empty(),
      "Unprotected Windows data source must stop before every environment call");

  auto four_kn = source_disk(false);
  four_kn.read_only = true;
  four_kn.logical_sector_size = 4096U;
  four_kn.physical_sector_size = 4096U;
  four_kn.sector_count = four_kn.size_bytes / four_kn.logical_sector_size;
  RescueObservations four_kn_observed;
  value.selected_source = four_kn;
  result = ytec::windowsapp::execute_windows_data_rescue_image_create(
      value, rescue_dependencies(four_kn, path, four_kn_observed));
  check(
      !result.has_value() && !four_kn_observed.opened &&
          four_kn_observed.guards.empty(),
      "4Kn Windows rescue image must stop before every environment call");

  auto unsupported_physical = source_disk(false);
  unsupported_physical.read_only = true;
  unsupported_physical.physical_sector_size = 3U * kSectorSize;
  RescueObservations unsupported_physical_observed;
  value.selected_source = unsupported_physical;
  result = ytec::windowsapp::execute_windows_data_rescue_image_create(
      value,
      rescue_dependencies(
          unsupported_physical, path, unsupported_physical_observed));
  check(
      !result.has_value() && !unsupported_physical_observed.opened &&
          unsupported_physical_observed.guards.empty(),
      "Unsupported physical sector pair must stop before every environment call");
}

void test_exact_mbr_reaches_tsumugi_executor() {
  Observations observed;
  const auto result = ytec::windowsapp::execute_online_image_create(
      request(), dependencies(observed));
  check(result.has_value(), "Verified exact MBR image should succeed");
  check(
      observed.opened && observed.bindings && observed.executed &&
          observed.destination_requirements.size() == 4U &&
          observed.destination_requirements[0] == 1U &&
          observed.destination_requirements[1] ==
              64U * 1024U * 1024U &&
          observed.destination_requirements[2] ==
              64U * 1024U * 1024U &&
          observed.destination_requirements[3] ==
              64U * 1024U * 1024U &&
          observed.destination_phases.size() == 4U &&
          observed.destination_phases[0] ==
              ytec::imageformat::WindowsTsumugiDestinationGuardPhase::
                  before_stage &&
          observed.destination_phases[1] ==
              ytec::imageformat::WindowsTsumugiDestinationGuardPhase::
                  before_stage &&
          observed.destination_phases[2] ==
              ytec::imageformat::WindowsTsumugiDestinationGuardPhase::
                  before_stage &&
          observed.destination_phases[3] ==
              ytec::imageformat::WindowsTsumugiDestinationGuardPhase::
                  before_commit_owned_partial &&
          observed.expected_partial_bytes[3] ==
              32U * 1024U * 1024U,
      "Destination must be checked before source work, at capacity, and in owned-partial mode before final VSS commit");
}

void test_fast_verification_mode_reaches_windows_vss_executor() {
  Observations observed;
  auto value = request();
  value.verification_mode =
      ytec::imageformat::TsumugiCreateVerificationMode::fast;
  const auto result = ytec::windowsapp::execute_online_image_create(
      value, dependencies(observed));
  check(
      result.has_value() && observed.executed &&
          observed.verification_mode.has_value() &&
          observed.verification_mode.value() ==
              ytec::imageformat::TsumugiCreateVerificationMode::fast,
      "Windows product planning must preserve an explicit fast verification choice");
}

void test_standard_user_stops_before_destination_or_source() {
  Observations observed;
  auto value = request();
  value.administrator = false;
  const auto result = ytec::windowsapp::execute_online_image_create(
      value, dependencies(observed));
  check(!result.has_value(), "Standard-user request must fail");
  check(
      result.error().code == ytec::clonecore::ErrorCode::access_denied &&
          !observed.opened && !observed.bindings && !observed.executed &&
          observed.destination_requirements.empty(),
      "Administrator gate must precede every disk and destination operation");
}

void test_unknown_verification_mode_stops_before_environment_io() {
  Observations observed;
  auto value = request();
  value.verification_mode = static_cast<
      ytec::imageformat::TsumugiCreateVerificationMode>(0xffU);
  const auto result = ytec::windowsapp::execute_online_image_create(
      value, dependencies(observed));
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::invalid_argument &&
            !observed.opened && !observed.bindings && !observed.executed,
        "an unknown Windows verification mode must fail before environment I/O");
}

void test_mutable_fat32_data_is_rejected_before_vss() {
  Observations observed;
  auto value = request();
  value.selected_source = source_disk(false, true);
  auto deps = dependencies(observed, true);
  const auto result = ytec::windowsapp::execute_online_image_create(
      value, deps);
  check(!result.has_value(), "Online FAT32 data image must fail closed");
  check(
      result.error().code ==
              ytec::clonecore::ErrorCode::unsupported_layout &&
          observed.opened && !observed.bindings && !observed.executed,
      "Mutable raw data rejection must happen before Volume/VSS execution");
}

void test_stale_source_stops_before_metadata() {
  Observations observed;
  auto deps = dependencies(observed);
  deps.open_read_only_disk =
      [&](const ytec::clonecore::StableDiskIdentity& expected) {
        observed.opened = true;
        auto result = open_fixture(expected);
        result.value().observed.identity.serial_suffix = "STALE999";
        return result;
      };
  const auto result = ytec::windowsapp::execute_online_image_create(
      request(), deps);
  check(!result.has_value(), "Stale source identity must fail");
  check(
      observed.opened && !observed.bindings && !observed.executed,
      "Stable reidentification must precede metadata and VSS access");
}

void test_locked_layout_drift_is_detected() {
  Observations observed;
  auto deps = dependencies(observed);
  deps.execute_backup =
      [&](const ytec::vssrequester::WindowsOnlineTsumugiBackupRequest&
              execution) {
        observed.executed = true;
        check(observed.reader != nullptr, "Locked reader must stay alive");
        observed.reader->mutate_partition_table();
        const auto revalidated =
            execution.prepared.image.revalidate_locked_layout();
        check(
            !revalidated.has_value() &&
                revalidated.error().code ==
                    ytec::clonecore::ErrorCode::identity_mismatch,
            "Changed partition bytes must fail the locked-layout callback");
        return ytec::clonecore::Result<
            ytec::vssrequester::OnlineTsumugiBackupReport>::failure(
            revalidated.error());
      };
  const auto result = ytec::windowsapp::execute_online_image_create(
      request(), deps);
  check(
      observed.executed,
      "Layout-drift scenario must reach the mock executor");
  check(
      !result.has_value(),
      "Layout drift must prevent a successful online image result");
}

void test_gpt_static_system_ranges_are_the_only_raw_sources() {
  Observations observed;
  auto value = request();
  value.selected_source = source_gpt_disk();
  auto deps = dependencies(observed);
  deps.open_read_only_disk =
      [&](const ytec::clonecore::StableDiskIdentity& expected) {
        observed.opened = true;
        auto fixture = make_gpt_disk();
        auto reader = std::make_unique<MemoryReader>(std::move(fixture.bytes));
        observed.reader = reader.get();
        return ytec::clonecore::Result<
            ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success(
            ytec::diskmodel::ReadOnlyPhysicalDiskHandle{
                .observed = ytec::diskmodel::ReidentifiedReadOnlyDisk{
                    .observed = source_gpt_disk(),
                    .identity = expected,
                },
                .reader = std::move(reader),
            });
      };
  deps.query_gpt_bindings =
      [&](const ytec::diskmodel::DiskInfo&,
          const ytec::clonecore::GptDisk& layout,
          const std::span<const std::uint32_t> selected_entries) {
        observed.bindings = true;
        check(layout.partitions.size() == 4U, "Expected four GPT partitions");
        check(
            selected_entries.size() == 4U &&
                selected_entries[0] == 0U && selected_entries[1] == 1U &&
                selected_entries[2] == 2U && selected_entries[3] == 3U,
            "Whole GPT selection must bind all table entries");
        return ytec::clonecore::Result<std::vector<
            ytec::clonecore::VolumeBitmapBinding>>::success({
            ytec::clonecore::VolumeBitmapBinding{
                .partition_entry_index = 2U,
                .volume_device_path =
                    L"\\\\?\\Volume{22222222-2222-2222-2222-222222222222}\\",
            },
        });
      };
  deps.execute_backup =
      [&](const ytec::vssrequester::WindowsOnlineTsumugiBackupRequest&
              execution) {
        observed.executed = true;
        const auto& prepared = execution.prepared.image;
        check(
            prepared.volumes.size() == 1U &&
                prepared.volumes.front().partition_entry_index == 3U &&
                prepared.raw_regions.size() == 2U &&
                prepared.raw_regions[0].partition_entry_index == 1U &&
                prepared.raw_regions[1].partition_entry_index == 4U,
            "GPT must map Windows NTFS to VSS and only ESP/Recovery to static raw");
        const auto& partitions = prepared.image.manifest.partitions;
        check(
            partitions.size() == 4U &&
                partitions[0].role ==
                    ytec::imageformat::
                        TsumugiManifestPartitionRole::efi_system &&
                partitions[1].role ==
                    ytec::imageformat::
                        TsumugiManifestPartitionRole::microsoft_reserved &&
                partitions[2].role ==
                    ytec::imageformat::TsumugiManifestPartitionRole::windows &&
                partitions[3].role ==
                    ytec::imageformat::TsumugiManifestPartitionRole::recovery,
            "GPT roles and normalized indices must remain explicit in the typed manifest");
        const auto encoded = ytec::imageformat::build_tsumugi_manifest_v1(
            prepared.image.manifest);
        check(encoded.has_value(), "GPT typed manifest must be canonical");
        check(
            prepared.revalidate_locked_layout().has_value(),
            "GPT primary and backup table bytes must revalidate");
        return ytec::clonecore::Result<
            ytec::vssrequester::OnlineTsumugiBackupReport>::success(
            ytec::vssrequester::OnlineTsumugiBackupReport{
                .final_file_committed_after_vss = true,
            });
      };
  const auto result = ytec::windowsapp::execute_online_image_create(value, deps);
  check(
      result.has_value() && observed.opened && observed.bindings &&
          observed.executed,
      "Verified GPT system disk should reach the exact Tsumugi executor");
}

void test_mbr_partition_selection_binds_only_selected_vss_volume() {
  Observations observed;
  auto value = request();
  value.selected_source = source_two_partition_mbr_disk();
  value.selected_partition_numbers = {2U};
  auto deps = dependencies(observed);
  deps.open_read_only_disk =
      [&](const ytec::clonecore::StableDiskIdentity& expected) {
        observed.opened = true;
        auto current = source_two_partition_mbr_disk();
        auto reader =
            std::make_unique<MemoryReader>(make_two_partition_mbr_disk());
        observed.reader = reader.get();
        return ytec::clonecore::Result<
            ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success({
            .observed = {
                .observed = std::move(current),
                .identity = expected,
            },
            .reader = std::move(reader),
        });
      };
  deps.query_mbr_bindings =
      [&](const ytec::diskmodel::DiskInfo&,
          const ytec::clonecore::MbrDisk& layout,
          const std::span<const std::uint32_t> selected_entries) {
        observed.bindings = true;
        check(
            layout.partitions.size() == 2U &&
                selected_entries.size() == 1U &&
                selected_entries[0] == 1U,
            "partition 2 selection must bind only MBR table entry 1");
        return ytec::clonecore::Result<std::vector<
            ytec::clonecore::VolumeBitmapBinding>>::success({
            ytec::clonecore::VolumeBitmapBinding{
                .partition_entry_index = 1U,
                .volume_device_path =
                    L"\\\\?\\Volume{33333333-3333-3333-3333-333333333333}\\",
            },
        });
      };
  deps.execute_backup =
      [&](const ytec::vssrequester::WindowsOnlineTsumugiBackupRequest&
              execution) {
        observed.executed = true;
        const auto& image_plan = execution.prepared.image;
        const auto& manifest = image_plan.image.manifest;
        const auto selection_flag = static_cast<std::uint32_t>(
            ytec::imageformat::TsumugiManifestFlags::partition_selection);
        check(
            execution.prepared.workflow.volumes.size() == 1U &&
                image_plan.volumes.size() == 1U &&
                image_plan.volumes[0].partition_entry_index == 2U &&
                image_plan.volumes[0].disk_offset ==
                    8192ULL * kSectorSize &&
                image_plan.raw_regions.empty(),
            "VSS and payload plans must contain only selected partition 2");
        check(
            manifest.partitions.size() == 2U &&
                (static_cast<std::uint32_t>(manifest.flags) &
                 selection_flag) != 0U &&
                !flag_set(
                    manifest.partitions[0].flags,
                    ytec::imageformat::
                        TsumugiManifestPartitionFlags::selected) &&
                manifest.partitions[0].minimum_target_bytes == 0U &&
                manifest.partitions[0].planned_target_bytes == 0U &&
                manifest.partitions[0].payload_logical_offset == 0U &&
                manifest.partitions[0].payload_logical_length == 0U &&
                flag_set(
                    manifest.partitions[1].flags,
                    ytec::imageformat::
                        TsumugiManifestPartitionFlags::selected) &&
                manifest.partitions[1].source_partition_number == 2U &&
                manifest.partitions[1].payload_logical_offset ==
                    8192ULL * kSectorSize &&
                manifest.partitions[1].payload_logical_length ==
                    4096ULL * kSectorSize,
            "typed manifest must retain the unselected record without payload and bind partition 2 exactly");
        check(
            ytec::imageformat::build_tsumugi_manifest_v1(manifest)
                .has_value(),
            "partial exact manifest must remain canonical Tsumugi v1");
        return ytec::clonecore::Result<
            ytec::vssrequester::OnlineTsumugiBackupReport>::success({
            .final_file_committed_after_vss = true,
        });
      };
  const auto result = ytec::windowsapp::execute_online_image_create(
      value, deps);
  check(
      result.has_value() && observed.opened && observed.bindings &&
          observed.executed,
      "selected MBR data partition must reach the real VSS planning backend");
}

void test_invalid_partition_selection_stops_before_environment_io() {
  for (const auto selection : {
           std::vector<std::uint32_t>{99U},
           std::vector<std::uint32_t>{1U, 1U}}) {
    Observations observed;
    auto value = request();
    value.selected_partition_numbers = selection;
    const auto result = ytec::windowsapp::execute_online_image_create(
        value, dependencies(observed));
    check(
        !result.has_value() && !observed.opened && !observed.bindings &&
            !observed.executed && observed.destination_requirements.empty(),
        "unknown or duplicate online selection must fail before environment I/O");
  }
}

void test_public_identity_hash_contract() {
  const auto identity = ytec::diskmodel::make_stable_disk_identity(
      source_disk(), true);
  check(identity.has_value(), "Fixture stable identity should build");
  MemoryReader reader(make_mbr_disk(false));
  const auto snapshot =
      ytec::imageformat::capture_partition_snapshot_v1(
          reader, ytec::imageformat::PartitionTableStyle::mbr);
  check(snapshot.has_value(), "Fixture snapshot should build");
  const auto hashes = ytec::windowsapp::make_tsumugi_source_identity_hashes(
      identity.value(), 4096U, snapshot.value());
  const auto model = ytec::windowsapp::hash_tsumugi_source_model(
      identity.value().model);
  const auto serial = ytec::windowsapp::hash_tsumugi_source_serial(
      identity.value().serial_suffix,
      identity.value().device_instance_id);
  check(
      hashes.has_value() && model.has_value() && serial.has_value() &&
          hashes.value().model == model.value() &&
          hashes.value().serial == serial.value(),
      "Restore preflight helpers must reproduce snapshot-independent model/serial hashes");
  const auto changed_model = ytec::windowsapp::hash_tsumugi_source_model(
      L"DIFFERENT MODEL");
  check(
      changed_model.has_value() && changed_model.value() != model.value(),
      "Model hash must distinguish a different physical source model");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"windows_data_rescue_image_uses_owned_staging_without_source_reread",
       test_windows_data_rescue_image_uses_owned_staging_without_source_reread},
      {"windows_data_rescue_rejects_unsafe_sources_before_environment_io",
       test_windows_data_rescue_rejects_unsafe_sources_before_environment_io},
      {"exact_mbr_reaches_tsumugi_executor",
       test_exact_mbr_reaches_tsumugi_executor},
      {"fast_verification_mode_reaches_windows_vss_executor",
       test_fast_verification_mode_reaches_windows_vss_executor},
      {"standard_user_stops_before_destination_or_source",
       test_standard_user_stops_before_destination_or_source},
      {"unknown_verification_mode_stops_before_environment_io",
       test_unknown_verification_mode_stops_before_environment_io},
      {"mutable_fat32_data_is_rejected_before_vss",
       test_mutable_fat32_data_is_rejected_before_vss},
      {"stale_source_stops_before_metadata",
       test_stale_source_stops_before_metadata},
      {"locked_layout_drift_is_detected",
       test_locked_layout_drift_is_detected},
      {"gpt_static_system_ranges_are_the_only_raw_sources",
       test_gpt_static_system_ranges_are_the_only_raw_sources},
      {"mbr_partition_selection_binds_only_selected_vss_volume",
       test_mbr_partition_selection_binds_only_selected_vss_volume},
      {"invalid_partition_selection_stops_before_environment_io",
       test_invalid_partition_selection_stops_before_environment_io},
      {"public_identity_hash_contract",
       test_public_identity_hash_contract},
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

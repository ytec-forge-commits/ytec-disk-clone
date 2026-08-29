#include "ytec/windowsapp/online_direct_clone.h"
#include "ytec/windowsapp/online_direct_clone_operation.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSectorSize = 512;
constexpr std::uint64_t kSourceSize = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetSize = 20ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kFirstLba = 2048;
constexpr std::uint32_t kPartitionSectors = 16384;
constexpr std::uint64_t kPartitionOffset =
    static_cast<std::uint64_t>(kFirstLba) * kSectorSize;
constexpr std::uint64_t kPartitionLength =
    static_cast<std::uint64_t>(kPartitionSectors) * kSectorSize;

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

class TestGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid> next_guid() override {
    ytec::clonecore::GptGuid value;
    value.bytes[0] = static_cast<std::byte>(++next_);
    value.bytes[15] = std::byte{0xA5};
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(value);
  }

 private:
  std::uint8_t next_{};
};

std::vector<std::byte> make_source_mbr() {
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kSourceSize), std::byte{0});
  write_little<std::uint32_t>(bytes, 440, 0x10203040U);
  const std::size_t entry = 446;
  bytes[entry] = std::byte{0x80};
  bytes[entry + 4] = std::byte{0x07};
  write_little(bytes, entry + 8, kFirstLba);
  write_little(bytes, entry + 12, kPartitionSectors);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};

  const std::size_t boot = static_cast<std::size_t>(kPartitionOffset);
  constexpr char kNtfsSignature[] = "NTFS    ";
  std::memcpy(bytes.data() + boot + 3, kNtfsSignature, 8);
  write_little<std::uint16_t>(bytes, boot + 11, kSectorSize);
  bytes[boot + 13] = std::byte{8};
  write_little<std::uint64_t>(
      bytes, boot + 40, kPartitionSectors);
  bytes[boot + 510] = std::byte{0x55};
  bytes[boot + 511] = std::byte{0xAA};
  return bytes;
}

std::vector<std::byte> make_source_mbr_exfat() {
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kSourceSize), std::byte{0});
  write_little<std::uint32_t>(bytes, 440, 0x10203040U);
  const std::size_t entry = 446;
  bytes[entry] = std::byte{0};
  bytes[entry + 4] = std::byte{0x07};
  write_little(bytes, entry + 8, kFirstLba);
  write_little(bytes, entry + 12, kPartitionSectors);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};

  const std::size_t boot = static_cast<std::size_t>(kPartitionOffset);
  std::fill_n(
      bytes.begin() + static_cast<std::ptrdiff_t>(boot),
      static_cast<std::size_t>(kPartitionLength),
      std::byte{0x6E});
  std::fill_n(
      bytes.begin() + static_cast<std::ptrdiff_t>(boot),
      kSectorSize,
      std::byte{0});
  constexpr char signature[] = "EXFAT   ";
  std::memcpy(bytes.data() + boot + 3, signature, 8);
  constexpr std::uint32_t fat_offset = 24U;
  constexpr std::uint32_t fat_length = 32U;
  constexpr std::uint32_t heap_offset = fat_offset + fat_length;
  constexpr std::uint32_t sectors_per_cluster = 8U;
  write_little<std::uint64_t>(bytes, boot + 64, kFirstLba);
  write_little<std::uint64_t>(bytes, boot + 72, kPartitionSectors);
  write_little<std::uint32_t>(bytes, boot + 80, fat_offset);
  write_little<std::uint32_t>(bytes, boot + 84, fat_length);
  write_little<std::uint32_t>(bytes, boot + 88, heap_offset);
  write_little<std::uint32_t>(
      bytes,
      boot + 92,
      (kPartitionSectors - heap_offset) / sectors_per_cluster);
  write_little<std::uint32_t>(bytes, boot + 96, 2U);
  write_little<std::uint16_t>(bytes, boot + 104, 0x0100U);
  bytes[boot + 108] = std::byte{9};
  bytes[boot + 109] = std::byte{3};
  bytes[boot + 110] = std::byte{1};
  bytes[boot + 112] = std::byte{0};
  bytes[boot + 510] = std::byte{0x55};
  bytes[boot + 511] = std::byte{0xAA};
  bytes[boot + 100] = std::byte{0xE7};
  return bytes;
}

std::vector<std::byte> make_source_mbr_fat32() {
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kSourceSize), std::byte{0});
  write_little<std::uint32_t>(bytes, 440, 0x10203040U);
  const std::size_t entry = 446;
  bytes[entry] = std::byte{0};
  bytes[entry + 4] = std::byte{0x0C};
  write_little(bytes, entry + 8, kFirstLba);
  write_little(bytes, entry + 12, kPartitionSectors);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};

  const std::size_t boot = static_cast<std::size_t>(kPartitionOffset);
  std::fill_n(
      bytes.begin() + static_cast<std::ptrdiff_t>(boot),
      static_cast<std::size_t>(kPartitionLength),
      std::byte{0x4F});
  std::fill_n(
      bytes.begin() + static_cast<std::ptrdiff_t>(boot),
      kSectorSize,
      std::byte{0});
  constexpr char signature[] = "FAT32   ";
  std::memcpy(bytes.data() + boot + 82, signature, 8);
  write_little<std::uint16_t>(bytes, boot + 11, kSectorSize);
  bytes[boot + 13] = std::byte{8};
  write_little<std::uint32_t>(bytes, boot + 32, kPartitionSectors);
  bytes[boot + 510] = std::byte{0x55};
  bytes[boot + 511] = std::byte{0xAA};
  bytes[boot + 100] = std::byte{0xF3};
  return bytes;
}

std::vector<std::byte> make_source_gpt_basic(const bool exfat) {
  ytec::clonecore::GptDisk disk;
  disk.logical_sector_size = kSectorSize;
  disk.sector_count = kSourceSize / kSectorSize;
  disk.disk_guid.bytes[0] = std::byte{0x11};
  disk.first_usable_lba = 34U;
  disk.last_usable_lba = disk.sector_count - 34U;
  disk.partition_entry_count = 128U;
  disk.partition_entry_size = 128U;
  ytec::clonecore::GptGuid partition_guid;
  partition_guid.bytes[0] = std::byte{0x22};
  disk.partitions.push_back(ytec::clonecore::GptPartition{
      .entry_index = 0U,
      .type_guid = ytec::clonecore::gpt_type_basic_data(),
      .unique_guid = partition_guid,
      .first_lba = kFirstLba,
      .last_lba = kFirstLba + kPartitionSectors - 1U,
      .name = u"Data",
  });
  TestGuidGenerator guids;
  const auto metadata = ytec::clonecore::make_gpt_write_plan(
      disk, kSourceSize, kSectorSize, guids);
  check(metadata.has_value(), "synthetic GPT metadata should build");
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kSourceSize), std::byte{0});
  for (const auto& write : metadata.value().writes) {
    std::copy(
        write.bytes.begin(),
        write.bytes.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
  const std::size_t boot = static_cast<std::size_t>(kPartitionOffset);
  if (exfat) {
    constexpr char signature[] = "EXFAT   ";
    std::memcpy(bytes.data() + boot + 3, signature, 8);
    constexpr std::uint32_t fat_offset = 24U;
    constexpr std::uint32_t fat_length = 32U;
    constexpr std::uint32_t heap_offset = fat_offset + fat_length;
    constexpr std::uint32_t sectors_per_cluster = 8U;
    write_little<std::uint64_t>(bytes, boot + 64, kFirstLba);
    write_little<std::uint64_t>(bytes, boot + 72, kPartitionSectors);
    write_little<std::uint32_t>(bytes, boot + 80, fat_offset);
    write_little<std::uint32_t>(bytes, boot + 84, fat_length);
    write_little<std::uint32_t>(bytes, boot + 88, heap_offset);
    write_little<std::uint32_t>(
        bytes,
        boot + 92,
        (kPartitionSectors - heap_offset) / sectors_per_cluster);
    write_little<std::uint32_t>(bytes, boot + 96, 2U);
    write_little<std::uint16_t>(bytes, boot + 104, 0x0100U);
    bytes[boot + 108] = std::byte{9};
    bytes[boot + 109] = std::byte{3};
    bytes[boot + 110] = std::byte{1};
    bytes[boot + 112] = std::byte{0};
  } else {
    constexpr char signature[] = "FAT32   ";
    std::memcpy(bytes.data() + boot + 82, signature, 8);
    write_little<std::uint16_t>(bytes, boot + 11, kSectorSize);
    bytes[boot + 13] = std::byte{8};
    write_little<std::uint32_t>(
        bytes, boot + 32, kPartitionSectors);
  }
  bytes[boot + 510] = std::byte{0x55};
  bytes[boot + 511] = std::byte{0xAA};
  return bytes;
}

std::vector<std::byte> make_snapshot_partition() {
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kPartitionLength), std::byte{0x5A});
  constexpr char kNtfsSignature[] = "NTFS    ";
  std::memcpy(bytes.data() + 3, kNtfsSignature, 8);
  write_little<std::uint16_t>(bytes, 11, kSectorSize);
  bytes[13] = std::byte{8};
  write_little<std::uint64_t>(bytes, 40, kPartitionSectors);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};
  bytes[100] = std::byte{0xA7};
  return bytes;
}

class MemoryReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  MemoryReader(
      std::vector<std::byte> bytes,
      std::shared_ptr<std::vector<std::uint64_t>> offsets = {},
      const bool* const retained_volume_lock_alive = nullptr)
      : bytes_(std::move(bytes)),
        offsets_(std::move(offsets)),
        retained_volume_lock_alive_(retained_volume_lock_alive) {}

  std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offsets_) {
      offsets_->push_back(offset);
    }
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_READ_FAULT,
              .operation = L"MemoryReader",
              .message = L"範囲外です",
          });
    }
    const std::uint64_t end = offset + length;
    if (retained_volume_lock_alive_ != nullptr &&
        *retained_volume_lock_alive_ &&
        offset < kPartitionOffset + kPartitionLength &&
        kPartitionOffset < end) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::access_denied,
              .native_code = ERROR_ACCESS_DENIED,
              .operation = L"MemoryReader retained Volume lock",
              .message = L"lock中のパーティションはlock handle以外から読めません",
          });
    }
    const auto first =
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

 private:
  std::vector<std::byte> bytes_;
  std::shared_ptr<std::vector<std::uint64_t>> offsets_;
  const bool* retained_volume_lock_alive_{};
};

class LockLeaseReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  LockLeaseReader(std::vector<std::byte> bytes, bool* const lease_alive)
      : reader_(std::move(bytes)), lease_alive_(lease_alive) {
    *lease_alive_ = true;
  }

  ~LockLeaseReader() override { *lease_alive_ = false; }

  std::uint64_t size_bytes() const noexcept override {
    return reader_.size_bytes();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return reader_.logical_sector_size();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    return reader_.read(offset, length);
  }

 private:
  MemoryReader reader_;
  bool* lease_alive_{};
};

class MemoryWriter final : public ytec::clonecore::ITargetDiskWriter {
 public:
  std::uint64_t size_bytes() const noexcept override {
    return kTargetSize;
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  ytec::clonecore::Status write_target(
      std::uint64_t,
      std::span<const std::byte>) override {
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      std::uint64_t,
      const std::size_t length) const override {
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(length));
  }

  ytec::clonecore::Status flush_target() override {
    return ytec::clonecore::success_status();
  }
};

class DummyBitmapProvider final
    : public ytec::clonecore::INtfsUsedRangeProvider {
 public:
  ytec::clonecore::Result<std::vector<ytec::clonecore::ByteRange>>
  query_used_ranges(
      std::uint32_t,
      const ytec::clonecore::NtfsGeometry&) override {
    return ytec::clonecore::Result<
        std::vector<ytec::clonecore::ByteRange>>::success({
        ytec::clonecore::ByteRange{.offset = 0, .length = 4096},
    });
  }
};

ytec::clonecore::StableDiskIdentity source_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 0,
      .model = L"ONLINE SOURCE",
      .size_bytes = kSourceSize,
      .logical_sector_size = kSectorSize,
      .serial_suffix = "SOURCE01",
      .device_instance_id = L"VIRTUAL\\ONLINE_SOURCE",
      .is_system_disk = true,
  };
}

ytec::clonecore::StableDiskIdentity target_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 1,
      .model = L"EXISTING TARGET",
      .size_bytes = kTargetSize,
      .logical_sector_size = kSectorSize,
      .serial_suffix = "TARGET01",
      .device_instance_id = L"VIRTUAL\\EXISTING_TARGET",
      .is_system_disk = false,
  };
}

ytec::clonecore::StableDiskIdentity exfat_source_identity() {
  auto identity = source_identity();
  identity.is_system_disk = false;
  return identity;
}

ytec::diskmodel::DiskInfo source_disk() {
  auto disk = ytec::diskmodel::DiskInfo{
      .disk_number = 0,
      .device_path = L"\\\\.\\PhysicalDrive0",
      .device_instance_id = L"VIRTUAL\\ONLINE_SOURCE",
      .model = L"ONLINE SOURCE",
      .size_bytes = kSourceSize,
      .sector_count = kSourceSize / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096,
      .bus_type = L"Virtual",
      .serial_suffix = "SOURCE01",
      .partition_style = ytec::diskmodel::PartitionStyle::mbr,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = true,
  };
  disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
      .number = 1,
      .offset_bytes = kPartitionOffset,
      .size_bytes = kPartitionLength,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"NTFS",
      .name = L"Windows",
  });
  return disk;
}

ytec::diskmodel::DiskInfo target_disk() {
  auto disk = ytec::diskmodel::DiskInfo{
      .disk_number = 1,
      .device_path = L"\\\\.\\PhysicalDrive1",
      .device_instance_id = L"VIRTUAL\\EXISTING_TARGET",
      .model = L"EXISTING TARGET",
      .size_bytes = kTargetSize,
      .sector_count = kTargetSize / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096,
      .bus_type = L"SATA",
      .serial_suffix = "TARGET01",
      .partition_style = ytec::diskmodel::PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
      .number = 1,
      .offset_bytes = 1024 * 1024,
      .size_bytes = 8 * 1024 * 1024,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"Basic Data",
      .name = L"既存NTFS",
  });
  return disk;
}

ytec::diskmodel::DiskInfo exfat_source_disk() {
  auto disk = source_disk();
  disk.is_system_disk = false;
  disk.partitions.front().type = L"exFAT";
  disk.partitions.front().name = L"Data";
  return disk;
}

ytec::diskmodel::DiskInfo fat32_source_disk() {
  auto disk = source_disk();
  disk.is_system_disk = false;
  disk.partitions.front().type = L"FAT32";
  disk.partitions.front().name = L"Data";
  return disk;
}

ytec::diskmodel::DiskInfo gpt_basic_source_disk(const bool exfat) {
  auto disk = source_disk();
  disk.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  disk.is_system_disk = false;
  disk.partitions.front().style = ytec::diskmodel::PartitionStyle::gpt;
  disk.partitions.front().type = exfat ? L"exFAT" : L"FAT32";
  disk.partitions.front().name = L"Data";
  return disk;
}

ytec::diskmodel::ReidentifiedPhysicalClone observation() {
  return ytec::diskmodel::ReidentifiedPhysicalClone{
      .source = source_disk(),
      .target = target_disk(),
      .source_identity = source_identity(),
      .target_identity = target_identity(),
  };
}

ytec::diskmodel::ReidentifiedPhysicalClone exfat_observation() {
  return ytec::diskmodel::ReidentifiedPhysicalClone{
      .source = exfat_source_disk(),
      .target = target_disk(),
      .source_identity = exfat_source_identity(),
      .target_identity = target_identity(),
  };
}

ytec::diskmodel::ReidentifiedPhysicalClone fat32_observation() {
  return ytec::diskmodel::ReidentifiedPhysicalClone{
      .source = fat32_source_disk(),
      .target = target_disk(),
      .source_identity = exfat_source_identity(),
      .target_identity = target_identity(),
  };
}

ytec::windowsapp::OnlineDirectCloneRequest valid_request() {
  const auto source_layout = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(source_disk());
  const auto target_layout = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(target_disk());
  check(
      source_layout.has_value() && target_layout.has_value(),
      "reviewed clone layouts must hash");
  return ytec::windowsapp::OnlineDirectCloneRequest{
      .administrator = true,
      .expected_source = source_identity(),
      .expected_target = target_identity(),
      .expected_source_layout_hash = source_layout.value(),
      .expected_target_layout_hash = target_layout.value(),
      .confirmation =
          ytec::clonecore::TargetConfirmation{
              .first_step_acknowledged = true,
              .typed_token = L"OK",
          },
  };
}

ytec::windowsapp::OnlineDirectCloneRequest valid_exfat_request() {
  const auto source_layout = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(exfat_source_disk());
  const auto target_layout = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(target_disk());
  check(source_layout.has_value() && target_layout.has_value(),
        "reviewed exFAT clone layouts must hash");
  return ytec::windowsapp::OnlineDirectCloneRequest{
      .administrator = true,
      .expected_source = exfat_source_identity(),
      .expected_target = target_identity(),
      .expected_source_layout_hash = source_layout.value(),
      .expected_target_layout_hash = target_layout.value(),
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
  };
}

ytec::windowsapp::OnlineDirectCloneRequest valid_fat32_request() {
  const auto source_layout = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(fat32_source_disk());
  const auto target_layout = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(target_disk());
  check(source_layout.has_value() && target_layout.has_value(),
        "reviewed FAT32 clone layouts must hash");
  return ytec::windowsapp::OnlineDirectCloneRequest{
      .administrator = true,
      .expected_source = exfat_source_identity(),
      .expected_target = target_identity(),
      .expected_source_layout_hash = source_layout.value(),
      .expected_target_layout_hash = target_layout.value(),
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
  };
}

ytec::imageformat::Sha256Digest verified_write_digest() {
  ytec::imageformat::Sha256Digest digest{};
  digest[0] = std::byte{0xA5};
  digest[31] = std::byte{0x5A};
  return digest;
}

ytec::windowsapp::OnlineDirectCloneOperationRequest
valid_operation_request() {
  ytec::operationcore::OperationId operation_id{};
  operation_id[0] = std::byte{0x42};
  return ytec::windowsapp::OnlineDirectCloneOperationRequest{
      .reviewed_source = source_disk(),
      .reviewed_target = target_disk(),
      .clone = valid_request(),
      .operation_id = operation_id,
  };
}

struct DependencyState final {
  std::vector<std::string> events;
  int reidentify_calls{};
  int offline_calls{};
  int engine_calls{};
  int boot_finalization_calls{};
  int vss_calls{};
  int locked_open_calls{};
  bool fail_final_reidentify{};
  bool fail_workflow_before_callback{};
  bool fail_boot_finalization{};
  bool throw_boot_finalization{};
  bool fail_offline_reprotection{};
  bool target_online{};
  bool drift_final_target_layout{};
  bool drift_opened_target_layout{};
  bool incomplete_engine_report{};
  bool zero_engine_bytes{};
  bool zero_write_digest{};
  bool source_is_system{true};
  bool source_is_exfat{};
  bool source_is_fat32{};
  bool fail_locked_volume_open{};
  bool lock_alive{};
  bool target_health_failing{};
  bool target_health_failing_on_final_reidentify{};
  std::shared_ptr<std::vector<std::uint64_t>> physical_reads =
      std::make_shared<std::vector<std::uint64_t>>();
};

ytec::clonecore::Error injected_error(std::wstring operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::identity_mismatch,
      .native_code = ERROR_DEVICE_NOT_CONNECTED,
      .operation = std::move(operation),
      .message = L"テスト注入",
  };
}

ytec::windowsapp::OnlineDirectCloneDependencies dependencies(
    DependencyState& state) {
  return ytec::windowsapp::OnlineDirectCloneDependencies{
      .reidentify_clone_selection =
          [&](const auto&, const auto&) {
            ++state.reidentify_calls;
            state.events.push_back("reidentify-selection");
            auto observed = state.source_is_fat32
                ? fat32_observation()
                : state.source_is_exfat ? exfat_observation()
                                        : observation();
            if (state.target_health_failing) {
              observed.target.health.state =
                  ytec::diskmodel::DiskHealthState::failing;
            }
            observed.source.is_system_disk = state.source_is_system;
            observed.source_identity.is_system_disk = state.source_is_system;
            return ytec::clonecore::Result<
                ytec::diskmodel::ReidentifiedPhysicalClone>::success(
                std::move(observed));
          },
      .reidentify_clone =
          [&](const auto&, const auto&, const auto&) {
            ++state.reidentify_calls;
            state.events.push_back("reidentify");
            if (state.fail_final_reidentify &&
                state.reidentify_calls == 2) {
              return ytec::clonecore::Result<
                  ytec::diskmodel::ReidentifiedPhysicalClone>::failure(
                  injected_error(L"最終再識別"));
            }
            auto observed = state.source_is_fat32
                ? fat32_observation()
                : state.source_is_exfat ? exfat_observation()
                                        : observation();
            if (state.target_health_failing) {
              observed.target.health.state =
                  ytec::diskmodel::DiskHealthState::failing;
            }
            if (state.target_health_failing_on_final_reidentify &&
                state.reidentify_calls == 2) {
              observed.target.health.state =
                  ytec::diskmodel::DiskHealthState::failing;
            }
            if (state.drift_final_target_layout &&
                state.reidentify_calls == 2) {
              observed.target.partitions[0].size_bytes += kSectorSize;
            }
            observed.source.is_system_disk = state.source_is_system;
            observed.source_identity.is_system_disk = state.source_is_system;
            return ytec::clonecore::Result<
                ytec::diskmodel::ReidentifiedPhysicalClone>::success(
                std::move(observed));
          },
      .open_read_only_source =
          [&](const auto& expected) {
            state.events.push_back("open-source");
            return ytec::clonecore::Result<
                ytec::diskmodel::ReadOnlyPhysicalDiskHandle>::success(
                ytec::diskmodel::ReadOnlyPhysicalDiskHandle{
                    .observed =
                        ytec::diskmodel::ReidentifiedReadOnlyDisk{
                    .observed = [&]() {
                      auto disk = state.source_is_fat32
                          ? fat32_source_disk()
                          : state.source_is_exfat ? exfat_source_disk()
                                                  : source_disk();
                      disk.is_system_disk = state.source_is_system;
                      return disk;
                    }(),
                            .identity = expected,
                        },
                    .reader = std::make_unique<MemoryReader>(
                        state.source_is_fat32
                            ? make_source_mbr_fat32()
                            : state.source_is_exfat
                                ? make_source_mbr_exfat()
                                : make_source_mbr(),
                        state.physical_reads,
                        &state.lock_alive),
                });
          },
      .query_gpt_bindings =
          [](const auto&, const auto&) {
            return ytec::clonecore::Result<std::vector<
                ytec::clonecore::VolumeBitmapBinding>>::failure(
                injected_error(L"予期しないGPT照会"));
          },
      .query_mbr_bindings =
          [&](const auto&, const auto& mbr) {
            state.events.push_back("bindings");
            check(mbr.partitions.size() == 1,
                  "MBR layout should reach binding query");
            return ytec::clonecore::Result<std::vector<
                ytec::clonecore::VolumeBitmapBinding>>::success({
                ytec::clonecore::VolumeBitmapBinding{
                    .partition_entry_index = 0,
                    .volume_device_path =
                        L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
                },
            });
          },
      .run_snapshot_workflow =
          [&](const auto& workflow,
              const auto&,
              const auto*,
              auto callback) {
            ++state.vss_calls;
            state.events.push_back("vss-start");
            check(workflow.volumes.size() == 1,
                  "Exactly one NTFS volume should be snapshotted");
            if (state.fail_workflow_before_callback) {
              return ytec::clonecore::Result<
                  ytec::vssrequester::WorkflowReport>::failure(
                  injected_error(L"VSS Writer"));
            }
            const auto copied = callback(
                ytec::vssrequester::SnapshotCopyContext{
                    .snapshot_set_id = L"{fixture}",
                    .mappings = {
                        ytec::vssrequester::SnapshotMapping{
                            .original_volume_guid_path =
                                workflow.volumes.front().volume_guid_path,
                            .snapshot_id =
                                L"{00000000-0000-0000-0000-000000000007}",
                            .snapshot_device_path =
                                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy7",
                            .provider_id =
                                L"{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}",
                            .creation_timestamp = 1'007,
                        },
                    },
                });
            if (!copied) {
              return ytec::clonecore::Result<
                  ytec::vssrequester::WorkflowReport>::failure(
                  copied.error());
            }
            state.events.push_back("vss-cleanup");
            return ytec::clonecore::Result<
                ytec::vssrequester::WorkflowReport>::success(
                ytec::vssrequester::WorkflowReport{
                    .snapshot_set_id = L"{fixture}",
                    .volume_count = 1,
                    .writer_count = 1,
                    .snapshot_data_copied = true,
                    .backup_completed = true,
                    .snapshots_deleted = true,
                });
          },
      .open_snapshot_reader =
          [&](const auto& open) {
            state.events.push_back("open-snapshot");
            check(open.expected_size_bytes == kPartitionLength,
                  "Snapshot reader must be pinned to partition length");
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::clonecore::ISourceDiskReader>>::success(
                std::make_unique<MemoryReader>(
                    make_snapshot_partition()));
          },
      .open_locked_volume =
          [&](const auto& open) {
            ++state.locked_open_calls;
            state.events.push_back("lock-volume");
            if ((!state.source_is_exfat && !state.source_is_fat32) ||
                state.fail_locked_volume_open) {
              return ytec::clonecore::Result<std::unique_ptr<
                  ytec::clonecore::ISourceDiskReader>>::failure(
                  injected_error(L"Volume lock"));
            }
            check(open.physical_disk_number == 0U &&
                      open.partition_index == 0U &&
                      open.disk_offset == kPartitionOffset &&
                      open.length == kPartitionLength &&
                      open.logical_sector_size == kSectorSize &&
                       open.expected_file_system ==
                           (state.source_is_fat32
                                ? ytec::windowsapp::
                                      OnlineDirectLockedFileSystem::fat32
                                : ytec::windowsapp::
                                      OnlineDirectLockedFileSystem::exfat),
                   "FAT32/exFAT lock request must retain disk, extent, sector, and filesystem");
            std::unique_ptr<ytec::clonecore::ISourceDiskReader> reader =
                std::make_unique<LockLeaseReader>(
                    [&]() {
                       auto disk = state.source_is_fat32
                           ? make_source_mbr_fat32()
                           : make_source_mbr_exfat();
                      return std::vector<std::byte>(
                          disk.begin() +
                              static_cast<std::ptrdiff_t>(kPartitionOffset),
                          disk.begin() + static_cast<std::ptrdiff_t>(
                              kPartitionOffset + kPartitionLength));
                    }(),
                    &state.lock_alive);
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::clonecore::ISourceDiskReader>>::success(
                std::move(reader));
          },
      .make_snapshot_bitmap_provider =
          [&](auto bindings) {
            state.events.push_back("bitmap");
            check(bindings.size() == 1 &&
                      bindings[0].partition_entry_index == 0,
                  "Snapshot bitmap must use the planned partition");
            std::unique_ptr<ytec::clonecore::INtfsUsedRangeProvider>
                provider = std::make_unique<DummyBitmapProvider>();
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::clonecore::INtfsUsedRangeProvider>>::success(
                std::move(provider));
          },
      .set_clone_target_offline =
          [&](const auto&, const auto&, const auto&, const bool offline) {
            state.events.push_back("offline-before-write");
            ++state.offline_calls;
            check(offline, "Target must be offline before the first write");
             check((!state.source_is_exfat && !state.source_is_fat32) ||
                       state.lock_alive,
                   "FAT32/exFAT Volume lock must be retained before target mutation");
            return ytec::clonecore::success_status();
          },
      .set_physical_target_offline =
          [&](const auto&, const auto&, const bool offline) {
            state.events.push_back(
                offline ? "offline-verify" : "online-for-boot");
            ++state.offline_calls;
            if (!offline) {
              state.target_online = true;
              return ytec::clonecore::success_status();
            }
            if (state.fail_offline_reprotection &&
                state.target_online) {
              return ytec::clonecore::Status::failure(
                  injected_error(L"offline再保護"));
            }
            state.target_online = false;
            return ytec::clonecore::success_status();
          },
      .open_offline_target =
          [&](const auto&, const auto&) {
            state.events.push_back("open-target");
            auto opened = target_disk();
            opened.offline = true;
            if (state.drift_opened_target_layout) {
              opened.partitions[0].size_bytes += kSectorSize;
            }
            return ytec::clonecore::Result<
                ytec::diskmodel::PhysicalTargetHandle>::success(
                ytec::diskmodel::PhysicalTargetHandle{
                    .observed =
                        ytec::diskmodel::ReidentifiedPhysicalTarget{
                            .target = std::move(opened),
                            .target_identity = target_identity(),
                        },
                    .target = std::make_unique<MemoryWriter>(),
                });
          },
      .collect_mbr_signatures =
          [&](const auto&, const auto& mbr) {
            state.events.push_back("mbr-signatures");
            return ytec::clonecore::Result<
                std::vector<std::uint32_t>>::success({
                mbr.disk_signature,
            });
          },
       .execute_clone_engine =
           [&](const auto& context) {
             state.events.push_back("engine");
             ++state.engine_calls;
             if (context.source == nullptr || context.target == nullptr ||
                 context.snapshot_bitmap_provider == nullptr) {
               return ytec::clonecore::Result<
                   ytec::windowsapp::OnlineDirectCloneEngineReport>::failure(
                   injected_error(L"engine I/O boundary"));
             }
             const auto mbr = context.source->read(0, 512);
            check(mbr.has_value() &&
                      mbr.value()[510] == std::byte{0x55},
                  "Disk metadata must use the physical read-only route");
            const auto snapshot =
                context.source->read(kPartitionOffset, 512);
             const std::byte expected_marker = state.source_is_fat32
                 ? std::byte{0xF3}
                 : state.source_is_exfat ? std::byte{0xE7}
                                         : std::byte{0xA7};
             check(snapshot.has_value() &&
                       snapshot.value()[100] == expected_marker,
                   (state.source_is_exfat || state.source_is_fat32)
                       ? "FAT32/exFAT data must use the retained Volume lock route"
                       : "NTFS data must use the Snapshot route");
            check((!state.source_is_exfat && !state.source_is_fat32) ||
                      state.lock_alive,
                  "FAT32/exFAT Volume lock must remain alive through the engine");
            const auto gap = context.source->read(4096, 512);
            check(!gap.has_value(),
                  "Unclassified live physical ranges must fail closed");
            return ytec::clonecore::Result<
                ytec::windowsapp::OnlineDirectCloneEngineReport>::success(
                ytec::windowsapp::OnlineDirectCloneEngineReport{
                    .copied_data_bytes =
                        state.zero_engine_bytes ? 0U : 4096U,
                    .copied_partition_count =
                        state.incomplete_engine_report ? 0U : 1U,
                    .verified_write_digest = state.zero_write_digest
                        ? ytec::imageformat::Sha256Digest{}
                        : verified_write_digest(),
                    .read_back_verified = true,
                    .partition_table_committed = true,
                });
          },
      .finalize_target_boot =
          [&](const auto& target, const auto style) {
            state.events.push_back("boot-finalize");
            ++state.boot_finalization_calls;
            check(target.disk_number == target_identity().disk_number &&
                      style == ytec::windowsapp::
                          OnlineDirectClonePartitionStyle::mbr,
                  "Boot finalizer must receive the reidentified target and style");
            if (state.throw_boot_finalization) {
              throw std::runtime_error("synthetic boot finalizer exception");
            }
            if (state.fail_boot_finalization) {
              return ytec::clonecore::Status::failure(
                  injected_error(L"BCD新規再構築"));
            }
            return ytec::clonecore::success_status();
          },
  };
}

void test_composite_reader_routes_snapshot_and_rejects_crossing() {
  MemoryReader physical(make_source_mbr());
  std::vector<ytec::windowsapp::OnlineDirectSnapshotReader> snapshots;
  snapshots.push_back(
      ytec::windowsapp::OnlineDirectSnapshotReader{
          .partition_index = 0,
          .disk_offset = kPartitionOffset,
          .length = kPartitionLength,
          .reader = std::make_unique<MemoryReader>(
              make_snapshot_partition()),
      });
  auto reader = ytec::windowsapp::make_online_direct_composite_reader(
      &physical,
      std::move(snapshots),
      {ytec::clonecore::ByteRange{.offset = 0, .length = 512}});
  check(reader.has_value(), "A disjoint composite reader should build");
  check(reader.value()->read(0, 512).has_value(),
        "Approved metadata should use physical reader");
  const auto snapshot = reader.value()->read(kPartitionOffset, 512);
  check(snapshot.has_value() &&
            snapshot.value()[100] == std::byte{0xA7},
        "Partition reads should use the Snapshot reader");
  check(!reader.value()->read(kPartitionOffset - 256, 512).has_value(),
        "A read crossing the Snapshot boundary must fail closed");
  check(!reader.value()->read(4096, 512).has_value(),
        "An unclassified physical gap must not be readable");
}

void test_mbr_0x07_exfat_layout_uses_locked_volume_route() {
  MemoryReader source(make_source_mbr_exfat());
  const std::vector<ytec::clonecore::VolumeBitmapBinding> bindings{
      ytec::clonecore::VolumeBitmapBinding{
          .partition_entry_index = 0U,
          .volume_device_path =
              L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
      },
  };
  const auto layout = ytec::windowsapp::build_online_direct_source_layout(
      exfat_source_disk(), source, bindings);
  check(layout.has_value(), "MBR 0x07 exFAT should form a locked route");
  check(layout.value().snapshot_partitions.empty() &&
            layout.value().locked_partitions.size() == 1U &&
            layout.value().locked_partitions.front().disk_offset ==
                kPartitionOffset &&
            layout.value().locked_partitions.front().length ==
                kPartitionLength &&
            layout.value().locked_partitions.front().file_system ==
                ytec::windowsapp::OnlineDirectLockedFileSystem::exfat,
        "MBR 0x07 exFAT must not be mislabeled as NTFS/VSS");
}

void test_gpt_basic_fat32_and_exfat_layouts_use_locked_routes() {
  const auto run = [](const bool exfat) {
    MemoryReader source(make_source_gpt_basic(exfat));
    const std::vector<ytec::clonecore::VolumeBitmapBinding> bindings{
        ytec::clonecore::VolumeBitmapBinding{
            .partition_entry_index = 0U,
            .volume_device_path =
                L"\\\\?\\Volume{22222222-2222-2222-2222-222222222222}\\",
        },
    };
    const auto layout = ytec::windowsapp::build_online_direct_source_layout(
        gpt_basic_source_disk(exfat), source, bindings);
    check(layout.has_value(),
          exfat ? "GPT basic exFAT should form a locked route"
                : "GPT basic FAT32 should form a locked route");
    check(layout.value().partition_style ==
              ytec::windowsapp::OnlineDirectClonePartitionStyle::gpt &&
              layout.value().snapshot_partitions.empty() &&
              layout.value().locked_partitions.size() == 1U &&
              layout.value().locked_partitions.front().file_system ==
                  (exfat
                       ? ytec::windowsapp::OnlineDirectLockedFileSystem::exfat
                       : ytec::windowsapp::OnlineDirectLockedFileSystem::fat32),
          "GPT FAT32/exFAT must not be mislabeled as NTFS/VSS");
  };
  run(false);
  run(true);
}

void test_online_fat32_and_exfat_require_exact_locked_volume_extent() {
  const auto run = [](const bool exfat) {
    auto bytes = make_source_gpt_basic(exfat);
    const std::size_t boot = static_cast<std::size_t>(kPartitionOffset);
    if (exfat) {
      write_little<std::uint64_t>(
          bytes, boot + 72U, kPartitionSectors - 1U);
      constexpr std::uint32_t heap_offset = 56U;
      constexpr std::uint32_t sectors_per_cluster = 8U;
      write_little<std::uint32_t>(
          bytes,
          boot + 92U,
          (kPartitionSectors - 1U - heap_offset) /
              sectors_per_cluster);
    } else {
      write_little<std::uint32_t>(
          bytes, boot + 32U, kPartitionSectors - 1U);
    }
    MemoryReader source(std::move(bytes));
    const std::vector<ytec::clonecore::VolumeBitmapBinding> bindings{
        ytec::clonecore::VolumeBitmapBinding{
            .partition_entry_index = 0U,
            .volume_device_path =
                L"\\\\?\\Volume{33333333-3333-3333-3333-333333333333}\\",
        },
    };
    const auto layout = ytec::windowsapp::build_online_direct_source_layout(
        gpt_basic_source_disk(exfat), source, bindings);
    check(!layout.has_value(),
          exfat
              ? "Online exFAT must reject partition slack outside the locked declared volume"
              : "Online FAT32 must reject partition slack outside the locked declared volume");
  };
  run(false);
  run(true);
}

void test_exfat_data_disk_clones_without_vss_under_retained_lock() {
  DependencyState state;
  state.source_is_exfat = true;
  state.source_is_system = false;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_exfat_request(), dependencies(state));
  check(result.has_value(),
        "A non-system exFAT disk should clone under an exclusive lock");
  check(!result.value().used_vss_snapshot &&
            !result.value().snapshot_backup_completed &&
            !result.value().snapshots_deleted &&
            result.value().locked_volume_count == 1U &&
            result.value().source_consistency_verified &&
            result.value().target_left_offline &&
            !result.value().boot_finalization_required &&
            state.vss_calls == 0 && state.locked_open_calls == 1 &&
            state.engine_calls == 1 && !state.lock_alive,
        "exFAT success must prove lock consistency, avoid VSS, and release the lock after copy");
  const auto locked = std::find(
      state.events.begin(), state.events.end(), "lock-volume");
  const auto offline = std::find(
      state.events.begin(), state.events.end(), "offline-before-write");
  check(locked != state.events.end() && offline != state.events.end() &&
            locked < offline,
        "exFAT lock acquisition must precede every target mutation");
}

void test_fat32_data_disk_clones_without_vss_under_retained_lock() {
  DependencyState state;
  state.source_is_fat32 = true;
  state.source_is_system = false;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_fat32_request(), dependencies(state));
  check(result.has_value(),
        "A non-system FAT32 disk should clone under an exclusive lock");
  check(!result.value().used_vss_snapshot &&
            !result.value().snapshot_backup_completed &&
            !result.value().snapshots_deleted &&
            result.value().locked_volume_count == 1U &&
            result.value().source_consistency_verified &&
            result.value().target_left_offline &&
            !result.value().boot_finalization_required &&
            state.vss_calls == 0 && state.locked_open_calls == 1 &&
            state.engine_calls == 1 && !state.lock_alive,
        "FAT32 success must prove lock consistency, avoid VSS, and release the lock after copy");
  const auto locked = std::find(
      state.events.begin(), state.events.end(), "lock-volume");
  const auto offline = std::find(
      state.events.begin(), state.events.end(), "offline-before-write");
  check(locked != state.events.end() && offline != state.events.end() &&
            locked < offline,
        "FAT32 lock acquisition must precede every target mutation");
}

void test_exfat_lock_failure_stops_before_target_mutation() {
  DependencyState state;
  state.source_is_exfat = true;
  state.source_is_system = false;
  state.fail_locked_volume_open = true;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_exfat_request(), dependencies(state));
  check(!result.has_value(), "An unavailable exFAT lock must fail closed");
  check(state.vss_calls == 0 && state.locked_open_calls == 1 &&
            state.offline_calls == 0 && state.engine_calls == 0 &&
            !state.lock_alive,
        "A failed exFAT lock must precede target offline and writes");
}

void test_existing_formatted_target_clones_and_stays_offline() {
  DependencyState state;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(state));
  check(result.has_value(),
        "An existing GPT/NTFS target should be accepted after OK");
  check(result.value().target_left_offline &&
            result.value().snapshot_backup_completed &&
            result.value().snapshots_deleted &&
            result.value().read_back_verified &&
            result.value().boot_finalization_required &&
            result.value().boot_finalization_completed,
        "Success must prove VSS cleanup and fresh boot finalization");
  check(state.reidentify_calls == 2 && state.offline_calls == 3 &&
            state.engine_calls == 1 && state.boot_finalization_calls == 1,
        "The operation must reidentify before the final offline/write phase");
  const auto final_reidentify = std::find(
      state.events.begin(), state.events.end(), "reidentify");
  check(final_reidentify != state.events.end(),
        "Reidentification events should be observable");
  const auto offline = std::find(
      state.events.begin(), state.events.end(), "offline-before-write");
  const auto engine =
      std::find(state.events.begin(), state.events.end(), "engine");
  const auto online = std::find(
      state.events.begin(), state.events.end(), "online-for-boot");
  const auto boot = std::find(
      state.events.begin(), state.events.end(), "boot-finalize");
  const auto final_offline = std::find(
      state.events.begin(), state.events.end(), "offline-verify");
  const auto cleanup =
      std::find(state.events.begin(), state.events.end(), "vss-cleanup");
  check(offline < engine && engine < online && online < boot &&
            boot < final_offline && final_offline < cleanup,
        "Writes, boot repair, final offline, and Snapshot cleanup must be ordered");
}

void test_boot_failure_is_reported_and_target_is_reofflined() {
  DependencyState state;
  state.fail_boot_finalization = true;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(state));
  check(!result.has_value(), "A boot finalization failure must fail the clone");
  check(state.engine_calls == 1 && state.boot_finalization_calls == 1 &&
            state.offline_calls >= 3 &&
            state.events.back() == "offline-verify",
        "A failed fresh BCD rebuild must still leave the target offline");
}

void test_boot_exception_reprotects_target_offline() {
  DependencyState state;
  state.throw_boot_finalization = true;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(state));
  check(!result.has_value(), "A boot finalizer exception must fail safely");
  check(
      !state.target_online && state.boot_finalization_calls == 1 &&
          state.events.back() == "offline-verify",
      "An exception in the temporary-online interval must re-protect target offline");
}

void test_unconfirmed_offline_state_is_reported_without_success_claim() {
  DependencyState state;
  state.fail_offline_reprotection = true;
  state.throw_boot_finalization = true;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(state));
  check(!result.has_value(), "Unconfirmed offline state must fail the clone");
  check(
      state.target_online &&
          result.error().message.find(
              L"offline状態の再確認にも失敗") != std::wstring::npos,
      "Failure must explicitly say that offline state could not be confirmed");
}

void test_ntfs_data_disk_skips_boot_repair_and_stays_offline() {
  DependencyState state;
  state.source_is_system = false;
  auto request = valid_request();
  request.expected_source.is_system_disk = false;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      request, dependencies(state));
  check(result.has_value(), "A basic NTFS data disk should clone through VSS");
  check(result.value().target_left_offline &&
            !result.value().boot_finalization_required &&
            !result.value().boot_finalization_completed &&
            state.boot_finalization_calls == 0 &&
            state.offline_calls == 2,
        "A data disk must not run BCDBoot and must remain offline");
}

void test_final_identity_change_stops_before_target_offline() {
  DependencyState state;
  state.fail_final_reidentify = true;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(state));
  check(!result.has_value(), "A final identity change must fail");
  check(state.reidentify_calls == 2 && state.offline_calls == 0 &&
            state.engine_calls == 0,
        "Identity mismatch must precede all target state changes and writes");
}

void test_reviewed_layout_drift_stops_before_engine() {
  DependencyState before_offline;
  before_offline.drift_final_target_layout = true;
  const auto changed_before = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(before_offline));
  check(
      !changed_before.has_value() && before_offline.offline_calls == 0 &&
          before_offline.engine_calls == 0,
      "target layout drift after review must stop before offline transition");

  DependencyState opened;
  opened.drift_opened_target_layout = true;
  const auto changed_handle = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(opened));
  check(
      !changed_handle.has_value() && opened.offline_calls >= 2 &&
          opened.engine_calls == 0,
      "layout drift on the exact opened target must keep it offline");
}

void test_vss_failure_does_not_touch_target() {
  DependencyState state;
  state.fail_workflow_before_callback = true;
  const auto result = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(state));
  check(!result.has_value(), "VSS Writer failure must fail closed");
  check(state.reidentify_calls == 1 && state.offline_calls == 0 &&
            state.engine_calls == 0,
        "A VSS failure before Snapshot copy must not touch the target");
}

void test_unmapped_or_unknown_source_layout_is_rejected() {
  MemoryReader source(make_source_mbr());
  auto disk = source_disk();
  const std::vector<ytec::clonecore::VolumeBitmapBinding> extra_bindings{
      ytec::clonecore::VolumeBitmapBinding{
          .partition_entry_index = 0,
          .volume_device_path =
              L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
      },
      ytec::clonecore::VolumeBitmapBinding{
          .partition_entry_index = 1,
          .volume_device_path =
              L"\\\\?\\Volume{22222222-2222-2222-2222-222222222222}\\",
      },
  };
  const auto extra = ytec::windowsapp::build_online_direct_source_layout(
      disk,
      source,
      extra_bindings);
  check(!extra.has_value(),
        "A Volume not present in the parsed layout must be rejected");

  auto unsupported_bytes = make_source_mbr();
  unsupported_bytes[446 + 4] = std::byte{0x83};
  MemoryReader unsupported(std::move(unsupported_bytes));
  const std::vector<ytec::clonecore::VolumeBitmapBinding> one_binding{
      ytec::clonecore::VolumeBitmapBinding{
          .partition_entry_index = 0,
          .volume_device_path =
              L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
      },
  };
  const auto layout =
      ytec::windowsapp::build_online_direct_source_layout(
          disk,
          unsupported,
          one_binding);
  check(!layout.has_value() &&
            layout.error().code ==
                ytec::clonecore::ErrorCode::unsupported_layout,
        "An unknown mutable filesystem must fail closed");
}

void test_operation_plan_lifecycle_completes_with_verified_evidence() {
  DependencyState state;
  const auto result =
      ytec::windowsapp::execute_online_direct_clone_operation(
          valid_operation_request(), dependencies(state));
  check(result.has_value(), "A reviewed clone operation should run");
  check(
      result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::completed &&
          result.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::completed &&
          result.value().clone.has_value() &&
          result.value().clone->target_left_offline &&
          result.value().lifecycle.processed_work_bytes == 4096U &&
          result.value().lifecycle.verified_work_bytes == 4096U,
      "OperationPlan must finish only with verified clone evidence");
  check(
      state.reidentify_calls == 3 && state.engine_calls == 1,
      "OperationPlan and controller must each retain fresh identity checks");
}

void test_operation_plan_requires_exact_uppercase_ok_before_execution() {
  DependencyState state;
  auto request = valid_operation_request();
  request.clone.confirmation.typed_token = L"ok";
  const auto result =
      ytec::windowsapp::execute_online_direct_clone_operation(
          request, dependencies(state));
  check(result.has_value(), "A lifecycle failure remains a report");
  check(
      result.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          result.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::awaiting_confirmation &&
          !result.value().clone.has_value() && state.reidentify_calls == 1 &&
          state.offline_calls == 0 && state.engine_calls == 0,
      "Lowercase confirmation must stop before VSS, offline, or writes");
}

void test_operation_plan_rejects_review_layout_drift() {
  DependencyState state;
  auto request = valid_operation_request();
  request.reviewed_target.partitions[0].size_bytes += kSectorSize;
  const auto result =
      ytec::windowsapp::execute_online_direct_clone_operation(
          request, dependencies(state));
  check(!result.has_value(), "Changed review data must not form a plan");
  check(
      state.reidentify_calls == 0 && state.offline_calls == 0 &&
          state.engine_calls == 0,
      "Plan mismatch must stop before device observation or mutation");
}

void test_abnormal_target_health_stops_before_vss_or_write() {
  DependencyState reviewed_state;
  auto reviewed_request = valid_operation_request();
  reviewed_request.reviewed_target.health.state =
      ytec::diskmodel::DiskHealthState::caution;
  const auto reviewed =
      ytec::windowsapp::execute_online_direct_clone_operation(
          reviewed_request, dependencies(reviewed_state));
  check(!reviewed.has_value(),
        "A reviewed target with abnormal health must not form a plan");
  check(reviewed_state.reidentify_calls == 0 &&
            reviewed_state.offline_calls == 0 &&
            reviewed_state.engine_calls == 0,
        "Reviewed target health must stop before device activity");

  DependencyState observed_state;
  observed_state.target_health_failing = true;
  const auto observed = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(observed_state));
  check(!observed.has_value(),
        "A freshly observed failing target must be rejected");
  check(observed_state.reidentify_calls == 1 &&
            observed_state.offline_calls == 0 &&
            observed_state.engine_calls == 0,
        "Fresh health rejection must precede VSS, offline, and writes");

  DependencyState final_state;
  final_state.target_health_failing_on_final_reidentify = true;
  const auto final = ytec::windowsapp::execute_online_direct_clone(
      valid_request(), dependencies(final_state));
  check(!final.has_value(),
        "A target that becomes unhealthy immediately before write must be rejected");
  check(final_state.reidentify_calls == 2 &&
            final_state.offline_calls == 0 &&
            final_state.engine_calls == 0,
        "Final health drift must stop before target offline or writes");
}

void test_operation_plan_rejects_incomplete_engine_evidence() {
  DependencyState missing_partition;
  missing_partition.incomplete_engine_report = true;
  const auto incomplete =
      ytec::windowsapp::execute_online_direct_clone_operation(
          valid_operation_request(), dependencies(missing_partition));
  check(
      incomplete.has_value() &&
          incomplete.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          incomplete.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::executing,
      "A partial partition report must not complete the lifecycle");

  DependencyState zero_bytes;
  zero_bytes.zero_engine_bytes = true;
  const auto empty =
      ytec::windowsapp::execute_online_direct_clone_operation(
          valid_operation_request(), dependencies(zero_bytes));
  check(
      empty.has_value() &&
          empty.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          empty.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::executing,
      "A zero-byte clone report must not complete the lifecycle");

  DependencyState missing_digest;
  missing_digest.zero_write_digest = true;
  const auto no_digest =
      ytec::windowsapp::execute_online_direct_clone_operation(
          valid_operation_request(), dependencies(missing_digest));
  check(
      no_digest.has_value() &&
          no_digest.value().lifecycle.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          no_digest.value().lifecycle.phase ==
              ytec::operationcore::OperationPhase::executing,
      "A clone without a verified-write digest must not complete");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"composite_reader_routes_snapshot_and_rejects_crossing",
       test_composite_reader_routes_snapshot_and_rejects_crossing},
      {"mbr_0x07_exfat_layout_uses_locked_volume_route",
       test_mbr_0x07_exfat_layout_uses_locked_volume_route},
      {"gpt_basic_fat32_and_exfat_layouts_use_locked_routes",
       test_gpt_basic_fat32_and_exfat_layouts_use_locked_routes},
      {"online_fat32_and_exfat_require_exact_locked_volume_extent",
       test_online_fat32_and_exfat_require_exact_locked_volume_extent},
      {"exfat_data_disk_clones_without_vss_under_retained_lock",
       test_exfat_data_disk_clones_without_vss_under_retained_lock},
      {"fat32_data_disk_clones_without_vss_under_retained_lock",
       test_fat32_data_disk_clones_without_vss_under_retained_lock},
      {"exfat_lock_failure_stops_before_target_mutation",
       test_exfat_lock_failure_stops_before_target_mutation},
      {"existing_formatted_target_clones_and_stays_offline",
       test_existing_formatted_target_clones_and_stays_offline},
      {"final_identity_change_stops_before_target_offline",
       test_final_identity_change_stops_before_target_offline},
      {"reviewed_layout_drift_stops_before_engine",
       test_reviewed_layout_drift_stops_before_engine},
      {"boot_failure_is_reported_and_target_is_reofflined",
       test_boot_failure_is_reported_and_target_is_reofflined},
      {"boot_exception_reprotects_target_offline",
       test_boot_exception_reprotects_target_offline},
      {"unconfirmed_offline_state_is_reported_without_success_claim",
       test_unconfirmed_offline_state_is_reported_without_success_claim},
      {"ntfs_data_disk_skips_boot_repair_and_stays_offline",
       test_ntfs_data_disk_skips_boot_repair_and_stays_offline},
      {"vss_failure_does_not_touch_target",
       test_vss_failure_does_not_touch_target},
      {"unmapped_or_unknown_source_layout_is_rejected",
       test_unmapped_or_unknown_source_layout_is_rejected},
      {"operation_plan_lifecycle_completes_with_verified_evidence",
       test_operation_plan_lifecycle_completes_with_verified_evidence},
      {"operation_plan_requires_exact_uppercase_ok_before_execution",
       test_operation_plan_requires_exact_uppercase_ok_before_execution},
      {"operation_plan_rejects_review_layout_drift",
       test_operation_plan_rejects_review_layout_drift},
      {"abnormal_target_health_stops_before_vss_or_write",
       test_abnormal_target_health_stops_before_vss_or_write},
      {"operation_plan_rejects_incomplete_engine_evidence",
       test_operation_plan_rejects_incomplete_engine_evidence},
  };
  std::size_t passed = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      ++passed;
      std::cout << "[PASS] " << name << '\n';
    } catch (const TestFailure& failure) {
      std::cerr << "[FAIL] " << name << ": "
                << failure.message << '\n';
      return 1;
    } catch (const std::exception& exception) {
      std::cerr << "[FAIL] " << name << ": "
                << exception.what() << '\n';
      return 1;
    }
  }
  std::cout << passed << " tests passed\n";
  return 0;
}

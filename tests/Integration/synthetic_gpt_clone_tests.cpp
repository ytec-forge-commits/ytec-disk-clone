#include "ytec/clonecore/crc32.h"
#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/clonecore/windows_volume_bitmap.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::clonecore::ByteRange;
using ytec::clonecore::Error;
using ytec::clonecore::ErrorCode;
using ytec::clonecore::GptDisk;
using ytec::clonecore::GptGuid;
using ytec::clonecore::GptPartition;
using ytec::clonecore::Result;
using ytec::clonecore::Status;

constexpr std::uint32_t kSectorSize = 512;
constexpr std::uint64_t kSourceSize = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetSize = 20ULL * 1024ULL * 1024ULL;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

Error test_error(const std::wstring& operation) {
  return Error{
      .code = ErrorCode::io_failed,
      .native_code = ERROR_INVALID_DATA,
      .operation = operation,
      .message = L"合成ディスクI/Oエラー",
  };
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

class MemorySourceReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  MemorySourceReader(
      const std::vector<std::byte>& storage,
      const std::uint32_t sector_size)
      : storage_(storage), sector_size_(sector_size) {}

  std::uint64_t size_bytes() const noexcept override { return storage_.size(); }
  std::uint32_t logical_sector_size() const noexcept override {
    return sector_size_;
  }

  Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > storage_.size() || length > storage_.size() - offset) {
      return Result<std::vector<std::byte>>::failure(
          test_error(L"合成コピー元読取り"));
    }
    const auto begin = storage_.begin() + static_cast<std::ptrdiff_t>(offset);
    return Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(begin, begin + static_cast<std::ptrdiff_t>(length)));
  }

 private:
  const std::vector<std::byte>& storage_;
  std::uint32_t sector_size_{};
};

class MemoryTargetWriter final : public ytec::clonecore::ITargetDiskWriter {
 public:
  MemoryTargetWriter(
      std::vector<std::byte>& storage,
      const std::uint32_t sector_size)
      : storage_(storage), sector_size_(sector_size) {}

  std::uint64_t size_bytes() const noexcept override { return storage_.size(); }
  std::uint32_t logical_sector_size() const noexcept override {
    return sector_size_;
  }

  Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (offset > storage_.size() || bytes.size() > storage_.size() - offset) {
      return Status::failure(test_error(L"合成コピー先書込み"));
    }
    std::copy(
        bytes.begin(),
        bytes.end(),
        storage_.begin() + static_cast<std::ptrdiff_t>(offset));
    ++write_count;
    last_write_offset = offset;
    return ytec::clonecore::success_status();
  }

  Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > storage_.size() || length > storage_.size() - offset) {
      return Result<std::vector<std::byte>>::failure(
          test_error(L"合成コピー先読戻し"));
    }
    const auto begin = storage_.begin() + static_cast<std::ptrdiff_t>(offset);
    std::vector<std::byte> result(
        begin, begin + static_cast<std::ptrdiff_t>(length));
    if (corrupt_read_back_at != 0 && offset >= corrupt_read_back_at &&
        !result.empty()) {
      result.front() ^= std::byte{0x01};
    }
    return Result<std::vector<std::byte>>::success(std::move(result));
  }

  Status flush_target() override {
    ++flush_count;
    return ytec::clonecore::success_status();
  }

  std::uint64_t corrupt_read_back_at{};
  std::uint64_t last_write_offset{};
  std::uint32_t write_count{};
  std::uint32_t flush_count{};

 private:
  std::vector<std::byte>& storage_;
  std::uint32_t sector_size_{};
};

class SequentialGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  explicit SequentialGuidGenerator(const std::uint8_t seed) : next_(seed) {}

  Result<GptGuid> next_guid() override {
    GptGuid guid;
    guid.bytes[0] = std::byte{next_++};
    guid.bytes[15] = std::byte{0xA5};
    return Result<GptGuid>::success(guid);
  }

 private:
  std::uint8_t next_{};
};

class SyntheticUsedRanges final
    : public ytec::clonecore::INtfsUsedRangeProvider {
 public:
  Result<std::vector<ByteRange>> query_used_ranges(
      const std::uint32_t,
      const ytec::clonecore::NtfsGeometry&) override {
    ++query_count;
    return Result<std::vector<ByteRange>>::success(
        {{.offset = 0, .length = 8192},
         {.offset = 32768, .length = 8192}});
  }

  std::uint32_t query_count{};
};

GptGuid guid_with_byte(const std::uint8_t value) {
  GptGuid guid;
  guid.bytes[0] = std::byte{value};
  guid.bytes[15] = std::byte{0x5A};
  return guid;
}

GptPartition partition(
    const std::uint32_t index,
    const GptGuid& type,
    const std::uint64_t first_lba,
    const std::uint64_t last_lba,
    const char16_t* name) {
  return GptPartition{
      .entry_index = index,
      .type_guid = type,
      .unique_guid = guid_with_byte(static_cast<std::uint8_t>(index + 10)),
      .first_lba = first_lba,
      .last_lba = last_lba,
      .attributes = 0,
      .name = name,
  };
}

GptDisk source_layout(const bool unknown_partition = false) {
  GptDisk disk;
  disk.logical_sector_size = kSectorSize;
  disk.sector_count = kSourceSize / kSectorSize;
  disk.disk_guid = guid_with_byte(1);
  disk.first_usable_lba = 34;
  disk.last_usable_lba = disk.sector_count - 34;
  disk.partition_entry_count = 128;
  disk.partition_entry_size = 128;
  disk.partitions.push_back(partition(
      0, ytec::clonecore::gpt_type_efi_system(), 2048, 3071, u"EFI"));
  disk.partitions.push_back(partition(
      1,
      ytec::clonecore::gpt_type_microsoft_reserved(),
      3072,
      3327,
      u"MSR"));
  disk.partitions.push_back(partition(
      2,
      unknown_partition ? guid_with_byte(0xF0)
                        : ytec::clonecore::gpt_type_basic_data(),
      3328,
      8191,
      u"Windows"));
  disk.partitions.push_back(partition(
      3,
      ytec::clonecore::gpt_type_windows_recovery(),
      8192,
      9215,
      u"Recovery"));
  return disk;
}

ByteRange byte_range(const GptPartition& partition_value) {
  return ByteRange{
      .offset = partition_value.first_lba * kSectorSize,
      .length =
          (partition_value.last_lba - partition_value.first_lba + 1) *
          kSectorSize,
  };
}

void apply_metadata(
    std::vector<std::byte>& storage,
    const ytec::clonecore::GptWritePlan& plan) {
  for (const auto& write : plan.writes) {
    check(
        write.offset <= storage.size() &&
            write.bytes.size() <= storage.size() - write.offset,
        "Synthetic GPT metadata must fit");
    std::copy(
        write.bytes.begin(),
        write.bytes.end(),
        storage.begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
}

void write_ntfs_boot(
    std::vector<std::byte>& storage,
    const ByteRange& range) {
  const std::size_t offset = static_cast<std::size_t>(range.offset);
  const char signature[] = "NTFS    ";
  std::memcpy(storage.data() + offset + 3, signature, 8);
  write_little<std::uint16_t>(storage, offset + 11, kSectorSize);
  storage[offset + 13] = std::byte{8};
  write_little<std::uint64_t>(
      storage, offset + 40, range.length / kSectorSize);
  storage[offset + 510] = std::byte{0x55};
  storage[offset + 511] = std::byte{0xAA};
}

void write_fat32_boot(
    std::vector<std::byte>& storage,
    const ByteRange& range) {
  const std::size_t offset = static_cast<std::size_t>(range.offset);
  const char signature[] = "FAT32   ";
  std::memcpy(storage.data() + offset + 82, signature, 8);
  write_little<std::uint16_t>(storage, offset + 11, kSectorSize);
  storage[offset + 13] = std::byte{8};
  write_little<std::uint32_t>(
      storage,
      offset + 32,
      static_cast<std::uint32_t>(range.length / kSectorSize));
  storage[offset + 510] = std::byte{0x55};
  storage[offset + 511] = std::byte{0xAA};
}

void write_exfat_boot(
    std::vector<std::byte>& storage,
    const ByteRange& range) {
  const std::size_t offset = static_cast<std::size_t>(range.offset);
  std::fill_n(
      storage.begin() + static_cast<std::ptrdiff_t>(offset),
      kSectorSize,
      std::byte{0});
  const char signature[] = "EXFAT   ";
  std::memcpy(storage.data() + offset + 3, signature, 8);
  const std::uint64_t total_sectors = range.length / kSectorSize;
  constexpr std::uint32_t fat_offset = 24U;
  constexpr std::uint32_t fat_length = 8U;
  constexpr std::uint32_t heap_offset = fat_offset + fat_length;
  constexpr std::uint32_t sectors_per_cluster = 8U;
  const auto cluster_count = static_cast<std::uint32_t>(
      (total_sectors - heap_offset) / sectors_per_cluster);
  write_little<std::uint64_t>(
      storage, offset + 64, range.offset / kSectorSize);
  write_little<std::uint64_t>(storage, offset + 72, total_sectors);
  write_little<std::uint32_t>(storage, offset + 80, fat_offset);
  write_little<std::uint32_t>(storage, offset + 84, fat_length);
  write_little<std::uint32_t>(storage, offset + 88, heap_offset);
  write_little<std::uint32_t>(storage, offset + 92, cluster_count);
  write_little<std::uint32_t>(storage, offset + 96, 2U);
  write_little<std::uint16_t>(storage, offset + 104, 0x0100U);
  storage[offset + 108] = std::byte{9};
  storage[offset + 109] = std::byte{3};
  storage[offset + 110] = std::byte{1};
  storage[offset + 112] = std::byte{0};
  storage[offset + 510] = std::byte{0x55};
  storage[offset + 511] = std::byte{0xAA};
}

struct SyntheticFixture final {
  explicit SyntheticFixture(const bool unknown_partition = false)
      : source(kSourceSize, std::byte{0}),
        target(kTargetSize, std::byte{0xCC}),
        source_reader(source, kSectorSize),
        target_writer(target, kSectorSize) {
    SequentialGuidGenerator source_guids(20);
    const auto metadata = ytec::clonecore::make_gpt_write_plan(
        source_layout(unknown_partition),
        kSourceSize,
        kSectorSize,
        source_guids);
    check(metadata.has_value(), "Synthetic source GPT should be generated");
    layout = metadata.value().target_disk;
    apply_metadata(source, metadata.value());

    const ByteRange efi = byte_range(layout.partitions[0]);
    const ByteRange windows = byte_range(layout.partitions[2]);
    const ByteRange recovery = byte_range(layout.partitions[3]);
    std::fill_n(
        source.begin() + static_cast<std::ptrdiff_t>(efi.offset),
        static_cast<std::ptrdiff_t>(efi.length),
        std::byte{0x31});
    std::fill_n(
        source.begin() + static_cast<std::ptrdiff_t>(windows.offset),
        static_cast<std::ptrdiff_t>(windows.length),
        std::byte{0x42});
    std::fill_n(
        source.begin() + static_cast<std::ptrdiff_t>(recovery.offset),
        static_cast<std::ptrdiff_t>(recovery.length),
        std::byte{0x53});
    write_fat32_boot(source, efi);
    write_ntfs_boot(source, windows);
    write_ntfs_boot(source, recovery);
  }

  std::vector<std::byte> source;
  std::vector<std::byte> target;
  GptDisk layout;
  MemorySourceReader source_reader;
  MemoryTargetWriter target_writer;
};

ytec::clonecore::StableDiskIdentity identity(
    const std::uint32_t number,
    const std::wstring& model,
    const std::uint64_t size,
    const std::string& serial,
    const std::wstring& instance) {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = number,
      .model = model,
      .size_bytes = size,
      .logical_sector_size = kSectorSize,
      .serial_suffix = serial,
      .device_instance_id = instance,
      .is_system_disk = false,
  };
}

ytec::clonecore::OfflineGptCloneRequest valid_request() {
  ytec::clonecore::OfflineGptCloneRequest request;
  request.expected_source =
      identity(4, L"Synthetic Source", kSourceSize, "SRC00001", L"TEST\\SRC");
  request.observed_source = request.expected_source;
  request.expected_target =
      identity(8, L"Synthetic Target", kTargetSize, "DST00008", L"TEST\\DST");
  request.observed_target = request.expected_target;
  request.confirmation.first_step_acknowledged = true;
  request.confirmation.typed_token =
      ytec::clonecore::make_target_confirmation_token(request.observed_target);
  request.maximum_chunk_bytes = 64U * 1024U;
  return request;
}

bool equal_range(
    const std::vector<std::byte>& left,
    const std::vector<std::byte>& right,
    const ByteRange& range) {
  const auto left_begin =
      left.begin() + static_cast<std::ptrdiff_t>(range.offset);
  const auto right_begin =
      right.begin() + static_cast<std::ptrdiff_t>(range.offset);
  return std::equal(
      left_begin,
      left_begin + static_cast<std::ptrdiff_t>(range.length),
      right_begin,
      right_begin + static_cast<std::ptrdiff_t>(range.length));
}

bool digest_is_zero(const std::array<std::byte, 32>& digest) {
  return std::all_of(
      digest.begin(),
      digest.end(),
      [](const std::byte value) { return value == std::byte{0}; });
}

void test_crc32_known_vector() {
  const char text[] = "123456789";
  const auto bytes = std::as_bytes(std::span(text, sizeof(text) - 1));
  check(
      ytec::clonecore::crc32(bytes) == 0xCBF43926U,
      "CRC32 must match the standard test vector");
}

void test_volume_bitmap_chunk_decode_and_rounding() {
  const ytec::clonecore::NtfsGeometry non_aligned_geometry{
      .bytes_per_sector = 512,
      .sectors_per_cluster = 8,
      .total_sectors = 2'867'199,
  };
  check(
      non_aligned_geometry.complete_cluster_count() == 358'399,
      "NTFS cluster count must ignore an incomplete trailing allocation unit");

  const std::vector<std::byte> bitmap{
      std::byte{0b00001011}, std::byte{0b00000011}};
  const auto decoded = ytec::clonecore::decode_volume_bitmap_chunk(
      0, 16, bitmap, 0, 16, 4096);
  check(decoded.has_value(), "Synthetic volume bitmap should decode");
  check(decoded.value().next_lcn == 16, "Bitmap decoder must advance to LCN 16");
  check(decoded.value().used_ranges.size() == 3, "Used runs should be coalesced");
  check(
      decoded.value().used_ranges[0].offset == 0 &&
          decoded.value().used_ranges[0].length == 8192,
      "Adjacent used clusters must form one range");

  const std::vector<std::byte> rounded_bitmap{
      std::byte{0b00001101}, std::byte{0}};
  const auto rounded = ytec::clonecore::decode_volume_bitmap_chunk(
      8, 16, rounded_bitmap, 10, 24, 4096);
  check(rounded.has_value(), "Rounded-down starting LCN should be accepted");
  check(
      rounded.value().used_ranges.size() == 1 &&
          rounded.value().used_ranges[0].offset == 10ULL * 4096ULL &&
          rounded.value().used_ranges[0].length == 8192,
      "Bits before the requested LCN must be ignored");
}

void test_snapshot_bitmap_provider_rejects_live_volume_paths() {
  const ytec::clonecore::NtfsGeometry geometry{
      .bytes_per_sector = 512,
      .sectors_per_cluster = 8,
      .total_sectors = 1024,
  };
  ytec::clonecore::WindowsSnapshotVolumeBitmapProvider snapshot_provider({
      ytec::clonecore::SnapshotVolumeBitmapBinding{
          .partition_entry_index = 0,
          .snapshot_device_path =
              L"\\\\?\\Volume{00000000-0000-0000-0000-000000000001}\\",
      },
  });
  const auto snapshot_result =
      snapshot_provider.query_used_ranges(0, geometry);
  check(
      !snapshot_result.has_value(),
      "Snapshot provider must reject live Volume GUID paths");
  check(
      snapshot_result.error().code == ErrorCode::invalid_argument,
      "Wrong path kind must fail before CreateFile");

  ytec::clonecore::WindowsVolumeBitmapProvider live_provider({
      ytec::clonecore::VolumeBitmapBinding{
          .partition_entry_index = 0,
          .volume_device_path =
              L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy1",
      },
  });
  const auto live_result = live_provider.query_used_ranges(0, geometry);
  check(
      !live_result.has_value(),
      "Live provider must reject VSS Snapshot paths");
  check(
      live_result.error().code == ErrorCode::invalid_argument,
      "Snapshot/live path mixing must fail before CreateFile");
}

void test_snapshot_bitmap_provider_rejects_duplicate_bindings() {
  const ytec::clonecore::NtfsGeometry geometry{
      .bytes_per_sector = 512,
      .sectors_per_cluster = 8,
      .total_sectors = 1024,
  };
  ytec::clonecore::WindowsSnapshotVolumeBitmapProvider provider({
      ytec::clonecore::SnapshotVolumeBitmapBinding{
          .partition_entry_index = 3,
          .snapshot_device_path =
              L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy1",
      },
      ytec::clonecore::SnapshotVolumeBitmapBinding{
          .partition_entry_index = 3,
          .snapshot_device_path =
              L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy2",
      },
  });
  const auto result = provider.query_used_ranges(3, geometry);
  check(!result.has_value(), "Duplicate Snapshot bindings must fail closed");
  check(
      result.error().code == ErrorCode::invalid_argument,
      "Duplicate binding failure must be explicit");
}

void test_bitlocker_boot_sector_is_rejected_explicitly() {
  std::vector<std::byte> boot_sector(512, std::byte{0});
  const char signature[] = "-FVE-FS-";
  std::memcpy(boot_sector.data() + 3, signature, 8);
  const auto geometry =
      ytec::clonecore::parse_ntfs_geometry(boot_sector, 512, 1024 * 1024);
  check(!geometry.has_value(), "BitLocker volume must be rejected");
  check(
      geometry.error().operation.find(L"BitLocker") != std::wstring::npos,
      "BitLocker rejection should explain that full decryption is required");
}

void test_synthetic_gpt_clone_success() {
  SyntheticFixture fixture;
  SyntheticUsedRanges used_ranges;
  SequentialGuidGenerator target_guids(100);
  std::vector<ytec::clonecore::DiskOperationProgress> progress_events;
  std::vector<ytec::clonecore::DiskOperationSafeBoundary> safe_boundaries;
  bool cancellation_requested_after_commit_started = false;
  auto request = valid_request();
  request.callbacks.progress =
      [&](const ytec::clonecore::DiskOperationProgress& progress) {
        progress_events.push_back(progress);
        if (progress.stage ==
            ytec::clonecore::DiskOperationStage::
                committing_partition_table) {
          cancellation_requested_after_commit_started = true;
        }
      };
  request.callbacks.cancellation_requested =
      [&]() { return cancellation_requested_after_commit_started; };
  request.callbacks.safe_boundary =
      [&](const ytec::clonecore::DiskOperationSafeBoundary& boundary) {
        safe_boundaries.push_back(boundary);
        return ytec::clonecore::DiskOperationControlDecision::
            continue_operation;
      };
  const auto result = ytec::clonecore::execute_offline_gpt_clone(
      request,
      fixture.source_reader,
      fixture.target_writer,
      used_ranges,
      target_guids);
  check(result.has_value(), "Synthetic GPT clone should succeed");
  check(result.value().read_back_verified, "All writes must be read back");
  check(result.value().primary_gpt_committed, "Primary GPT must be committed");
  check(
      !digest_is_zero(result.value().verified_write_digest),
      "Successful GPT clone must expose a non-zero verified-write digest");
  check(result.value().copied_partition_count == 3, "Three partitions copy data");
  check(result.value().recreated_partition_count == 1, "MSR is recreated only");
  check(
      fixture.target_writer.last_write_offset == kSectorSize,
      "Primary GPT header must be the final write");
  check(!progress_events.empty(), "Clone progress should be observable");
  check(
      !safe_boundaries.empty() &&
          safe_boundaries.back().kind ==
              ytec::clonecore::DiskOperationSafeBoundaryKind::
                  verified_chunk &&
          safe_boundaries.back().completed_bytes ==
              result.value().copied_data_bytes,
      "GPT clone must expose every read-back-verified chunk boundary");
  for (std::size_t index = 1; index < progress_events.size(); ++index) {
    check(
        progress_events[index].read_bytes >=
                progress_events[index - 1].read_bytes &&
            progress_events[index].written_bytes >=
                progress_events[index - 1].written_bytes &&
            progress_events[index].verified_bytes >=
                progress_events[index - 1].verified_bytes,
        "Clone progress counters must be monotonic");
  }
  check(
      std::all_of(
          progress_events.begin(),
          progress_events.end(),
          [](const ytec::clonecore::DiskOperationProgress& progress) {
            return !progress.pause_allowed ||
                progress.stage ==
                ytec::clonecore::DiskOperationStage::copying_data;
          }),
      "GPT metadata invalidation and commit must never advertise pause");
  const auto& final_progress = progress_events.back();
  const std::string final_progress_diagnostic =
      "Completed GPT progress should expose exact verified totals; "
      "read=" +
      std::to_string(final_progress.read_bytes) +
      ", written=" + std::to_string(final_progress.written_bytes) +
      ", verified=" + std::to_string(final_progress.verified_bytes) +
      ", totalRead=" + std::to_string(final_progress.total_read_bytes) +
      ", expected=" + std::to_string(result.value().copied_data_bytes) +
      ", stage=" +
      std::to_string(static_cast<int>(final_progress.stage)) +
      ", cancellationAllowed=" +
      (final_progress.cancellation_allowed ? "true" : "false");
  check(
      final_progress.stage ==
              ytec::clonecore::DiskOperationStage::completed &&
          final_progress.read_bytes == result.value().copied_data_bytes &&
          final_progress.written_bytes ==
              result.value().copied_data_bytes &&
          final_progress.verified_bytes ==
              result.value().copied_data_bytes &&
          final_progress.total_read_bytes ==
              result.value().copied_data_bytes &&
          !final_progress.cancellation_allowed,
      final_progress_diagnostic);
  check(
      cancellation_requested_after_commit_started,
      "The test should request cancellation after the non-cancellable commit boundary");
  check(
      fixture.target[512U * 1024U] == std::byte{0} &&
          fixture.target[static_cast<std::size_t>(
              kTargetSize - 512U * 1024U)] == std::byte{0},
      "Existing partition metadata across both target ends should be cleared");

  MemorySourceReader target_reader(fixture.target, kSectorSize);
  const auto target_gpt = ytec::clonecore::parse_gpt(target_reader);
  check(target_gpt.has_value(), "Cloned target GPT should parse");
  check(
      target_gpt.value().disk_guid != fixture.layout.disk_guid,
      "Target disk GUID must be regenerated");
  check(
      target_gpt.value().sector_count == kTargetSize / kSectorSize,
      "Backup GPT must move to the larger target end");
  for (std::size_t index = 0; index < fixture.layout.partitions.size(); ++index) {
    check(
        target_gpt.value().partitions[index].unique_guid !=
            fixture.layout.partitions[index].unique_guid,
        "Every target partition GUID must be regenerated");
  }

  check(
      equal_range(
          fixture.source, fixture.target, byte_range(fixture.layout.partitions[0])),
      "EFI FAT32 partition must be copied raw");
  check(
      equal_range(
          fixture.source, fixture.target, byte_range(fixture.layout.partitions[3])),
      "Recovery NTFS partition must be copied raw");
  const ByteRange windows = byte_range(fixture.layout.partitions[2]);
  check(
      std::equal(
          fixture.source.begin() + static_cast<std::ptrdiff_t>(windows.offset),
          fixture.source.begin() +
              static_cast<std::ptrdiff_t>(windows.offset + 8192),
          fixture.target.begin() + static_cast<std::ptrdiff_t>(windows.offset)),
      "First NTFS used range must be copied");
  check(
      fixture.target[static_cast<std::size_t>(windows.offset + 16384)] ==
          std::byte{0xCC},
      "Unused NTFS clusters must not be copied");
  const ByteRange msr = byte_range(fixture.layout.partitions[1]);
  check(
      fixture.target[static_cast<std::size_t>(msr.offset)] == std::byte{0xCC},
      "MSR payload must not be copied");
}

void test_gpt_basic_fat32_and_exfat_copy_whole_partition() {
  const auto run = [](const bool exfat) {
    SyntheticFixture fixture;
    const ByteRange basic = byte_range(fixture.layout.partitions[2]);
    std::fill_n(
        fixture.source.begin() +
            static_cast<std::ptrdiff_t>(basic.offset),
        kSectorSize,
        std::byte{0});
    if (exfat) {
      write_exfat_boot(fixture.source, basic);
    } else {
      write_fat32_boot(fixture.source, basic);
    }

    SyntheticUsedRanges plan_ranges;
    SequentialGuidGenerator plan_guids(100);
    const auto plan = ytec::clonecore::build_offline_gpt_clone_plan(
        fixture.source_reader,
        fixture.target_writer,
        plan_ranges,
        plan_guids);
    check(plan.has_value(),
          exfat ? "GPT basic exFAT should plan"
                : "GPT basic FAT32 should plan");
    check(plan_ranges.query_count == 0U,
          "FAT32/exFAT exact planning must not infer NTFS bitmap ranges");
    const auto copy = std::find_if(
        plan.value().partition_copies.begin(),
        plan.value().partition_copies.end(),
        [](const auto& candidate) { return candidate.entry_index == 2U; });
    check(copy != plan.value().partition_copies.end() &&
              copy->source_ranges.size() == 1U &&
              copy->source_ranges.front().offset == basic.offset &&
              copy->source_ranges.front().length == basic.length &&
              copy->mode ==
                  (exfat
                       ? ytec::clonecore::PartitionCopyMode::basic_exfat_raw
                       : ytec::clonecore::PartitionCopyMode::basic_fat32_raw),
          "FAT32/exFAT exact planning must cover the complete partition");

    SyntheticUsedRanges execute_ranges;
    SequentialGuidGenerator execute_guids(120);
    const auto result = ytec::clonecore::execute_offline_gpt_clone(
        valid_request(),
        fixture.source_reader,
        fixture.target_writer,
        execute_ranges,
        execute_guids);
    check(result.has_value(),
          exfat ? "GPT basic exFAT exact clone should succeed"
                : "GPT basic FAT32 exact clone should succeed");
    check(execute_ranges.query_count == 0U,
          "FAT32/exFAT execution must not request an NTFS bitmap");
    check(equal_range(fixture.source, fixture.target, basic),
          "FAT32/exFAT exact clone must preserve every partition byte");
  };

  run(false);
  run(true);
}

void test_malformed_exfat_is_rejected_before_target_write() {
  SyntheticFixture fixture;
  const ByteRange basic = byte_range(fixture.layout.partitions[2]);
  write_exfat_boot(fixture.source, basic);
  fixture.source[static_cast<std::size_t>(basic.offset + 11U)] =
      std::byte{0x01};
  SyntheticUsedRanges used_ranges;
  SequentialGuidGenerator target_guids(100);
  const auto result = ytec::clonecore::execute_offline_gpt_clone(
      valid_request(),
      fixture.source_reader,
      fixture.target_writer,
      used_ranges,
      target_guids);
  check(!result.has_value(), "Malformed exFAT must fail closed");
  check(fixture.target_writer.write_count == 0U,
        "Malformed exFAT must fail before target metadata invalidation");
  check(used_ranges.query_count == 0U,
        "Malformed exFAT must not fall back to an NTFS bitmap route");
}

void test_verified_write_digest_changes_with_copied_data() {
  SyntheticFixture original;
  SyntheticFixture changed;
  const ByteRange windows = byte_range(changed.layout.partitions[2]);
  changed.source[static_cast<std::size_t>(windows.offset + 32768U)] ^=
      std::byte{0x01};

  SyntheticUsedRanges original_ranges;
  SyntheticUsedRanges changed_ranges;
  SequentialGuidGenerator original_guids(100);
  SequentialGuidGenerator changed_guids(100);
  const auto original_result = ytec::clonecore::execute_offline_gpt_clone(
      valid_request(),
      original.source_reader,
      original.target_writer,
      original_ranges,
      original_guids);
  const auto changed_result = ytec::clonecore::execute_offline_gpt_clone(
      valid_request(),
      changed.source_reader,
      changed.target_writer,
      changed_ranges,
      changed_guids);

  check(
      original_result.has_value() && changed_result.has_value(),
      "Both synthetic GPT clones must complete before comparing evidence");
  check(
      original_result.value().verified_write_digest !=
          changed_result.value().verified_write_digest,
      "Changing copied GPT payload bytes must change the verified-write digest");
}

void test_cancellation_leaves_primary_gpt_invalid() {
  SyntheticFixture fixture;
  SyntheticUsedRanges used_ranges;
  SequentialGuidGenerator target_guids(100);
  bool cancellation_requested = false;
  bool commit_started = false;
  std::uint64_t safe_boundary_count = 0U;
  auto request = valid_request();
  request.callbacks.progress =
      [&](const ytec::clonecore::DiskOperationProgress& progress) {
        if (progress.stage ==
            ytec::clonecore::DiskOperationStage::
                committing_partition_table) {
          commit_started = true;
        }
      };
  request.callbacks.cancellation_requested =
      [&]() { return cancellation_requested; };
  request.callbacks.safe_boundary =
      [&](const ytec::clonecore::DiskOperationSafeBoundary&) {
        ++safe_boundary_count;
        cancellation_requested = true;
        return ytec::clonecore::DiskOperationControlDecision::
            cancel_operation;
      };

  const auto result = ytec::clonecore::execute_offline_gpt_clone(
      request,
      fixture.source_reader,
      fixture.target_writer,
      used_ranges,
      target_guids);
  check(!result.has_value(), "A requested GPT clone cancellation must stop");
  check(
      result.error().code == ErrorCode::cancelled,
      "GPT cancellation should use the dedicated cancelled error");
  check(!commit_started, "Cancellation must precede the GPT commit boundary");
  check(
      safe_boundary_count == 1U,
      "GPT cancellation must be honoured at the first verified chunk boundary");
  check(
      std::all_of(
          fixture.target.begin() + kSectorSize,
          fixture.target.begin() + kSectorSize * 2,
          [](const std::byte value) { return value == std::byte{0}; }),
      "Cancelled clone must leave the primary GPT invalid");
}

void test_confirmation_failure_writes_nothing() {
  SyntheticFixture fixture;
  SyntheticUsedRanges used_ranges;
  SequentialGuidGenerator target_guids(100);
  auto request = valid_request();
  request.confirmation.typed_token = L"ERASE WRONG DISK";
  const auto result = ytec::clonecore::execute_offline_gpt_clone(
      request,
      fixture.source_reader,
      fixture.target_writer,
      used_ranges,
      target_guids);
  check(!result.has_value(), "Wrong confirmation must fail");
  check(
      result.error().code == ErrorCode::confirmation_required,
      "Wrong confirmation should have a dedicated error");
  check(fixture.target_writer.write_count == 0, "No write is allowed before confirmation");
}

void test_identity_drift_writes_nothing() {
  SyntheticFixture fixture;
  SyntheticUsedRanges used_ranges;
  SequentialGuidGenerator target_guids(100);
  auto request = valid_request();
  request.observed_target.serial_suffix = "CHANGED8";
  const auto result = ytec::clonecore::execute_offline_gpt_clone(
      request,
      fixture.source_reader,
      fixture.target_writer,
      used_ranges,
      target_guids);
  check(!result.has_value(), "Identity drift must fail");
  check(
      result.error().code == ErrorCode::identity_mismatch,
      "Identity drift should be explicit");
  check(fixture.target_writer.write_count == 0, "Identity drift must fail before writes");
}

void test_corrupt_source_gpt_writes_nothing() {
  SyntheticFixture fixture;
  fixture.source[kSectorSize + 40] ^= std::byte{0x01};
  SyntheticUsedRanges used_ranges;
  SequentialGuidGenerator target_guids(100);
  const auto result = ytec::clonecore::execute_offline_gpt_clone(
      valid_request(),
      fixture.source_reader,
      fixture.target_writer,
      used_ranges,
      target_guids);
  check(!result.has_value(), "Corrupt GPT must fail");
  check(fixture.target_writer.write_count == 0, "Corrupt GPT must fail before target writes");
}

void test_unknown_partition_writes_nothing() {
  SyntheticFixture fixture(true);
  SyntheticUsedRanges used_ranges;
  SequentialGuidGenerator target_guids(100);
  const auto result = ytec::clonecore::execute_offline_gpt_clone(
      valid_request(),
      fixture.source_reader,
      fixture.target_writer,
      used_ranges,
      target_guids);
  check(!result.has_value(), "Unknown partition type must fail closed");
  check(
      result.error().code == ErrorCode::unsupported_layout,
      "Unknown partition should be reported as unsupported");
  check(fixture.target_writer.write_count == 0, "Unknown layout must fail before target writes");
}

void test_read_back_failure_leaves_primary_gpt_invalid() {
  SyntheticFixture fixture;
  const ByteRange efi = byte_range(fixture.layout.partitions[0]);
  fixture.target_writer.corrupt_read_back_at = efi.offset;
  SyntheticUsedRanges used_ranges;
  SequentialGuidGenerator target_guids(100);
  const auto result = ytec::clonecore::execute_offline_gpt_clone(
      valid_request(),
      fixture.source_reader,
      fixture.target_writer,
      used_ranges,
      target_guids);
  check(!result.has_value(), "Read-back mismatch must fail");
  check(
      result.error().code == ErrorCode::verification_failed,
      "Read-back mismatch should have a verification error");
  check(
      std::all_of(
          fixture.target.begin() + kSectorSize,
          fixture.target.begin() + kSectorSize * 2,
          [](const std::byte value) { return value == std::byte{0}; }),
      "Primary GPT must stay invalid after data verification failure");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"crc32_known_vector", test_crc32_known_vector},
      {"volume_bitmap_chunk_decode_and_rounding",
       test_volume_bitmap_chunk_decode_and_rounding},
      {"snapshot_bitmap_provider_rejects_live_volume_paths",
       test_snapshot_bitmap_provider_rejects_live_volume_paths},
      {"snapshot_bitmap_provider_rejects_duplicate_bindings",
       test_snapshot_bitmap_provider_rejects_duplicate_bindings},
      {"bitlocker_boot_sector_is_rejected_explicitly",
       test_bitlocker_boot_sector_is_rejected_explicitly},
      {"synthetic_gpt_clone_success", test_synthetic_gpt_clone_success},
      {"gpt_basic_fat32_and_exfat_copy_whole_partition",
       test_gpt_basic_fat32_and_exfat_copy_whole_partition},
      {"malformed_exfat_is_rejected_before_target_write",
       test_malformed_exfat_is_rejected_before_target_write},
      {"verified_write_digest_changes_with_copied_data",
       test_verified_write_digest_changes_with_copied_data},
      {"cancellation_leaves_primary_gpt_invalid",
       test_cancellation_leaves_primary_gpt_invalid},
      {"confirmation_failure_writes_nothing",
       test_confirmation_failure_writes_nothing},
      {"identity_drift_writes_nothing", test_identity_drift_writes_nothing},
      {"corrupt_source_gpt_writes_nothing",
       test_corrupt_source_gpt_writes_nothing},
      {"unknown_partition_writes_nothing",
       test_unknown_partition_writes_nothing},
      {"read_back_failure_leaves_primary_gpt_invalid",
       test_read_back_failure_leaves_primary_gpt_invalid},
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

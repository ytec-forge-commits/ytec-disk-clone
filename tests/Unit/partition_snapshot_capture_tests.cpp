#include "ytec/clonecore/gpt.h"
#include "ytec/imageformat/partition_snapshot.h"

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

constexpr std::uint32_t kSectorSize = 512;
constexpr std::uint64_t kDiskSize = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kSectorCount = kDiskSize / kSectorSize;
constexpr std::uint64_t kGptEntrySectors = 32;
constexpr std::uint64_t kGptLeadingSectors = 2 + kGptEntrySectors;
constexpr std::uint64_t kGptTrailingSectors = 1 + kGptEntrySectors;

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

ytec::clonecore::Error read_error() {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_READ_FAULT,
      .operation = L"合成パーティション表読取り",
      .message = L"許可されたメタデータ領域外です",
  };
}

class MetadataOnlyReader final
    : public ytec::clonecore::ISourceDiskReader {
 public:
  MetadataOnlyReader(
      std::vector<std::byte> storage,
      const bool gpt)
      : storage_(std::move(storage)), gpt_(gpt) {}

  std::uint64_t size_bytes() const noexcept override {
    return storage_.size();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > storage_.size() ||
        length > storage_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          read_error());
    }
    const std::uint64_t end = offset + length;
    const std::uint64_t leading_end =
        gpt_ ? kGptLeadingSectors * kSectorSize : kSectorSize;
    const std::uint64_t trailing_begin =
        kDiskSize - kGptTrailingSectors * kSectorSize;
    if (end > leading_end &&
        (!gpt_ || offset < trailing_begin)) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          read_error());
    }
    reads.emplace_back(offset, length);
    const auto first =
        storage_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

  mutable std::vector<std::pair<std::uint64_t, std::size_t>> reads;

 private:
  std::vector<std::byte> storage_;
  bool gpt_{};
};

class SequentialGuidGenerator final
    : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    ytec::clonecore::GptGuid guid;
    guid.bytes[0] = std::byte{next_++};
    guid.bytes[15] = std::byte{0xA5};
    return ytec::clonecore::Result<
        ytec::clonecore::GptGuid>::success(guid);
  }

 private:
  std::uint8_t next_{1};
};

ytec::clonecore::GptGuid guid(const std::uint8_t value) {
  ytec::clonecore::GptGuid result;
  result.bytes[0] = std::byte{value};
  result.bytes[15] = std::byte{0x5A};
  return result;
}

std::vector<std::byte> valid_mbr_disk() {
  std::vector<std::byte> disk(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  write_little<std::uint32_t>(disk, 440, 0x1234ABCDU);
  disk[446] = std::byte{0x80};
  disk[450] = std::byte{0x07};
  write_little<std::uint32_t>(disk, 454, 2048);
  write_little<std::uint32_t>(disk, 458, 4096);
  disk[510] = std::byte{0x55};
  disk[511] = std::byte{0xAA};
  return disk;
}

std::vector<std::byte> valid_gpt_disk() {
  ytec::clonecore::GptDisk layout{
      .logical_sector_size = kSectorSize,
      .sector_count = kSectorCount,
      .disk_guid = guid(0x10),
      .first_usable_lba = kGptLeadingSectors,
      .last_usable_lba = kSectorCount - kGptTrailingSectors - 1,
      .partition_entry_count = 128,
      .partition_entry_size = 128,
      .partitions = {
          ytec::clonecore::GptPartition{
              .entry_index = 0,
              .type_guid = ytec::clonecore::gpt_type_basic_data(),
              .unique_guid = guid(0x20),
              .first_lba = 2048,
              .last_lba = 4095,
              .attributes = 0,
              .name = u"Windows",
          },
      },
  };
  SequentialGuidGenerator generator;
  const auto plan = ytec::clonecore::make_gpt_write_plan(
      layout, kDiskSize, kSectorSize, generator);
  check(plan.has_value(), "Synthetic GPT plan should build");

  std::vector<std::byte> disk(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  for (const auto& write : plan.value().writes) {
    check(
        write.offset <= disk.size() &&
            write.bytes.size() <= disk.size() - write.offset,
        "Synthetic GPT metadata should fit");
    std::copy(
        write.bytes.begin(),
        write.bytes.end(),
        disk.begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
  return disk;
}

void test_mbr_capture_reads_only_sector_zero() {
  MetadataOnlyReader reader(valid_mbr_disk(), false);
  const auto encoded =
      ytec::imageformat::capture_partition_snapshot_v1(
          reader, ytec::imageformat::PartitionTableStyle::mbr);
  check(encoded.has_value(), "Valid MBR capture should succeed");
  const auto snapshot =
      ytec::imageformat::inspect_partition_snapshot_v1(encoded.value());
  check(snapshot.has_value(), "Captured MBR should inspect");
  check(
      snapshot.value().regions.size() == 1 &&
          snapshot.value().regions[0].data.size() == kSectorSize &&
          snapshot.value().regions[0].data[510] == std::byte{0x55},
      "Captured MBR should contain exactly sector zero");
  check(
      std::all_of(
          reader.reads.begin(),
          reader.reads.end(),
          [](const auto& read) {
            return read.first == 0 && read.second == kSectorSize;
          }),
      "MBR capture must never read partition contents");
}

void test_gpt_capture_reads_only_both_metadata_ends() {
  MetadataOnlyReader reader(valid_gpt_disk(), true);
  const auto encoded =
      ytec::imageformat::capture_partition_snapshot_v1(
          reader, ytec::imageformat::PartitionTableStyle::gpt);
  check(encoded.has_value(), "Valid GPT capture should succeed");
  const auto snapshot =
      ytec::imageformat::inspect_partition_snapshot_v1(encoded.value());
  check(snapshot.has_value(), "Captured GPT should inspect");
  check(
      snapshot.value().regions.size() == 2 &&
          snapshot.value().regions.front().disk_offset == 0 &&
          snapshot.value().regions.front().data.size() ==
              kGptLeadingSectors * kSectorSize &&
          snapshot.value().regions.back().disk_offset ==
              kDiskSize - kGptTrailingSectors * kSectorSize &&
          snapshot.value().regions.back().data.size() ==
              kGptTrailingSectors * kSectorSize,
      "GPT capture should contain only canonical leading and trailing metadata");
}

void test_corrupt_partition_tables_fail_closed() {
  auto mbr = valid_mbr_disk();
  mbr[510] = std::byte{0};
  MetadataOnlyReader invalid_mbr(std::move(mbr), false);
  check(
      !ytec::imageformat::capture_partition_snapshot_v1(
           invalid_mbr,
           ytec::imageformat::PartitionTableStyle::mbr)
           .has_value(),
      "Invalid MBR must be rejected");

  auto gpt = valid_gpt_disk();
  gpt[kSectorSize] ^= std::byte{0x01};
  MetadataOnlyReader invalid_gpt(std::move(gpt), true);
  check(
      !ytec::imageformat::capture_partition_snapshot_v1(
           invalid_gpt,
           ytec::imageformat::PartitionTableStyle::gpt)
           .has_value(),
      "Invalid GPT must be rejected");
}

void test_unknown_style_does_not_read() {
  MetadataOnlyReader reader(valid_mbr_disk(), false);
  check(
      !ytec::imageformat::capture_partition_snapshot_v1(
           reader,
           static_cast<ytec::imageformat::PartitionTableStyle>(99))
           .has_value(),
      "Unknown partition-table style must be rejected");
  check(
      reader.reads.empty(),
      "Unknown style must be rejected before source I/O");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"mbr_capture_reads_only_sector_zero",
       test_mbr_capture_reads_only_sector_zero},
      {"gpt_capture_reads_only_both_metadata_ends",
       test_gpt_capture_reads_only_both_metadata_ends},
      {"corrupt_partition_tables_fail_closed",
       test_corrupt_partition_tables_fail_closed},
      {"unknown_style_does_not_read",
       test_unknown_style_does_not_read},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << failure.message << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

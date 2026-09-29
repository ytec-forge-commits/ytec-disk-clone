#include "ytec/clonecore/mbr.h"

#include <Windows.h>

#include <array>
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
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T>
T read_little(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

class SyntheticMbrReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit SyntheticMbrReader(
      std::vector<std::byte> sector,
      const std::uint64_t sector_count = 200'000,
      const std::uint32_t sector_size = 512)
      : sector_(std::move(sector)),
        size_bytes_(sector_count * sector_size),
        sector_size_(sector_size) {}

  std::uint64_t size_bytes() const noexcept override { return size_bytes_; }
  std::uint32_t logical_sector_size() const noexcept override {
    return sector_size_;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset != 0 || length != 512 || sector_.size() != 512) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_READ_FAULT,
              .operation = L"合成MBR読取り",
              .message = L"範囲外です",
          });
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(sector_);
  }

 private:
  std::vector<std::byte> sector_;
  std::uint64_t size_bytes_{};
  std::uint32_t sector_size_{};
};

void write_partition(
    std::vector<std::byte>& sector,
    const std::size_t index,
    const bool active,
    const std::uint8_t type,
    const std::uint32_t first_lba,
    const std::uint32_t sector_count) {
  const std::size_t offset = 446 + index * 16;
  std::span<std::byte> bytes(sector);
  bytes[offset] = active ? std::byte{0x80} : std::byte{0};
  bytes[offset + 1] = std::byte{0xFE};
  bytes[offset + 2] = std::byte{0xFF};
  bytes[offset + 3] = std::byte{0xFF};
  bytes[offset + 4] = std::byte{type};
  bytes[offset + 5] = std::byte{0xFE};
  bytes[offset + 6] = std::byte{0xFF};
  bytes[offset + 7] = std::byte{0xFF};
  write_little(bytes, offset + 8, first_lba);
  write_little(bytes, offset + 12, sector_count);
}

std::vector<std::byte> valid_mbr() {
  std::vector<std::byte> sector(512, std::byte{0});
  for (std::size_t index = 0; index < 440; ++index) {
    sector[index] = std::byte{static_cast<unsigned char>((index % 251) + 1)};
  }
  std::span<std::byte> bytes(sector);
  write_little(bytes, 440, 0x12345678U);
  write_partition(sector, 0, true, 0x07, 2048, 100'000);
  write_partition(sector, 1, false, 0x27, 120'000, 20'000);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};
  return sector;
}

class SequenceSignatureGenerator final
    : public ytec::clonecore::IMbrSignatureGenerator {
 public:
  explicit SequenceSignatureGenerator(std::vector<std::uint32_t> values)
      : values_(std::move(values)) {}

  ytec::clonecore::Result<std::uint32_t> next_signature() override {
    if (index_ >= values_.size()) {
      return ytec::clonecore::Result<std::uint32_t>::success(0);
    }
    return ytec::clonecore::Result<std::uint32_t>::success(values_[index_++]);
  }

 private:
  std::vector<std::uint32_t> values_;
  std::size_t index_{};
};

ytec::clonecore::MbrDisk parsed_valid_mbr() {
  const SyntheticMbrReader reader(valid_mbr());
  const auto parsed = ytec::clonecore::parse_mbr(reader);
  check(parsed.has_value(), "The valid synthetic MBR should parse");
  return parsed.value();
}

void test_valid_primary_mbr_parses() {
  const auto disk = parsed_valid_mbr();
  check(disk.disk_signature == 0x12345678U, "Disk signature should parse");
  check(disk.partitions.size() == 2, "Two primary partitions should parse");
  check(disk.partitions[0].active, "The Windows partition should be active");
  check(disk.partitions[0].type == 0x07, "The NTFS type should be retained");
  check(
      disk.partitions[1].first_lba == 120'000,
      "The recovery partition LBA should be retained");
}

void test_invalid_signature_is_rejected() {
  auto sector = valid_mbr();
  sector[511] = std::byte{0};
  check(
      !ytec::clonecore::parse_mbr(SyntheticMbrReader(std::move(sector)))
           .has_value(),
      "A missing 55AA signature must be rejected");
}

void test_invalid_boot_indicator_is_rejected() {
  auto sector = valid_mbr();
  sector[446] = std::byte{0x01};
  check(
      !ytec::clonecore::parse_mbr(SyntheticMbrReader(std::move(sector)))
           .has_value(),
      "Only 0 and 80h boot indicators are valid");
}

void test_multiple_active_partitions_are_rejected() {
  auto sector = valid_mbr();
  sector[446 + 16] = std::byte{0x80};
  check(
      !ytec::clonecore::parse_mbr(SyntheticMbrReader(std::move(sector)))
           .has_value(),
      "Ambiguous multiple Active partitions must fail closed");
}

void test_overlapping_partitions_are_rejected() {
  auto sector = valid_mbr();
  write_partition(sector, 1, false, 0x27, 90'000, 20'000);
  check(
      !ytec::clonecore::parse_mbr(SyntheticMbrReader(std::move(sector)))
           .has_value(),
      "Overlapping primary partitions must be rejected");
}

void test_out_of_range_partition_is_rejected() {
  auto sector = valid_mbr();
  write_partition(sector, 1, false, 0x27, 190'000, 20'000);
  check(
      !ytec::clonecore::parse_mbr(SyntheticMbrReader(std::move(sector)))
           .has_value(),
      "A partition beyond the disk must be rejected");
}

void test_protective_and_extended_layouts_are_rejected() {
  auto protective = valid_mbr();
  write_partition(protective, 0, false, 0xEE, 1, 199'999);
  check(
      !ytec::clonecore::parse_mbr(SyntheticMbrReader(std::move(protective)))
           .has_value(),
      "A GPT protective MBR must not enter the MBR clone path");

  auto extended = valid_mbr();
  write_partition(extended, 1, false, 0x0F, 120'000, 20'000);
  check(
      !ytec::clonecore::parse_mbr(SyntheticMbrReader(std::move(extended)))
           .has_value(),
      "Extended partitions must wait for strict EBR parsing");
}

void test_non_512_sector_mbr_is_rejected() {
  check(
      !ytec::clonecore::parse_mbr(
           SyntheticMbrReader(valid_mbr(), 200'000, 4096))
           .has_value(),
      "Legacy MBR writes must remain limited to 512-byte sectors");
}

void test_write_plan_regenerates_noncolliding_signature() {
  const auto source = parsed_valid_mbr();
  SequenceSignatureGenerator generator(
      {0, source.disk_signature, 0xAABBCCDDU, 0x87654321U});
  const std::array<std::uint32_t, 1> connected{0xAABBCCDDU};
  const auto plan = ytec::clonecore::make_mbr_write_plan(
      source,
      250'000ULL * 512ULL,
      512,
      generator,
      connected);
  check(plan.has_value(), "A unique generated signature should be accepted");
  check(
      plan.value().target_disk.disk_signature == 0x87654321U,
      "Zero, source, and connected signatures must be skipped");
  check(plan.value().sector.size() == 512, "The write plan should contain one MBR sector");
  const std::span<const std::byte> sector(plan.value().sector);
  check(
      read_little<std::uint32_t>(sector, 440) == 0x87654321U,
      "The serialized target signature should be new");
  check(
      sector[446] == std::byte{0x80} && sector[510] == std::byte{0x55} &&
          sector[511] == std::byte{0xAA},
      "Active state and MBR signature should be preserved");
  check(
      std::equal(source.bootstrap.begin(), source.bootstrap.end(), sector.begin()),
      "The source bootstrap code should be retained for later BIOS repair");
}

void test_write_plan_requires_active_and_capacity() {
  auto source = parsed_valid_mbr();
  source.partitions[0].active = false;
  SequenceSignatureGenerator generator({0x87654321U});
  check(
      !ytec::clonecore::make_mbr_write_plan(
           source, 250'000ULL * 512ULL, 512, generator)
           .has_value(),
      "A BIOS clone plan must have exactly one Active partition");

  source = parsed_valid_mbr();
  SequenceSignatureGenerator capacity_generator({0x87654321U});
  check(
      !ytec::clonecore::make_mbr_write_plan(
           source, 100'000ULL * 512ULL, 512, capacity_generator)
           .has_value(),
      "A smaller target disk must be rejected");
}

void test_signature_collision_exhaustion_is_rejected() {
  const auto source = parsed_valid_mbr();
  SequenceSignatureGenerator generator(
      std::vector<std::uint32_t>(32, source.disk_signature));
  check(
      !ytec::clonecore::make_mbr_write_plan(
           source, 250'000ULL * 512ULL, 512, generator)
           .has_value(),
      "Repeated signature collisions must stop after a bounded retry count");
}

void test_add_partition_plan_preserves_existing_mbr_identity() {
  const auto current = parsed_valid_mbr();
  const auto plan = ytec::clonecore::make_mbr_add_partition_plan(
      current,
      {
          .first_lba = 150'000U,
          .sector_count = 20'000U,
          .type = 0x07U,
      });
  check(plan.has_value(), "A non-overlapping empty primary slot should plan");
  check(
      plan.value().target_disk.disk_signature == current.disk_signature &&
          plan.value().target_disk.bootstrap == current.bootstrap &&
          plan.value().target_disk.partitions.size() == 3U &&
          plan.value().target_disk.partitions.back().table_index == 2U &&
          !plan.value().target_disk.partitions.back().active,
      "Adding a partition must preserve identity and choose the first empty non-active entry");
  const auto parsed = ytec::clonecore::parse_mbr(SyntheticMbrReader(
      plan.value().sector, current.sector_count));
  check(parsed.has_value() && parsed.value().partitions.size() == 3U,
        "The preserving MBR sector must parse with all existing entries");
  check(
      !ytec::clonecore::make_mbr_add_partition_plan(
           current,
           {
               .first_lba = 90'000U,
               .sector_count = 20'000U,
               .type = 0x07U,
           })
           .has_value(),
      "An overlapping preserving MBR addition must fail closed");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"valid_primary_mbr_parses", test_valid_primary_mbr_parses},
      {"invalid_signature_is_rejected", test_invalid_signature_is_rejected},
      {"invalid_boot_indicator_is_rejected",
       test_invalid_boot_indicator_is_rejected},
      {"multiple_active_partitions_are_rejected",
       test_multiple_active_partitions_are_rejected},
      {"overlapping_partitions_are_rejected",
       test_overlapping_partitions_are_rejected},
      {"out_of_range_partition_is_rejected",
       test_out_of_range_partition_is_rejected},
      {"protective_and_extended_layouts_are_rejected",
       test_protective_and_extended_layouts_are_rejected},
      {"non_512_sector_mbr_is_rejected", test_non_512_sector_mbr_is_rejected},
      {"write_plan_regenerates_noncolliding_signature",
       test_write_plan_regenerates_noncolliding_signature},
      {"write_plan_requires_active_and_capacity",
       test_write_plan_requires_active_and_capacity},
      {"signature_collision_exhaustion_is_rejected",
       test_signature_collision_exhaustion_is_rejected},
      {"add_partition_plan_preserves_existing_mbr_identity",
       test_add_partition_plan_preserves_existing_mbr_identity},
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

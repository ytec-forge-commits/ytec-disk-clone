#include "ytec/imageformat/backup_manifest.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

template <typename T>
T read_little(
    const std::vector<std::byte>& bytes,
    const std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
void write_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

ytec::imageformat::BackupImageManifest sample_manifest() {
  using ytec::imageformat::BackupFileSystem;
  using ytec::imageformat::BackupManifestPartition;
  using ytec::imageformat::BackupPartitionRole;

  ytec::imageformat::BackupImageManifest manifest;
  manifest.source = ytec::clonecore::StableDiskIdentity{
      .disk_number = 0,
      .model = L"Y-TEC 合成 NVMe",
      .size_bytes = 64ULL * kGiB,
      .logical_sector_size = 512,
      .serial_suffix = "ABC12345",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC\\SYNTHETIC",
      .is_system_disk = true,
  };
  manifest.physical_sector_size = 4096;
  manifest.partition_style =
      ytec::imageformat::BackupPartitionStyle::gpt;
  manifest.boot_mode = ytec::imageformat::BackupBootMode::uefi;
  manifest.windows_major = 10;
  manifest.windows_minor = 0;
  manifest.windows_build = 26100;
  manifest.windows_architecture = "AMD64";
  manifest.bitlocker_fully_decrypted = true;
  manifest.compression = ytec::imageformat::DcimgCompression::none;
  manifest.compression_version = 0;
  manifest.chunk_size = ytec::imageformat::kDcimgChunkSize16MiB;
  manifest.created_utc = "2026-07-31T12:34:56Z";
  manifest.app_version = "0.1.0";
  manifest.partitions = {
      BackupManifestPartition{
          .table_index = 0,
          .offset_bytes = 1ULL * kMiB,
          .length_bytes = 100ULL * kMiB,
          .role = BackupPartitionRole::efi_system,
          .file_system = BackupFileSystem::fat32,
          .cluster_size = 4096,
          .name = L"EFI システム",
      },
      BackupManifestPartition{
          .table_index = 1,
          .offset_bytes = 101ULL * kMiB,
          .length_bytes = 16ULL * kMiB,
          .role = BackupPartitionRole::microsoft_reserved,
          .file_system = BackupFileSystem::none,
          .cluster_size = 0,
          .name = L"Microsoft reserved",
      },
      BackupManifestPartition{
          .table_index = 2,
          .offset_bytes = 117ULL * kMiB,
          .length_bytes = 60ULL * kGiB,
          .role = BackupPartitionRole::windows_ntfs,
          .file_system = BackupFileSystem::ntfs,
          .cluster_size = 4096,
          .name = L"Windows",
      },
      BackupManifestPartition{
          .table_index = 3,
          .offset_bytes = 117ULL * kMiB + 60ULL * kGiB,
          .length_bytes = 800ULL * kMiB,
          .role = BackupPartitionRole::recovery_ntfs,
          .file_system = BackupFileSystem::ntfs,
          .cluster_size = 4096,
          .name = L"回復",
      },
  };
  return manifest;
}

void test_round_trip_is_canonical_and_complete() {
  const auto first =
      ytec::imageformat::build_backup_manifest_v1(sample_manifest());
  const auto second =
      ytec::imageformat::build_backup_manifest_v1(sample_manifest());
  check(first.has_value() && second.has_value(),
        "Valid manifest should build");
  check(first.value() == second.value(),
        "Same manifest must encode deterministically");
  const auto parsed =
      ytec::imageformat::inspect_backup_manifest_v1(first.value());
  check(parsed.has_value(), "Canonical manifest should inspect");
  check(
      parsed.value().source.model == L"Y-TEC 合成 NVMe" &&
          parsed.value().source.serial_suffix == "ABC12345" &&
          parsed.value().windows_build == 26100 &&
          parsed.value().boot_mode ==
              ytec::imageformat::BackupBootMode::uefi &&
          parsed.value().bitlocker_fully_decrypted &&
          parsed.value().partitions.size() == 4 &&
          parsed.value().partitions.back().name == L"回復",
      "All required identity, Windows, boot, BitLocker, and partitions should round trip");
}

void test_security_and_compression_profile_are_strict() {
  auto manifest = sample_manifest();
  manifest.bitlocker_fully_decrypted = false;
  check(
      !ytec::imageformat::build_backup_manifest_v1(manifest).has_value(),
      "BitLocker not fully decrypted must fail");
  manifest = sample_manifest();
  manifest.compression =
      ytec::imageformat::DcimgCompression::zstandard;
  manifest.compression_version = 1;
  const auto compressed =
      ytec::imageformat::build_backup_manifest_v1(manifest);
  check(compressed.has_value(), "Approved Zstandard profile 1 should build");
  const auto inspected =
      ytec::imageformat::inspect_backup_manifest_v1(compressed.value());
  check(
      inspected.has_value() &&
          inspected.value().compression ==
              ytec::imageformat::DcimgCompression::zstandard &&
          inspected.value().compression_version == 1U,
      "Approved compression metadata should round-trip canonically");

  manifest.compression_version = 0;
  check(!ytec::imageformat::build_backup_manifest_v1(manifest).has_value(),
        "Unknown Zstandard profile versions must fail");
  manifest = sample_manifest();
  manifest.compression_version = 1;
  check(!ytec::imageformat::build_backup_manifest_v1(manifest).has_value(),
        "None compression cannot carry a profile version");
}

void test_sector_size_pair_accepts_16k_and_rejects_invalid_values() {
  auto manifest = sample_manifest();
  manifest.physical_sector_size = 16U * 1024U;
  const auto encoded =
      ytec::imageformat::build_backup_manifest_v1(manifest);
  check(encoded.has_value(),
        "A 512-byte logical / 16 KiB physical sector pair should build");
  const auto inspected =
      ytec::imageformat::inspect_backup_manifest_v1(encoded.value());
  check(
      inspected.has_value() &&
          inspected.value().physical_sector_size == 16U * 1024U,
      "The 16 KiB physical sector size should round-trip canonically");

  manifest = sample_manifest();
  manifest.physical_sector_size = 3U * 512U;
  check(
      !ytec::imageformat::build_backup_manifest_v1(manifest).has_value(),
      "A non-power-of-two physical sector size must fail");
  manifest.physical_sector_size = 128U * 1024U;
  check(
      !ytec::imageformat::build_backup_manifest_v1(manifest).has_value(),
      "A physical sector size above the 64 KiB format limit must fail");
}

void test_partition_overlap_or_wrong_boot_pair_is_rejected() {
  auto manifest = sample_manifest();
  manifest.partitions[3].offset_bytes =
      manifest.partitions[2].offset_bytes + 1ULL * kGiB;
  check(
      !ytec::imageformat::build_backup_manifest_v1(manifest).has_value(),
      "Overlapping partition metadata must fail");
  manifest = sample_manifest();
  manifest.boot_mode =
      ytec::imageformat::BackupBootMode::legacy_bios;
  check(
      !ytec::imageformat::build_backup_manifest_v1(manifest).has_value(),
      "GPT with Legacy BIOS must fail");
}

void test_data_disk_manifest_round_trips_without_boot_metadata() {
  auto manifest = sample_manifest();
  manifest.format_minor =
      ytec::imageformat::kBackupManifestMinorVersion;
  manifest.source.is_system_disk = false;
  manifest.boot_mode = ytec::imageformat::BackupBootMode::none;
  manifest.partitions = {
      ytec::imageformat::BackupManifestPartition{
          .table_index = 0,
          .offset_bytes = 1ULL * kMiB,
          .length_bytes = 60ULL * kGiB,
          .role = ytec::imageformat::BackupPartitionRole::ntfs_data,
          .file_system = ytec::imageformat::BackupFileSystem::ntfs,
          .cluster_size = 4096,
          .name = L"DATA",
      },
  };
  const auto encoded =
      ytec::imageformat::build_backup_manifest_v1(manifest);
  check(encoded.has_value(), "A basic NTFS data disk manifest should build");
  const auto inspected =
      ytec::imageformat::inspect_backup_manifest_v1(encoded.value());
  check(
      inspected.has_value() &&
          inspected.value().format_minor ==
              ytec::imageformat::kBackupManifestMinorVersion &&
          !inspected.value().source.is_system_disk &&
          inspected.value().boot_mode ==
              ytec::imageformat::BackupBootMode::none &&
          inspected.value().partitions.front().role ==
              ytec::imageformat::BackupPartitionRole::ntfs_data,
      "Data-only type, boot mode, and role should round-trip canonically");

  manifest.format_minor =
      ytec::imageformat::kLegacyBackupManifestMinorVersion;
  check(
      !ytec::imageformat::build_backup_manifest_v1(manifest).has_value(),
      "Legacy manifest 1.0 must not silently encode a data-only disk");
}

void test_reserved_unknown_and_trailing_bytes_are_rejected() {
  auto encoded =
      ytec::imageformat::build_backup_manifest_v1(sample_manifest())
          .take_value();
  encoded[108] = std::byte{1};
  check(
      !ytec::imageformat::inspect_backup_manifest_v1(encoded).has_value(),
      "Non-zero header reserved bytes must fail");

  encoded =
      ytec::imageformat::build_backup_manifest_v1(sample_manifest())
          .take_value();
  write_little<std::uint16_t>(
      encoded,
      ytec::imageformat::kBackupManifestHeaderSize + 4,
      99);
  check(
      !ytec::imageformat::inspect_backup_manifest_v1(encoded).has_value(),
      "Unknown partition role must fail");

  encoded =
      ytec::imageformat::build_backup_manifest_v1(sample_manifest())
          .take_value();
  encoded.push_back(std::byte{0});
  check(
      !ytec::imageformat::inspect_backup_manifest_v1(encoded).has_value(),
      "Trailing bytes must fail");
}

void test_invalid_utf8_and_noncanonical_offsets_are_rejected() {
  auto encoded =
      ytec::imageformat::build_backup_manifest_v1(sample_manifest())
          .take_value();
  const std::uint64_t model_offset =
      read_little<std::uint64_t>(encoded, 144);
  encoded[static_cast<std::size_t>(model_offset)] = std::byte{0xFF};
  check(
      !ytec::imageformat::inspect_backup_manifest_v1(encoded).has_value(),
      "Invalid UTF-8 must fail");

  encoded =
      ytec::imageformat::build_backup_manifest_v1(sample_manifest())
          .take_value();
  const std::uint64_t created_offset =
      read_little<std::uint64_t>(encoded, 96);
  write_little<std::uint64_t>(encoded, 96, created_offset + 1);
  check(
      !ytec::imageformat::inspect_backup_manifest_v1(encoded).has_value(),
      "Noncanonical string placement must fail");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"round_trip_is_canonical_and_complete",
       test_round_trip_is_canonical_and_complete},
      {"security_and_compression_profile_are_strict",
       test_security_and_compression_profile_are_strict},
      {"sector_size_pair_accepts_16k_and_rejects_invalid_values",
       test_sector_size_pair_accepts_16k_and_rejects_invalid_values},
      {"partition_overlap_or_wrong_boot_pair_is_rejected",
       test_partition_overlap_or_wrong_boot_pair_is_rejected},
      {"data_disk_manifest_round_trips_without_boot_metadata",
       test_data_disk_manifest_round_trips_without_boot_metadata},
      {"reserved_unknown_and_trailing_bytes_are_rejected",
       test_reserved_unknown_and_trailing_bytes_are_rejected},
      {"invalid_utf8_and_noncanonical_offsets_are_rejected",
       test_invalid_utf8_and_noncanonical_offsets_are_rejected},
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

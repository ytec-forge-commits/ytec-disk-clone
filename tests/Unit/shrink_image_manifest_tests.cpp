#include "ytec/imageformat/shrink_image_manifest.h"

#include <cstddef>
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

ytec::imageformat::Sha256Digest digest(const unsigned char seed) {
  ytec::imageformat::Sha256Digest result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = std::byte{static_cast<unsigned char>(seed + index)};
  }
  return result;
}

ytec::imageformat::ShrinkImageManifest sample_manifest() {
  using namespace ytec::migrationcore;
  return ytec::imageformat::ShrinkImageManifest{
      .source = ytec::clonecore::StableDiskIdentity{
          .disk_number = 1,
          .model = L"Y-TEC 1TB HDD",
          .size_bytes = 1000ULL * kGiB,
          .logical_sector_size = 512,
          .serial_suffix = "MIG12345",
          .device_instance_id = L"SCSI\\DISK&VEN_YTEC\\MIGRATION",
          .is_system_disk = true,
      },
      .physical_sector_size = 4096,
      .partition_style = MigrationPartitionStyle::gpt,
      .windows_major = 10,
      .windows_minor = 0,
      .windows_build = 19045,
      .windows_architecture = "AMD64",
      .bitlocker_fully_decrypted = true,
      .created_utc = "2026-08-03T03:00:00Z",
      .app_version = "0.2.0",
      .partitions = {
          ytec::imageformat::ShrinkImagePartition{
              .source_table_index = 0,
              .role = MigrationPartitionRole::efi_system,
              .file_system = MigrationFileSystem::fat32,
              .source_size_bytes = 200ULL * kMiB,
              .cluster_size = 4096,
              .label = L"SYSTEM",
          },
          ytec::imageformat::ShrinkImagePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::microsoft_reserved,
              .file_system = MigrationFileSystem::none,
              .source_size_bytes = 16ULL * kMiB,
          },
          ytec::imageformat::ShrinkImagePartition{
              .source_table_index = 2,
              .role = MigrationPartitionRole::windows,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 998ULL * kGiB,
              .used_bytes = 300ULL * kGiB,
              .cluster_size = 4096,
              .label = L"Windows",
              .payload_file_name = "volume-002.wim",
              .payload_length_bytes = 180ULL * kGiB,
              .payload_sha256 = digest(1),
          },
          ytec::imageformat::ShrinkImagePartition{
              .source_table_index = 3,
              .role = MigrationPartitionRole::recovery,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 1ULL * kGiB,
              .used_bytes = 600ULL * kMiB,
              .cluster_size = 4096,
              .label = L"Recovery",
              .payload_file_name = "volume-003.wim",
              .payload_length_bytes = 500ULL * kMiB,
              .payload_sha256 = digest(7),
          },
      },
  };
}

void test_round_trip_binds_every_wim_payload() {
  const auto encoded =
      ytec::imageformat::build_shrink_image_manifest_v1(sample_manifest());
  check(encoded.has_value(), "Valid shrink image manifest should build");
  const auto inspected =
      ytec::imageformat::inspect_shrink_image_manifest_v1(encoded.value());
  check(
      inspected.has_value() && inspected.value().source.is_system_disk &&
          inspected.value().partitions.size() == 4 &&
          inspected.value().partitions[2].payload_file_name ==
              "volume-002.wim" &&
          inspected.value().partitions[2].payload_sha256 == digest(1),
      "Source identity, role, WIM name, length, and digest should round-trip");
  const auto second =
      ytec::imageformat::build_shrink_image_manifest_v1(inspected.value());
  check(second.has_value() && second.value() == encoded.value(),
        "Canonical re-encoding should be byte-identical");
}

void test_unsafe_or_duplicate_payload_names_fail() {
  auto manifest = sample_manifest();
  manifest.partitions[2].payload_file_name = "..\\escape.wim";
  check(
      !ytec::imageformat::build_shrink_image_manifest_v1(manifest).has_value(),
      "Payload paths must not escape the bundle directory");
  manifest = sample_manifest();
  manifest.partitions[3].payload_file_name =
      manifest.partitions[2].payload_file_name;
  check(
      !ytec::imageformat::build_shrink_image_manifest_v1(manifest).has_value(),
      "Payload file names must be unique");
}

void test_missing_hash_or_payload_is_rejected() {
  auto manifest = sample_manifest();
  manifest.partitions[2].payload_sha256 = {};
  check(
      !ytec::imageformat::build_shrink_image_manifest_v1(manifest).has_value(),
      "A nonempty NTFS volume requires a nonzero SHA-256");
  manifest = sample_manifest();
  manifest.partitions[2].payload_file_name.clear();
  manifest.partitions[2].payload_length_bytes = 0;
  manifest.partitions[2].payload_sha256 = {};
  check(
      !ytec::imageformat::build_shrink_image_manifest_v1(manifest).has_value(),
      "A nonempty NTFS volume requires a WIM payload");
}

void test_partition_semantics_and_source_capacity_fail_closed() {
  using namespace ytec::migrationcore;
  auto manifest = sample_manifest();
  manifest.partitions[0].file_system = MigrationFileSystem::ntfs;
  check(
      !ytec::imageformat::build_shrink_image_manifest_v1(manifest).has_value(),
      "EFI metadata must retain its exact FAT32 semantics");

  manifest = sample_manifest();
  manifest.partitions[2].active = true;
  check(
      !ytec::imageformat::build_shrink_image_manifest_v1(manifest).has_value(),
      "GPT partitions must not smuggle MBR active state");

  manifest = sample_manifest();
  manifest.partitions[2].source_size_bytes = 1000ULL * kGiB;
  check(
      !ytec::imageformat::build_shrink_image_manifest_v1(manifest).has_value(),
      "Declared partition bytes must fit the observed source disk");

  manifest = sample_manifest();
  manifest.partitions[2].label = std::wstring{L'D', L'A', L'T', L'A', L'\0', L'X'};
  check(
      !ytec::imageformat::build_shrink_image_manifest_v1(manifest).has_value(),
      "Embedded NUL characters in volume labels must fail closed");
}

void test_corruption_unknown_role_and_trailing_bytes_fail() {
  auto encoded =
      ytec::imageformat::build_shrink_image_manifest_v1(sample_manifest())
          .take_value();
  encoded[200] = std::byte{1};
  check(
      !ytec::imageformat::inspect_shrink_image_manifest_v1(encoded).has_value(),
      "Reserved header bytes must remain zero");
  encoded =
      ytec::imageformat::build_shrink_image_manifest_v1(sample_manifest())
          .take_value();
  encoded[ytec::imageformat::kShrinkImageManifestHeaderSize + 4] =
      std::byte{99};
  check(
      !ytec::imageformat::inspect_shrink_image_manifest_v1(encoded).has_value(),
      "Unknown partition roles must fail closed");
  encoded =
      ytec::imageformat::build_shrink_image_manifest_v1(sample_manifest())
          .take_value();
  encoded.push_back(std::byte{0});
  check(
      !ytec::imageformat::inspect_shrink_image_manifest_v1(encoded).has_value(),
      "Trailing bytes must fail closed");
}

void test_data_only_manifest_never_requires_boot_roles() {
  using namespace ytec::migrationcore;
  auto manifest = sample_manifest();
  manifest.source.is_system_disk = false;
  manifest.windows_major = 0;
  manifest.windows_minor = 0;
  manifest.windows_build = 0;
  manifest.windows_architecture.clear();
  manifest.partitions = {
      ytec::imageformat::ShrinkImagePartition{
          .source_table_index = 0,
          .role = MigrationPartitionRole::data,
          .file_system = MigrationFileSystem::ntfs,
          .source_size_bytes = 900ULL * kGiB,
          .used_bytes = 200ULL * kGiB,
          .cluster_size = 4096,
          .label = L"DATA",
          .payload_file_name = "volume-000.wim",
          .payload_length_bytes = 150ULL * kGiB,
          .payload_sha256 = digest(11),
      },
  };
  const auto encoded =
      ytec::imageformat::build_shrink_image_manifest_v1(manifest);
  check(encoded.has_value(), "A data-only shrink image should build");
  const auto inspected =
      ytec::imageformat::inspect_shrink_image_manifest_v1(encoded.value());
  check(
      inspected.has_value() && !inspected.value().source.is_system_disk &&
          inspected.value().partitions.front().role ==
              MigrationPartitionRole::data,
      "Data-only image should remain explicitly non-system");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"round_trip_binds_every_wim_payload",
       test_round_trip_binds_every_wim_payload},
      {"unsafe_or_duplicate_payload_names_fail",
       test_unsafe_or_duplicate_payload_names_fail},
      {"missing_hash_or_payload_is_rejected",
       test_missing_hash_or_payload_is_rejected},
      {"partition_semantics_and_source_capacity_fail_closed",
       test_partition_semantics_and_source_capacity_fail_closed},
      {"corruption_unknown_role_and_trailing_bytes_fail",
       test_corruption_unknown_role_and_trailing_bytes_fail},
      {"data_only_manifest_never_requires_boot_roles",
       test_data_only_manifest_never_requires_boot_roles},
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

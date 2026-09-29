#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_manifest.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kDiskBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionOffset = 1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kPartitionBytes = 8ULL * 1024ULL * 1024ULL;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename T>
void overwrite_little(
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<std::byte> mbr_snapshot(
    const std::uint64_t source_bytes = kDiskBytes) {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = source_bytes,
      .logical_sector_size = 512,
  };
  ytec::imageformat::PartitionTableRegion region;
  region.disk_offset = 0U;
  region.data.assign(512U, std::byte{0});
  region.data[446U + 4U] = std::byte{0x07};
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));
  const auto encoded =
      ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(encoded.has_value(), "Synthetic MBR snapshot should build");
  return encoded.value();
}

ytec::imageformat::TsumugiManifest exact_manifest() {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::exact,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .flags = TsumugiManifestFlags::source_contains_windows |
          TsumugiManifestFlags::automatic_surplus_allocation,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512,
      .physical_sector_size = 4096,
      .created_utc = "2026-08-04T19:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = mbr_snapshot(),
  };
  manifest.source_model_hash[0] = std::byte{0x11};
  manifest.source_serial_hash[0] = std::byte{0x22};
  manifest.source_state_hash[0] = std::byte{0x33};
  TsumugiManifestPartition partition{
      .source_table_index = 1,
      .source_partition_number = 1,
      .role = TsumugiManifestPartitionRole::windows,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected |
          TsumugiManifestPartitionFlags::required |
          TsumugiManifestPartitionFlags::active |
          TsumugiManifestPartitionFlags::contains_windows,
      .source_offset = kPartitionOffset,
      .source_size = kPartitionBytes,
      .used_bytes = 3ULL * 1024ULL * 1024ULL,
      .minimum_target_bytes = kPartitionBytes,
      .planned_target_bytes = kPartitionBytes,
      .payload_logical_offset = kPartitionOffset,
      .payload_logical_length = kPartitionBytes,
      .name_utf8 = "Windows",
      .label_utf8 = "\xE3\x82\xB7\xE3\x82\xB9\xE3\x83\x86\xE3\x83\xA0",
  };
  partition.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(partition));
  return manifest;
}

void test_exact_round_trip_is_canonical() {
  const auto built =
      ytec::imageformat::build_tsumugi_manifest_v1(exact_manifest());
  check(built.has_value(), "Exact manifest should build");
  const auto inspected =
      ytec::imageformat::inspect_tsumugi_manifest_v1(built.value());
  check(inspected.has_value(), "Exact manifest should inspect");
  check(inspected.value().partitions.size() == 1U,
        "One partition should survive");
  check(inspected.value().partitions[0].label_utf8 ==
            exact_manifest().partitions[0].label_utf8,
        "UTF-8 label should survive");
  const auto rebuilt =
      ytec::imageformat::build_tsumugi_manifest_v1(inspected.value());
  check(rebuilt.has_value() && rebuilt.value() == built.value(),
        "Re-encoding must be byte-identical");
}

void test_unknown_and_reserved_bits_are_rejected() {
  auto bytes = ytec::imageformat::build_tsumugi_manifest_v1(
      exact_manifest()).value();
  overwrite_little<std::uint32_t>(bytes, 24U, 0x80000000U);
  check(!ytec::imageformat::inspect_tsumugi_manifest_v1(bytes).has_value(),
        "Unknown manifest flags must fail");

  bytes = ytec::imageformat::build_tsumugi_manifest_v1(
      exact_manifest()).value();
  bytes[92U] = std::byte{1};
  check(!ytec::imageformat::inspect_tsumugi_manifest_v1(bytes).has_value(),
        "Nonzero header reserve must fail");

  bytes = ytec::imageformat::build_tsumugi_manifest_v1(
      exact_manifest()).value();
  bytes.push_back(std::byte{0});
  check(!ytec::imageformat::inspect_tsumugi_manifest_v1(bytes).has_value(),
        "Trailing bytes must fail");
}

void test_shrink_selection_and_unselected_partition() {
  using namespace ytec::imageformat;
  auto manifest = exact_manifest();
  manifest.mode = TsumugiManifestMode::shrink;
  manifest.flags = TsumugiManifestFlags::partition_selection |
      TsumugiManifestFlags::source_contains_windows;
  auto& windows = manifest.partitions[0];
  windows.minimum_target_bytes = 4ULL * 1024ULL * 1024ULL;
  windows.planned_target_bytes = 6ULL * 1024ULL * 1024ULL;
  windows.payload_logical_offset = 0U;
  windows.payload_logical_length = 4ULL * 1024ULL * 1024ULL;
  windows.payload_encoding =
      TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
  windows.payload_format_version = kTsumugiWimPayloadFormatVersion;
  windows.cluster_size = 4096U;
  TsumugiManifestPartition skipped{
      .source_table_index = 2,
      .source_partition_number = 2,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::exfat,
      .flags = TsumugiManifestPartitionFlags::none,
      .source_offset = 10ULL * 1024ULL * 1024ULL,
      .source_size = 4ULL * 1024ULL * 1024ULL,
      .used_bytes = 1ULL * 1024ULL * 1024ULL,
      .name_utf8 = "Data",
      .label_utf8 = "D",
  };
  skipped.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(skipped));
  const auto built = build_tsumugi_manifest_v1(manifest);
  check(built.has_value(), "Shrink selection should build");
  check(inspect_tsumugi_manifest_v1(built.value()).has_value(),
        "Shrink selection should inspect");

  manifest.partitions[0].minimum_target_bytes =
      manifest.partitions[0].used_bytes - 512U;
  check(!build_tsumugi_manifest_v1(manifest).has_value(),
        "Shrink target minimum must cover the filesystem's used bytes");
}

void test_rescue_forbids_resize_and_surplus_allocation() {
  using namespace ytec::imageformat;
  auto manifest = exact_manifest();
  manifest.mode = TsumugiManifestMode::rescue;
  check(!build_tsumugi_manifest_v1(manifest).has_value(),
        "Rescue must reject automatic surplus allocation");
  manifest.flags = TsumugiManifestFlags::source_contains_windows;
  manifest.partitions[0].planned_target_bytes -= 512U;
  check(!build_tsumugi_manifest_v1(manifest).has_value(),
        "Rescue must preserve the exact partition size");
}

void test_snapshot_geometry_and_mbr_ids_are_bound() {
  auto manifest = exact_manifest();
  manifest.partition_snapshot = mbr_snapshot(kDiskBytes + 512U);
  check(!ytec::imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
        "Snapshot disk size mismatch must fail");

  manifest = exact_manifest();
  manifest.partitions[0].unique_id[0] = std::byte{1};
  check(!ytec::imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
        "MBR unused unique ID bytes must be zero");
}

void test_invalid_utf8_and_required_unselected_fail() {
  auto manifest = exact_manifest();
  manifest.partitions[0].label_utf8 = std::string("\xC0\xAF", 2U);
  check(!ytec::imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
        "Overlong UTF-8 must fail");

  manifest = exact_manifest();
  manifest.partitions[0].flags =
      ytec::imageformat::TsumugiManifestPartitionFlags::required;
  manifest.partitions[0].minimum_target_bytes = 0U;
  manifest.partitions[0].planned_target_bytes = 0U;
  manifest.partitions[0].payload_logical_offset = 0U;
  manifest.partitions[0].payload_logical_length = 0U;
  check(!ytec::imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
        "Required partitions cannot be unselected");
}

void test_selection_bitlocker_and_shrink_flags_are_canonical() {
  using namespace ytec::imageformat;
  auto manifest = exact_manifest();
  manifest.flags = manifest.flags |
      TsumugiManifestFlags::partition_selection;
  check(!build_tsumugi_manifest_v1(manifest).has_value(),
        "Partition-selection flag requires an unselected record");

  manifest = exact_manifest();
  manifest.flags = manifest.flags |
      TsumugiManifestFlags::bitlocker_source_was_unlocked;
  check(!build_tsumugi_manifest_v1(manifest).has_value(),
        "Top-level BitLocker flag requires a selected unlocked partition");

  manifest.partitions[0].flags =
      manifest.partitions[0].flags |
      TsumugiManifestPartitionFlags::bitlocker_was_unlocked;
  check(build_tsumugi_manifest_v1(manifest).has_value(),
        "Matching selected BitLocker flags should build");

  manifest = exact_manifest();
  manifest.mode = TsumugiManifestMode::shrink;
  manifest.partitions[0].file_system = TsumugiManifestFileSystem::unknown;
  manifest.partitions[0].minimum_target_bytes = 4ULL * 1024ULL * 1024ULL;
  manifest.partitions[0].planned_target_bytes = 6ULL * 1024ULL * 1024ULL;
  manifest.partitions[0].payload_logical_offset = 0U;
  manifest.partitions[0].payload_logical_length = 4ULL * 1024ULL * 1024ULL;
  manifest.partitions[0].payload_encoding =
      TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
  manifest.partitions[0].payload_format_version =
      kTsumugiWimPayloadFormatVersion;
  manifest.partitions[0].cluster_size = 4096U;
  check(!build_tsumugi_manifest_v1(manifest).has_value(),
        "Unknown filesystems cannot be represented as shrunken payloads");
}

void test_shrink_payload_encoding_is_canonical_and_raw_is_exact() {
  using namespace ytec::imageformat;
  auto manifest = exact_manifest();
  manifest.mode = TsumugiManifestMode::shrink;
  auto& partition = manifest.partitions[0];
  partition.minimum_target_bytes = 4ULL * 1024ULL * 1024ULL;
  partition.planned_target_bytes = 6ULL * 1024ULL * 1024ULL;
  partition.payload_logical_offset = 0U;
  partition.payload_logical_length = 4ULL * 1024ULL * 1024ULL;
  partition.payload_encoding =
      TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
  partition.payload_format_version = kTsumugiWimPayloadFormatVersion;
  partition.cluster_size = 4096U;
  const auto encoded = build_tsumugi_manifest_v1(manifest);
  check(encoded.has_value(), "A versioned WIM shrink payload should build");
  const auto inspected = inspect_tsumugi_manifest_v1(encoded.value());
  check(
      inspected.has_value() &&
          inspected.value().partitions[0].payload_encoding ==
              TsumugiManifestPayloadEncoding::microsoft_wim_single_image &&
          inspected.value().partitions[0].cluster_size == 4096U &&
          tsumugi_manifest_requires_shrink_archive_adapter(
              inspected.value()),
      "Encoding, version, cluster geometry, and adapter requirement must round-trip");

  partition.payload_logical_length += 13U;
  check(
      build_tsumugi_manifest_v1(manifest).has_value(),
      "A WIM byte stream may exceed target minimum and must retain its exact non-sector-aligned file length");
  partition.payload_logical_length -= 13U;

  partition.payload_format_version =
      kTsumugiWimPayloadFormatVersion + 1U;
  check(
      !build_tsumugi_manifest_v1(manifest).has_value(),
      "An unknown mandatory WIM payload version must fail closed");
  partition.payload_format_version = kTsumugiWimPayloadFormatVersion;
  partition.cluster_size = 0U;
  check(
      !build_tsumugi_manifest_v1(manifest).has_value(),
      "A WIM payload without bounded cluster geometry must fail closed");

  partition.payload_encoding = TsumugiManifestPayloadEncoding::exact_raw;
  partition.payload_format_version = 0U;
  partition.cluster_size = 0U;
  check(
      !build_tsumugi_manifest_v1(manifest).has_value(),
      "RAW fallback must not retain a shortened archive range");
  partition.minimum_target_bytes = partition.source_size;
  partition.planned_target_bytes = partition.source_size;
  partition.payload_logical_offset = partition.source_offset;
  partition.payload_logical_length = partition.source_size;
  check(
      build_tsumugi_manifest_v1(manifest).has_value() &&
          !tsumugi_manifest_requires_shrink_archive_adapter(manifest),
      "RAW fallback is valid only when the complete source extent is required");
  ++partition.payload_logical_offset;
  check(
      !build_tsumugi_manifest_v1(manifest).has_value(),
      "Exact RAW must remain aligned even though WIM is a byte stream");
}

void test_rescue_is_whole_disk_selection_only() {
  using namespace ytec::imageformat;
  auto manifest = exact_manifest();
  manifest.mode = TsumugiManifestMode::rescue;
  manifest.flags = TsumugiManifestFlags::source_contains_windows |
      TsumugiManifestFlags::partition_selection;
  TsumugiManifestPartition skipped{
      .source_table_index = 2,
      .source_partition_number = 2,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::none,
      .source_offset = 10ULL * 1024ULL * 1024ULL,
      .source_size = 4ULL * 1024ULL * 1024ULL,
      .used_bytes = 1ULL * 1024ULL * 1024ULL,
      .name_utf8 = "Data",
      .label_utf8 = "D",
  };
  skipped.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(skipped));
  check(!build_tsumugi_manifest_v1(manifest).has_value(),
        "Rescue mode must retain the whole source layout");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"exact_round_trip_is_canonical", test_exact_round_trip_is_canonical},
      {"unknown_and_reserved_bits_are_rejected",
       test_unknown_and_reserved_bits_are_rejected},
      {"shrink_selection_and_unselected_partition",
       test_shrink_selection_and_unselected_partition},
      {"rescue_forbids_resize_and_surplus_allocation",
       test_rescue_forbids_resize_and_surplus_allocation},
      {"snapshot_geometry_and_mbr_ids_are_bound",
       test_snapshot_geometry_and_mbr_ids_are_bound},
      {"invalid_utf8_and_required_unselected_fail",
       test_invalid_utf8_and_required_unselected_fail},
      {"selection_bitlocker_and_shrink_flags_are_canonical",
       test_selection_bitlocker_and_shrink_flags_are_canonical},
      {"shrink_payload_encoding_is_canonical_and_raw_is_exact",
       test_shrink_payload_encoding_is_canonical_and_raw_is_exact},
      {"rescue_is_whole_disk_selection_only",
       test_rescue_is_whole_disk_selection_only},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

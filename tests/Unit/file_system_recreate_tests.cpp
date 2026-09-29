#include "ytec/migrationcore/file_system_recreate.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint64_t kCanonicalTime = 119'600'064'000'000'000ULL +
    100ULL *
        ytec::migrationcore::kFileSystemRecreateTimestampQuantum100ns;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::migrationcore::FileSystemRecreateSha256 digest(
    const std::uint8_t seed) {
  ytec::migrationcore::FileSystemRecreateSha256 result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] = std::byte{
        static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index))};
  }
  return result;
}

ytec::migrationcore::FileSystemRecreateSha256 empty_sha256() {
  using Digest = ytec::migrationcore::FileSystemRecreateSha256;
  return Digest{
      std::byte{0xE3}, std::byte{0xB0}, std::byte{0xC4}, std::byte{0x42},
      std::byte{0x98}, std::byte{0xFC}, std::byte{0x1C}, std::byte{0x14},
      std::byte{0x9A}, std::byte{0xFB}, std::byte{0xF4}, std::byte{0xC8},
      std::byte{0x99}, std::byte{0x6F}, std::byte{0xB9}, std::byte{0x24},
      std::byte{0x27}, std::byte{0xAE}, std::byte{0x41}, std::byte{0xE4},
      std::byte{0x64}, std::byte{0x9B}, std::byte{0x93}, std::byte{0x4C},
      std::byte{0xA4}, std::byte{0x95}, std::byte{0x99}, std::byte{0x1B},
      std::byte{0x78}, std::byte{0x52}, std::byte{0xB8}, std::byte{0x55},
  };
}

std::string to_hex(
    const ytec::migrationcore::FileSystemRecreateSha256& value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2U);
  for (const std::byte item : value) {
    const auto byte = std::to_integer<std::uint8_t>(item);
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0FU]);
  }
  return result;
}

ytec::migrationcore::CanonicalFileSystemTreeEntry directory(
    std::wstring path) {
  using namespace ytec::migrationcore;
  return CanonicalFileSystemTreeEntry{
      .relative_path = std::move(path),
      .kind = FileSystemRecreateEntryKind::directory,
      .portable_attributes = recreate_attribute_read_only,
      .creation_time_utc_100ns = kCanonicalTime,
      .last_write_time_utc_100ns = kCanonicalTime,
      .hard_link_count = 1U,
      .opened_handle_identity_stable = true,
      .unnamed_stream_hashed_to_stable_eof = false,
      .namespace_supported = true,
  };
}

ytec::migrationcore::CanonicalFileSystemTreeEntry file(
    std::wstring path,
    const std::uint64_t size,
    const std::uint8_t hash_seed) {
  using namespace ytec::migrationcore;
  return CanonicalFileSystemTreeEntry{
      .relative_path = std::move(path),
      .kind = FileSystemRecreateEntryKind::regular_file,
      .size_bytes = size,
      .portable_attributes = recreate_attribute_archive,
      .creation_time_utc_100ns = kCanonicalTime,
      .last_write_time_utc_100ns = kCanonicalTime,
      .content_sha256 = size == 0U ? empty_sha256() : digest(hash_seed),
      .hard_link_count = 1U,
      .opened_handle_identity_stable = true,
      .unnamed_stream_hashed_to_stable_eof = true,
      .namespace_supported = true,
  };
}

ytec::migrationcore::CanonicalFileSystemTree fat32_tree() {
  using namespace ytec::migrationcore;
  return CanonicalFileSystemTree{
      .file_system = MigrationFileSystem::fat32,
      .source_table_index = 7U,
      .enumeration_epoch_sha256 = digest(0x40U),
      .namespace_fully_enumerated = true,
      .opened_handles_only = true,
      .every_regular_file_hashed_to_stable_eof = true,
      .short_name_aliases_collision_free = true,
      .entries = {
          directory(L"Docs"),
          file(L"Docs\\alpha.txt", 1'000U, 0x20U),
          file(L"empty.bin", 0U, 0U),
      },
  };
}

ytec::migrationcore::FileSystemRecreateFormatGeometry fat32_geometry() {
  using namespace ytec::migrationcore;
  return FileSystemRecreateFormatGeometry{
      .file_system = MigrationFileSystem::fat32,
      .target_volume_bytes = 4ULL * kGiB,
      .logical_sector_size = 512U,
      .cluster_size = 32ULL * 1024ULL,
  };
}

ytec::migrationcore::FileSystemRecreatePlanningRequest fat32_request() {
  using namespace ytec::migrationcore;
  return FileSystemRecreatePlanningRequest{
      .target_partition_number = 3U,
      .target_partition_offset_bytes = 1ULL * kMiB,
      .target_geometry = fat32_geometry(),
      .source_tree = fat32_tree(),
  };
}

void test_fat32_plan_binds_tree_geometry_and_capacity() {
  using namespace ytec::migrationcore;
  const auto result = plan_file_system_recreation(fat32_request());
  check(result.has_value(), "A canonical FAT32 tree should plan");
  const auto& plan = result.value();
  check(
      plan.source_table_index() == 7U &&
          plan.target_partition_number() == 3U &&
          plan.target_partition_offset_bytes() == 1ULL * kMiB &&
          plan.target_geometry().file_system == MigrationFileSystem::fat32 &&
          plan.entries().size() == 3U && plan.source_remains_unchanged(),
      "The immutable plan should bind source, target, geometry and entries");
  check(
      plan.capacity().total_content_bytes == 1'000U &&
          plan.capacity().regular_file_allocation_bytes == 32ULL * 1024ULL &&
          plan.capacity().directory_allocation_bytes == 64ULL * 1024ULL &&
          plan.capacity().namespace_record_bytes > 0U &&
          plan.capacity().conservative_format_overhead_bytes > 0U &&
          plan.capacity().minimum_free_reserve_bytes >= 16ULL * kMiB &&
          plan.capacity().minimum_required_volume_bytes < 4ULL * kGiB,
      "Capacity must include file clusters, directories, format and reserve");
  check(
      plan.canonical_manifest_sha256() !=
              FileSystemRecreateSha256{} &&
          plan.plan_sha256() != FileSystemRecreateSha256{},
      "Canonical manifest and full plan need non-zero SHA-256 bindings");
}

void test_canonical_hash_is_sealed_and_plan_hash_binds_epoch_and_target() {
  using namespace ytec::migrationcore;
  auto request = fat32_request();
  const auto first = plan_file_system_recreation(request);
  check(first.has_value(), "The sealed-hash fixture should plan");
  const std::string actual = to_hex(first.value().canonical_manifest_sha256());
  check(
      actual ==
          "0a8816c7dcf0c1e6ea24507dabc749d8d45e905b4a791673fa0e0882b352da6e",
      "Canonical fixture SHA-256 drifted; actual=" + actual);

  const auto repeated = plan_file_system_recreation(request);
  check(
      repeated.has_value() &&
          repeated.value().canonical_manifest_sha256() ==
              first.value().canonical_manifest_sha256() &&
          repeated.value().plan_sha256() == first.value().plan_sha256(),
      "The same canonical input must have deterministic hashes");

  request.source_tree.enumeration_epoch_sha256 = digest(0x60U);
  const auto new_epoch = plan_file_system_recreation(request);
  check(
      new_epoch.has_value() &&
          new_epoch.value().canonical_manifest_sha256() ==
              first.value().canonical_manifest_sha256() &&
          new_epoch.value().plan_sha256() != first.value().plan_sha256(),
      "Epoch must bind the plan without changing tree equivalence");

  request = fat32_request();
  request.target_partition_number = 4U;
  const auto new_target = plan_file_system_recreation(request);
  check(
      new_target.has_value() &&
          new_target.value().plan_sha256() != first.value().plan_sha256(),
      "The target partition must be part of the plan hash");

  request = fat32_request();
  request.source_tree.entries[1].content_sha256[0] = std::byte{0xFF};
  const auto new_content = plan_file_system_recreation(request);
  check(
      new_content.has_value() &&
          new_content.value().canonical_manifest_sha256() !=
              first.value().canonical_manifest_sha256(),
      "Any content digest change must change the canonical manifest hash");
}

void test_names_must_be_canonical_unique_ordered_and_parented() {
  using namespace ytec::migrationcore;
  auto request = fat32_request();
  std::swap(request.source_tree.entries[0], request.source_tree.entries[2]);
  check(
      !plan_file_system_recreation(request),
      "Unsorted entries must fail closed");

  request = fat32_request();
  request.source_tree.entries.insert(
      request.source_tree.entries.begin() + 1, directory(L"docs"));
  check(
      !plan_file_system_recreation(request),
      "Case-insensitive path collisions must fail");

  request = fat32_request();
  request.source_tree.entries.push_back(file(L"Missing\\child.bin", 1U, 1U));
  check(
      !plan_file_system_recreation(request),
      "A missing parent directory must fail");

  request = fat32_request();
  request.source_tree.entries = {
      file(L"Parent", 1U, 1U),
      file(L"Parent\\child.bin", 1U, 2U),
  };
  check(
      !plan_file_system_recreation(request),
      "A regular file cannot act as a parent");

  const std::vector<std::wstring> invalid_names{
      L"\\absolute.bin",
      L"dot..",
      L"name:stream",
      L"trailing. ",
      L"CON.txt",
      L"a\\..\\b",
  };
  for (const auto& invalid_name : invalid_names) {
    request = fat32_request();
    request.source_tree.entries = {file(invalid_name, 1U, 1U)};
    check(
        !plan_file_system_recreation(request),
        "An unsupported Windows namespace name must fail");
  }

  request = fat32_request();
  request.source_tree.entries = {
      file(std::wstring(1U, static_cast<wchar_t>(0xD800U)), 1U, 1U),
  };
  check(
      !plan_file_system_recreation(request),
      "An unpaired UTF-16 surrogate must fail");
}

void test_opened_handle_metadata_evidence_is_fail_closed() {
  using namespace ytec::migrationcore;
  auto request = fat32_request();
  request.source_tree.source_table_index = 0U;
  check(
      !plan_file_system_recreation(request),
      "A zero source partition-table index must fail");

  request = fat32_request();
  request.source_tree.namespace_fully_enumerated = false;
  check(!plan_file_system_recreation(request), "Partial enumeration must fail");

  request = fat32_request();
  request.source_tree.short_name_aliases_collision_free = false;
  check(!plan_file_system_recreation(request), "Short-name ambiguity must fail");

  request = fat32_request();
  request.source_tree.entries[1].alternate_data_stream_count = 1U;
  check(!plan_file_system_recreation(request), "ADS must fail");

  request = fat32_request();
  request.source_tree.entries[1].reparse_tag = 0xA000000CU;
  check(!plan_file_system_recreation(request), "A reparse point must fail");

  request = fat32_request();
  request.source_tree.entries[1].hard_link_count = 2U;
  check(!plan_file_system_recreation(request), "A hardlink must fail");

  request = fat32_request();
  request.source_tree.entries[1].opened_handle_identity_stable = false;
  check(!plan_file_system_recreation(request), "Unstable identity must fail");

  request = fat32_request();
  request.source_tree.entries[1].unnamed_stream_hashed_to_stable_eof = false;
  check(!plan_file_system_recreation(request), "A partial file hash must fail");

  request = fat32_request();
  request.source_tree.entries[1].portable_attributes = 0x1000U;
  check(!plan_file_system_recreation(request), "Unsupported attributes must fail");

  request = fat32_request();
  request.source_tree.entries[0].content_sha256 = digest(1U);
  check(!plan_file_system_recreation(request), "Directories cannot carry content");

  request = fat32_request();
  request.source_tree.entries[2].content_sha256 = digest(1U);
  check(
      !plan_file_system_recreation(request),
      "An empty file must carry the canonical SHA-256 of empty content");

  request = fat32_request();
  request.source_tree.entries[1].content_sha256 =
      FileSystemRecreateSha256{};
  check(
      !plan_file_system_recreation(request),
      "A non-empty file cannot use an all-zero uncomputed content digest");

  request = fat32_request();
  ++request.source_tree.entries[1].creation_time_utc_100ns;
  check(
      !plan_file_system_recreation(request),
      "A timestamp outside the canonical two-second quantum must fail");
}

void test_fat32_exfat_limits_geometry_capacity_and_overflow() {
  using namespace ytec::migrationcore;
  auto request = fat32_request();
  request.source_tree.entries = {
      file(L"too-large.bin", kFat32MaximumRecreatedFileBytes + 1U, 1U),
  };
  request.target_geometry.target_volume_bytes = 16ULL * kGiB;
  request.target_geometry.cluster_size = 64ULL * 1024ULL;
  check(
      !plan_file_system_recreation(request),
      "FAT32 must reject a file larger than 4 GiB minus one byte");

  request = fat32_request();
  request.source_tree.file_system = MigrationFileSystem::exfat;
  request.source_tree.entries = {file(L"large.bin", 5ULL * kGiB, 1U)};
  request.target_geometry = FileSystemRecreateFormatGeometry{
      .file_system = MigrationFileSystem::exfat,
      .target_volume_bytes = 8ULL * kGiB,
      .logical_sector_size = 4096U,
      .cluster_size = 128ULL * 1024ULL,
  };
  check(
      plan_file_system_recreation(request).has_value(),
      "exFAT should accept a file above the FAT32 maximum when capacity fits");

  request.target_geometry.target_volume_bytes = 1ULL * kGiB;
  check(
      !plan_file_system_recreation(request),
      "Payload plus conservative overhead must fit before any formatter starts");

  request = fat32_request();
  request.target_geometry.cluster_size = 128ULL * 1024ULL;
  check(
      !plan_file_system_recreation(request),
      "FAT32 clusters outside the supported geometry must fail");

  request = fat32_request();
  request.target_partition_offset_bytes = 1ULL * kMiB + 1U;
  check(
      !plan_file_system_recreation(request),
      "The immutable target offset must be sector aligned");

  request = fat32_request();
  request.source_tree.file_system = MigrationFileSystem::ntfs;
  request.target_geometry.file_system = MigrationFileSystem::ntfs;
  check(
      !plan_file_system_recreation(request),
      "This contract must never absorb the separately audited NTFS WIM route");

  request = fat32_request();
  request.source_tree.file_system = MigrationFileSystem::exfat;
  request.source_tree.entries = {
      file(L"a.bin", kExfatMaximumRecreatedFileBytes, 1U),
      file(L"b.bin", kExfatMaximumRecreatedFileBytes, 2U),
  };
  request.target_geometry = FileSystemRecreateFormatGeometry{
      .file_system = MigrationFileSystem::exfat,
      .target_volume_bytes = 8ULL * kGiB,
      .logical_sector_size = 512U,
      .cluster_size = 2ULL * kMiB,
  };
  const auto overflow = plan_file_system_recreation(request);
  check(
      !overflow.has_value() &&
          overflow.error().native_code == ERROR_ARITHMETIC_OVERFLOW,
      "Aggregate exFAT allocation overflow must be detected");
}

void test_target_readback_requires_exact_tree_content_and_binding() {
  using namespace ytec::migrationcore;
  // This is an in-memory adapter model only.  Production must obtain both
  // trees and actual format geometry from opened handles after independent
  // stable-disk/partition re-identification; the test never formats or opens a
  // disk and intentionally cannot prove that future Win32 connection.
  const auto planned = plan_file_system_recreation(fat32_request());
  check(planned.has_value(), "Readback fixture should plan");
  const auto& plan = planned.value();

  auto readback = FileSystemRecreateTargetReadback{
      .target_partition_number = plan.target_partition_number(),
      .target_partition_offset_bytes = plan.target_partition_offset_bytes(),
      .actual_geometry = plan.target_geometry(),
      .target_tree = fat32_tree(),
  };
  readback.target_tree.enumeration_epoch_sha256 = digest(0x80U);
  const auto verified = verify_recreated_file_system_tree(plan, readback);
  check(
      verified.has_value() &&
          verified.value().exact_tree_and_content_equivalence &&
          verified.value().directory_count == 1U &&
          verified.value().regular_file_count == 2U &&
          verified.value().regular_file_bytes_read == 1'000U &&
          verified.value().expected_manifest_sha256 ==
              verified.value().observed_manifest_sha256,
      "Complete target readback should prove exact tree and content equality");

  auto changed = readback;
  changed.target_tree.entries[1].content_sha256[0] = std::byte{0xFF};
  check(
      !verify_recreated_file_system_tree(plan, changed),
      "A content-hash mismatch must fail completion");

  changed = readback;
  changed.target_tree.entries[1].portable_attributes =
      recreate_attribute_hidden;
  check(
      !verify_recreated_file_system_tree(plan, changed),
      "A portable metadata mismatch must fail completion");

  changed = readback;
  changed.target_tree.namespace_fully_enumerated = false;
  check(
      !verify_recreated_file_system_tree(plan, changed),
      "Partial target namespace enumeration must fail completion");

  changed = readback;
  ++changed.target_partition_number;
  check(
      !verify_recreated_file_system_tree(plan, changed),
      "A different target partition must fail before comparison");

  changed = readback;
  changed.actual_geometry.cluster_size = 64ULL * 1024ULL;
  check(
      !verify_recreated_file_system_tree(plan, changed),
      "Actual format geometry drift must fail completion");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"fat32_plan_binds_tree_geometry_and_capacity",
       test_fat32_plan_binds_tree_geometry_and_capacity},
      {"canonical_hash_is_sealed_and_plan_hash_binds_epoch_and_target",
       test_canonical_hash_is_sealed_and_plan_hash_binds_epoch_and_target},
      {"names_must_be_canonical_unique_ordered_and_parented",
       test_names_must_be_canonical_unique_ordered_and_parented},
      {"opened_handle_metadata_evidence_is_fail_closed",
       test_opened_handle_metadata_evidence_is_fail_closed},
      {"fat32_exfat_limits_geometry_capacity_and_overflow",
       test_fat32_exfat_limits_geometry_capacity_and_overflow},
      {"target_readback_requires_exact_tree_content_and_binding",
       test_target_readback_requires_exact_tree_content_and_binding},
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

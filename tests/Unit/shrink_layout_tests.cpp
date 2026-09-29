#include "ytec/migrationcore/shrink_layout.h"

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

ytec::migrationcore::ShrinkMigrationRequest gpt_windows_request(
    const std::uint64_t target_size = 500ULL * kGiB) {
  using namespace ytec::migrationcore;
  return ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::gpt,
      .target_style = MigrationPartitionStyle::gpt,
      .target_size_bytes = target_size,
      .target_logical_sector_size = 512,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 0,
              .role = MigrationPartitionRole::efi_system,
              .file_system = MigrationFileSystem::fat32,
              .source_size_bytes = 200ULL * kMiB,
          },
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::microsoft_reserved,
              .file_system = MigrationFileSystem::none,
              .source_size_bytes = 16ULL * kMiB,
          },
          ShrinkSourcePartition{
              .source_table_index = 2,
              .role = MigrationPartitionRole::windows,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 998ULL * kGiB,
              .used_bytes = 300ULL * kGiB,
              .cluster_size = 4096,
              .label = L"Windows",
          },
          ShrinkSourcePartition{
              .source_table_index = 3,
              .role = MigrationPartitionRole::recovery,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 1ULL * kGiB,
              .used_bytes = 600ULL * kMiB,
              .cluster_size = 4096,
              .label = L"Recovery",
          },
      },
  };
}

void test_one_tb_300_used_fits_500_gib_target() {
  using namespace ytec::migrationcore;
  const auto result = plan_shrink_migration(gpt_windows_request());
  check(result.has_value(), "1 TB / 300 GiB used should fit a 500 GiB target");
  check(
      result.value().source_remains_unchanged &&
          result.value().boot_finalization_required &&
          result.value().minimum_target_size_bytes < 500ULL * kGiB &&
          result.value().target_partitions.size() == 4,
      "Windows GPT plan should recreate ESP/MSR and retain Windows/recovery");
  const auto& partitions = result.value().target_partitions;
  check(
      partitions[0].role == MigrationPartitionRole::efi_system &&
          partitions[1].role == MigrationPartitionRole::microsoft_reserved &&
          partitions[2].role == MigrationPartitionRole::windows &&
          partitions[3].role == MigrationPartitionRole::recovery &&
          partitions[2].size_bytes > 300ULL * kGiB,
      "Windows target order and free-space reserve should be deterministic");
}

void test_target_below_used_plus_reserve_fails_closed() {
  const auto result = ytec::migrationcore::plan_shrink_migration(
      gpt_windows_request(305ULL * kGiB));
  check(!result.has_value(), "A target below used bytes plus reserve must fail");
  check(
      result.error().native_code == ERROR_DISK_FULL,
      "Capacity failure should report disk full before any I/O");
}

void test_gpt_data_disk_uses_space_without_boot_partitions() {
  using namespace ytec::migrationcore;
  const auto result = plan_shrink_migration(ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::gpt,
      .target_style = MigrationPartitionStyle::gpt,
      .target_size_bytes = 250ULL * kGiB,
      .target_logical_sector_size = 512,
      .source_is_windows_system = false,
      .windows_is_amd64 = false,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 0,
              .role = MigrationPartitionRole::microsoft_reserved,
              .file_system = MigrationFileSystem::none,
              .source_size_bytes = 16ULL * kMiB,
          },
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::data,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 600ULL * kGiB,
              .used_bytes = 100ULL * kGiB,
              .cluster_size = 4096,
              .label = L"DATA-1",
          },
          ShrinkSourcePartition{
              .source_table_index = 2,
              .role = MigrationPartitionRole::data,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 400ULL * kGiB,
              .used_bytes = 50ULL * kGiB,
              .cluster_size = 4096,
              .label = L"DATA-2",
          },
      },
  });
  check(result.has_value(), "Two basic NTFS data volumes should be shrinkable");
  check(
      !result.value().boot_finalization_required &&
          result.value().target_partitions.size() == 3 &&
          result.value().target_partitions[0].role ==
              MigrationPartitionRole::microsoft_reserved &&
          result.value().target_partitions[1].role ==
              MigrationPartitionRole::data &&
          result.value().target_partitions[2].role ==
              MigrationPartitionRole::data,
      "Data-only GPT should preserve data/MSR roles and skip boot finalization");
}

void test_mbr_system_reserved_is_preserved_as_active_content() {
  using namespace ytec::migrationcore;
  const auto result = plan_shrink_migration(ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::mbr,
      .target_style = MigrationPartitionStyle::mbr,
      .target_size_bytes = 200ULL * kGiB,
      .target_logical_sector_size = 512,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::windows,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 450ULL * kGiB,
              .used_bytes = 100ULL * kGiB,
              .cluster_size = 4096,
              .label = L"Windows",
          },
          ShrinkSourcePartition{
              .source_table_index = 0,
              .role = MigrationPartitionRole::bios_system,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 500ULL * kMiB,
              .used_bytes = 80ULL * kMiB,
              .cluster_size = 4096,
              .label = L"System Reserved",
              .active = true,
          },
      },
  });
  check(result.has_value(), "Common BIOS System Reserved layout should plan");
  check(
      result.value().target_partitions[0].role ==
              MigrationPartitionRole::bios_system &&
          result.value().target_partitions[0].active &&
          result.value().target_partitions[1].role ==
              MigrationPartitionRole::windows,
      "BIOS system partition should stay first and active");
}

void test_encrypted_or_cross_style_request_is_rejected() {
  auto request = gpt_windows_request();
  request.bitlocker_fully_decrypted = false;
  check(
      !ytec::migrationcore::plan_shrink_migration(request).has_value(),
      "BitLocker must be fully decrypted");
  request = gpt_windows_request();
  request.target_style = ytec::migrationcore::MigrationPartitionStyle::mbr;
  check(
      !ytec::migrationcore::plan_shrink_migration(request).has_value(),
      "Shrink mode must not silently combine format conversion");
}

void test_exfat_and_fat32_are_file_archive_capable() {
  using namespace ytec::migrationcore;
  check(
      classify_shrink_file_system(MigrationFileSystem::ntfs) ==
              ShrinkFileSystemDisposition::file_archive &&
          classify_shrink_file_system(MigrationFileSystem::exfat) ==
              ShrinkFileSystemDisposition::file_archive &&
          classify_shrink_file_system(MigrationFileSystem::fat32) ==
              ShrinkFileSystemDisposition::file_archive &&
          classify_shrink_file_system(MigrationFileSystem::unsupported) ==
              ShrinkFileSystemDisposition::exact_raw_only,
      "Only the three supported filesystems should use file archives");

  const auto result = plan_shrink_migration(ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::gpt,
      .target_style = MigrationPartitionStyle::gpt,
      .target_size_bytes = 80ULL * kGiB,
      .target_logical_sector_size = 512,
      .source_is_windows_system = false,
      .windows_is_amd64 = false,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::data,
              .file_system = MigrationFileSystem::exfat,
              .source_size_bytes = 200ULL * kGiB,
              .used_bytes = 20ULL * kGiB,
              .cluster_size = 128ULL * 1024ULL,
              .label = L"MEDIA",
          },
          ShrinkSourcePartition{
              .source_table_index = 2,
              .role = MigrationPartitionRole::data,
              .file_system = MigrationFileSystem::fat32,
              .source_size_bytes = 64ULL * kGiB,
              .used_bytes = 8ULL * kGiB,
              .cluster_size = 32ULL * 1024ULL,
              .label = L"TRANSFER",
          },
      },
  });
  check(result.has_value(), "exFAT and FAT32 should enter file-archive planning");
  check(
      result.value().target_partitions[0].file_system ==
              MigrationFileSystem::exfat &&
          result.value().target_partitions[0].action ==
              MigrationPartitionAction::apply_file_image &&
          result.value().target_partitions[1].file_system ==
              MigrationFileSystem::fat32,
      "The target plan must retain each supported filesystem");
}

void test_unsupported_file_system_requires_original_raw_size() {
  using namespace ytec::migrationcore;
  auto request = ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::mbr,
      .target_style = MigrationPartitionStyle::mbr,
      .target_size_bytes = 100ULL * kGiB,
      .target_logical_sector_size = 512,
      .source_is_windows_system = false,
      .windows_is_amd64 = false,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::data,
              .file_system = MigrationFileSystem::unsupported,
              .source_size_bytes = 120ULL * kGiB,
              .used_bytes = 10ULL * kGiB,
          },
      },
  };
  auto result = plan_shrink_migration(request);
  check(
      !result.has_value() && result.error().native_code == ERROR_DISK_FULL,
      "Used bytes must never make an unsupported filesystem look shrinkable");

  request.target_size_bytes = 140ULL * kGiB;
  result = plan_shrink_migration(request);
  check(
      result.has_value() &&
          result.value().target_partitions[0].action ==
              MigrationPartitionAction::copy_exact_raw &&
          result.value().target_partitions[0].size_bytes == 120ULL * kGiB,
      "An unsupported filesystem must retain its complete RAW extent");
}

void test_mbr_to_gpt_and_surplus_policy_are_explicit() {
  using namespace ytec::migrationcore;
  auto request = ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::mbr,
      .target_style = MigrationPartitionStyle::gpt,
      .target_size_bytes = 300ULL * kGiB,
      .target_logical_sector_size = 512,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation = ShrinkSurplusAllocation::leave_unallocated,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 1,
              .role = MigrationPartitionRole::bios_system,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 500ULL * kMiB,
              .used_bytes = 80ULL * kMiB,
              .cluster_size = 4096,
              .active = true,
          },
          ShrinkSourcePartition{
              .source_table_index = 2,
              .role = MigrationPartitionRole::windows,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 500ULL * kGiB,
              .used_bytes = 100ULL * kGiB,
              .cluster_size = 4096,
          },
      },
  };
  const auto unallocated = plan_shrink_migration(request);
  check(unallocated.has_value(), "MBR system disks should support MBR-to-GPT planning");
  check(
      unallocated.value().target_style == MigrationPartitionStyle::gpt &&
          unallocated.value().target_partitions.size() == 3U &&
          unallocated.value().target_partitions[0].role ==
              MigrationPartitionRole::efi_system &&
          unallocated.value().target_partitions[1].role ==
              MigrationPartitionRole::microsoft_reserved &&
          unallocated.value().target_partitions[2].role ==
              MigrationPartitionRole::windows &&
          unallocated.value().unallocated_tail_bytes > 100ULL * kGiB,
      "MBR boot content should be replaced by ESP/MSR while surplus stays unallocated");

  request.surplus_allocation =
      ShrinkSurplusAllocation::automatic_proportional;
  const auto automatic = plan_shrink_migration(request);
  check(
      automatic.has_value() &&
          automatic.value().unallocated_tail_bytes < 1ULL * kMiB &&
          request.target_size_bytes -
                  (automatic.value().target_partitions.back().offset_bytes +
                   automatic.value().target_partitions.back().size_bytes) >=
              2ULL * kMiB &&
          automatic.value().target_partitions.back().size_bytes >
              unallocated.value().target_partitions.back().size_bytes,
      "Automatic allocation should consume aligned surplus while reserving "
      "the transient GPT tail on expandable volumes");
}

void test_partition_count_and_mbr_lba_limits_fail_closed() {
  using namespace ytec::migrationcore;
  auto request = ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::mbr,
      .target_style = MigrationPartitionStyle::mbr,
      .target_size_bytes = 100ULL * kGiB,
      .target_logical_sector_size = 512U,
      .source_is_windows_system = false,
      .windows_is_amd64 = false,
      .bitlocker_fully_decrypted = true,
  };
  for (std::uint32_t index = 1U; index <= 5U; ++index) {
    request.source_partitions.push_back(ShrinkSourcePartition{
        .source_table_index = index,
        .role = MigrationPartitionRole::data,
        .file_system = MigrationFileSystem::ntfs,
        .source_size_bytes = 10ULL * kGiB,
        .used_bytes = 1ULL * kGiB,
        .cluster_size = 4096U,
    });
  }
  check(
      !plan_shrink_migration(request).has_value(),
      "More than four MBR basic partitions must fail closed");

  request.source_partitions.resize(1U);
  request.target_size_bytes = (1ULL << 32U) * 512ULL + 512ULL;
  check(
      !plan_shrink_migration(request).has_value(),
      "An MBR target beyond the 32-bit LBA range must fail closed");
}

void test_authenticated_minimum_floor_can_only_raise_capacity() {
  using namespace ytec::migrationcore;
  auto request = ShrinkMigrationRequest{
      .source_style = MigrationPartitionStyle::mbr,
      .target_style = MigrationPartitionStyle::mbr,
      .target_size_bytes = 20ULL * kGiB,
      .target_logical_sector_size = 512U,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation = ShrinkSurplusAllocation::leave_unallocated,
      .source_partitions = {
          ShrinkSourcePartition{
              .source_table_index = 1U,
              .role = MigrationPartitionRole::data,
              .file_system = MigrationFileSystem::ntfs,
              .source_size_bytes = 8ULL * kGiB,
              .used_bytes = 1ULL * kGiB,
              .minimum_target_bytes = 6ULL * kGiB,
              .cluster_size = 4096U,
          },
      },
  };
  const auto archive = plan_shrink_migration(request);
  check(
      archive.has_value() &&
          archive.value().target_partitions[0].size_bytes == 6ULL * kGiB,
      "an authenticated archive floor must raise the generic reserve");

  request.source_partitions[0].file_system =
      MigrationFileSystem::unsupported;
  request.source_partitions[0].minimum_target_bytes = 6ULL * kGiB;
  check(
      !plan_shrink_migration(request).has_value(),
      "an exact RAW floor smaller than its source extent must be rejected");
  request.source_partitions[0].minimum_target_bytes = 8ULL * kGiB;
  const auto raw = plan_shrink_migration(request);
  check(
      raw.has_value() &&
          raw.value().target_partitions[0].size_bytes == 8ULL * kGiB,
      "exact RAW must accept only the full authenticated source extent");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"one_tb_300_used_fits_500_gib_target",
       test_one_tb_300_used_fits_500_gib_target},
      {"target_below_used_plus_reserve_fails_closed",
       test_target_below_used_plus_reserve_fails_closed},
      {"gpt_data_disk_uses_space_without_boot_partitions",
       test_gpt_data_disk_uses_space_without_boot_partitions},
      {"mbr_system_reserved_is_preserved_as_active_content",
       test_mbr_system_reserved_is_preserved_as_active_content},
      {"encrypted_or_cross_style_request_is_rejected",
       test_encrypted_or_cross_style_request_is_rejected},
      {"exfat_and_fat32_are_file_archive_capable",
       test_exfat_and_fat32_are_file_archive_capable},
      {"unsupported_file_system_requires_original_raw_size",
       test_unsupported_file_system_requires_original_raw_size},
      {"mbr_to_gpt_and_surplus_policy_are_explicit",
       test_mbr_to_gpt_and_surplus_policy_are_explicit},
      {"partition_count_and_mbr_lba_limits_fail_closed",
       test_partition_count_and_mbr_lba_limits_fail_closed},
      {"authenticated_minimum_floor_can_only_raise_capacity",
       test_authenticated_minimum_floor_can_only_raise_capacity},
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

#include "ytec/windowsapp/online_shrink_image_plan.h"

#include "ytec/imageformat/partition_snapshot.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint64_t kDiskBytes = 64ULL * kGiB;

void check(const bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::StableDiskIdentity source_identity(const bool system) {
  return {
      .disk_number = 4U,
      .model = L"Synthetic Tsumugi Source",
      .size_bytes = kDiskBytes,
      .logical_sector_size = 512U,
      .serial_suffix = "A1B2C3D4",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_SYNTHETIC\\4",
      .is_system_disk = system,
  };
}

std::vector<std::byte> mbr_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  ytec::imageformat::PartitionTableRegion region{
      .disk_offset = 0U,
      .data = std::vector<std::byte>(512U, std::byte{0}),
  };
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));
  auto built = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(built.has_value(), "MBR snapshot fixture should build");
  return built.take_value();
}

std::array<std::byte, 16U> mbr_type(const std::uint8_t value) {
  std::array<std::byte, 16U> result{};
  result[0] = static_cast<std::byte>(value);
  return result;
}

ytec::windowsshrink::ShrinkSourceAnalysis system_analysis() {
  using namespace ytec;
  windowsshrink::ShrinkSourceAnalysis analysis{
      .source = source_identity(true),
      .physical_sector_size = 4096U,
      .partition_style = migrationcore::MigrationPartitionStyle::mbr,
      .windows_version = windowsshrink::WindowsSourceVersion{
          .major = 10U,
          .build = 26100U,
          .architecture = "AMD64",
      },
      .bitlocker_fully_decrypted = true,
      .created_utc = "2026-08-09T02:30:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = mbr_snapshot(),
  };
  analysis.partitions = {
      windowsshrink::AnalyzedShrinkPartition{
          .source_table_index = 1U,
          .role = migrationcore::MigrationPartitionRole::bios_system,
          .file_system = migrationcore::MigrationFileSystem::ntfs,
          .source_offset_bytes = 1ULL * kMiB,
          .source_size_bytes = 512ULL * kMiB,
          .used_bytes = 90ULL * kMiB,
          .cluster_size = 4096U,
          .active = true,
          .label = L"System Reserved",
          .type_id = mbr_type(0x07U),
      },
      windowsshrink::AnalyzedShrinkPartition{
          .source_table_index = 2U,
          .role = migrationcore::MigrationPartitionRole::windows,
          .file_system = migrationcore::MigrationFileSystem::ntfs,
          .source_offset_bytes = 513ULL * kMiB,
          .source_size_bytes = 50ULL * kGiB,
          .used_bytes = 20ULL * kGiB,
          .cluster_size = 4096U,
          .label = L"Windows 日本語",
          .type_id = mbr_type(0x07U),
      },
      windowsshrink::AnalyzedShrinkPartition{
          .source_table_index = 3U,
          .role = migrationcore::MigrationPartitionRole::recovery,
          .file_system = migrationcore::MigrationFileSystem::unsupported,
          .source_offset_bytes = 52ULL * kGiB,
          .source_size_bytes = 1ULL * kGiB,
          .used_bytes = 1ULL * kGiB,
          .type_id = mbr_type(0x27U),
      },
  };
  analysis.content_volumes = {
      {.source_table_index = 1U,
       .volume_guid_path =
           L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\"},
      {.source_table_index = 2U,
       .volume_guid_path =
           L"\\\\?\\Volume{22222222-2222-2222-2222-222222222222}\\"},
  };
  return analysis;
}

ytec::windowsapp::WindowsOnlineShrinkImagePlanRequest request_fixture() {
  return {
      .analysis = system_analysis(),
      .final_path = L"D:\\Images\\system-shrink.tsumugi",
      .storage_file_system =
          ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
      .administrator = true,
  };
}

bool has_flag(
    const ytec::imageformat::TsumugiManifestFlags value,
    const ytec::imageformat::TsumugiManifestFlags flag) {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

bool has_flag(
    const ytec::imageformat::TsumugiManifestPartitionFlags value,
    const ytec::imageformat::TsumugiManifestPartitionFlags flag) {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

void test_system_plan_binds_vss_manifest_and_capture() {
  std::string password = "Correct Horse Battery Staple";
  auto request = request_fixture();
  request.encryption_password = password;
  request.verification_mode =
      ytec::imageformat::TsumugiCreateVerificationMode::fast;
  auto result = ytec::windowsapp::plan_windows_online_shrink_image(request);
  check(result.has_value(), "system shrink plan should build");
  const auto& plan = result.value();
  const auto& manifest = plan.image_template.manifest;
  check(plan.workflow.administrator, "workflow must remain elevated");
  check(plan.workflow.volumes.size() == 2U,
        "every NTFS partition must join one Snapshot Set");
  check(plan.capture.snapshot_volumes.size() == 2U,
        "capture plan must bind every NTFS Volume GUID");
  check(manifest.partitions.size() == 3U,
        "all reviewed partitions must be selected");
  check(has_flag(
            manifest.flags,
            ytec::imageformat::
                TsumugiManifestFlags::source_contains_windows),
        "system image must carry source_contains_windows");
  check(has_flag(
            manifest.flags,
            ytec::imageformat::
                TsumugiManifestFlags::automatic_surplus_allocation),
        "automatic surplus allocation must be the default");
  check(has_flag(
            manifest.partitions[1].flags,
            ytec::imageformat::
                TsumugiManifestPartitionFlags::contains_windows),
        "one Windows partition must be explicit");
  check(has_flag(
            manifest.partitions[0].flags,
            ytec::imageformat::
                TsumugiManifestPartitionFlags::required) &&
            has_flag(
                manifest.partitions[2].flags,
                ytec::imageformat::
                    TsumugiManifestPartitionFlags::required),
        "system and recovery partitions must be unselectable requirements");
  check(manifest.partitions[1].minimum_target_bytes >
            manifest.partitions[1].used_bytes &&
            manifest.partitions[1].minimum_target_bytes <
                manifest.partitions[1].source_size,
        "Windows target floor must include reserve but permit shrink");
  check(manifest.partitions[2].file_system ==
            ytec::imageformat::TsumugiManifestFileSystem::unknown &&
            manifest.partitions[2].minimum_target_bytes ==
                manifest.partitions[2].source_size,
        "unprobed static partition must remain exact RAW sized");
  check(plan.image_template.encryption.has_value() &&
            plan.image_template.encryption->password == password,
        "password view must be passed only to the active image request");
  check(plan.image_template.verification_mode ==
            ytec::imageformat::TsumugiCreateVerificationMode::fast,
        "the reviewed shrink plan must preserve an explicit fast verification choice");
  check(plan.image_template.chunks.empty() &&
            plan.image_template.source_session == nullptr,
        "payload and source generation must remain unset until VSS callback");
  check(plan.capture.reviewed_manifest.partitions.size() ==
            plan.image_template.manifest.partitions.size(),
        "capture and writer must share the reviewed partition count");
  for (std::size_t index = 0U;
       index < plan.capture.reviewed_manifest.partitions.size();
       ++index) {
    const auto& capture = plan.capture.reviewed_manifest.partitions[index];
    const auto& writer = plan.image_template.manifest.partitions[index];
    check(capture.source_table_index == writer.source_table_index &&
              capture.source_offset == writer.source_offset &&
              capture.source_size == writer.source_size &&
              capture.flags == writer.flags &&
              capture.file_system == writer.file_system,
          "capture and writer must share the exact reviewed partition set");
  }
}

void test_data_plan_has_no_windows_or_required_flags() {
  auto request = request_fixture();
  request.analysis = {};
  request.analysis.source = source_identity(false);
  request.analysis.physical_sector_size = 4096U;
  request.analysis.partition_style =
      ytec::migrationcore::MigrationPartitionStyle::mbr;
  request.analysis.bitlocker_fully_decrypted = true;
  request.analysis.created_utc = "2026-08-09T03:00:00Z";
  request.analysis.app_version = "1.0.0";
  request.analysis.partition_snapshot = mbr_snapshot();
  request.analysis.partitions = {
      ytec::windowsshrink::AnalyzedShrinkPartition{
          .source_table_index = 1U,
          .role = ytec::migrationcore::MigrationPartitionRole::data,
          .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
          .source_offset_bytes = 1ULL * kMiB,
          .source_size_bytes = 40ULL * kGiB,
          .used_bytes = 8ULL * kGiB,
          .cluster_size = 4096U,
          .label = L"Data",
          .type_id = mbr_type(0x07U),
      },
  };
  request.analysis.content_volumes = {
      {.source_table_index = 1U,
       .volume_guid_path =
           L"\\\\?\\Volume{AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA}\\"},
  };
  auto result = ytec::windowsapp::plan_windows_online_shrink_image(request);
  check(result.has_value(), "data-only shrink plan should build");
  check(!has_flag(
            result.value().image_template.manifest.flags,
            ytec::imageformat::
                TsumugiManifestFlags::source_contains_windows),
        "data image must not claim Windows");
  check(!has_flag(
            result.value().image_template.manifest.partitions[0].flags,
            ytec::imageformat::
                TsumugiManifestPartitionFlags::required),
        "data partition must not be forced by Windows dependency rules");
}

void test_rejects_missing_or_duplicate_volume_mapping() {
  auto missing = request_fixture();
  missing.analysis.content_volumes.pop_back();
  check(!ytec::windowsapp::plan_windows_online_shrink_image(missing),
        "missing NTFS Volume GUID must fail closed");

  auto duplicate = request_fixture();
  duplicate.analysis.content_volumes[1].source_table_index = 1U;
  check(!ytec::windowsapp::plan_windows_online_shrink_image(duplicate),
        "duplicate NTFS mapping must fail closed");
}

void test_rejects_unsupported_live_filesystem_and_overlap() {
  auto unsupported = request_fixture();
  unsupported.analysis.partitions[1].file_system =
      ytec::migrationcore::MigrationFileSystem::exfat;
  check(!ytec::windowsapp::plan_windows_online_shrink_image(unsupported),
        "Windows VSS slice must not pretend exFAT capture support");

  auto overlap = request_fixture();
  overlap.analysis.partitions[1].source_offset_bytes = 256ULL * kMiB;
  check(!ytec::windowsapp::plan_windows_online_shrink_image(overlap),
        "overlapping reviewed extents must fail before VSS");
}

void test_rejects_snapshot_and_destination_mismatch() {
  auto snapshot = request_fixture();
  snapshot.analysis.source.size_bytes -= 512U;
  check(!ytec::windowsapp::plan_windows_online_shrink_image(snapshot),
        "snapshot/source size mismatch must fail");

  auto storage = request_fixture();
  storage.storage_file_system =
      ytec::imageformat::TsumugiImageStorageFileSystem::fat32;
  check(!ytec::windowsapp::plan_windows_online_shrink_image(storage),
        "FAT32 destination must fail before source capture");
}

}  // namespace

int main() {
  struct Test final {
    const char* name;
    void (*run)();
  };
  const std::array tests{
      Test{"system_plan_binds_vss_manifest_and_capture",
           test_system_plan_binds_vss_manifest_and_capture},
      Test{"data_plan_has_no_windows_or_required_flags",
           test_data_plan_has_no_windows_or_required_flags},
      Test{"rejects_missing_or_duplicate_volume_mapping",
           test_rejects_missing_or_duplicate_volume_mapping},
      Test{"rejects_unsupported_live_filesystem_and_overlap",
           test_rejects_unsupported_live_filesystem_and_overlap},
      Test{"rejects_snapshot_and_destination_mismatch",
           test_rejects_snapshot_and_destination_mismatch},
  };
  std::size_t failures{};
  for (const auto& test : tests) {
    try {
      test.run();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
    }
  }
  return failures == 0U ? 0 : 1;
}

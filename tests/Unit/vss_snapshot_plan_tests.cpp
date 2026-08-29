#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/vssrequester/snapshot_metadata.h"
#include "ytec/vssrequester/snapshot_plan.h"

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
constexpr std::uint64_t kDiskSize = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kSectorCount = kDiskSize / kSectorSize;

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

class MemoryReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit MemoryReader(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::io_failed,
              .native_code = ERROR_READ_FAULT,
              .operation = L"合成VSS計画読取り",
              .message = L"範囲外です",
          });
    }
    reads_.emplace_back(offset, length);
    const auto first =
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

  void clear_reads() const { reads_.clear(); }

  [[nodiscard]] const std::vector<std::pair<std::uint64_t, std::size_t>>&
  reads() const noexcept {
    return reads_;
  }

 private:
  std::vector<std::byte> bytes_;
  mutable std::vector<std::pair<std::uint64_t, std::size_t>> reads_;
};

class GuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    ytec::clonecore::GptGuid value;
    value.bytes[0] = std::byte{next_++};
    value.bytes[15] = std::byte{0xA5};
    return ytec::clonecore::Result<
        ytec::clonecore::GptGuid>::success(value);
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

void write_ntfs_boot(
    std::vector<std::byte>& disk,
    const std::uint64_t first_lba,
    const std::uint64_t sector_count) {
  const std::size_t offset =
      static_cast<std::size_t>(first_lba * kSectorSize);
  constexpr char kNtfsSignature[] = "NTFS    ";
  std::memcpy(disk.data() + offset + 3, kNtfsSignature, 8);
  write_little<std::uint16_t>(disk, offset + 11, kSectorSize);
  disk[offset + 13] = std::byte{8};
  write_little<std::uint64_t>(disk, offset + 40, sector_count);
  disk[offset + 510] = std::byte{0x55};
  disk[offset + 511] = std::byte{0xAA};
}

void write_fat32_boot(
    std::vector<std::byte>& disk,
    const std::uint64_t first_lba,
    const std::uint32_t sector_count) {
  const std::size_t offset =
      static_cast<std::size_t>(first_lba * kSectorSize);
  constexpr char kFat32Signature[] = "FAT32   ";
  write_little<std::uint16_t>(disk, offset + 11, kSectorSize);
  disk[offset + 13] = std::byte{8};
  write_little<std::uint32_t>(disk, offset + 32, sector_count);
  std::memcpy(disk.data() + offset + 82, kFat32Signature, 8);
  disk[offset + 510] = std::byte{0x55};
  disk[offset + 511] = std::byte{0xAA};
}

struct GptFixture final {
  std::vector<std::byte> bytes;
  ytec::clonecore::GptDisk layout;
};

GptFixture make_gpt_fixture() {
  ytec::clonecore::GptDisk source{
      .logical_sector_size = kSectorSize,
      .sector_count = kSectorCount,
      .disk_guid = guid(0x10),
      .first_usable_lba = 34,
      .last_usable_lba = kSectorCount - 34,
      .partition_entry_count = 128,
      .partition_entry_size = 128,
      .partitions = {
          ytec::clonecore::GptPartition{
              .entry_index = 0,
              .type_guid = ytec::clonecore::gpt_type_efi_system(),
              .unique_guid = guid(0x20),
              .first_lba = 2048,
              .last_lba = 3071,
              .name = u"EFI",
          },
          ytec::clonecore::GptPartition{
              .entry_index = 1,
              .type_guid = ytec::clonecore::gpt_type_microsoft_reserved(),
              .unique_guid = guid(0x21),
              .first_lba = 3072,
              .last_lba = 3327,
              .name = u"MSR",
          },
          ytec::clonecore::GptPartition{
              .entry_index = 2,
              .type_guid = ytec::clonecore::gpt_type_basic_data(),
              .unique_guid = guid(0x22),
              .first_lba = 4096,
              .last_lba = 16383,
              .name = u"Windows",
          },
          ytec::clonecore::GptPartition{
              .entry_index = 3,
              .type_guid = ytec::clonecore::gpt_type_windows_recovery(),
              .unique_guid = guid(0x23),
              .first_lba = 18432,
              .last_lba = 20479,
              .name = u"Recovery",
          },
      },
  };
  GuidGenerator generator;
  const auto plan = ytec::clonecore::make_gpt_write_plan(
      source, kDiskSize, kSectorSize, generator);
  check(plan.has_value(), "Synthetic GPT should build");
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  for (const auto& write : plan.value().writes) {
    std::copy(
        write.bytes.begin(),
        write.bytes.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
  write_fat32_boot(bytes, 2048, 1024);
  write_ntfs_boot(bytes, 4096, 12288);
  write_ntfs_boot(bytes, 18432, 2048);
  return GptFixture{
      .bytes = std::move(bytes),
      .layout = plan.value().target_disk,
  };
}

std::vector<std::byte> make_mbr_disk() {
  std::vector<std::byte> bytes(
      static_cast<std::size_t>(kDiskSize), std::byte{0});
  write_little<std::uint32_t>(bytes, 440, 0x1234ABCDU);
  const auto partition = [&](const std::size_t index,
                             const bool active,
                             const std::uint8_t type,
                             const std::uint32_t first_lba,
                             const std::uint32_t sectors) {
    const std::size_t offset = 446 + index * 16;
    bytes[offset] = active ? std::byte{0x80} : std::byte{0};
    bytes[offset + 4] = static_cast<std::byte>(type);
    write_little(bytes, offset + 8, first_lba);
    write_little(bytes, offset + 12, sectors);
  };
  partition(0, false, 0x0C, 2048, 1024);
  partition(1, true, 0x07, 4096, 12288);
  partition(2, false, 0x27, 18432, 2048);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};
  write_fat32_boot(bytes, 2048, 1024);
  write_ntfs_boot(bytes, 4096, 12288);
  write_ntfs_boot(bytes, 18432, 2048);
  return bytes;
}

ytec::vssrequester::SnapshotImagePlanOptions options(
    const ytec::clonecore::ISourceDiskReader& reader,
    const ytec::imageformat::PartitionTableStyle style,
    const std::uint32_t physical_sector_size = 4096U) {
  const auto snapshot =
      ytec::imageformat::capture_partition_snapshot_v1(reader, style);
  check(snapshot.has_value(), "Fixture partition snapshot should build");
  using ytec::imageformat::BackupFileSystem;
  using ytec::imageformat::BackupManifestPartition;
  using ytec::imageformat::BackupPartitionRole;
  ytec::imageformat::BackupImageManifest backup;
  backup.source = ytec::clonecore::StableDiskIdentity{
      .disk_number = 0,
      .model = L"VSS PLAN FIXTURE",
      .size_bytes = reader.size_bytes(),
      .logical_sector_size = reader.logical_sector_size(),
      .serial_suffix = "PLAN0001",
      .device_instance_id = L"VIRTUAL\\VSS_PLAN",
      .is_system_disk = true,
  };
  backup.physical_sector_size = physical_sector_size;
  backup.partition_style =
      style == ytec::imageformat::PartitionTableStyle::gpt
      ? ytec::imageformat::BackupPartitionStyle::gpt
      : ytec::imageformat::BackupPartitionStyle::mbr;
  backup.boot_mode =
      style == ytec::imageformat::PartitionTableStyle::gpt
      ? ytec::imageformat::BackupBootMode::uefi
      : ytec::imageformat::BackupBootMode::legacy_bios;
  backup.windows_major = 10;
  backup.windows_build = 19045;
  backup.windows_architecture = "AMD64";
  backup.bitlocker_fully_decrypted = true;
  backup.created_utc = "2026-07-31T12:00:00Z";
  backup.app_version = "0.1.0";
  if (style == ytec::imageformat::PartitionTableStyle::gpt) {
    backup.partitions = {
        BackupManifestPartition{
            .table_index = 0,
            .offset_bytes = 2048ULL * kSectorSize,
            .length_bytes = 1024ULL * kSectorSize,
            .role = BackupPartitionRole::efi_system,
            .file_system = BackupFileSystem::fat32,
            .cluster_size = 4096,
            .name = L"EFI",
        },
        BackupManifestPartition{
            .table_index = 1,
            .offset_bytes = 3072ULL * kSectorSize,
            .length_bytes = 256ULL * kSectorSize,
            .role = BackupPartitionRole::microsoft_reserved,
            .file_system = BackupFileSystem::none,
            .cluster_size = 0,
            .name = L"MSR",
        },
        BackupManifestPartition{
            .table_index = 2,
            .offset_bytes = 4096ULL * kSectorSize,
            .length_bytes = 12288ULL * kSectorSize,
            .role = BackupPartitionRole::windows_ntfs,
            .file_system = BackupFileSystem::ntfs,
            .cluster_size = 4096,
            .name = L"Windows",
        },
        BackupManifestPartition{
            .table_index = 3,
            .offset_bytes = 18432ULL * kSectorSize,
            .length_bytes = 2048ULL * kSectorSize,
            .role = BackupPartitionRole::recovery_ntfs,
            .file_system = BackupFileSystem::ntfs,
            .cluster_size = 4096,
            .name = L"Recovery",
        },
    };
  } else {
    backup.partitions = {
        BackupManifestPartition{
            .table_index = 0,
            .offset_bytes = 2048ULL * kSectorSize,
            .length_bytes = 1024ULL * kSectorSize,
            .role = BackupPartitionRole::fat32_data,
            .file_system = BackupFileSystem::fat32,
            .cluster_size = 4096,
            .name = L"FAT32",
        },
        BackupManifestPartition{
            .table_index = 1,
            .offset_bytes = 4096ULL * kSectorSize,
            .length_bytes = 12288ULL * kSectorSize,
            .role = BackupPartitionRole::windows_ntfs,
            .file_system = BackupFileSystem::ntfs,
            .cluster_size = 4096,
            .name = L"Windows",
        },
        BackupManifestPartition{
            .table_index = 2,
            .offset_bytes = 18432ULL * kSectorSize,
            .length_bytes = 2048ULL * kSectorSize,
            .role = BackupPartitionRole::recovery_ntfs,
            .file_system = BackupFileSystem::ntfs,
            .cluster_size = 4096,
            .name = L"Recovery",
        },
    };
  }
  const auto manifest =
      ytec::imageformat::build_backup_manifest_v1(backup);
  check(manifest.has_value(), "Fixture backup manifest should build");
  return ytec::vssrequester::SnapshotImagePlanOptions{
      .administrator = true,
      .physical_sector_size = physical_sector_size,
      .manifest = manifest.value(),
      .partition_table_snapshot = snapshot.value(),
  };
}

std::vector<ytec::clonecore::VolumeBitmapBinding> binding(
    const std::uint32_t index) {
  return {
      ytec::clonecore::VolumeBitmapBinding{
          .partition_entry_index = index,
          .volume_device_path =
              L"\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
      },
  };
}

ytec::vssrequester::SnapshotMetadataContext metadata_context(
    const std::uint32_t physical_sector_size = 4096U) {
  return ytec::vssrequester::SnapshotMetadataContext{
      .source =
          ytec::clonecore::StableDiskIdentity{
              .disk_number = 0,
              .model = L"VSS PLAN FIXTURE",
              .size_bytes = kDiskSize,
              .logical_sector_size = kSectorSize,
              .serial_suffix = "PLAN0001",
              .device_instance_id = L"VIRTUAL\\VSS_PLAN",
              .is_system_disk = true,
          },
      .physical_sector_size = physical_sector_size,
      .windows_major = 10,
      .windows_minor = 0,
      .windows_build = 19045,
      .windows_architecture = "AMD64",
      .created_utc = "2026-07-31T12:00:00Z",
      .app_version = "0.1.0",
  };
}

void test_metadata_builder_captures_gpt_manifest_and_table() {
  auto fixture = make_gpt_fixture();
  MemoryReader reader(std::move(fixture.bytes));
  const auto result =
      ytec::vssrequester::build_gpt_snapshot_metadata(
          reader, metadata_context());
  check(result.has_value(), "Supported GPT metadata should build");
  const auto manifest =
      ytec::imageformat::inspect_backup_manifest_v1(
          result.value().backup_manifest);
  check(manifest.has_value(), "Generated GPT manifest should inspect");
  check(
      manifest.value().partitions.size() == 4 &&
          manifest.value().partition_style ==
              ytec::imageformat::BackupPartitionStyle::gpt &&
          manifest.value().boot_mode ==
              ytec::imageformat::BackupBootMode::uefi &&
          manifest.value().bitlocker_fully_decrypted &&
          manifest.value().compression ==
              ytec::imageformat::DcimgCompression::zstandard &&
          manifest.value().compression_version ==
              ytec::imageformat::kDcimgZstandardProfileVersion,
      "GPT metadata should record layout, boot mode, decryption gate, and compression profile");
  check(
      !result.value().partition_table_snapshot.empty(),
      "GPT metadata should capture the primary and backup GPT");
}

void test_metadata_builder_captures_mbr_manifest_and_table() {
  MemoryReader reader(make_mbr_disk());
  const auto result =
      ytec::vssrequester::build_mbr_snapshot_metadata(
          reader, metadata_context());
  check(result.has_value(), "Supported MBR metadata should build");
  const auto manifest =
      ytec::imageformat::inspect_backup_manifest_v1(
          result.value().backup_manifest);
  check(manifest.has_value(), "Generated MBR manifest should inspect");
  check(
      manifest.value().partitions.size() == 3 &&
          manifest.value().partition_style ==
              ytec::imageformat::BackupPartitionStyle::mbr &&
          manifest.value().boot_mode ==
              ytec::imageformat::BackupBootMode::legacy_bios &&
          manifest.value().compression ==
              ytec::imageformat::DcimgCompression::zstandard,
      "MBR metadata should record all supported partitions, BIOS mode, and compression");
  const auto snapshot =
      ytec::imageformat::inspect_partition_snapshot_v1(
          result.value().partition_table_snapshot);
  check(
      snapshot.has_value() &&
          snapshot.value().regions.size() == 1 &&
          snapshot.value().regions[0].disk_offset == 0 &&
          snapshot.value().regions[0].data.size() == kSectorSize,
      "MBR metadata should capture exactly sector zero");
}

void test_16k_physical_sector_reaches_metadata_and_plan() {
  auto fixture = make_gpt_fixture();
  MemoryReader reader(std::move(fixture.bytes));
  const auto metadata =
      ytec::vssrequester::build_gpt_snapshot_metadata(
          reader, metadata_context(16U * 1024U));
  check(metadata.has_value(),
        "A 16 KiB physical sector source should build GPT metadata");
  const auto manifest =
      ytec::imageformat::inspect_backup_manifest_v1(
          metadata.value().backup_manifest);
  check(
      manifest.has_value() &&
          manifest.value().physical_sector_size == 16U * 1024U,
      "GPT metadata should preserve the 16 KiB physical sector size");

  const auto plan =
      ytec::vssrequester::prepare_gpt_snapshot_image_plan(
          fixture.layout,
          reader,
          binding(2),
          options(
              reader,
              ytec::imageformat::PartitionTableStyle::gpt,
              16U * 1024U));
  check(
      plan.has_value() &&
          plan.value().image_copy.physical_sector_size ==
              16U * 1024U,
      "The VSS image plan should preserve the validated 16 KiB sector size");
}

void test_metadata_builder_rejects_bitlocker_on_disk_signature() {
  auto fixture = make_gpt_fixture();
  constexpr char kBitLockerSignature[] = "-FVE-FS-";
  const std::size_t windows_boot =
      static_cast<std::size_t>(4096ULL * kSectorSize);
  std::memcpy(
      fixture.bytes.data() + windows_boot + 3,
      kBitLockerSignature,
      8);
  MemoryReader reader(std::move(fixture.bytes));
  const auto result =
      ytec::vssrequester::build_gpt_snapshot_metadata(
          reader, metadata_context());
  check(
      !result.has_value(),
      "An on-disk BitLocker signature must fail the fully-decrypted gate");
}

void test_gpt_plan_routes_vss_raw_and_recreated_partitions() {
  auto fixture = make_gpt_fixture();
  MemoryReader reader(std::move(fixture.bytes));
  const auto result =
      ytec::vssrequester::prepare_gpt_snapshot_image_plan(
          fixture.layout, reader, binding(2), options(
              reader, ytec::imageformat::PartitionTableStyle::gpt));
  check(result.has_value(), "Supported GPT image plan should succeed");
  const auto& plan = result.value();
  check(
      plan.snapshot_partition_count == 1 &&
          plan.raw_partition_count == 2 &&
          plan.recreated_partition_count == 1,
      "GPT plan should route Basic to VSS, EFI/Recovery to raw, MSR recreate");
  check(
      plan.workflow.administrator &&
          plan.workflow.volumes.size() == 1 &&
          plan.image_copy.volumes.size() == 1 &&
          plan.image_copy.raw_regions.size() == 2,
      "Workflow and image plan counts should stay aligned");
  check(
      plan.image_copy.raw_regions[0].disk_offset ==
              2048ULL * kSectorSize &&
          plan.image_copy.raw_regions[1].disk_offset ==
              18432ULL * kSectorSize,
      "Raw GPT ranges should preserve physical partition offsets");
}

void test_gpt_selected_entries_skip_unselected_partition_payload_reads() {
  auto fixture = make_gpt_fixture();
  MemoryReader reader(std::move(fixture.bytes));
  auto plan_options =
      options(reader, ytec::imageformat::PartitionTableStyle::gpt);
  plan_options.selected_partition_entry_indices = {1U, 2U};
  reader.clear_reads();
  const auto result = ytec::vssrequester::prepare_gpt_snapshot_image_plan(
      fixture.layout, reader, binding(2), plan_options);
  check(result.has_value(), "selected GPT VSS plan should succeed");
  check(
      result.value().snapshot_partition_count == 1U &&
          result.value().recreated_partition_count == 1U &&
          result.value().raw_partition_count == 0U &&
          result.value().image_copy.volumes.size() == 1U &&
          result.value().image_copy.volumes[0].partition_entry_index == 2U &&
          result.value().image_copy.raw_regions.empty(),
      "selected GPT plan must contain only MSR recreation and Windows VSS payload");
  const auto intersects = [](const auto& read,
                             const std::uint64_t begin,
                             const std::uint64_t end) {
    return read.first < end &&
        read.first + static_cast<std::uint64_t>(read.second) > begin;
  };
  const bool read_unselected_payload = std::any_of(
      reader.reads().begin(), reader.reads().end(), [&](const auto& read) {
        return intersects(
                   read,
                   2048ULL * kSectorSize,
                   3072ULL * kSectorSize) ||
            intersects(
                   read,
                   18432ULL * kSectorSize,
                   20480ULL * kSectorSize);
      });
  check(
      !read_unselected_payload,
      "planner must not probe or copy unselected ESP and Recovery payload sectors");
}

void test_gpt_selected_entries_reject_noncanonical_or_unknown_values() {
  auto fixture = make_gpt_fixture();
  MemoryReader reader(std::move(fixture.bytes));
  for (const auto selected : {
           std::vector<std::uint32_t>{2U, 1U},
           std::vector<std::uint32_t>{2U, 2U},
           std::vector<std::uint32_t>{2U, 99U}}) {
    auto plan_options =
        options(reader, ytec::imageformat::PartitionTableStyle::gpt);
    plan_options.selected_partition_entry_indices = selected;
    check(
        !ytec::vssrequester::prepare_gpt_snapshot_image_plan(
             fixture.layout, reader, binding(2), plan_options)
             .has_value(),
        "noncanonical or unknown selected GPT entry must fail closed");
  }
}

void test_mbr_plan_routes_vss_and_raw_partitions() {
  MemoryReader reader(make_mbr_disk());
  const auto parsed = ytec::clonecore::parse_mbr(reader);
  check(parsed.has_value(), "Synthetic MBR should parse");
  const auto result =
      ytec::vssrequester::prepare_mbr_snapshot_image_plan(
          parsed.value(), reader, binding(1), options(
              reader, ytec::imageformat::PartitionTableStyle::mbr));
  check(result.has_value(), "Supported MBR image plan should succeed");
  check(
      result.value().snapshot_partition_count == 1 &&
          result.value().raw_partition_count == 2 &&
          result.value().recreated_partition_count == 0,
      "MBR plan should route 0x07 to VSS and FAT32/Recovery to raw");
}

void test_missing_or_extra_volume_binding_fails_closed() {
  auto fixture = make_gpt_fixture();
  MemoryReader reader(std::move(fixture.bytes));
  const auto plan_options =
      options(reader, ytec::imageformat::PartitionTableStyle::gpt);
  check(
      !ytec::vssrequester::prepare_gpt_snapshot_image_plan(
           fixture.layout, reader, {}, plan_options)
           .has_value(),
      "Missing Basic-data Volume binding must fail");
  auto bindings = binding(2);
  bindings.push_back(ytec::clonecore::VolumeBitmapBinding{
      .partition_entry_index = 99,
      .volume_device_path =
          L"\\\\?\\Volume{99999999-9999-9999-9999-999999999999}\\",
  });
  check(
      !ytec::vssrequester::prepare_gpt_snapshot_image_plan(
           fixture.layout, reader, bindings, plan_options)
           .has_value(),
      "Extra Volume binding must fail");
}

void test_manifest_and_copy_compression_mismatch_fails_closed() {
  auto fixture = make_gpt_fixture();
  MemoryReader reader(std::move(fixture.bytes));
  auto plan_options =
      options(reader, ytec::imageformat::PartitionTableStyle::gpt);
  plan_options.compression =
      ytec::imageformat::DcimgCompression::zstandard;
  const auto result =
      ytec::vssrequester::prepare_gpt_snapshot_image_plan(
          fixture.layout, reader, binding(2), plan_options);
  check(
      !result.has_value(),
      "A manifest/container compression mismatch must fail before VSS");
}

void test_stale_parsed_layout_or_snapshot_fails_closed() {
  auto fixture = make_gpt_fixture();
  MemoryReader reader(std::move(fixture.bytes));
  auto stale_layout = fixture.layout;
  stale_layout.partitions[2].first_lba += 1;
  auto plan_options =
      options(reader, ytec::imageformat::PartitionTableStyle::gpt);
  check(
      !ytec::vssrequester::prepare_gpt_snapshot_image_plan(
           stale_layout, reader, binding(2), plan_options)
           .has_value(),
      "A stale parsed GPT layout must fail reparse identity");
  plan_options.partition_table_snapshot.back() ^= std::byte{0x01};
  check(
      !ytec::vssrequester::prepare_gpt_snapshot_image_plan(
           fixture.layout, reader, binding(2), plan_options)
           .has_value(),
      "A corrupted partition snapshot must fail before VSS");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"metadata_builder_captures_gpt_manifest_and_table",
       test_metadata_builder_captures_gpt_manifest_and_table},
      {"metadata_builder_captures_mbr_manifest_and_table",
       test_metadata_builder_captures_mbr_manifest_and_table},
      {"physical_16k_reaches_metadata_and_plan",
       test_16k_physical_sector_reaches_metadata_and_plan},
      {"metadata_builder_rejects_bitlocker_on_disk_signature",
       test_metadata_builder_rejects_bitlocker_on_disk_signature},
      {"gpt_plan_routes_vss_raw_and_recreated_partitions",
       test_gpt_plan_routes_vss_raw_and_recreated_partitions},
      {"gpt_selected_entries_skip_unselected_partition_payload_reads",
       test_gpt_selected_entries_skip_unselected_partition_payload_reads},
      {"gpt_selected_entries_reject_noncanonical_or_unknown_values",
       test_gpt_selected_entries_reject_noncanonical_or_unknown_values},
      {"mbr_plan_routes_vss_and_raw_partitions",
       test_mbr_plan_routes_vss_and_raw_partitions},
      {"missing_or_extra_volume_binding_fails_closed",
       test_missing_or_extra_volume_binding_fails_closed},
      {"manifest_and_copy_compression_mismatch_fails_closed",
       test_manifest_and_copy_compression_mismatch_fails_closed},
      {"stale_parsed_layout_or_snapshot_fails_closed",
       test_stale_parsed_layout_or_snapshot_fails_closed},
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

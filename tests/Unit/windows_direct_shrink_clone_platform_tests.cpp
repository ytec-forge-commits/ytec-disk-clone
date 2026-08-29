#include "ytec/windowsapp/windows_direct_shrink_clone_platform.h"

#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/migrationcore/direct_clone_plan.h"
#include "ytec/operationcore/operation.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint32_t kSectorSize = 512U;
constexpr std::uint64_t kExactRawBytes = 8ULL * kMiB - kSectorSize;
constexpr std::wstring_view kSourceVolume =
    L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
constexpr std::wstring_view kBiosSystemVolume =
    L"\\\\?\\Volume{01234567-89AB-CDEF-0123-456789ABCDEF}\\";
constexpr std::wstring_view kRecoveryVolume =
    L"\\\\?\\Volume{66666666-7777-8888-9999-AAAAAAAAAAAA}\\";
constexpr std::wstring_view kDataVolume =
    L"\\\\?\\Volume{BBBBBBBB-CCCC-DDDD-EEEE-FFFFFFFFFFFF}\\";

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <std::size_t Size>
std::array<std::byte, Size> filled(const std::uint8_t value) {
  std::array<std::byte, Size> result{};
  result.fill(static_cast<std::byte>(value));
  return result;
}

ytec::clonecore::Error injected_error(
    const ytec::clonecore::ErrorCode code,
    std::wstring operation) {
  return {
      .code = code,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::move(operation),
      .message = L"合成失敗",
  };
}

template <typename T>
ytec::clonecore::Result<T> injected_failure(
    const ytec::clonecore::ErrorCode code,
    std::wstring operation) {
  return ytec::clonecore::Result<T>::failure(
      injected_error(code, std::move(operation)));
}

ytec::diskmodel::PartitionInfo data_partition(
    const ytec::diskmodel::PartitionStyle style) {
  return {
      .number = 1U,
      .offset_bytes = 1ULL * kMiB,
      .size_bytes = 4ULL * kGiB,
      .style = style,
      .type = style == ytec::diskmodel::PartitionStyle::gpt
          ? L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"
          : L"0x07",
      .identifier = style == ytec::diskmodel::PartitionStyle::gpt
          ? L"{11111111-1111-1111-1111-111111111111}"
          : L"0x10203040-1",
      .name = L"Data",
      .bootable = false,
  };
}

ytec::diskmodel::DiskInfo source_disk(
    const ytec::diskmodel::PartitionStyle style,
    const bool include_microsoft_reserved = false) {
  ytec::diskmodel::DiskInfo result{
      .disk_number = 2U,
      .device_path = L"\\\\.\\PhysicalDrive2",
      .device_interface_path = L"\\\\?\\SCSI#Disk&Ven_YTEC&Prod_Source",
      .connection_location_path = L"PCIROOT(0)#PCI(0100)",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_SOURCE\\SOURCE-A",
      .model = L"YTEC SYNTHETIC SOURCE",
      .size_bytes = 16ULL * kGiB,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "SOURCE01",
      .partition_style = style,
      .disk_identifier = style == ytec::diskmodel::PartitionStyle::gpt
          ? L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"
          : L"0x10203040",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  result.sector_count = result.size_bytes / result.logical_sector_size;
  result.partitions.push_back(data_partition(style));
  if (include_microsoft_reserved) {
    result.partitions.push_back(ytec::diskmodel::PartitionInfo{
        .number = 2U,
        .offset_bytes = 5ULL * kGiB,
        .size_bytes = 16ULL * kMiB,
        .style = ytec::diskmodel::PartitionStyle::gpt,
        .type = L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}",
        .identifier = L"{22222222-2222-2222-2222-222222222222}",
        .name = L"",
        .bootable = false,
    });
  }
  return result;
}

ytec::diskmodel::DiskInfo target_disk(
    const std::uint64_t size_bytes = 12ULL * kGiB) {
  ytec::diskmodel::DiskInfo result{
      .disk_number = 5U,
      .device_path = L"\\\\.\\PhysicalDrive5",
      .device_interface_path = L"\\\\?\\SCSI#Disk&Ven_YTEC&Prod_Target",
      .connection_location_path = L"PCIROOT(0)#PCI(0200)",
      .device_instance_id = L"SCSI\\DISK&VEN_YTEC&PROD_TARGET\\TARGET-A",
      .model = L"YTEC SYNTHETIC TARGET",
      .size_bytes = size_bytes,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "TARGET01",
      .partition_style = ytec::diskmodel::PartitionStyle::raw,
      .disk_identifier = L"RAW",
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  result.sector_count = result.size_bytes / result.logical_sector_size;
  return result;
}

ytec::clonecore::StableDiskIdentity stable_identity(
    const ytec::diskmodel::DiskInfo& disk) {
  auto result = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(result.has_value(), "synthetic stable identity must build");
  return result.take_value();
}

ytec::imageformat::Sha256Digest layout_hash(
    const ytec::diskmodel::DiskInfo& disk) {
  auto result = ytec::imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(disk);
  check(result.has_value(), "synthetic layout hash must build");
  return result.take_value();
}

struct Fixture final {
  ytec::diskmodel::DiskInfo source;
  ytec::diskmodel::DiskInfo target;
  ytec::clonecore::StableDiskIdentity source_identity;
  ytec::clonecore::StableDiskIdentity target_identity;
  ytec::imageformat::Sha256Digest source_layout{};
  ytec::imageformat::Sha256Digest target_layout{};
  ytec::windowsapp::WindowsDirectShrinkClonePlan plan;
};

Fixture fixture(
    const ytec::diskmodel::PartitionStyle source_style,
    const ytec::migrationcore::ShrinkSurplusAllocation allocation =
        ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated,
    const bool include_microsoft_reserved = false,
    const bool include_exact_raw = false) {
  auto source = source_disk(source_style, include_microsoft_reserved);
  if (include_exact_raw) {
    check(
        source_style == ytec::diskmodel::PartitionStyle::gpt &&
            !include_microsoft_reserved,
        "synthetic exact RAW fixture requires a simple GPT source");
    source.partitions.push_back(ytec::diskmodel::PartitionInfo{
        .number = 2U,
        .offset_bytes = 5ULL * kGiB,
        .size_bytes = kExactRawBytes,
        .style = ytec::diskmodel::PartitionStyle::gpt,
        .type = L"{9A9A9A9A-9A9A-9A9A-9A9A-9A9A9A9A9A9A}",
        .identifier = L"{22222222-3333-4444-5555-666666666666}",
        .name = L"Linux data",
        .bootable = false,
    });
  }
  auto target = target_disk();
  ytec::migrationcore::DirectClonePlanningRequest direct_request{
      .mode_choice = ytec::migrationcore::DirectCloneModeChoice::shrink,
      .partition_style_choice = ytec::migrationcore::
          DirectClonePartitionStyleChoice::preserve,
      .source_style = source_style == ytec::diskmodel::PartitionStyle::gpt
          ? ytec::migrationcore::MigrationPartitionStyle::gpt
          : ytec::migrationcore::MigrationPartitionStyle::mbr,
      .source_size_bytes = source.size_bytes,
      .source_logical_sector_size = kSectorSize,
      .target_size_bytes = target.size_bytes,
      .target_logical_sector_size = kSectorSize,
      .source_is_windows_system = false,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation = allocation,
      .surplus_target_source_table_index =
          allocation == ytec::migrationcore::ShrinkSurplusAllocation::
                  selected_data_partition
          ? std::optional<std::uint32_t>{1U}
          : std::nullopt,
      .source_partitions = {
          ytec::migrationcore::DirectCloneSourcePartition{
              .partition = {
                  .source_table_index = 1U,
                  .role = ytec::migrationcore::MigrationPartitionRole::data,
                  .file_system =
                      ytec::migrationcore::MigrationFileSystem::ntfs,
                  .source_size_bytes = 4ULL * kGiB,
                  .used_bytes = 1ULL * kGiB,
                  .cluster_size = 4096U,
                  .label = L"Data",
                  .active = false,
              },
              .selected = true,
              .required_for_windows = false,
          },
      },
  };
  if (include_microsoft_reserved) {
    direct_request.source_partitions.push_back(
        ytec::migrationcore::DirectCloneSourcePartition{
            .partition = {
                .source_table_index = 2U,
                .role = ytec::migrationcore::
                    MigrationPartitionRole::microsoft_reserved,
                .file_system =
                    ytec::migrationcore::MigrationFileSystem::none,
                .source_size_bytes = 16ULL * kMiB,
                .used_bytes = 0U,
                .cluster_size = 0U,
                .label = L"",
                .active = false,
            },
            .selected = true,
            .required_for_windows = false,
        });
  }
  if (include_exact_raw) {
    direct_request.source_partitions.push_back(
        ytec::migrationcore::DirectCloneSourcePartition{
            .partition = {
                .source_table_index = 2U,
                .role = ytec::migrationcore::MigrationPartitionRole::data,
                .file_system =
                    ytec::migrationcore::MigrationFileSystem::unsupported,
                .source_size_bytes = kExactRawBytes,
                .used_bytes = kExactRawBytes,
                .cluster_size = 0U,
                .label = L"Linux data",
                .active = false,
            },
            .selected = true,
            .required_for_windows = false,
        });
  }
  auto direct = ytec::migrationcore::plan_direct_clone(direct_request);
  check(direct.has_value(), "synthetic direct plan must build");
  auto source_id = stable_identity(source);
  auto target_id = stable_identity(target);
  auto source_digest = layout_hash(source);
  auto target_digest = layout_hash(target);
  ytec::windowsapp::WindowsDirectShrinkPlanningRequest product_request{
      .administrator = true,
      .bitlocker_fully_decrypted = true,
      .target_is_active_rescue_media = false,
      .reviewed_source = source,
      .reviewed_target = target,
      .expected_source = source_id,
      .expected_target = target_id,
      .expected_source_layout_hash = source_digest,
      .expected_target_layout_hash = target_digest,
      .expected_source_partition_snapshot_hash =
          source_style == ytec::diskmodel::PartitionStyle::mbr
          ? filled<32U>(0x42U)
          : ytec::imageformat::Sha256Digest{},
      .operation_id = filled<16U>(0x31U),
      .ntfs_volumes = {
          {
              .source_table_index = 1U,
              .source_offset_bytes = 1ULL * kMiB,
              .source_size_bytes = 4ULL * kGiB,
              .original_volume_guid_path = std::wstring(kSourceVolume),
          },
      },
  };
  if (include_exact_raw) {
    product_request.exact_raw_partitions.push_back(
        ytec::windowsapp::WindowsDirectShrinkExactRawPartition{
            .source_table_index = 2U,
            .source_offset_bytes = 5ULL * kGiB,
            .source_size_bytes = kExactRawBytes,
            .source_partition_type = filled<16U>(0x9AU),
        });
  }
  if (source_style == ytec::diskmodel::PartitionStyle::mbr) {
    product_request.mbr_preserve_binding =
        ytec::windowsapp::WindowsDirectShrinkMbrPlanBinding{
            .source_sector0_hash = filled<32U>(0x42U),
            .source_bootstrap = filled<440U>(0x90U),
            .source_disk_signature = 0x10203040U,
            .target_disk_signature = 0x50607080U,
            .planning_signature_inventory_hash = filled<32U>(0x43U),
        };
  }
  auto plan = ytec::windowsapp::build_windows_direct_shrink_clone_plan(
      product_request, direct.value());
  check(plan.has_value(), "synthetic product plan must build");
  return {
      .source = std::move(source),
      .target = std::move(target),
      .source_identity = std::move(source_id),
      .target_identity = std::move(target_id),
      .source_layout = source_digest,
      .target_layout = target_digest,
      .plan = plan.take_value(),
  };
}

Fixture system_fixture(
    const ytec::migrationcore::ShrinkSurplusAllocation allocation) {
  auto source = source_disk(ytec::diskmodel::PartitionStyle::gpt);
  source.size_bytes = 64ULL * kGiB;
  source.sector_count = source.size_bytes / source.logical_sector_size;
  source.is_system_disk = true;
  source.partitions = {
      {
          .number = 1U,
          .offset_bytes = 1ULL * kMiB,
          .size_bytes = 200ULL * kMiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
          .identifier = L"{10000000-0000-0000-0000-000000000001}",
          .name = L"SYSTEM",
      },
      {
          .number = 2U,
          .offset_bytes = 201ULL * kMiB,
          .size_bytes = 16ULL * kMiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}",
          .identifier = L"{10000000-0000-0000-0000-000000000002}",
          .name = L"",
      },
      {
          .number = 3U,
          .offset_bytes = 217ULL * kMiB,
          .size_bytes = 32ULL * kGiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
          .identifier = L"{10000000-0000-0000-0000-000000000003}",
          .name = L"Windows",
      },
      {
          .number = 4U,
          .offset_bytes = 217ULL * kMiB + 32ULL * kGiB,
          .size_bytes = 2ULL * kGiB,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{DE94BBA4-06D1-4D40-A16A-BFD50179D6AC}",
          .identifier = L"{10000000-0000-0000-0000-000000000004}",
          .name = L"Recovery",
      },
  };
  auto target = target_disk(56ULL * kGiB);
  ytec::migrationcore::DirectClonePlanningRequest direct_request{
      .mode_choice = ytec::migrationcore::DirectCloneModeChoice::shrink,
      .partition_style_choice = ytec::migrationcore::
          DirectClonePartitionStyleChoice::preserve,
      .source_style = ytec::migrationcore::MigrationPartitionStyle::gpt,
      .source_size_bytes = source.size_bytes,
      .source_logical_sector_size = kSectorSize,
      .target_size_bytes = target.size_bytes,
      .target_logical_sector_size = kSectorSize,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation = allocation,
      .source_partitions = {
          {
              .partition = {
                  .source_table_index = 1U,
                  .role = ytec::migrationcore::MigrationPartitionRole::efi_system,
                  .file_system = ytec::migrationcore::MigrationFileSystem::fat32,
                  .source_size_bytes = 200ULL * kMiB,
                  .used_bytes = 32ULL * kMiB,
                  .cluster_size = 4096U,
                  .label = L"SYSTEM",
              },
              .selected = true,
          },
          {
              .partition = {
                  .source_table_index = 2U,
                  .role = ytec::migrationcore::MigrationPartitionRole::microsoft_reserved,
                  .file_system = ytec::migrationcore::MigrationFileSystem::none,
                  .source_size_bytes = 16ULL * kMiB,
                  .used_bytes = 0U,
                  .cluster_size = 0U,
              },
              .selected = true,
          },
          {
              .partition = {
                  .source_table_index = 3U,
                  .role = ytec::migrationcore::MigrationPartitionRole::windows,
                  .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
                  .source_size_bytes = 32ULL * kGiB,
                  .used_bytes = 8ULL * kGiB,
                  .cluster_size = 4096U,
                  .label = L"Windows",
              },
              .selected = true,
          },
          {
              .partition = {
                  .source_table_index = 4U,
                  .role = ytec::migrationcore::MigrationPartitionRole::recovery,
                  .file_system = ytec::migrationcore::MigrationFileSystem::ntfs,
                  .source_size_bytes = 2ULL * kGiB,
                  .used_bytes = 1ULL * kGiB,
                  .cluster_size = 4096U,
                  .label = L"Recovery",
              },
              .selected = true,
              .required_for_windows = true,
          },
      },
  };
  auto direct = ytec::migrationcore::plan_direct_clone(direct_request);
  check(direct.has_value(), "synthetic system direct plan must build");
  auto source_id = stable_identity(source);
  auto target_id = stable_identity(target);
  auto source_digest = layout_hash(source);
  auto target_digest = layout_hash(target);
  ytec::windowsapp::WindowsDirectShrinkPlanningRequest product_request{
      .administrator = true,
      .bitlocker_fully_decrypted = true,
      .target_is_active_rescue_media = false,
      .reviewed_source = source,
      .reviewed_target = target,
      .expected_source = source_id,
      .expected_target = target_id,
      .expected_source_layout_hash = source_digest,
      .expected_target_layout_hash = target_digest,
      .operation_id = filled<16U>(0x39U),
      .ntfs_volumes = {
          {
              .source_table_index = 3U,
              .source_offset_bytes = 217ULL * kMiB,
              .source_size_bytes = 32ULL * kGiB,
              .original_volume_guid_path = std::wstring(kSourceVolume),
          },
          {
              .source_table_index = 4U,
              .source_offset_bytes = 217ULL * kMiB + 32ULL * kGiB,
              .source_size_bytes = 2ULL * kGiB,
              .original_volume_guid_path = std::wstring(kRecoveryVolume),
          },
      },
  };
  auto plan = ytec::windowsapp::build_windows_direct_shrink_clone_plan(
      product_request, direct.value());
  check(plan.has_value(), "synthetic system product plan must build");
  return {
      .source = std::move(source),
      .target = std::move(target),
      .source_identity = std::move(source_id),
      .target_identity = std::move(target_id),
      .source_layout = source_digest,
      .target_layout = target_digest,
      .plan = plan.take_value(),
  };
}

Fixture mbr_system_fixture(
    const bool separate_bios_system,
    const bool include_fourth_data_partition = false,
    const bool preserve_mbr = false) {
  using FileSystem = ytec::migrationcore::MigrationFileSystem;
  using Role = ytec::migrationcore::MigrationPartitionRole;

  auto source = source_disk(ytec::diskmodel::PartitionStyle::mbr);
  source.size_bytes = 64ULL * kGiB;
  source.sector_count = source.size_bytes / source.logical_sector_size;
  source.is_system_disk = true;
  source.partitions.clear();

  std::uint32_t next_number = 1U;
  std::uint64_t next_offset = 1ULL * kMiB;
  std::vector<ytec::migrationcore::DirectCloneSourcePartition>
      source_partitions;
  std::vector<ytec::windowsapp::WindowsDirectShrinkNtfsVolume> volumes;

  if (separate_bios_system) {
    source.partitions.push_back({
        .number = next_number,
        .offset_bytes = next_offset,
        .size_bytes = 500ULL * kMiB,
        .style = ytec::diskmodel::PartitionStyle::mbr,
        .type = L"0x07",
        .identifier = L"0x10203040-1",
        .name = L"System Reserved",
        .bootable = true,
    });
    source_partitions.push_back({
        .partition = {
            .source_table_index = next_number,
            .role = Role::bios_system,
            .file_system = FileSystem::ntfs,
            .source_size_bytes = 500ULL * kMiB,
            .used_bytes = 80ULL * kMiB,
            .cluster_size = 4096U,
            .label = L"System Reserved",
            .active = true,
        },
        .selected = true,
    });
    if (preserve_mbr) {
      volumes.push_back({
          .source_table_index = next_number,
          .source_offset_bytes = next_offset,
          .source_size_bytes = 500ULL * kMiB,
          .original_volume_guid_path = std::wstring(kBiosSystemVolume),
      });
    }
    ++next_number;
    next_offset += 500ULL * kMiB;
  }

  const auto windows_number = next_number++;
  const auto windows_offset = next_offset;
  source.partitions.push_back({
      .number = windows_number,
      .offset_bytes = windows_offset,
      .size_bytes = 32ULL * kGiB,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x07",
      .identifier = L"0x10203040-" + std::to_wstring(windows_number),
      .name = L"Windows",
      .bootable = !separate_bios_system,
  });
  source_partitions.push_back({
      .partition = {
          .source_table_index = windows_number,
          .role = Role::windows,
          .file_system = FileSystem::ntfs,
          .source_size_bytes = 32ULL * kGiB,
          .used_bytes = 8ULL * kGiB,
          .cluster_size = 4096U,
          .label = L"Windows",
          .active = !separate_bios_system,
      },
      .selected = true,
  });
  volumes.push_back({
      .source_table_index = windows_number,
      .source_offset_bytes = windows_offset,
      .source_size_bytes = 32ULL * kGiB,
      .original_volume_guid_path = std::wstring(kSourceVolume),
  });
  next_offset += 32ULL * kGiB;

  const auto recovery_number = next_number++;
  const auto recovery_offset = next_offset;
  source.partitions.push_back({
      .number = recovery_number,
      .offset_bytes = recovery_offset,
      .size_bytes = 2ULL * kGiB,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x27",
      .identifier = L"0x10203040-" + std::to_wstring(recovery_number),
      .name = L"Recovery",
      .bootable = false,
  });
  source_partitions.push_back({
      .partition = {
          .source_table_index = recovery_number,
          .role = Role::recovery,
          .file_system = FileSystem::ntfs,
          .source_size_bytes = 2ULL * kGiB,
          .used_bytes = 1ULL * kGiB,
          .cluster_size = 4096U,
          .label = L"Recovery",
      },
      .selected = true,
      .required_for_windows = true,
  });
  volumes.push_back({
      .source_table_index = recovery_number,
      .source_offset_bytes = recovery_offset,
      .source_size_bytes = 2ULL * kGiB,
      .original_volume_guid_path = std::wstring(kRecoveryVolume),
  });
  next_offset += 2ULL * kGiB;

  if (include_fourth_data_partition) {
    const auto data_number = next_number;
    source.partitions.push_back({
        .number = data_number,
        .offset_bytes = next_offset,
        .size_bytes = 4ULL * kGiB,
        .style = ytec::diskmodel::PartitionStyle::mbr,
        .type = L"0x07",
        .identifier = L"0x10203040-" + std::to_wstring(data_number),
        .name = L"Data",
        .bootable = false,
    });
    source_partitions.push_back({
        .partition = {
            .source_table_index = data_number,
            .role = Role::data,
            .file_system = FileSystem::ntfs,
            .source_size_bytes = 4ULL * kGiB,
            .used_bytes = 1ULL * kGiB,
            .cluster_size = 4096U,
            .label = L"Data",
        },
        .selected = true,
    });
    volumes.push_back({
        .source_table_index = data_number,
        .source_offset_bytes = next_offset,
        .source_size_bytes = 4ULL * kGiB,
        .original_volume_guid_path = std::wstring(kDataVolume),
    });
  }

  auto target = target_disk(56ULL * kGiB);
  ytec::migrationcore::DirectClonePlanningRequest direct_request{
      .mode_choice = ytec::migrationcore::DirectCloneModeChoice::shrink,
      .partition_style_choice = preserve_mbr
          ? ytec::migrationcore::DirectClonePartitionStyleChoice::preserve
          : ytec::migrationcore::
                DirectClonePartitionStyleChoice::mbr_to_gpt,
      .source_style = ytec::migrationcore::MigrationPartitionStyle::mbr,
      .source_size_bytes = source.size_bytes,
      .source_logical_sector_size = kSectorSize,
      .target_size_bytes = target.size_bytes,
      .target_logical_sector_size = kSectorSize,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .mbr_to_gpt_eligible = true,
      .surplus_allocation =
          ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated,
      .source_partitions = std::move(source_partitions),
  };
  auto direct = ytec::migrationcore::plan_direct_clone(direct_request);
  check(direct.has_value(), "synthetic MBR direct plan must build");

  auto source_id = stable_identity(source);
  auto target_id = stable_identity(target);
  auto source_digest = layout_hash(source);
  auto target_digest = layout_hash(target);
  ytec::windowsapp::WindowsDirectShrinkPlanningRequest product_request{
      .administrator = true,
      .bitlocker_fully_decrypted = true,
      .target_is_active_rescue_media = false,
      .reviewed_source = source,
      .reviewed_target = target,
      .expected_source = source_id,
      .expected_target = target_id,
      .expected_source_layout_hash = source_digest,
      .expected_target_layout_hash = target_digest,
      .expected_source_partition_snapshot_hash = filled<32U>(0x47U),
      .operation_id = filled<16U>(separate_bios_system ? 0x45U : 0x46U),
      .ntfs_volumes = std::move(volumes),
  };
  if (preserve_mbr) {
    product_request.mbr_preserve_binding =
        ytec::windowsapp::WindowsDirectShrinkMbrPlanBinding{
            .source_sector0_hash = filled<32U>(0x47U),
            .source_bootstrap = filled<440U>(0x90U),
            .source_disk_signature = 0x10203040U,
            .target_disk_signature = 0x50607080U,
            .planning_signature_inventory_hash = filled<32U>(0x48U),
        };
  }
  auto plan = ytec::windowsapp::build_windows_direct_shrink_clone_plan(
      product_request, direct.value());
  check(plan.has_value(), "synthetic MBR product plan must build");
  return {
      .source = std::move(source),
      .target = std::move(target),
      .source_identity = std::move(source_id),
      .target_identity = std::move(target_id),
      .source_layout = source_digest,
      .target_layout = target_digest,
      .plan = plan.take_value(),
  };
}

class SequenceGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  ytec::clonecore::Result<ytec::clonecore::GptGuid> next_guid() override {
    ytec::clonecore::GptGuid value{};
    value.bytes[0] = static_cast<std::byte>(++next_);
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(value);
  }

 private:
  std::uint8_t next_{};
};

class SparseWriter final : public ytec::clonecore::ITargetDiskWriter {
 public:
  struct Record final {
    std::uint64_t offset{};
    std::vector<std::byte> bytes;
  };

  explicit SparseWriter(const std::uint64_t size) : size_(size) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return size_;
  }
  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }
  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (offset > size_ || bytes.size() > size_ - offset) {
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::invalid_argument,
          L"合成writer範囲"));
    }
    ++attempted_write_count_;
    if ((fail_next_write_offset_.has_value() &&
         *fail_next_write_offset_ == offset) ||
        (fail_write_number_.has_value() &&
         *fail_write_number_ == attempted_write_count_)) {
      fail_next_write_offset_.reset();
      fail_write_number_.reset();
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成writer注入失敗"));
    }
    records_.push_back({offset, {bytes.begin(), bytes.end()}});
    ++write_count_;
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > size_ || length > size_ - offset) {
      return injected_failure<std::vector<std::byte>>(
          ytec::clonecore::ErrorCode::invalid_argument,
          L"合成reader範囲");
    }
    if (fail_read_after_write_number_.has_value() &&
        *fail_read_after_write_number_ == attempted_write_count_) {
      fail_read_after_write_number_.reset();
      last_injected_read_failure_offset_ = offset;
      return injected_failure<std::vector<std::byte>>(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成reader注入失敗");
    }
    std::vector<std::byte> result(length, std::byte{0});
    const std::uint64_t end = offset + length;
    for (const auto& record : records_) {
      const std::uint64_t record_end = record.offset + record.bytes.size();
      const std::uint64_t begin = (std::max)(offset, record.offset);
      const std::uint64_t overlap_end = (std::min)(end, record_end);
      if (begin >= overlap_end) {
        continue;
      }
      std::copy(
          record.bytes.begin() + static_cast<std::ptrdiff_t>(
              begin - record.offset),
          record.bytes.begin() + static_cast<std::ptrdiff_t>(
              overlap_end - record.offset),
          result.begin() + static_cast<std::ptrdiff_t>(begin - offset));
    }
    if (corrupt_next_read_ && !result.empty()) {
      result[0] ^= std::byte{0x01};
      corrupt_next_read_ = false;
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }
  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    ++flush_count_;
    return ytec::clonecore::success_status();
  }
  void corrupt_next_read() noexcept { corrupt_next_read_ = true; }
  void fail_next_write_at(const std::uint64_t offset) noexcept {
    fail_next_write_offset_ = offset;
  }
  void fail_on_write_number(const std::size_t number) noexcept {
    fail_write_number_ = number;
  }
  void fail_read_after_write_number(const std::size_t number) noexcept {
    fail_read_after_write_number_ = number;
  }
  [[nodiscard]] std::size_t attempted_write_count() const noexcept {
    return attempted_write_count_;
  }
  [[nodiscard]] std::optional<std::uint64_t>
  last_injected_read_failure_offset() const noexcept {
    return last_injected_read_failure_offset_;
  }
  [[nodiscard]] std::size_t write_count() const noexcept {
    return write_count_;
  }
  [[nodiscard]] std::size_t flush_count() const noexcept {
    return flush_count_;
  }
  [[nodiscard]] const std::vector<Record>& records() const noexcept {
    return records_;
  }

 private:
  std::uint64_t size_{};
  std::vector<Record> records_;
  mutable bool corrupt_next_read_{};
  mutable std::optional<std::size_t> fail_read_after_write_number_;
  mutable std::optional<std::uint64_t>
      last_injected_read_failure_offset_;
  std::optional<std::uint64_t> fail_next_write_offset_;
  std::optional<std::size_t> fail_write_number_;
  std::size_t attempted_write_count_{};
  std::size_t write_count_{};
  std::size_t flush_count_{};
};

class PatternSourceReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit PatternSourceReader(const std::uint64_t size_bytes) noexcept
      : size_bytes_(size_bytes) {}

  std::uint64_t size_bytes() const noexcept override { return size_bytes_; }
  std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }
  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > size_bytes_ || length > size_bytes_ - offset) {
      return injected_failure<std::vector<std::byte>>(
          ytec::clonecore::ErrorCode::invalid_argument,
          L"合成RAW source範囲");
    }
    std::vector<std::byte> result(length);
    for (std::size_t index = 0U; index < length; ++index) {
      result[index] = static_cast<std::byte>(
          static_cast<std::uint8_t>((offset + index) % 251U + 1U));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }

 private:
  std::uint64_t size_bytes_{};
};

std::vector<std::byte> read_writer_bytes(
    const SparseWriter& writer,
    const std::uint64_t offset,
    const std::size_t length) {
  auto observed = writer.read_back(offset, length);
  check(observed.has_value(), "sparse writer read must succeed");
  return observed.take_value();
}

template <std::size_t Size>
std::array<std::byte, Size> read_writer_array(
    const SparseWriter& writer,
    const std::uint64_t offset) {
  const auto bytes = read_writer_bytes(writer, offset, Size);
  std::array<std::byte, Size> result{};
  std::copy(bytes.begin(), bytes.end(), result.begin());
  return result;
}

std::uint64_t read_writer_u64(
    const SparseWriter& writer,
    const std::uint64_t offset) {
  const auto bytes = read_writer_array<8U>(writer, offset);
  std::uint64_t result{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    result |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(
                  bytes[index])) <<
        (index * 8U);
  }
  return result;
}

std::uint32_t read_writer_u32(
    const SparseWriter& writer,
    const std::uint64_t offset) {
  const auto bytes = read_writer_array<4U>(writer, offset);
  std::uint32_t result{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    result |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(
                  bytes[index])) <<
        (index * 8U);
  }
  return result;
}

bool writer_mbr_sector0_is_valid(const SparseWriter& writer) {
  const auto sector = read_writer_array<512U>(writer, 0U);
  return sector[510U] == std::byte{0x55} &&
      sector[511U] == std::byte{0xAA};
}

std::uint32_t writer_mbr_active_partition_count(
    const SparseWriter& writer) {
  const auto sector = read_writer_array<512U>(writer, 0U);
  std::uint32_t count{};
  for (std::size_t index = 0U; index < 4U; ++index) {
    if (sector[446U + index * 16U] == std::byte{0x80}) {
      ++count;
    }
  }
  return count;
}

bool writer_primary_gpt_header_is_valid(const SparseWriter& writer) {
  constexpr std::array<std::byte, 8U> kSignature{
      static_cast<std::byte>('E'),
      static_cast<std::byte>('F'),
      static_cast<std::byte>('I'),
      static_cast<std::byte>(' '),
      static_cast<std::byte>('P'),
      static_cast<std::byte>('A'),
      static_cast<std::byte>('R'),
      static_cast<std::byte>('T'),
  };
  return read_writer_array<8U>(writer, kSectorSize) == kSignature;
}

struct RawGptTaskState final {
  std::array<std::byte, 16U> disk_guid{};
  std::array<std::byte, 16U> type_guid{};
  std::array<std::byte, 16U> unique_guid{};
  std::uint64_t first_lba{};
  std::uint64_t last_lba{};
  std::uint64_t attributes{};
};

RawGptTaskState read_raw_gpt_task(
    const SparseWriter& writer,
    const std::uint32_t target_number) {
  check(target_number != 0U, "GPT target number must be non-zero");
  constexpr std::uint64_t kPrimaryHeaderDiskGuidOffset = 56U;
  constexpr std::uint64_t kPrimaryEntriesOffset = 2U * kSectorSize;
  constexpr std::uint64_t kEntryBytes = 128U;
  const auto entry = kPrimaryEntriesOffset +
      static_cast<std::uint64_t>(target_number - 1U) * kEntryBytes;
  return {
      .disk_guid = read_writer_array<16U>(
          writer, kSectorSize + kPrimaryHeaderDiskGuidOffset),
      .type_guid = read_writer_array<16U>(writer, entry),
      .unique_guid = read_writer_array<16U>(writer, entry + 16U),
      .first_lba = read_writer_u64(writer, entry + 32U),
      .last_lba = read_writer_u64(writer, entry + 40U),
      .attributes = read_writer_u64(writer, entry + 48U),
  };
}

bool all_zero_bytes(const std::span<const std::byte> bytes) {
  return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

struct PlatformState final {
  Fixture fixture;
  std::vector<std::string> events;
  bool offline{};
  bool reidentifier_mismatch{};
  bool source_reidentifier_mismatch{};
  bool source_layout_drift{};
  bool fail_efi_format{};
  bool fail_wim_apply{};
  bool fail_ntfs_extension{};
  bool mismatch_ntfs_extension_size{};
  bool fail_extension_readback{};
  bool fail_boot_finalization{};
  bool fail_winre_finalization{};
  bool boot_request_exact{};
  bool boot_nonboot_gpt_verified{true};
  bool boot_ownership_safe_before{true};
  bool boot_ownership_revalidated{true};
  bool boot_namespace_read_back{true};
  bool winre_request_exact{};
  bool mbr_signature_collision{};
  std::uint32_t mbr_observer_calls{};
  std::uint32_t fail_offline_attempts{};
  SparseWriter* writer{};
};

class MockIo final
    : public ytec::windowsapp::IWindowsTsumugiShrinkRestorePlatformIo {
 public:
  explicit MockIo(std::shared_ptr<PlatformState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkTargetObservation>
  observe_original_target(
      const ytec::imageformat::Sha256Digest& connection_hash) override {
    state_->events.push_back("observe");
    auto target = state_->fixture.target;
    target.offline = state_->offline;
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsTsumugiShrinkTargetObservation>::success({
        .physical = {
            .target = std::move(target),
            .target_identity = state_->fixture.target_identity,
        },
        .restore_identity = {
            .stable_identity_hash = filled<32U>(0x41U),
            .disk_size = state_->fixture.target.size_bytes,
            .logical_sector_size = kSectorSize,
            .connection_instance_hash = connection_hash,
        },
    });
  }

  [[nodiscard]] ytec::clonecore::Status validate_work_paths_disjoint(
      const ytec::windowsapp::WindowsShrinkWorkPaths&,
      const std::uint32_t) override {
    state_->events.push_back("unexpected-work-paths");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status set_target_offline(
      const bool offline) override {
    state_->events.push_back(offline ? "disk-offline" : "disk-online");
    if (offline && state_->fail_offline_attempts != 0U) {
      --state_->fail_offline_attempts;
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成offline失敗"));
    }
    state_->offline = offline;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::diskmodel::PhysicalTargetHandle>
  open_offline_target() override {
    state_->events.push_back("open-writer");
    auto writer = std::make_unique<SparseWriter>(
        state_->fixture.target.size_bytes);
    state_->writer = writer.get();
    auto target = state_->fixture.target;
    target.offline = true;
    return ytec::clonecore::Result<
        ytec::diskmodel::PhysicalTargetHandle>::success({
        .observed = {
            .target = std::move(target),
            .target_identity = state_->fixture.target_identity,
        },
        .target = std::move(writer),
    });
  }

  [[nodiscard]] ytec::clonecore::Status notify_layout_changed() override {
    state_->events.push_back("layout-notify");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding>
  bind_online_volume(
      const std::uint32_t number,
      const std::uint64_t offset,
      const std::uint64_t size) override {
    state_->events.push_back("bind-" + std::to_string(number));
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding>::success({
        .final_target_number = number,
        .disk_number = state_->fixture.target.disk_number,
        .target_offset = offset,
        .target_size = size,
        .volume_device_path =
            L"\\\\?\\Volume{" + std::to_wstring(number) + L"}\\",
    });
  }

  [[nodiscard]] ytec::clonecore::Status format_volume(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const ytec::imageformat::TsumugiManifestFileSystem file_system,
      const std::uint64_t) override {
    state_->events.push_back(
        "format-" + std::to_string(volume.final_target_number));
    if (state_->fail_efi_format &&
        file_system ==
            ytec::imageformat::TsumugiManifestFileSystem::fat32) {
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成format失敗"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkFileSystemReadbackEvidence>
  verify_volume_readback(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const ytec::imageformat::TsumugiManifestFileSystem,
      const std::uint64_t,
      const bool content) override {
    state_->events.push_back(
        std::string(content ? "verify-content-" : "verify-fs-") +
        std::to_string(volume.final_target_number));
    const bool extension_readback = content &&
        std::find(
            state_->events.begin(),
            state_->events.end(),
            "extend-" + std::to_string(volume.final_target_number)) !=
            state_->events.end();
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsTsumugiShrinkFileSystemReadbackEvidence>::success({
        .directory_count = content ? 4U : 1U,
        .regular_file_count = content ? 3U : 0U,
        .regular_file_bytes_read = content ? 8192U : 0U,
        .reparse_point_count = 0U,
        .file_system_metadata_verified = true,
        .namespace_fully_enumerated =
            content && !(extension_readback && state_->fail_extension_readback),
        .every_regular_file_read_to_eof =
            content && !(extension_readback && state_->fail_extension_readback),
    });
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsTsumugiShrinkNtfsExtensionEvidence>
  extend_ntfs_volume_to_exact_extent_and_verify(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume,
      const std::uint64_t previous_partition_size,
      const std::uint64_t) override {
    state_->events.push_back(
        "extend-" + std::to_string(volume.final_target_number));
    if (state_->fail_ntfs_extension) {
      return injected_failure<ytec::windowsapp::
          WindowsTsumugiShrinkNtfsExtensionEvidence>(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成NTFS伸長失敗");
    }
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsTsumugiShrinkNtfsExtensionEvidence>::success({
        .previous_file_system_bytes = previous_partition_size,
        .final_file_system_bytes = state_->mismatch_ntfs_extension_size
            ? volume.target_size - kSectorSize
            : volume.target_size,
        .final_partition_extent_bytes = volume.target_size,
        .exact_single_extent_reverified = true,
        .ntfs_sector_count_reverified = true,
        .flushed = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Status dismount_and_offline_volume(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding& volume)
      override {
    state_->events.push_back(
        "dismount-" + std::to_string(volume.final_target_number));
    state_->offline = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status begin_owned_staged_wim(
      const std::wstring&,
      const std::uint32_t,
      const std::uint64_t) override {
    state_->events.push_back("unexpected-stage-begin");
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status append_owned_staged_wim(
      const std::uint64_t,
      const std::uint64_t,
      const bool,
      const std::span<const std::byte>) override {
    state_->events.push_back("unexpected-stage-append");
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Result<
      ytec::imageformat::Sha256Digest>
  verify_and_lock_single_image_wim(const std::uint32_t) override {
    state_->events.push_back("unexpected-stage-verify");
    return ytec::clonecore::Result<
        ytec::imageformat::Sha256Digest>::success(filled<32U>(0x51U));
  }
  [[nodiscard]] ytec::clonecore::Status apply_locked_wim(
      const ytec::windowsapp::WindowsTsumugiShrinkVolumeBinding&,
      const std::wstring&,
      const ytec::imageformat::Sha256Digest&) override {
    state_->events.push_back("unexpected-stage-apply");
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Status discard_owned_staged_wim() override {
    state_->events.push_back("unexpected-stage-discard");
    return ytec::clonecore::success_status();
  }

 private:
  std::shared_ptr<PlatformState> state_;
};

class MockWimStore final
    : public ytec::windowsapp::IWindowsDirectShrinkOwnedWimStore {
 public:
  explicit MockWimStore(std::shared_ptr<PlatformState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::windowsapp::WindowsDirectShrinkOwnedWimEvidence>
  capture_and_seal(
      const std::uint32_t source_table_index,
      const std::wstring&,
      const std::uint64_t) override {
    state_->events.push_back("wim-capture");
    return ytec::clonecore::Result<ytec::windowsapp::
        WindowsDirectShrinkOwnedWimEvidence>::success({
        .source_table_index = source_table_index,
        .length = 4096U,
        .hash = filled<32U>(0x61U),
        .sealed_without_write_or_delete_sharing = true,
        .flushed = true,
        .complete_read_back_hash_verified = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Status apply_locked_and_reverify(
      const std::uint32_t,
      const ytec::imageformat::Sha256Digest&,
      const std::wstring&) override {
    state_->events.push_back("wim-apply");
    if (state_->fail_wim_apply) {
      return ytec::clonecore::Status::failure(injected_error(
          ytec::clonecore::ErrorCode::io_failed,
          L"合成WIM apply失敗"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status discard_exact(
      const std::uint32_t,
      const ytec::imageformat::Sha256Digest&) override {
    state_->events.push_back("wim-discard");
    return ytec::clonecore::success_status();
  }

 private:
  std::shared_ptr<PlatformState> state_;
};

ytec::diskmodel::ReidentifiedPhysicalClone observation(
    const PlatformState& state) {
  auto source = state.fixture.source;
  auto source_identity = state.fixture.source_identity;
  if (state.source_reidentifier_mismatch) {
    source_identity.model += L" CHANGED";
  }
  if (state.source_layout_drift && !source.partitions.empty()) {
    source.partitions.back().bootable = !source.partitions.back().bootable;
  }
  auto target = state.fixture.target;
  target.offline = state.offline;
  auto target_identity = state.fixture.target_identity;
  if (state.reidentifier_mismatch) {
    target_identity.model += L" CHANGED";
  }
  return {
      .source = std::move(source),
      .target = std::move(target),
      .source_identity = std::move(source_identity),
      .target_identity = std::move(target_identity),
  };
}

ytec::windowsapp::WindowsDirectShrinkClonePlatformDependencies dependencies(
    const std::shared_ptr<PlatformState>& state) {
  ytec::imageformat::Sha256Digest connection{};
  connection.fill(std::byte{0x71});
  return {
      .target_io = std::make_unique<MockIo>(state),
      .guid_generator = std::make_unique<SequenceGuidGenerator>(),
      .make_wim_store =
          [state](
              const std::wstring&,
              const std::uint64_t,
              const std::uint64_t,
              const ytec::clonecore::DiskOperationCallbacks&) {
            state->events.push_back("wim-store-create");
            std::unique_ptr<
                ytec::windowsapp::IWindowsDirectShrinkOwnedWimStore> store =
                std::make_unique<MockWimStore>(state);
            return ytec::clonecore::Result<std::unique_ptr<
                ytec::windowsapp::IWindowsDirectShrinkOwnedWimStore>>::success(
                    std::move(store));
          },
      .connection_instance_hash = connection,
      .reidentify_confirmed =
          [state](
              const ytec::clonecore::StableDiskIdentity&,
              const ytec::clonecore::StableDiskIdentity&,
              const ytec::clonecore::TargetConfirmation&) {
            state->events.push_back("reidentify");
            return ytec::clonecore::Result<
                 ytec::diskmodel::ReidentifiedPhysicalClone>::success(
                     observation(*state));
           },
      .finalize_boot =
          [state](const ytec::windowsapp::
                      WindowsDirectShrinkBootFinalizationRequest& request) {
            state->events.push_back("finalize-boot");
            const bool legacy_bios = request.firmware ==
                ytec::bootrepair::BcdBootFirmware::bios;
            const auto windows = std::find_if(
                state->fixture.plan.tasks().begin(),
                state->fixture.plan.tasks().end(),
                [](const auto& task) {
                  return task.role == ytec::migrationcore::
                      MigrationPartitionRole::windows;
                });
            const auto system = std::find_if(
                state->fixture.plan.tasks().begin(),
                state->fixture.plan.tasks().end(),
                [legacy_bios](const auto& task) {
                  return legacy_bios
                      ? task.active &&
                          (task.role == ytec::migrationcore::
                               MigrationPartitionRole::bios_system ||
                           task.role == ytec::migrationcore::
                               MigrationPartitionRole::windows)
                      : task.role == ytec::migrationcore::
                            MigrationPartitionRole::efi_system;
                });
            const auto expected_mbr_signature = legacy_bios &&
                    state->fixture.plan.mbr_preserve_binding().has_value()
                ? state->fixture.plan.mbr_preserve_binding()
                      ->target_disk_signature
                : 0U;
            state->boot_request_exact =
                windows != state->fixture.plan.tasks().end() &&
                system != state->fixture.plan.tasks().end() &&
                ytec::clonecore::validate_stable_identity(
                    state->fixture.source_identity,
                    request.expected_source,
                    L"合成BCDBootコピー元").has_value() &&
                ytec::clonecore::validate_stable_identity(
                    state->fixture.target_identity,
                    request.expected_target,
                    L"合成BCDBootコピー先").has_value() &&
                request.confirmation.first_step_acknowledged &&
                request.confirmation.typed_token == L"OK" &&
                request.expected_target_disk_number ==
                    state->fixture.target.disk_number &&
                request.expected_windows_partition_number ==
                    windows->target_number &&
                request.expected_windows_partition_offset ==
                    windows->target_offset_bytes &&
                request.expected_windows_partition_size ==
                    windows->target_size_bytes &&
                request.expected_system_partition_number ==
                    system->target_number &&
                request.expected_system_partition_offset ==
                    system->target_offset_bytes &&
                request.expected_system_partition_size ==
                    system->target_size_bytes &&
                request.expected_mbr_disk_signature ==
                    expected_mbr_signature &&
                request.firmware ==
                    (legacy_bios
                         ? ytec::bootrepair::BcdBootFirmware::bios
                         : ytec::bootrepair::BcdBootFirmware::uefi) &&
                request.windows_volume_root ==
                    L"\\\\?\\Volume{" +
                        std::to_wstring(windows->target_number) + L"}\\" &&
                request.system_volume_root ==
                    L"\\\\?\\Volume{" +
                        std::to_wstring(system->target_number) + L"}\\";
            if (!state->boot_request_exact) {
              return injected_failure<ytec::windowsapp::
                  WindowsDirectShrinkBootFinalizationEvidence>(
                  ytec::clonecore::ErrorCode::identity_mismatch,
                  L"合成BCDBoot exact request不一致");
            }
            if (state->fail_boot_finalization) {
              return injected_failure<ytec::windowsapp::
                  WindowsDirectShrinkBootFinalizationEvidence>(
                  ytec::clonecore::ErrorCode::io_failed,
                  L"合成BCDBoot最終化失敗");
            }
            return ytec::clonecore::Result<ytec::windowsapp::
                WindowsDirectShrinkBootFinalizationEvidence>::success({
                .microsoft_signed_bcdboot = true,
                .fresh_bcd_store_read_back_verified = true,
                .construction_gpt_non_bootable_verified =
                    !legacy_bios && state->boot_nonboot_gpt_verified,
                .efi_ownership_safe_before_mount =
                    !legacy_bios && state->boot_ownership_safe_before,
                .efi_ownership_revalidated_before_mutation =
                    !legacy_bios && state->boot_ownership_revalidated,
                .microsoft_boot_namespace_read_back_verified =
                    !legacy_bios && state->boot_namespace_read_back,
                .temporary_mounts_released = true,
                .final_target_reidentified = true,
                .partition_layout_unchanged = true,
                .nvram_unchanged = true,
                .legacy_bios = legacy_bios,
                .exact_target_volume_extents = true,
                .target_only_reconstruction = true,
            });
          },
      .finalize_winre =
          [state](const ytec::windowsapp::
                      WindowsDirectShrinkWinReFinalizationRequest& request) {
            state->events.push_back("finalize-winre");
            const auto windows = std::find_if(
                state->fixture.plan.tasks().begin(),
                state->fixture.plan.tasks().end(),
                [](const auto& task) {
                  return task.role == ytec::migrationcore::
                      MigrationPartitionRole::windows;
                });
            const auto recovery = std::find_if(
                state->fixture.plan.tasks().begin(),
                state->fixture.plan.tasks().end(),
                [](const auto& task) {
                  return task.role == ytec::migrationcore::
                      MigrationPartitionRole::recovery;
                });
            const bool legacy_bios = state->fixture.plan.partition_style() ==
                ytec::migrationcore::MigrationPartitionStyle::mbr;
            const auto expected_mbr_signature = legacy_bios &&
                    state->fixture.plan.mbr_preserve_binding().has_value()
                ? state->fixture.plan.mbr_preserve_binding()
                      ->target_disk_signature
                : 0U;
            state->winre_request_exact =
                windows != state->fixture.plan.tasks().end() &&
                recovery != state->fixture.plan.tasks().end() &&
                ytec::clonecore::validate_stable_identity(
                    state->fixture.source_identity,
                    request.expected_source,
                    L"合成WinREコピー元").has_value() &&
                ytec::clonecore::validate_stable_identity(
                    state->fixture.target_identity,
                    request.expected_target,
                    L"合成WinREコピー先").has_value() &&
                request.confirmation.first_step_acknowledged &&
                request.confirmation.typed_token == L"OK" &&
                request.expected_target_disk_number ==
                    state->fixture.target.disk_number &&
                request.expected_windows_partition_number ==
                    windows->target_number &&
                request.expected_windows_partition_offset ==
                    windows->target_offset_bytes &&
                request.expected_windows_partition_size ==
                    windows->target_size_bytes &&
                request.expected_recovery_partition_number ==
                    recovery->target_number &&
                request.expected_recovery_partition_offset ==
                    recovery->target_offset_bytes &&
                request.expected_recovery_partition_size ==
                    recovery->target_size_bytes &&
                request.expected_partition_style ==
                    state->fixture.plan.partition_style() &&
                request.expected_mbr_disk_signature ==
                    expected_mbr_signature &&
                request.windows_volume_root ==
                    L"\\\\?\\Volume{" +
                        std::to_wstring(windows->target_number) + L"}\\" &&
                request.recovery_volume_root ==
                    L"\\\\?\\Volume{" +
                        std::to_wstring(recovery->target_number) + L"}\\";
            if (!state->winre_request_exact) {
              return injected_failure<ytec::windowsapp::
                  WindowsDirectShrinkWinReFinalizationEvidence>(
                  ytec::clonecore::ErrorCode::identity_mismatch,
                  L"合成WinRE exact request不一致");
            }
            if (state->fail_winre_finalization) {
              return injected_failure<ytec::windowsapp::
                  WindowsDirectShrinkWinReFinalizationEvidence>(
                  ytec::clonecore::ErrorCode::io_failed,
                  L"合成WinRE最終化失敗");
            }
            return ytec::clonecore::Result<ytec::windowsapp::
                WindowsDirectShrinkWinReFinalizationEvidence>::success({
                .registered_partition_number =
                    request.expected_recovery_partition_number,
                .registered_image_size_bytes = 1ULL * kMiB,
                .microsoft_signed_reagentc = true,
                .cloned_source_registration_disabled = true,
                .candidate_identity_locked = true,
                .fixed_setreimage_arguments = true,
                .fixed_enable_arguments = true,
                .target_revalidated_before_each_mutation_and_diagnostic = true,
                .read_only_reinspection_completed = true,
                .registered_location_matches_expected_target = true,
                .registered_image_present = true,
                .temporary_mounts_released = true,
            });
          },
      .observe_mbr_safety =
          [state](
              const ytec::clonecore::StableDiskIdentity&,
              const ytec::clonecore::StableDiskIdentity&,
              const bool include_target_signature) {
            ++state->mbr_observer_calls;
            const auto& binding =
                state->fixture.plan.mbr_preserve_binding();
            if (!binding.has_value()) {
              return injected_failure<ytec::windowsapp::
                  WindowsDirectShrinkMbrSafetyEvidence>(
                  ytec::clonecore::ErrorCode::invalid_argument,
                  L"合成MBR binding欠落");
            }
            std::vector<std::uint32_t> connected{
                binding->source_disk_signature,
            };
            if (state->mbr_signature_collision) {
              connected.push_back(binding->target_disk_signature);
            }
            return ytec::clonecore::Result<ytec::windowsapp::
                WindowsDirectShrinkMbrSafetyEvidence>::success({
                .source_sector0_hash = binding->source_sector0_hash,
                .source_bootstrap = binding->source_bootstrap,
                .source_disk_signature = binding->source_disk_signature,
                .connected_mbr_signatures_excluding_target =
                    std::move(connected),
                .target_mbr_signature = include_target_signature
                    ? std::optional<std::uint32_t>{0xA0B0C0D0U}
                    : std::nullopt,
            });
          },
  };
}

ytec::windowsapp::WindowsDirectShrinkClonePlatformRequest platform_request() {
  return {
      .confirmation = {
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
  };
}

std::unique_ptr<ytec::windowsapp::IWindowsDirectShrinkClonePlatform>
make_platform(const std::shared_ptr<PlatformState>& state) {
  auto made = ytec::windowsapp::
      make_windows_direct_shrink_clone_platform_with_dependencies(
          state->fixture.plan,
          observation(*state),
          platform_request(),
          dependencies(state));
  check(made.has_value(), "production platform factory must accept GPT slice");
  return made.take_value();
}

struct ReadyToCommit final {
  std::unique_ptr<ytec::windowsapp::IWindowsDirectShrinkClonePlatform>
      platform;
  ytec::windowsapp::WindowsDirectShrinkCheckpointEvidence checkpoint;
};

ReadyToCommit execute_until_sealed_checkpoint(
    const std::shared_ptr<PlatformState>& state) {
  auto platform = make_platform(state);
  check(state->events.empty(), "factory construction must perform no I/O");
  auto plan_hash = ytec::operationcore::hash_operation_plan(
      state->fixture.plan.operation_plan());
  check(plan_hash.has_value(), "operation plan hash must build");
  auto checkpoint = platform->begin_target_owned_staging(
      state->fixture.plan, plan_hash.value());
  check(checkpoint.has_value(), "target-owned staging must begin");
  auto prepared = platform->prepare_non_archive_partitions_and_verify(
      state->fixture.plan.tasks());
  check(prepared.has_value(), "non-archive preparation must pass");
  std::uint64_t completed = prepared.value().prepared_task_count;
  std::uint64_t verified = prepared.value().verified_target_bytes;
  auto aggregate = prepared.value().write_digest;
  auto current = checkpoint.take_value();
  if (completed != 0U) {
    auto persisted = platform->persist_prepared_partitions_checkpoint(
        current, completed, verified, aggregate);
    check(persisted.has_value(), "prepared checkpoint must persist");
    current = persisted.take_value();
  }
  std::uint32_t snapshot_number{};
  for (const auto& task : state->fixture.plan.tasks()) {
    if (task.kind != ytec::windowsapp::
            WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
      continue;
    }
    ++snapshot_number;
    auto archive = platform->capture_ntfs_wim_to_owned_staging(
        task,
        {
            .original_volume_guid_path = task.original_volume_guid_path,
            .snapshot_id = L"{22222222-2222-3333-4444-" +
                std::to_wstring(555555555550ULL + snapshot_number) + L"}",
            .snapshot_device_path =
                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy" +
                std::to_wstring(6U + snapshot_number),
        });
    check(archive.has_value(), "mock WIM capture must pass");
    auto applied = platform->apply_staged_ntfs_wim_and_verify(
        task, archive.value());
    check(applied.has_value(), "mock WIM apply/readback must pass");
    auto discarded = platform->discard_exact_staged_archive(archive.value());
    check(discarded.has_value(), "exact WIM discard must pass");
    ++completed;
    verified += applied.value().verified_target_bytes;
    aggregate = applied.value().target_write_digest;
    auto progress = platform->persist_progress_checkpoint(
        current, completed, verified, aggregate);
    check(progress.has_value(), "progress checkpoint must persist");
    current = progress.take_value();
  }
  auto sealed = platform->seal_commit_ready_checkpoint(
      current,
      completed,
      verified,
      aggregate);
  check(sealed.has_value(), "commit-ready checkpoint must seal");
  return {
      .platform = std::move(platform),
      .checkpoint = sealed.take_value(),
  };
}

ReadyToCommit execute_until_commit_ready(
    const std::shared_ptr<PlatformState>& state) {
  auto sealed = execute_until_sealed_checkpoint(state);
  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, sealed.checkpoint);
  check(prepared.has_value() &&
            prepared.value().record_hash == sealed.checkpoint.record_hash &&
            prepared.value().revision == sealed.checkpoint.revision,
        "hidden-final extents must preserve the exact commit-ready checkpoint");
  if (state->fixture.plan.boot_finalization_required()) {
    auto boot = sealed.platform->finalize_boot_from_staged_layout_and_verify(
        state->fixture.plan);
    check(boot.has_value() && boot.value().completed &&
              boot.value().boot_files_read_back_verified &&
              boot.value().recovery_configuration_verified &&
              boot.value().target_offline,
          "boot and recovery finalization evidence must pass");
  }
  sealed.checkpoint = prepared.take_value();
  return sealed;
}

ytec::clonecore::Result<ytec::windowsapp::WindowsDirectShrinkBootEvidence>
attempt_system_boot_finalization(
    const std::shared_ptr<PlatformState>& state) {
  auto sealed = execute_until_sealed_checkpoint(state);
  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, sealed.checkpoint);
  if (!prepared) {
    return ytec::clonecore::Result<
        ytec::windowsapp::WindowsDirectShrinkBootEvidence>::failure(
        prepared.error());
  }
  return sealed.platform->finalize_boot_from_staged_layout_and_verify(
      state->fixture.plan);
}

void test_factory_accepts_reviewed_mbr_preserve_before_any_io() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::mbr),
  });
  auto made = ytec::windowsapp::
      make_windows_direct_shrink_clone_platform_with_dependencies(
          state->fixture.plan,
          observation(*state),
          platform_request(),
          dependencies(state));
  check(made.has_value(),
        "production factory must accept reviewed MBR-preserving data-only shrink");
  check(state->fixture.plan.mbr_preserve_binding().has_value() &&
            state->fixture.plan.partition_style() ==
                ytec::migrationcore::MigrationPartitionStyle::mbr &&
            state->events.empty() && state->mbr_observer_calls == 0U,
        "MBR factory construction must bind a fresh signature without performing I/O");
}

void test_mbr_data_only_uses_hidden_then_sector0_last() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::mbr),
  });
  auto sealed = execute_until_sealed_checkpoint(state);
  const auto checkpoint = read_writer_bytes(
      *state->writer,
      state->fixture.plan.checkpoint_offset_bytes(),
      static_cast<std::size_t>(
          ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, sealed.checkpoint);
  check(prepared.has_value(),
        "MBR data-only hidden-final preparation must pass");

  const auto hidden = read_writer_array<512U>(*state->writer, 0U);
  const auto& binding = *state->fixture.plan.mbr_preserve_binding();
  check(writer_mbr_sector0_is_valid(*state->writer) &&
            all_zero_bytes(std::span<const std::byte>(
                hidden.data(), binding.source_bootstrap.size())) &&
            read_writer_u32(*state->writer, 440U) ==
                binding.target_disk_signature &&
            writer_mbr_active_partition_count(*state->writer) == 0U &&
            read_writer_bytes(
                *state->writer,
                state->fixture.plan.checkpoint_offset_bytes(),
                checkpoint.size()) == checkpoint,
        "hidden MBR must keep the fresh signature and checkpoint while withholding bootstrap and Active bits");

  auto revalidated = sealed.platform->revalidate_before_final_commit(
      state->fixture.plan, prepared.value());
  check(revalidated.has_value(),
        "data-only hidden MBR must pass final source/signature revalidation");
  const auto writes_before_commit = state->writer->records().size();
  auto committed = sealed.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value() &&
            committed.value().final_partition_style ==
                ytec::migrationcore::MigrationPartitionStyle::mbr &&
            committed.value().source_mbr_sector0_unchanged &&
            committed.value().source_mbr_bootstrap_unchanged &&
            committed.value().target_mbr_signature_collision_free &&
            committed.value().final_mbr_sector0_read_back_verified &&
            committed.value().final_mbr_disk_signature ==
                binding.target_disk_signature &&
            committed.value().final_mbr_active_partition_count == 0U &&
            state->offline,
        "data-only final evidence must prove source unchanged, fresh signature, zero Active, and offline completion");
  const auto final_sector = read_writer_array<512U>(*state->writer, 0U);
  check(std::equal(
            binding.source_bootstrap.begin(),
            binding.source_bootstrap.end(),
            final_sector.begin()) &&
            writer_mbr_sector0_is_valid(*state->writer) &&
            state->writer->records().size() == writes_before_commit + 2U &&
            state->writer->records()[writes_before_commit].offset == 0U &&
            state->writer->records()[writes_before_commit + 1U].offset ==
                state->fixture.plan.checkpoint_offset_bytes() &&
            all_zero_bytes(read_writer_bytes(
                *state->writer,
                state->fixture.plan.checkpoint_offset_bytes(),
                checkpoint.size())),
        "visible MBR sector0 must be the last layout write and checkpoint retirement the only later write");
}

void test_mbr_system_uses_exact_legacy_bios_target_and_one_active() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = mbr_system_fixture(true, false, true),
  });
  auto sealed = execute_until_sealed_checkpoint(state);
  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, sealed.checkpoint);
  check(prepared.has_value() &&
            writer_mbr_active_partition_count(*state->writer) == 0U,
        "system MBR must remain inactive through extent preparation");
  auto boot = sealed.platform->finalize_boot_from_staged_layout_and_verify(
      state->fixture.plan);
  check(boot.has_value() && boot.value().required &&
            boot.value().completed && boot.value().target_only_reconstruction &&
            boot.value().exact_target_volume_extents &&
            boot.value().legacy_bios && boot.value().real_boot_not_claimed &&
            state->boot_request_exact && state->winre_request_exact &&
            writer_mbr_active_partition_count(*state->writer) == 0U,
        "BIOS BCDBoot must use exact target Volume GUID/extents without exposing Active or claiming a real boot");
  auto revalidated = sealed.platform->revalidate_before_final_commit(
      state->fixture.plan, prepared.value());
  check(revalidated.has_value(),
        "Legacy BIOS MBR must revalidate immediately before publication");
  auto committed = sealed.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value() &&
            committed.value().final_mbr_active_partition_count == 1U &&
            writer_mbr_active_partition_count(*state->writer) == 1U &&
            state->mbr_observer_calls >= 4U,
        "system MBR must publish exactly one immutable Active bit only in final sector0");
  const auto sector = read_writer_array<512U>(*state->writer, 0U);
  for (std::size_t index = 0U; index < 4U; ++index) {
    const auto type = std::to_integer<std::uint8_t>(
        sector[446U + index * 16U + 4U]);
    check(type == 0U || type == 0x07U || type == 0x27U,
          "final MBR must contain only reviewed 0x07/0x27 primary types");
  }
}

void test_mbr_publication_failures_preserve_latch_contract() {
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = fixture(ytec::diskmodel::PartitionStyle::mbr),
    });
    auto ready = execute_until_commit_ready(state);
    const auto checkpoint = read_writer_bytes(
        *state->writer,
        state->fixture.plan.checkpoint_offset_bytes(),
        static_cast<std::size_t>(
            ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
    state->mbr_signature_collision = true;
    auto revalidated = ready.platform->revalidate_before_final_commit(
        state->fixture.plan, ready.checkpoint);
    check(!revalidated.has_value() && state->offline &&
              !writer_mbr_sector0_is_valid(*state->writer) &&
              read_writer_bytes(
                  *state->writer,
                  state->fixture.plan.checkpoint_offset_bytes(),
                  checkpoint.size()) == checkpoint,
          "a fresh-signature collision before publication must leave no valid MBR and retain the checkpoint");
  }
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = fixture(ytec::diskmodel::PartitionStyle::mbr),
    });
    auto ready = execute_until_commit_ready(state);
    auto revalidated = ready.platform->revalidate_before_final_commit(
        state->fixture.plan, ready.checkpoint);
    check(revalidated.has_value(),
          "final MBR readback-failure fixture must revalidate");
    const auto checkpoint = read_writer_bytes(
        *state->writer,
        state->fixture.plan.checkpoint_offset_bytes(),
        static_cast<std::size_t>(
            ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
    state->writer->fail_read_after_write_number(
        state->writer->attempted_write_count() + 1U);
    auto committed = ready.platform->commit_final_layout_last(
        state->fixture.plan, revalidated.value());
    check(!committed.has_value() && state->offline &&
              !writer_mbr_sector0_is_valid(*state->writer) &&
              read_writer_bytes(
                  *state->writer,
                  state->fixture.plan.checkpoint_offset_bytes(),
                  checkpoint.size()) == checkpoint,
          "failed final sector0 readback must invalidate the incomplete target and retain its checkpoint");
  }
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = fixture(ytec::diskmodel::PartitionStyle::mbr),
    });
    auto ready = execute_until_commit_ready(state);
    auto revalidated = ready.platform->revalidate_before_final_commit(
        state->fixture.plan, ready.checkpoint);
    check(revalidated.has_value(),
          "cleanup-pending MBR fixture must revalidate");
    state->writer->fail_next_write_at(
        state->fixture.plan.checkpoint_offset_bytes());
    auto committed = ready.platform->commit_final_layout_last(
        state->fixture.plan, revalidated.value());
    check(committed.has_value() && !committed.value().checkpoint_retired &&
              committed.value().checkpoint_retirement_pending &&
              writer_mbr_sector0_is_valid(*state->writer),
          "checkpoint retirement failure after the sector0 latch must report cleanup-pending success");
    const auto sector0 = read_writer_array<512U>(*state->writer, 0U);
    const auto writes_before_abort = state->writer->write_count();
    ready.platform->abort_keep_offline_incomplete();
    check(state->writer->write_count() == writes_before_abort &&
              read_writer_array<512U>(*state->writer, 0U) == sector0 &&
              state->offline,
          "abort after final sector0 readback must never destroy the latched MBR");
  }
}

void test_factory_accepts_explicit_mbr_to_gpt_and_four_primary_layout() {
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = mbr_system_fixture(true),
    });
    auto made = ytec::windowsapp::
        make_windows_direct_shrink_clone_platform_with_dependencies(
            state->fixture.plan,
            observation(*state),
            platform_request(),
            dependencies(state));
    check(made.has_value(),
          "explicit target-only MBR-to-GPT plan must reach the GPT platform");
    check(state->events.empty(), "factory construction must remain I/O-free");
    check(
        state->fixture.plan.source_partition_style() ==
                ytec::migrationcore::MigrationPartitionStyle::mbr &&
            state->fixture.plan.partition_style() ==
                ytec::migrationcore::MigrationPartitionStyle::gpt &&
            state->fixture.plan.partition_style_choice() ==
                ytec::migrationcore::
                    DirectClonePartitionStyleChoice::mbr_to_gpt &&
            state->fixture.plan.source_partition_snapshot_hash() ==
                filled<32U>(0x47U) &&
            std::count_if(
                state->fixture.plan.source_partition_mappings().begin(),
                state->fixture.plan.source_partition_mappings().end(),
                [](const auto& mapping) {
                  return mapping.disposition == ytec::windowsapp::
                      WindowsDirectShrinkSourcePartitionDisposition::
                          replaced_by_generated_uefi_boot;
                }) == 1,
        "separate active BIOS system must be explicitly replaced by generated UEFI boot metadata");
  }
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = mbr_system_fixture(true, true),
    });
    auto made = ytec::windowsapp::
        make_windows_direct_shrink_clone_platform_with_dependencies(
            state->fixture.plan,
            observation(*state),
            platform_request(),
            dependencies(state));
    check(made.has_value(),
          "four-primary MBR source must remain eligible for target reconstruction");
    check(state->fixture.plan.source_partition_mappings().size() == 4U &&
              state->events.empty(),
          "all four MBR primaries must have immutable mappings without factory I/O");
  }
}

void test_mbr_to_gpt_active_windows_source_completes_without_mbr2gpt() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = mbr_system_fixture(false),
  });
  check(state->fixture.source.partitions.front().bootable,
        "active-Windows fixture must have an active Windows partition");
  check(
      std::none_of(
          state->fixture.plan.source_partition_mappings().begin(),
          state->fixture.plan.source_partition_mappings().end(),
          [](const auto& mapping) {
            return mapping.disposition == ytec::windowsapp::
                WindowsDirectShrinkSourcePartitionDisposition::
                    replaced_by_generated_uefi_boot;
          }),
      "active Windows must transfer directly rather than inventing an omitted BIOS partition");

  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(),
        "active-Windows MBR source must pass final read-only revalidation");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value() && committed.value().source_reidentified &&
            committed.value().source_layout_unchanged &&
            committed.value().primary_layout_committed_last && state->offline,
        "active-Windows MBR reconstruction must commit GPT last and remain offline");
  check(
      std::none_of(
          state->events.begin(), state->events.end(), [](const auto& event) {
            return event.find("mbr2gpt") != std::string::npos ||
                event.find("host-registry") != std::string::npos;
          }),
      "target-only reconstruction must not invoke MBR2GPT or host-registry mutation");
}

void test_mbr_to_gpt_source_identity_or_layout_drift_withholds_final_gpt() {
  const auto run = [](const bool identity_drift) {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = mbr_system_fixture(true),
    });
    auto ready = execute_until_commit_ready(state);
    state->source_reidentifier_mismatch = identity_drift;
    state->source_layout_drift = !identity_drift;
    auto revalidated = ready.platform->revalidate_before_final_commit(
        state->fixture.plan, ready.checkpoint);
    check(!revalidated.has_value() && state->offline,
          identity_drift
              ? "source identity drift must stop final GPT publication"
              : "source MBR layout drift must stop final GPT publication");
    auto committed = ready.platform->commit_final_layout_last(
        state->fixture.plan, ready.checkpoint);
    check(!committed.has_value(),
          "aborted source drift path must never publish the visible final GPT");
  };
  run(true);
  run(false);
}

void test_mbr_to_gpt_format_boot_winre_and_commit_failures_stay_incomplete() {
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = mbr_system_fixture(true),
        .fail_efi_format = true,
    });
    auto platform = make_platform(state);
    auto plan_hash = ytec::operationcore::hash_operation_plan(
        state->fixture.plan.operation_plan());
    check(plan_hash.has_value(), "MBR format-failure plan hash must build");
    auto checkpoint = platform->begin_target_owned_staging(
        state->fixture.plan, plan_hash.value());
    check(checkpoint.has_value(),
          "MBR format-failure path must begin target-owned staging");
    auto prepared = platform->prepare_non_archive_partitions_and_verify(
        state->fixture.plan.tasks());
    check(
        !prepared.has_value() && state->offline &&
            std::find(state->events.begin(), state->events.end(),
                      "wim-store-create") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "format-1") != state->events.end(),
        "staging NTFS must succeed before the injected ESP format failure keeps the target offline and incomplete");
  }
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = mbr_system_fixture(true),
        .fail_boot_finalization = true,
    });
    auto boot = attempt_system_boot_finalization(state);
    check(!boot.has_value() && state->offline &&
              std::find(state->events.begin(), state->events.end(),
                        "finalize-winre") == state->events.end(),
          "MBR BCDBoot failure must stop before WinRE and final commit");
  }
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = mbr_system_fixture(true),
        .fail_winre_finalization = true,
    });
    auto boot = attempt_system_boot_finalization(state);
    check(!boot.has_value() && state->offline &&
              std::find(state->events.begin(), state->events.end(),
                        "finalize-winre") != state->events.end(),
          "MBR WinRE failure must stop before final GPT publication");
  }
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = mbr_system_fixture(true),
    });
    auto ready = execute_until_commit_ready(state);
    auto revalidated = ready.platform->revalidate_before_final_commit(
        state->fixture.plan, ready.checkpoint);
    check(revalidated.has_value(),
          "MBR commit-failure fixture must reach revalidated commit-ready");
    state->writer->corrupt_next_read();
    auto committed = ready.platform->commit_final_layout_last(
        state->fixture.plan, revalidated.value());
    check(!committed.has_value() && state->offline,
          "commit readback failure must keep the reconstructed target incomplete and offline");
  }
}

void test_factory_rejects_layout_drift_before_any_io() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto drifted = observation(*state);
  drifted.target.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  drifted.target.disk_identifier =
      L"{99999999-9999-9999-9999-999999999999}";
  drifted.target.partitions.push_back(
      data_partition(ytec::diskmodel::PartitionStyle::gpt));
  auto made = ytec::windowsapp::
      make_windows_direct_shrink_clone_platform_with_dependencies(
          state->fixture.plan,
          drifted,
          platform_request(),
          dependencies(state));
  check(!made.has_value(), "layout drift must reject production factory");
  check(state->events.empty(), "layout drift must fail before I/O");
}

void test_success_publishes_final_gpt_last_and_leaves_offline() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "commit-ready state must revalidate");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value(), "final GPT commit must pass");
  check(committed.value().primary_layout_committed_last,
        "final evidence must report primary layout committed last");
  check(committed.value().checkpoint_retired &&
            !committed.value().checkpoint_retirement_pending &&
            committed.value().staging_removed &&
            committed.value().construction_layout_non_bootable &&
            committed.value().checkpoint_retained_through_extensions_and_boot &&
            committed.value().boot_completed_before_final_layout_publication &&
            committed.value().final_layout_published_before_checkpoint_retirement,
        "final evidence must prove nonboot construction, commit-last, and checkpoint retirement");
  check(state->offline, "completed target must remain offline");
  check(state->writer != nullptr && state->writer->write_count() != 0U &&
            state->writer->write_count() == state->writer->flush_count(),
        "every raw metadata write must be followed by a flush");
  check(std::find(state->events.begin(), state->events.end(),
                  "wim-capture") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "wim-apply") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "wim-discard") != state->events.end(),
        "owned WIM lifecycle must be complete");
  check(std::none_of(
            state->events.begin(),
            state->events.end(),
            [](const std::string& event) {
              return event.starts_with("unexpected-");
            }),
      "direct platform must not use restore-stream staging methods");
}

void test_exact_raw_flushes_and_reads_back_every_chunk() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(
          ytec::diskmodel::PartitionStyle::gpt,
          ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated,
          false,
          true),
  });
  auto platform = make_platform(state);
  const auto raw = std::find_if(
      state->fixture.plan.tasks().begin(),
      state->fixture.plan.tasks().end(),
      [](const auto& task) {
        return task.kind == ytec::windowsapp::
            WindowsDirectShrinkPartitionTaskKind::copy_exact_raw;
      });
  check(raw != state->fixture.plan.tasks().end(),
        "mixed fixture must contain exact RAW task");
  auto plan_hash = ytec::operationcore::hash_operation_plan(
      state->fixture.plan.operation_plan());
  check(plan_hash.has_value(), "RAW operation plan hash must build");
  auto checkpoint = platform->begin_target_owned_staging(
      state->fixture.plan, plan_hash.value());
  check(checkpoint.has_value() && state->writer != nullptr,
        "RAW target-owned staging must begin offline");
  const auto writes_before = state->writer->write_count();
  const auto flushes_before = state->writer->flush_count();
  PatternSourceReader source(state->fixture.source.size_bytes);
  auto copied = platform->copy_exact_raw_and_verify(*raw, source);
  check(
      copied.has_value() &&
          copied.value().verified_target_bytes == kExactRawBytes &&
          copied.value().verified_chunk_count == 2U &&
          copied.value().source_sha256 == copied.value().target_sha256 &&
          copied.value().target_write_digest == copied.value().target_sha256 &&
          copied.value().source_reader_read_only &&
          copied.value().source_extent_exact &&
          copied.value().every_write_flushed &&
          copied.value().every_chunk_read_back &&
          copied.value().complete_target_hash_verified &&
          copied.value().target_offline,
      "exact RAW evidence must bind full source/target SHA and two verified chunks");
  check(
      state->writer->write_count() - writes_before == 2U &&
          state->writer->flush_count() - flushes_before == 2U &&
          state->offline,
      "each 4 MiB RAW chunk must have exactly one write and flush while offline");

  auto tamper_state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(
          ytec::diskmodel::PartitionStyle::gpt,
          ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated,
          false,
          true),
  });
  auto tamper_platform = make_platform(tamper_state);
  auto tamper_hash = ytec::operationcore::hash_operation_plan(
      tamper_state->fixture.plan.operation_plan());
  auto tamper_checkpoint = tamper_platform->begin_target_owned_staging(
      tamper_state->fixture.plan, tamper_hash.value());
  check(tamper_checkpoint.has_value() && tamper_state->writer != nullptr,
        "tamper RAW staging must begin");
  const auto tamper_raw = std::find_if(
      tamper_state->fixture.plan.tasks().begin(),
      tamper_state->fixture.plan.tasks().end(),
      [](const auto& task) {
        return task.kind == ytec::windowsapp::
            WindowsDirectShrinkPartitionTaskKind::copy_exact_raw;
      });
  tamper_state->writer->corrupt_next_read();
  PatternSourceReader tamper_source(tamper_state->fixture.source.size_bytes);
  auto rejected = tamper_platform->copy_exact_raw_and_verify(
      *tamper_raw, tamper_source);
  check(
      !rejected.has_value() && tamper_state->offline,
      "RAW chunk readback mismatch must abort and keep target offline incomplete");
  auto forbidden_commit = tamper_platform->commit_final_layout_last(
      tamper_state->fixture.plan, tamper_checkpoint.value());
  check(!forbidden_commit.has_value(),
        "aborted RAW copy must never publish the final layout");

  auto cancelled_state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(
          ytec::diskmodel::PartitionStyle::gpt,
          ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated,
          false,
          true),
  });
  auto cancelled_request = platform_request();
  cancelled_request.callbacks.safe_boundary = [](const auto& boundary) {
    return boundary.kind == ytec::clonecore::
                                DiskOperationSafeBoundaryKind::verified_chunk &&
            boundary.completed_units == 1U
        ? ytec::clonecore::DiskOperationControlDecision::cancel_operation
        : ytec::clonecore::DiskOperationControlDecision::continue_operation;
  };
  auto cancelled_platform = ytec::windowsapp::
      make_windows_direct_shrink_clone_platform_with_dependencies(
          cancelled_state->fixture.plan,
          observation(*cancelled_state),
          cancelled_request,
          dependencies(cancelled_state));
  check(cancelled_platform.has_value(),
        "cancel RAW production platform must build");
  auto cancelled_hash = ytec::operationcore::hash_operation_plan(
      cancelled_state->fixture.plan.operation_plan());
  auto cancelled_checkpoint = cancelled_platform.value()->
      begin_target_owned_staging(
          cancelled_state->fixture.plan, cancelled_hash.value());
  check(cancelled_checkpoint.has_value() && cancelled_state->writer != nullptr,
        "cancel RAW staging must begin offline");
  const auto cancelled_raw = std::find_if(
      cancelled_state->fixture.plan.tasks().begin(),
      cancelled_state->fixture.plan.tasks().end(),
      [](const auto& task) {
        return task.kind == ytec::windowsapp::
            WindowsDirectShrinkPartitionTaskKind::copy_exact_raw;
      });
  PatternSourceReader cancelled_source(
      cancelled_state->fixture.source.size_bytes);
  auto cancelled = cancelled_platform.value()->copy_exact_raw_and_verify(
      *cancelled_raw, cancelled_source);
  check(
      !cancelled.has_value() &&
          cancelled.error().code == ytec::clonecore::ErrorCode::cancelled &&
          cancelled_state->offline && cancelled_state->writer->write_count() != 0U &&
          cancelled_state->writer->write_count() ==
              cancelled_state->writer->flush_count(),
      "RAW cancellation at the first verified chunk must leave an offline incomplete target");
  auto cancelled_commit = cancelled_platform.value()->commit_final_layout_last(
      cancelled_state->fixture.plan, cancelled_checkpoint.value());
  check(!cancelled_commit.has_value(),
        "cancelled RAW copy must never publish the final layout");
}

void test_readback_tamper_aborts_and_withholds_final_layout() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  check(state->writer != nullptr, "writer must remain owned by platform");
  state->writer->corrupt_next_read();
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(!revalidated.has_value(), "raw GPT readback tamper must fail closed");
  check(state->offline, "tamper abort must keep target offline");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, ready.checkpoint);
  check(!committed.has_value(), "aborted platform must never final-commit");
}

void test_identity_drift_at_commit_aborts_before_final_write() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  const auto writes_before = state->writer->write_count();
  state->reidentifier_mismatch = true;
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(!revalidated.has_value(), "identity drift must stop final commit");
  check(state->offline, "identity drift abort must keep target offline");
  check(state->writer->write_count() > writes_before,
        "abort must visibly invalidate incomplete target metadata");
}

void test_abort_never_invalidates_until_offline_is_proven() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  const auto writes_before = state->writer->write_count();
  state->fail_offline_attempts = 1U;
  ready.platform->abort_keep_offline_incomplete();
  check(state->writer->write_count() == writes_before,
        "failed offline proof must forbid raw metadata invalidation");
  ready.platform->abort_keep_offline_incomplete();
  check(state->writer->write_count() > writes_before && state->offline,
        "idempotent abort retry must withhold metadata only after offline proof");
}

void test_failed_apply_still_allows_exact_archive_discard() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto platform = make_platform(state);
  auto plan_hash = ytec::operationcore::hash_operation_plan(
      state->fixture.plan.operation_plan());
  check(plan_hash.has_value(), "operation plan hash must build");
  auto checkpoint = platform->begin_target_owned_staging(
      state->fixture.plan, plan_hash.value());
  check(checkpoint.has_value(), "target-owned staging must begin");
  const auto task = state->fixture.plan.tasks().front();
  auto archive = platform->capture_ntfs_wim_to_owned_staging(
      task,
      {
          .original_volume_guid_path = std::wstring(kSourceVolume),
          .snapshot_id = L"{22222222-2222-2222-2222-222222222222}",
          .snapshot_device_path =
              L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy7",
      });
  check(archive.has_value(), "mock WIM capture must pass");
  state->fail_wim_apply = true;
  auto applied = platform->apply_staged_ntfs_wim_and_verify(
      task, archive.value());
  check(!applied.has_value(), "injected WIM apply failure must surface");
  auto discarded = platform->discard_exact_staged_archive(archive.value());
  check(discarded.has_value(),
        "failed apply must still permit exact owned-WIM cleanup");
  check(state->offline &&
            std::find(state->events.begin(), state->events.end(),
                      "wim-discard") != state->events.end(),
        "failed apply cleanup must discard exact WIM and leave target offline");
}

void test_gpt_system_leave_unallocated_finalizes_boot_and_winre() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = system_fixture(
          ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated),
  });
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "system commit-ready state must revalidate");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value(), "GPT system leave-unallocated must commit");
  check(
      committed.value().hidden_final_layout_published_and_read_back &&
          committed.value().extended_ntfs_partition_count == 0U &&
          committed.value().every_required_ntfs_extension_verified &&
          committed.value().primary_layout_committed_last && state->offline,
      "system leave path must publish verified final GPT last without extension");
  check(
      std::find(state->events.begin(), state->events.end(), "format-1") !=
              state->events.end() &&
          std::find(state->events.begin(), state->events.end(),
                    "finalize-boot") != state->events.end() &&
          std::find(state->events.begin(), state->events.end(),
                    "finalize-winre") != state->events.end() &&
          state->boot_request_exact && state->winre_request_exact &&
          std::none_of(
              state->events.begin(), state->events.end(), [](const auto& event) {
                return event.starts_with("extend-");
              }),
      "system leave path must prepare ESP and prove BCDBoot/WinRE without growth");
}

void test_gpt_system_automatic_extends_every_planned_ntfs_before_visibility() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = system_fixture(ytec::migrationcore::
          ShrinkSurplusAllocation::automatic_proportional),
  });
  check(
      state->fixture.plan.ntfs_extension_task_count() != 0U &&
          state->fixture.plan.staging().final_growth_owner_target_number
              .has_value(),
      "automatic system fixture must own staging in a planned NTFS growth extent");
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "automatic commit-ready state must revalidate");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value(), "automatic GPT system path must commit");
  const auto extension_events = static_cast<std::uint64_t>(std::count_if(
      state->events.begin(), state->events.end(), [](const auto& event) {
        return event.starts_with("extend-");
      }));
  check(
      committed.value().hidden_final_layout_published_and_read_back &&
          committed.value().extended_ntfs_partition_count ==
              state->fixture.plan.ntfs_extension_task_count() &&
          extension_events == state->fixture.plan.ntfs_extension_task_count() &&
          committed.value().every_required_ntfs_extension_verified &&
          committed.value().primary_layout_committed_last && state->offline,
      "automatic path must verify each NTFS growth while hidden, then publish visible GPT last");
}

void test_selected_data_extension_verifies_exact_owner_size_and_readback() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(
          ytec::diskmodel::PartitionStyle::gpt,
          ytec::migrationcore::ShrinkSurplusAllocation::
              selected_data_partition),
  });
  check(
      state->fixture.plan.surplus_target_source_table_index() == 1U &&
          state->fixture.plan.ntfs_extension_task_count() == 1U &&
          state->fixture.plan.staging().final_growth_owner_target_number
              .has_value(),
      "selected-data plan must bind one reviewed source owner to one growth target");
  const auto task = std::find_if(
      state->fixture.plan.tasks().begin(),
      state->fixture.plan.tasks().end(),
      [](const auto& value) { return value.source_table_index == 1U; });
  check(
      task != state->fixture.plan.tasks().end() &&
          task->role ==
              ytec::migrationcore::MigrationPartitionRole::data &&
          task->target_number ==
              *state->fixture.plan.staging().final_growth_owner_target_number,
      "selected-data owner must resolve through task source identity, never row order");

  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "selected-data commit-ready state must revalidate");
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(
      committed.has_value() &&
          committed.value().targeted_surplus_source_table_index == 1U &&
          committed.value().targeted_surplus_target_number ==
              task->target_number &&
          committed.value().targeted_surplus_previous_file_system_bytes ==
              task->construction_size_bytes &&
          committed.value().targeted_surplus_final_file_system_bytes ==
              task->target_size_bytes &&
          committed.value().targeted_surplus_owner_verified &&
          committed.value().targeted_surplus_exact_size_verified &&
          committed.value().targeted_surplus_readback_verified &&
          committed.value().extended_ntfs_partition_count == 1U &&
          state->offline,
      "production evidence must prove selected owner, exact final size, full readback and offline commit");

  for (std::size_t failure = 0U; failure < 2U; ++failure) {
    auto failed_state = std::make_shared<PlatformState>(PlatformState{
        .fixture = fixture(
            ytec::diskmodel::PartitionStyle::gpt,
            ytec::migrationcore::ShrinkSurplusAllocation::
                selected_data_partition),
        .mismatch_ntfs_extension_size = failure == 0U,
        .fail_extension_readback = failure == 1U,
    });
    auto sealed = execute_until_sealed_checkpoint(failed_state);
    const auto prepared = sealed.platform->
        prepare_final_extents_keep_incomplete_and_verify(
            failed_state->fixture.plan, sealed.checkpoint);
    check(
        !prepared.has_value() && failed_state->offline,
        failure == 0U
            ? "selected-data exact-size mismatch must fail closed"
            : "selected-data namespace readback gap must fail closed");
  }
}

void test_data_only_gpt_preserves_one_selected_microsoft_reserved_partition() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(
          ytec::diskmodel::PartitionStyle::gpt,
          ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated,
          true),
  });
  const auto msr = std::find_if(
      state->fixture.plan.tasks().begin(),
      state->fixture.plan.tasks().end(),
      [](const auto& task) {
        return task.role == ytec::migrationcore::
            MigrationPartitionRole::microsoft_reserved;
      });
  check(
      msr != state->fixture.plan.tasks().end() &&
          msr->kind == ytec::windowsapp::
              WindowsDirectShrinkPartitionTaskKind::
                  recreate_microsoft_reserved &&
          msr->construction_size_bytes == msr->target_size_bytes,
      "data-only GPT must preserve one selected MSR as fixed metadata-only task");
  auto platform = make_platform(state);
  check(
      platform != nullptr && state->events.empty(),
      "production factory must accept the reviewed data-only GPT+MSR layout without I/O");
}

void test_system_boot_or_winre_failure_aborts_before_commit_ready() {
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = system_fixture(
            ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated),
        .fail_boot_finalization = true,
    });
    const auto boot = attempt_system_boot_finalization(state);
    check(
        !boot.has_value() && state->offline &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-boot") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-winre") == state->events.end(),
        "BCDBoot failure must abort offline before WinRE and final publication");
  }
  {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = system_fixture(
            ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated),
        .fail_winre_finalization = true,
    });
    const auto boot = attempt_system_boot_finalization(state);
    check(
        !boot.has_value() && state->offline &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-boot") != state->events.end() &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-winre") != state->events.end(),
        "WinRE registration failure must abort offline before final publication");
  }
}

void test_automatic_extension_failure_invalidates_and_keeps_offline() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = system_fixture(ytec::migrationcore::
          ShrinkSurplusAllocation::automatic_proportional),
      .fail_ntfs_extension = true,
  });
  auto sealed = execute_until_sealed_checkpoint(state);
  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, sealed.checkpoint);
  check(
      !prepared.has_value() && state->offline &&
          std::any_of(
              state->events.begin(), state->events.end(), [](const auto& event) {
                return event.starts_with("extend-");
              }),
      "extension failure must invalidate the incomplete target and keep it offline");
}

void test_gpt_phases_reuse_fresh_guids_and_withhold_boot_until_final() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = system_fixture(ytec::migrationcore::
          ShrinkSurplusAllocation::automatic_proportional),
  });
  auto sealed = execute_until_sealed_checkpoint(state);
  check(state->writer != nullptr && writer_primary_gpt_header_is_valid(
                                      *state->writer),
        "temporary construction GPT must be valid");
  const auto checkpoint_before = read_writer_bytes(
      *state->writer,
      state->fixture.plan.checkpoint_offset_bytes(),
      static_cast<std::size_t>(
          ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
  check(!all_zero_bytes(checkpoint_before),
        "commit-ready checkpoint must be durable at the fixed offset");
  const std::string checkpoint_text(
      reinterpret_cast<const char*>(checkpoint_before.data()),
      checkpoint_before.size());
  check(checkpoint_text.find("YTEC-DSC-CHK-V2") != std::string::npos &&
            checkpoint_text.find("YTEC-DSC-CHK-V1") == std::string::npos,
        "checkpoint record must reject the stale V1 domain");

  std::vector<RawGptTaskState> temporary;
  temporary.reserve(state->fixture.plan.tasks().size());
  for (const auto& task : state->fixture.plan.tasks()) {
    const auto raw = read_raw_gpt_task(*state->writer, task.target_number);
    check(raw.type_guid == ytec::clonecore::gpt_type_basic_data().bytes &&
              raw.attributes == 0x8000000000000000ULL &&
              raw.first_lba * kSectorSize == task.target_offset_bytes &&
              (raw.last_lba - raw.first_lba + 1U) * kSectorSize ==
                  task.construction_size_bytes &&
              !all_zero_bytes(raw.disk_guid) &&
              !all_zero_bytes(raw.unique_guid),
          "temporary GPT must expose every task as hidden BasicData at construction extent");
    temporary.push_back(raw);
  }

  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, sealed.checkpoint);
  check(prepared.has_value(), "hidden-final extent preparation must pass");
  check(read_writer_bytes(
            *state->writer,
            state->fixture.plan.checkpoint_offset_bytes(),
            checkpoint_before.size()) == checkpoint_before,
        "hidden-final publication and NTFS growth must preserve checkpoint bytes");
  std::vector<RawGptTaskState> hidden;
  hidden.reserve(state->fixture.plan.tasks().size());
  std::size_t index{};
  for (const auto& task : state->fixture.plan.tasks()) {
    const auto raw = read_raw_gpt_task(*state->writer, task.target_number);
    check(raw.disk_guid == temporary[index].disk_guid &&
              raw.unique_guid == temporary[index].unique_guid &&
              raw.type_guid == ytec::clonecore::gpt_type_basic_data().bytes &&
              raw.attributes == 0x8000000000000000ULL &&
              raw.first_lba * kSectorSize == task.target_offset_bytes &&
              (raw.last_lba - raw.first_lba + 1U) * kSectorSize ==
                  task.target_size_bytes,
          "hidden-final GPT must retain fresh GUIDs and final extents without exposing an ESP");
    hidden.push_back(raw);
    ++index;
  }

  auto boot = sealed.platform->finalize_boot_from_staged_layout_and_verify(
      state->fixture.plan);
  check(boot.has_value() && state->boot_request_exact &&
            state->winre_request_exact &&
            read_writer_bytes(
                *state->writer,
                state->fixture.plan.checkpoint_offset_bytes(),
                checkpoint_before.size()) == checkpoint_before,
        "BCDBoot and WinRE must use final extents while retaining the exact checkpoint");
  auto revalidated = sealed.platform->revalidate_before_final_commit(
      state->fixture.plan, prepared.value());
  check(revalidated.has_value(), "hidden-final state must revalidate");
  auto committed = sealed.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value() && committed.value().checkpoint_retired &&
            !committed.value().checkpoint_retirement_pending &&
            committed.value().construction_layout_non_bootable &&
            committed.value().checkpoint_retained_through_extensions_and_boot &&
            committed.value().boot_completed_before_final_layout_publication &&
            committed.value().final_layout_published_before_checkpoint_retirement,
        "final evidence must prove the required publication and retirement order");
  index = 0U;
  for (const auto& task : state->fixture.plan.tasks()) {
    const auto raw = read_raw_gpt_task(*state->writer, task.target_number);
    check(raw.disk_guid == hidden[index].disk_guid &&
              raw.unique_guid == hidden[index].unique_guid &&
              raw.first_lba == hidden[index].first_lba &&
              raw.last_lba == hidden[index].last_lba,
          "final GPT must keep the construction disk/partition GUIDs and final extents");
    if (task.role ==
        ytec::migrationcore::MigrationPartitionRole::efi_system) {
      check(raw.type_guid == ytec::clonecore::gpt_type_efi_system().bytes,
            "only final publication may expose the generated ESP type");
    }
    ++index;
  }
  check(writer_primary_gpt_header_is_valid(*state->writer) &&
            all_zero_bytes(read_writer_bytes(
                *state->writer,
                state->fixture.plan.checkpoint_offset_bytes(),
                checkpoint_before.size())),
        "verified final GPT must remain valid and retire the checkpoint last");
}

void test_abort_after_hidden_final_preserves_checkpoint_and_invalidates_only_gpt() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto sealed = execute_until_sealed_checkpoint(state);
  const auto checkpoint = read_writer_bytes(
      *state->writer,
      state->fixture.plan.checkpoint_offset_bytes(),
      static_cast<std::size_t>(
          ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, sealed.checkpoint);
  check(prepared.has_value() && writer_primary_gpt_header_is_valid(*state->writer),
        "hidden-final data-only state must be valid before abort");
  sealed.platform->abort_keep_offline_incomplete();
  check(state->offline && !writer_primary_gpt_header_is_valid(*state->writer) &&
            read_writer_bytes(
                *state->writer,
                state->fixture.plan.checkpoint_offset_bytes(),
                checkpoint.size()) == checkpoint,
        "abort must invalidate exact GPT ranges while preserving the durable checkpoint");
}

void test_data_only_prepare_latches_synthetic_boot_completion() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto sealed = execute_until_sealed_checkpoint(state);
  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, sealed.checkpoint);
  check(prepared.has_value() &&
            std::find(state->events.begin(), state->events.end(),
                      "finalize-boot") == state->events.end(),
        "data-only prepare must not invoke BCDBoot");
  auto revalidated = sealed.platform->revalidate_before_final_commit(
      state->fixture.plan, prepared.value());
  check(revalidated.has_value(),
        "data-only prepare must latch the core-synthesized non-required boot state");
  auto committed = sealed.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value() && state->offline,
        "data-only path must complete with the same commit-last ordering");
}

void test_prepare_rejects_nonserialized_checkpoint_evidence_drift() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto sealed = execute_until_sealed_checkpoint(state);
  const auto durable_checkpoint = read_writer_bytes(
      *state->writer,
      state->fixture.plan.checkpoint_offset_bytes(),
      static_cast<std::size_t>(
          ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
  auto drifted = sealed.checkpoint;
  drifted.target_offline = false;
  auto prepared =
      sealed.platform->prepare_final_extents_keep_incomplete_and_verify(
          state->fixture.plan, drifted);
  check(!prepared.has_value() && state->offline &&
            !writer_primary_gpt_header_is_valid(*state->writer) &&
            read_writer_bytes(
                *state->writer,
                state->fixture.plan.checkpoint_offset_bytes(),
                durable_checkpoint.size()) == durable_checkpoint,
        "prepare must compare every checkpoint evidence field, not only serialized bytes or record hash");
}

void test_final_publication_failure_keeps_checkpoint_and_nonboot_target() {
  constexpr std::size_t kExactGptInvalidationWrites = 5U;
  constexpr std::size_t kFinalGptWrites = 5U;

  const auto verify_aborted_nonboot_target = [](
      const std::shared_ptr<PlatformState>& state,
      ReadyToCommit& ready,
      const std::vector<std::byte>& checkpoint,
      const ytec::clonecore::Result<
          ytec::windowsapp::WindowsDirectShrinkFinalCommitEvidence>&
          committed) {
    check(!committed.has_value() &&
              committed.error().code == ytec::clonecore::ErrorCode::io_failed &&
              state->offline &&
              !writer_primary_gpt_header_is_valid(*state->writer) &&
              read_writer_bytes(
                  *state->writer,
                  state->fixture.plan.checkpoint_offset_bytes(),
                  checkpoint.size()) == checkpoint,
          "pre-latch final GPT failure must be classified as an aborted, offline, nonboot target with its checkpoint retained");
    const auto writes_after_failed_commit = state->writer->write_count();
    ready.platform->abort_keep_offline_incomplete();
    check(state->writer->write_count() == writes_after_failed_commit &&
              state->offline &&
              !writer_primary_gpt_header_is_valid(*state->writer),
          "an already classified publication abort must be idempotent and remain nonboot");
  };

  for (std::size_t final_write = 0U; final_write < kFinalGptWrites;
       ++final_write) {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
    });
    auto ready = execute_until_commit_ready(state);
    auto revalidated = ready.platform->revalidate_before_final_commit(
        state->fixture.plan, ready.checkpoint);
    check(revalidated.has_value(), "pre-publication state must revalidate");
    const auto checkpoint = read_writer_bytes(
        *state->writer,
        state->fixture.plan.checkpoint_offset_bytes(),
        static_cast<std::size_t>(
            ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
    state->writer->fail_on_write_number(
        state->writer->attempted_write_count() +
        kExactGptInvalidationWrites + final_write + 1U);
    const auto committed = ready.platform->commit_final_layout_last(
        state->fixture.plan, revalidated.value());
    verify_aborted_nonboot_target(state, ready, checkpoint, committed);
  }

  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(),
        "primary-header readback fixture must revalidate");
  const auto checkpoint = read_writer_bytes(
      *state->writer,
      state->fixture.plan.checkpoint_offset_bytes(),
      static_cast<std::size_t>(
          ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
  state->writer->fail_read_after_write_number(
      state->writer->attempted_write_count() +
      kExactGptInvalidationWrites + kFinalGptWrites);
  const auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(state->writer->last_injected_read_failure_offset() == kSectorSize,
        "injected final readback failure must occur after the primary GPT header write");
  verify_aborted_nonboot_target(state, ready, checkpoint, committed);
}

void test_checkpoint_retirement_failure_reports_cleanup_pending_and_keeps_final() {
  auto state = std::make_shared<PlatformState>(PlatformState{
      .fixture = fixture(ytec::diskmodel::PartitionStyle::gpt),
  });
  auto ready = execute_until_commit_ready(state);
  auto revalidated = ready.platform->revalidate_before_final_commit(
      state->fixture.plan, ready.checkpoint);
  check(revalidated.has_value(), "cleanup-pending state must revalidate");
  const auto checkpoint = read_writer_bytes(
      *state->writer,
      state->fixture.plan.checkpoint_offset_bytes(),
      static_cast<std::size_t>(
          ytec::windowsapp::kWindowsDirectShrinkCheckpointRecordBytes));
  state->writer->fail_next_write_at(
      state->fixture.plan.checkpoint_offset_bytes());
  auto committed = ready.platform->commit_final_layout_last(
      state->fixture.plan, revalidated.value());
  check(committed.has_value() && !committed.value().checkpoint_retired &&
            committed.value().checkpoint_retirement_pending &&
            writer_primary_gpt_header_is_valid(*state->writer) &&
            read_writer_bytes(
                *state->writer,
                state->fixture.plan.checkpoint_offset_bytes(),
                checkpoint.size()) == checkpoint,
        "checkpoint retirement failure after final readback must be successful cleanup-pending completion");
  const auto writes_before_abort = state->writer->write_count();
  ready.platform->abort_keep_offline_incomplete();
  check(state->writer->write_count() == writes_before_abort && state->offline &&
            writer_primary_gpt_header_is_valid(*state->writer),
        "abort after the final publication latch must never invalidate the completed GPT");
}

void test_boot_finalization_rejects_missing_nonboot_or_ownership_proof() {
  for (std::size_t failure = 0U; failure < 4U; ++failure) {
    auto state = std::make_shared<PlatformState>(PlatformState{
        .fixture = system_fixture(
            ytec::migrationcore::ShrinkSurplusAllocation::leave_unallocated),
    });
    switch (failure) {
      case 0U:
        state->boot_nonboot_gpt_verified = false;
        break;
      case 1U:
        state->boot_ownership_safe_before = false;
        break;
      case 2U:
        state->boot_ownership_revalidated = false;
        break;
      default:
        state->boot_namespace_read_back = false;
        break;
    }
    const auto boot = attempt_system_boot_finalization(state);
    check(!boot.has_value() && state->offline && state->boot_request_exact,
          "ESP/type/attrs, untrusted ownership, ownership drift, and post-BCDBoot namespace gaps must fail closed");
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"factory_accepts_reviewed_mbr_preserve_before_any_io",
       test_factory_accepts_reviewed_mbr_preserve_before_any_io},
      {"mbr_data_only_uses_hidden_then_sector0_last",
       test_mbr_data_only_uses_hidden_then_sector0_last},
      {"mbr_system_uses_exact_legacy_bios_target_and_one_active",
       test_mbr_system_uses_exact_legacy_bios_target_and_one_active},
      {"mbr_publication_failures_preserve_latch_contract",
       test_mbr_publication_failures_preserve_latch_contract},
      {"factory_accepts_explicit_mbr_to_gpt_and_four_primary_layout",
       test_factory_accepts_explicit_mbr_to_gpt_and_four_primary_layout},
      {"mbr_to_gpt_active_windows_source_completes_without_mbr2gpt",
       test_mbr_to_gpt_active_windows_source_completes_without_mbr2gpt},
      {"mbr_to_gpt_source_identity_or_layout_drift_withholds_final_gpt",
       test_mbr_to_gpt_source_identity_or_layout_drift_withholds_final_gpt},
      {"mbr_to_gpt_format_boot_winre_and_commit_failures_stay_incomplete",
       test_mbr_to_gpt_format_boot_winre_and_commit_failures_stay_incomplete},
      {"factory_rejects_layout_drift_before_any_io",
       test_factory_rejects_layout_drift_before_any_io},
      {"success_publishes_final_gpt_last_and_leaves_offline",
       test_success_publishes_final_gpt_last_and_leaves_offline},
      {"exact_raw_flushes_and_reads_back_every_chunk",
       test_exact_raw_flushes_and_reads_back_every_chunk},
      {"readback_tamper_aborts_and_withholds_final_layout",
       test_readback_tamper_aborts_and_withholds_final_layout},
      {"identity_drift_at_commit_aborts_before_final_write",
       test_identity_drift_at_commit_aborts_before_final_write},
      {"abort_never_invalidates_until_offline_is_proven",
       test_abort_never_invalidates_until_offline_is_proven},
      {"failed_apply_still_allows_exact_archive_discard",
       test_failed_apply_still_allows_exact_archive_discard},
      {"gpt_system_leave_unallocated_finalizes_boot_and_winre",
       test_gpt_system_leave_unallocated_finalizes_boot_and_winre},
      {"gpt_system_automatic_extends_every_planned_ntfs_before_visibility",
       test_gpt_system_automatic_extends_every_planned_ntfs_before_visibility},
      {"selected_data_extension_verifies_exact_owner_size_and_readback",
       test_selected_data_extension_verifies_exact_owner_size_and_readback},
      {"data_only_gpt_preserves_one_selected_microsoft_reserved_partition",
       test_data_only_gpt_preserves_one_selected_microsoft_reserved_partition},
      {"system_boot_or_winre_failure_aborts_before_commit_ready",
       test_system_boot_or_winre_failure_aborts_before_commit_ready},
      {"automatic_extension_failure_invalidates_and_keeps_offline",
       test_automatic_extension_failure_invalidates_and_keeps_offline},
      {"gpt_phases_reuse_fresh_guids_and_withhold_boot_until_final",
       test_gpt_phases_reuse_fresh_guids_and_withhold_boot_until_final},
      {"abort_after_hidden_final_preserves_checkpoint_and_invalidates_only_gpt",
       test_abort_after_hidden_final_preserves_checkpoint_and_invalidates_only_gpt},
      {"data_only_prepare_latches_synthetic_boot_completion",
       test_data_only_prepare_latches_synthetic_boot_completion},
      {"prepare_rejects_nonserialized_checkpoint_evidence_drift",
       test_prepare_rejects_nonserialized_checkpoint_evidence_drift},
      {"final_publication_failure_keeps_checkpoint_and_nonboot_target",
       test_final_publication_failure_keeps_checkpoint_and_nonboot_target},
      {"checkpoint_retirement_failure_reports_cleanup_pending_and_keeps_final",
       test_checkpoint_retirement_failure_reports_cleanup_pending_and_keeps_final},
      {"boot_finalization_rejects_missing_nonboot_or_ownership_proof",
       test_boot_finalization_rejects_missing_nonboot_or_ownership_proof},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

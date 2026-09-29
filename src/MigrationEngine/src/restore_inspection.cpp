#include "ytec/migrationengine/restore_inspection.h"

#include "ytec/migrationengine/shrink_bundle.h"

#include <cstdint>
#include <utility>

namespace ytec::migrationengine {

clonecore::Result<imageformat::RestoreImageInspectionReport>
inspect_shrink_bundle_for_restore(const std::wstring& manifest_path) {
  auto bundle = verify_shrink_bundle_read_only(manifest_path);
  if (!bundle) {
    return clonecore::Result<
        imageformat::RestoreImageInspectionReport>::failure(bundle.error());
  }
  imageformat::BackupImageManifest summary{
      .format_minor = bundle.value().manifest.source.is_system_disk
          ? imageformat::kLegacyBackupManifestMinorVersion
          : imageformat::kBackupManifestMinorVersion,
      .source = bundle.value().manifest.source,
      .physical_sector_size = bundle.value().manifest.physical_sector_size,
      .partition_style =
          bundle.value().manifest.partition_style ==
                  migrationcore::MigrationPartitionStyle::gpt
              ? imageformat::BackupPartitionStyle::gpt
              : imageformat::BackupPartitionStyle::mbr,
      .boot_mode = !bundle.value().manifest.source.is_system_disk
          ? imageformat::BackupBootMode::none
          : bundle.value().manifest.partition_style ==
                    migrationcore::MigrationPartitionStyle::gpt
                ? imageformat::BackupBootMode::uefi
                : imageformat::BackupBootMode::legacy_bios,
      .windows_major = bundle.value().manifest.windows_major,
      .windows_minor = bundle.value().manifest.windows_minor,
      .windows_build = bundle.value().manifest.windows_build,
      .windows_architecture =
          bundle.value().manifest.windows_architecture,
      .bitlocker_fully_decrypted =
          bundle.value().manifest.bitlocker_fully_decrypted,
      .created_utc = bundle.value().manifest.created_utc,
      .app_version = bundle.value().manifest.app_version,
  };
  std::uint64_t synthetic_offset = 1024U * 1024U;
  for (const auto& partition : bundle.value().manifest.partitions) {
    imageformat::BackupPartitionRole role =
        imageformat::BackupPartitionRole::ntfs_data;
    imageformat::BackupFileSystem file_system =
        imageformat::BackupFileSystem::ntfs;
    switch (partition.role) {
      case migrationcore::MigrationPartitionRole::efi_system:
        role = imageformat::BackupPartitionRole::efi_system;
        file_system = imageformat::BackupFileSystem::fat32;
        break;
      case migrationcore::MigrationPartitionRole::microsoft_reserved:
        role = imageformat::BackupPartitionRole::microsoft_reserved;
        file_system = imageformat::BackupFileSystem::none;
        break;
      case migrationcore::MigrationPartitionRole::bios_system:
      case migrationcore::MigrationPartitionRole::windows:
        role = imageformat::BackupPartitionRole::windows_ntfs;
        break;
      case migrationcore::MigrationPartitionRole::recovery:
        role = imageformat::BackupPartitionRole::recovery_ntfs;
        break;
      case migrationcore::MigrationPartitionRole::data:
        role = imageformat::BackupPartitionRole::ntfs_data;
        break;
    }
    summary.partitions.push_back(imageformat::BackupManifestPartition{
        .table_index = partition.source_table_index,
        .offset_bytes = synthetic_offset,
        .length_bytes = partition.source_size_bytes,
        .role = role,
        .file_system = file_system,
        .cluster_size = partition.cluster_size,
        .name = partition.label,
    });
    synthetic_offset += partition.source_size_bytes;
  }
  imageformat::DcimgHeader header{
      .source_disk_size = bundle.value().manifest.source.size_bytes,
      .logical_sector_size =
          bundle.value().manifest.source.logical_sector_size,
      .physical_sector_size = bundle.value().manifest.physical_sector_size,
  };
  return clonecore::Result<
      imageformat::RestoreImageInspectionReport>::success(
      imageformat::RestoreImageInspectionReport{
          .canonical_path = bundle.value().manifest_path,
          .image_length = bundle.value().manifest_length_bytes,
          .global_hash = bundle.value().manifest_sha256,
          .header = header,
          .manifest = std::move(summary),
          .complete_container_verified = true,
          .metadata_verified = true,
          .restore_layout_verified = true,
          .restore_execution_enabled = true,
          .shrink_manifest = bundle.value().manifest,
      });
}

}  // namespace ytec::migrationengine

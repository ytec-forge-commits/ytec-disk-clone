#include "ytec/vssrequester/snapshot_metadata.h"

#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/imageformat/partition_snapshot.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ytec::vssrequester {
namespace {

clonecore::Error metadata_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

clonecore::Status validate_context(
    const clonecore::ISourceDiskReader& source,
    const SnapshotMetadataContext& context) {
  const auto identity = clonecore::validate_stable_identity(
      context.source, context.source, L"バックアップコピー元");
  if (!identity) {
    return identity;
  }
  if (context.source.size_bytes != source.size_bytes() ||
      context.source.logical_sector_size !=
          source.logical_sector_size() ||
      !imageformat::is_supported_sector_size_pair(
          source.logical_sector_size(),
          context.physical_sector_size)) {
    return clonecore::Status::failure(metadata_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"バックアップメタデータ読取り元",
        L"安定識別情報と読取り専用ディスクの容量またはセクターが一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Result<clonecore::ByteRange> make_range(
    const std::uint64_t first_lba,
    const std::uint64_t sector_count,
    const clonecore::ISourceDiskReader& source) {
  const std::uint64_t sector_size = source.logical_sector_size();
  if (sector_count == 0 ||
      first_lba >
          std::numeric_limits<std::uint64_t>::max() / sector_size ||
      sector_count >
          std::numeric_limits<std::uint64_t>::max() / sector_size) {
    return clonecore::Result<clonecore::ByteRange>::failure(metadata_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"バックアップパーティション範囲",
        L"パーティション範囲がオーバーフローしました"));
  }
  const std::uint64_t offset = first_lba * sector_size;
  const std::uint64_t length = sector_count * sector_size;
  if (offset > source.size_bytes() ||
      length > source.size_bytes() - offset) {
    return clonecore::Result<clonecore::ByteRange>::failure(metadata_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"バックアップパーティション範囲",
        L"パーティションがディスク境界外です"));
  }
  return clonecore::Result<clonecore::ByteRange>::success(
      clonecore::ByteRange{.offset = offset, .length = length});
}

clonecore::Result<std::vector<std::byte>> read_boot(
    const clonecore::ISourceDiskReader& source,
    const clonecore::ByteRange& range) {
  auto bytes = source.read(range.offset, source.logical_sector_size());
  if (!bytes) {
    return bytes;
  }
  if (bytes.value().size() != source.logical_sector_size()) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        metadata_error(
            clonecore::ErrorCode::io_failed,
            ERROR_HANDLE_EOF,
            L"バックアップブートセクター読取り",
            L"ブートセクターを完全に読み取れませんでした"));
  }
  return bytes;
}

imageformat::BackupImageManifest base_manifest(
    const SnapshotMetadataContext& context,
    const imageformat::BackupPartitionStyle style) {
  return imageformat::BackupImageManifest{
      .format_minor = context.source.is_system_disk
          ? imageformat::kLegacyBackupManifestMinorVersion
          : imageformat::kBackupManifestMinorVersion,
      .source = context.source,
      .physical_sector_size = context.physical_sector_size,
      .partition_style = style,
      .boot_mode = !context.source.is_system_disk
          ? imageformat::BackupBootMode::none
          : style == imageformat::BackupPartitionStyle::gpt
                ? imageformat::BackupBootMode::uefi
                : imageformat::BackupBootMode::legacy_bios,
      .windows_major = context.windows_major,
      .windows_minor = context.windows_minor,
      .windows_build = context.windows_build,
      .windows_architecture = context.windows_architecture,
      .bitlocker_fully_decrypted = true,
      .compression = imageformat::DcimgCompression::zstandard,
      .compression_version =
          imageformat::kDcimgZstandardProfileVersion,
      .chunk_size = imageformat::kDcimgChunkSize16MiB,
      .created_utc = context.created_utc,
      .app_version = context.app_version,
  };
}

void sort_partitions(imageformat::BackupImageManifest& manifest) {
  std::sort(
      manifest.partitions.begin(),
      manifest.partitions.end(),
      [](const auto& left, const auto& right) {
        return left.offset_bytes < right.offset_bytes;
      });
}

}  // namespace

clonecore::Result<GptSnapshotMetadata>
build_gpt_snapshot_metadata(
    const clonecore::ISourceDiskReader& read_only_source,
    const SnapshotMetadataContext& context) {
  const auto valid = validate_context(read_only_source, context);
  if (!valid) {
    return clonecore::Result<GptSnapshotMetadata>::failure(valid.error());
  }
  auto layout = clonecore::parse_gpt(read_only_source);
  if (!layout) {
    return clonecore::Result<GptSnapshotMetadata>::failure(
        layout.error());
  }
  auto manifest =
      base_manifest(context, imageformat::BackupPartitionStyle::gpt);
  for (const auto& partition : layout.value().partitions) {
    if (partition.last_lba < partition.first_lba) {
      return clonecore::Result<GptSnapshotMetadata>::failure(
          metadata_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"バックアップGPTパーティション",
              L"GPTパーティション終端が開始位置より前です"));
    }
    const auto range = make_range(
        partition.first_lba,
        partition.last_lba - partition.first_lba + 1,
        read_only_source);
    if (!range) {
      return clonecore::Result<GptSnapshotMetadata>::failure(
          range.error());
    }
    imageformat::BackupManifestPartition record{
        .table_index = partition.entry_index,
        .offset_bytes = range.value().offset,
        .length_bytes = range.value().length,
        .name = std::wstring(
            partition.name.begin(), partition.name.end()),
    };
    if (partition.type_guid ==
        clonecore::gpt_type_microsoft_reserved()) {
      record.role =
          imageformat::BackupPartitionRole::microsoft_reserved;
      record.file_system = imageformat::BackupFileSystem::none;
      manifest.partitions.push_back(std::move(record));
      continue;
    }
    const auto boot = read_boot(read_only_source, range.value());
    if (!boot) {
      return clonecore::Result<GptSnapshotMetadata>::failure(
          boot.error());
    }
    if (partition.type_guid == clonecore::gpt_type_efi_system()) {
      const auto fat = clonecore::parse_fat32_geometry(
          boot.value(),
          read_only_source.logical_sector_size(),
          range.value().length);
      if (!fat) {
        return clonecore::Result<GptSnapshotMetadata>::failure(
            fat.error());
      }
      if (!context.source.is_system_disk) {
        return clonecore::Result<GptSnapshotMetadata>::failure(
            metadata_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"データ専用GPTのEFI領域",
                L"データ専用ディスクにEFIシステム領域があるため種別を確定できません"));
      }
      record.role = imageformat::BackupPartitionRole::efi_system;
      record.file_system = imageformat::BackupFileSystem::fat32;
      record.cluster_size = fat.value().cluster_size();
    } else if (
        partition.type_guid == clonecore::gpt_type_basic_data() ||
        partition.type_guid ==
            clonecore::gpt_type_windows_recovery()) {
      const auto ntfs = clonecore::parse_ntfs_geometry(
          boot.value(),
          read_only_source.logical_sector_size(),
          range.value().length);
      if (!ntfs) {
        return clonecore::Result<GptSnapshotMetadata>::failure(
            ntfs.error());
      }
      if (!context.source.is_system_disk &&
          partition.type_guid == clonecore::gpt_type_windows_recovery()) {
        return clonecore::Result<GptSnapshotMetadata>::failure(
            metadata_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"データ専用GPTの回復領域",
                L"データ専用ディスクにWindows回復領域があるため種別を確定できません"));
      }
      record.role = !context.source.is_system_disk
          ? imageformat::BackupPartitionRole::ntfs_data
          : partition.type_guid == clonecore::gpt_type_basic_data()
                ? imageformat::BackupPartitionRole::windows_ntfs
                : imageformat::BackupPartitionRole::recovery_ntfs;
      record.file_system = imageformat::BackupFileSystem::ntfs;
      record.cluster_size = ntfs.value().cluster_size();
    } else {
      return clonecore::Result<GptSnapshotMetadata>::failure(
          metadata_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"バックアップGPTパーティション種別",
              L"未対応または不明なGPTパーティション種別を検出しました"));
    }
    manifest.partitions.push_back(std::move(record));
  }
  sort_partitions(manifest);
  auto encoded_manifest =
      imageformat::build_backup_manifest_v1(manifest);
  if (!encoded_manifest) {
    return clonecore::Result<GptSnapshotMetadata>::failure(
        encoded_manifest.error());
  }
  auto snapshot = imageformat::capture_partition_snapshot_v1(
      read_only_source, imageformat::PartitionTableStyle::gpt);
  if (!snapshot) {
    return clonecore::Result<GptSnapshotMetadata>::failure(
        snapshot.error());
  }
  return clonecore::Result<GptSnapshotMetadata>::success(
      GptSnapshotMetadata{
          .layout = layout.take_value(),
          .backup_manifest = encoded_manifest.take_value(),
          .partition_table_snapshot = snapshot.take_value(),
      });
}

clonecore::Result<MbrSnapshotMetadata>
build_mbr_snapshot_metadata(
    const clonecore::ISourceDiskReader& read_only_source,
    const SnapshotMetadataContext& context) {
  const auto valid = validate_context(read_only_source, context);
  if (!valid) {
    return clonecore::Result<MbrSnapshotMetadata>::failure(valid.error());
  }
  auto layout = clonecore::parse_mbr(read_only_source);
  if (!layout) {
    return clonecore::Result<MbrSnapshotMetadata>::failure(
        layout.error());
  }
  auto manifest =
      base_manifest(context, imageformat::BackupPartitionStyle::mbr);
  for (const auto& partition : layout.value().partitions) {
    const auto range = make_range(
        partition.first_lba,
        partition.sector_count,
        read_only_source);
    if (!range) {
      return clonecore::Result<MbrSnapshotMetadata>::failure(
          range.error());
    }
    const auto boot = read_boot(read_only_source, range.value());
    if (!boot) {
      return clonecore::Result<MbrSnapshotMetadata>::failure(
          boot.error());
    }
    imageformat::BackupManifestPartition record{
        .table_index = partition.table_index,
        .offset_bytes = range.value().offset,
        .length_bytes = range.value().length,
        .name = L"MBR partition " +
            std::to_wstring(partition.table_index + 1),
    };
    if (partition.type == 0x07 || partition.type == 0x27) {
      if (partition.type == 0x27 && partition.active) {
        return clonecore::Result<MbrSnapshotMetadata>::failure(
            metadata_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"バックアップMBR Active回復区画",
                L"回復区画をBIOS起動対象として扱いません"));
      }
      const auto ntfs = clonecore::parse_ntfs_geometry(
          boot.value(),
          read_only_source.logical_sector_size(),
          range.value().length);
      if (!ntfs) {
        return clonecore::Result<MbrSnapshotMetadata>::failure(
            ntfs.error());
      }
      if (!context.source.is_system_disk && partition.type == 0x27) {
        return clonecore::Result<MbrSnapshotMetadata>::failure(
            metadata_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"データ専用MBRの回復領域",
                L"データ専用ディスクにWindows回復領域があるため種別を確定できません"));
      }
      record.role = !context.source.is_system_disk
          ? imageformat::BackupPartitionRole::ntfs_data
          : partition.type == 0x07
                ? imageformat::BackupPartitionRole::windows_ntfs
                : imageformat::BackupPartitionRole::recovery_ntfs;
      record.file_system = imageformat::BackupFileSystem::ntfs;
      record.cluster_size = ntfs.value().cluster_size();
    } else if (partition.type == 0x0B || partition.type == 0x0C) {
      const auto fat = clonecore::parse_fat32_geometry(
          boot.value(),
          read_only_source.logical_sector_size(),
          range.value().length);
      if (!fat) {
        return clonecore::Result<MbrSnapshotMetadata>::failure(
            fat.error());
      }
      record.role = imageformat::BackupPartitionRole::fat32_data;
      record.file_system = imageformat::BackupFileSystem::fat32;
      record.cluster_size = fat.value().cluster_size();
    } else {
      return clonecore::Result<MbrSnapshotMetadata>::failure(
          metadata_error(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"バックアップMBRパーティション種別",
              L"未対応または不明なMBRパーティション種別を検出しました"));
    }
    manifest.partitions.push_back(std::move(record));
  }
  sort_partitions(manifest);
  auto encoded_manifest =
      imageformat::build_backup_manifest_v1(manifest);
  if (!encoded_manifest) {
    return clonecore::Result<MbrSnapshotMetadata>::failure(
        encoded_manifest.error());
  }
  auto snapshot = imageformat::capture_partition_snapshot_v1(
      read_only_source, imageformat::PartitionTableStyle::mbr);
  if (!snapshot) {
    return clonecore::Result<MbrSnapshotMetadata>::failure(
        snapshot.error());
  }
  return clonecore::Result<MbrSnapshotMetadata>::success(
      MbrSnapshotMetadata{
          .layout = layout.take_value(),
          .backup_manifest = encoded_manifest.take_value(),
          .partition_table_snapshot = snapshot.take_value(),
      });
}

}  // namespace ytec::vssrequester

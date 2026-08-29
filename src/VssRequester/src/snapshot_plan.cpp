#include "ytec/vssrequester/snapshot_plan.h"

#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/partition_snapshot.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::vssrequester {
namespace {

clonecore::Error plan_error(
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

clonecore::Result<clonecore::ByteRange> make_range(
    const std::uint64_t first_lba,
    const std::uint64_t sector_count,
    const std::uint32_t sector_size,
    const std::uint64_t disk_size,
    const std::wstring_view operation) {
  if (sector_size == 0 || sector_count == 0 ||
      first_lba >
          std::numeric_limits<std::uint64_t>::max() / sector_size ||
      sector_count >
          std::numeric_limits<std::uint64_t>::max() / sector_size) {
    return clonecore::Result<clonecore::ByteRange>::failure(plan_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"パーティション位置または長さがオーバーフローしました"));
  }
  const std::uint64_t offset = first_lba * sector_size;
  const std::uint64_t length = sector_count * sector_size;
  if (offset > disk_size || length > disk_size - offset) {
    return clonecore::Result<clonecore::ByteRange>::failure(plan_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"パーティションがディスク境界外です"));
  }
  return clonecore::Result<clonecore::ByteRange>::success(
      clonecore::ByteRange{.offset = offset, .length = length});
}

clonecore::Status validate_options(
    const clonecore::ISourceDiskReader& source,
    const SnapshotImagePlanOptions& options,
    const imageformat::PartitionTableStyle expected_style) {
  if (!imageformat::is_supported_sector_size_pair(
          source.logical_sector_size(),
          options.physical_sector_size) ||
      (options.chunk_size != imageformat::kImageChunkSize16MiB &&
       options.chunk_size != imageformat::kImageChunkSize32MiB) ||
      options.verification_block_bytes == 0 ||
      options.verification_block_bytes >
          imageformat::kImageChunkSize32MiB ||
      options.manifest.empty() ||
      options.partition_table_snapshot.empty()) {
    return clonecore::Status::failure(plan_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSSイメージ計画オプション",
        L"セクター、チャンク、検証単位、またはメタデータが不正です"));
  }
  const auto supplied = imageformat::inspect_partition_snapshot_v1(
      options.partition_table_snapshot);
  if (!supplied ||
      supplied.value().style != expected_style ||
      supplied.value().source_disk_size != source.size_bytes() ||
      supplied.value().logical_sector_size !=
          source.logical_sector_size()) {
    return clonecore::Status::failure(plan_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"VSSイメージ パーティション表再確認",
        L"保存予定のパーティション表が読取り元と同じ形式・寸法ではありません"));
  }
  const auto backup_manifest =
      imageformat::inspect_backup_manifest_v1(options.manifest);
  const auto backup_style =
      expected_style == imageformat::PartitionTableStyle::gpt
      ? imageformat::BackupPartitionStyle::gpt
      : imageformat::BackupPartitionStyle::mbr;
  if (!backup_manifest ||
      backup_manifest.value().source.size_bytes != source.size_bytes() ||
      backup_manifest.value().source.logical_sector_size !=
          source.logical_sector_size() ||
      backup_manifest.value().physical_sector_size !=
          options.physical_sector_size ||
      backup_manifest.value().partition_style != backup_style ||
      backup_manifest.value().chunk_size != options.chunk_size ||
      backup_manifest.value().compression !=
          options.compression) {
    return clonecore::Status::failure(plan_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"VSSイメージ バックアップマニフェスト",
        L"マニフェストのディスク、セクター、方式、チャンク、または圧縮が計画と一致しません"));
  }
  const auto observed =
      imageformat::capture_partition_snapshot_v1(source, expected_style);
  if (!observed) {
    return clonecore::Status::failure(observed.error());
  }
  if (observed.value() != options.partition_table_snapshot) {
    return clonecore::Status::failure(plan_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_CRC,
        L"VSSイメージ パーティション表同一性",
        L"保存予定のパーティション表が現在の読取り元と一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::size_t> find_unique_binding(
    const std::uint32_t partition_index,
    const std::span<const clonecore::VolumeBitmapBinding> bindings,
    const std::span<const std::uint8_t> used) {
  std::size_t found = bindings.size();
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    if (bindings[index].partition_entry_index != partition_index) {
      continue;
    }
    if (found != bindings.size() || used[index] ||
        bindings[index].volume_device_path.empty()) {
      return clonecore::Result<std::size_t>::failure(plan_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"VSS Volume GUID対応",
          L"NTFSパーティションへのVolume GUID対応が空または重複しています"));
    }
    found = index;
  }
  if (found == bindings.size()) {
    return clonecore::Result<std::size_t>::failure(plan_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"VSS Volume GUID対応",
        L"対象NTFSパーティションのVolume GUIDがありません"));
  }
  return clonecore::Result<std::size_t>::success(found);
}

clonecore::Result<std::vector<std::byte>> read_boot_sector(
    const clonecore::ISourceDiskReader& source,
    const clonecore::ByteRange& range) {
  auto bytes = source.read(range.offset, source.logical_sector_size());
  if (!bytes) {
    return bytes;
  }
  if (bytes.value().size() != source.logical_sector_size()) {
    return clonecore::Result<std::vector<std::byte>>::failure(plan_error(
        clonecore::ErrorCode::io_failed,
        ERROR_HANDLE_EOF,
        L"VSS固定領域ブートセクター",
        L"ブートセクターを完全に読み取れませんでした"));
  }
  return bytes;
}

clonecore::Result<const imageformat::BackupManifestPartition*>
find_manifest_partition(
    const imageformat::BackupImageManifest& manifest,
    const std::uint32_t table_index,
    const clonecore::ByteRange& range,
    const imageformat::BackupPartitionRole expected_role) {
  const imageformat::BackupManifestPartition* found = nullptr;
  for (const auto& partition : manifest.partitions) {
    if (partition.table_index != table_index) {
      continue;
    }
    if (found != nullptr ||
        partition.offset_bytes != range.offset ||
        partition.length_bytes != range.length ||
        partition.role != expected_role) {
      return clonecore::Result<
          const imageformat::BackupManifestPartition*>::failure(plan_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"VSSイメージ マニフェストパーティション",
          L"パーティション番号、範囲、または役割が現在のディスクと一致しません"));
    }
    found = &partition;
  }
  if (found == nullptr) {
    return clonecore::Result<
        const imageformat::BackupManifestPartition*>::failure(plan_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"VSSイメージ マニフェストパーティション",
        L"現在のディスクパーティションがマニフェストにありません"));
  }
  return clonecore::Result<
      const imageformat::BackupManifestPartition*>::success(found);
}

PreparedSnapshotImagePlan base_plan(
    const clonecore::ISourceDiskReader& source,
    const SnapshotImagePlanOptions& options) {
  PreparedSnapshotImagePlan result;
  result.workflow.administrator = options.administrator;
  result.image_copy.source_disk_size = source.size_bytes();
  result.image_copy.logical_sector_size =
      source.logical_sector_size();
  result.image_copy.physical_sector_size =
      options.physical_sector_size;
  result.image_copy.chunk_size = options.chunk_size;
  result.image_copy.compression = options.compression;
  result.image_copy.verification_block_bytes =
      options.verification_block_bytes;
  result.image_copy.manifest = options.manifest;
  result.image_copy.partition_table_snapshot =
      options.partition_table_snapshot;
  return result;
}

clonecore::Status reject_unused_bindings(
    const std::span<const std::uint8_t> used) {
  if (std::any_of(
          used.begin(),
          used.end(),
          [](const std::uint8_t value) { return value == 0; })) {
    return clonecore::Status::failure(plan_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"VSS Volume GUID余剰対応",
        L"パーティション表に存在しない余剰Volume GUID対応があります"));
  }
  return clonecore::success_status();
}

bool same_gpt(
    const clonecore::GptDisk& expected,
    const clonecore::GptDisk& observed) {
  if (expected.logical_sector_size != observed.logical_sector_size ||
      expected.sector_count != observed.sector_count ||
      expected.disk_guid != observed.disk_guid ||
      expected.first_usable_lba != observed.first_usable_lba ||
      expected.last_usable_lba != observed.last_usable_lba ||
      expected.partition_entry_count != observed.partition_entry_count ||
      expected.partition_entry_size != observed.partition_entry_size ||
      expected.partitions.size() != observed.partitions.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.partitions.size(); ++index) {
    const auto& left = expected.partitions[index];
    const auto& right = observed.partitions[index];
    if (left.entry_index != right.entry_index ||
        left.type_guid != right.type_guid ||
        left.unique_guid != right.unique_guid ||
        left.first_lba != right.first_lba ||
        left.last_lba != right.last_lba ||
        left.attributes != right.attributes ||
        left.name != right.name) {
      return false;
    }
  }
  return true;
}

bool same_mbr(
    const clonecore::MbrDisk& expected,
    const clonecore::MbrDisk& observed) {
  if (expected.logical_sector_size != observed.logical_sector_size ||
      expected.sector_count != observed.sector_count ||
      expected.disk_signature != observed.disk_signature ||
      expected.bootstrap != observed.bootstrap ||
      expected.partitions.size() != observed.partitions.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.partitions.size(); ++index) {
    const auto& left = expected.partitions[index];
    const auto& right = observed.partitions[index];
    if (left.table_index != right.table_index ||
        left.active != right.active ||
        left.first_chs != right.first_chs ||
        left.type != right.type ||
        left.last_chs != right.last_chs ||
        left.first_lba != right.first_lba ||
        left.sector_count != right.sector_count) {
      return false;
    }
  }
  return true;
}

clonecore::Status validate_selected_partition_entries(
    const std::span<const std::uint32_t> selected,
    const std::span<const std::uint32_t> known,
    const std::wstring_view operation) {
  if (selected.empty()) {
    return clonecore::success_status();
  }
  if (!std::is_sorted(selected.begin(), selected.end()) ||
      std::adjacent_find(selected.begin(), selected.end()) != selected.end()) {
    return clonecore::Status::failure(plan_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"選択したpartition entry indexがcanonical順序または一意性を満たしません"));
  }
  for (const auto entry : selected) {
    if (std::find(known.begin(), known.end(), entry) == known.end()) {
      return clonecore::Status::failure(plan_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          std::wstring(operation),
          L"選択したpartition entry indexが再解析レイアウトにありません"));
    }
  }
  return clonecore::success_status();
}

bool partition_entry_selected(
    const SnapshotImagePlanOptions& options,
    const std::uint32_t entry_index) noexcept {
  return options.selected_partition_entry_indices.empty() ||
      std::binary_search(
          options.selected_partition_entry_indices.begin(),
          options.selected_partition_entry_indices.end(),
          entry_index);
}

}  // namespace

clonecore::Result<PreparedSnapshotImagePlan>
prepare_gpt_snapshot_image_plan(
    const clonecore::GptDisk& source_gpt,
    const clonecore::ISourceDiskReader& read_only_source,
    const std::span<const clonecore::VolumeBitmapBinding> ntfs_bindings,
    const SnapshotImagePlanOptions& options) {
  if (source_gpt.logical_sector_size !=
          read_only_source.logical_sector_size() ||
      source_gpt.sector_count >
          std::numeric_limits<std::uint64_t>::max() /
              source_gpt.logical_sector_size ||
      source_gpt.sector_count * source_gpt.logical_sector_size !=
          read_only_source.size_bytes()) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(plan_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"VSS GPT読取り元寸法",
        L"GPT解析結果と読取り専用ディスクの寸法が一致しません"));
  }
  const auto valid = validate_options(
      read_only_source, options, imageformat::PartitionTableStyle::gpt);
  if (!valid) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        valid.error());
  }
  const auto observed_gpt = clonecore::parse_gpt(read_only_source);
  if (!observed_gpt || !same_gpt(source_gpt, observed_gpt.value())) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        observed_gpt
            ? plan_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_INVALID_DATA,
                  L"VSS GPT再解析同一性",
                  L"指定GPTと現在の読取り元GPTが一致しません")
            : observed_gpt.error());
  }
  const auto backup_manifest =
      imageformat::inspect_backup_manifest_v1(options.manifest);
  if (!backup_manifest ||
      backup_manifest.value().partitions.size() !=
          source_gpt.partitions.size()) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        backup_manifest
            ? plan_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_INVALID_DATA,
                  L"VSS GPTマニフェスト件数",
                  L"GPTとマニフェストのパーティション件数が一致しません")
            : backup_manifest.error());
  }
  std::vector<std::uint32_t> known_entries;
  known_entries.reserve(source_gpt.partitions.size());
  for (const auto& partition : source_gpt.partitions) {
    known_entries.push_back(partition.entry_index);
  }
  const auto selected_entries = validate_selected_partition_entries(
      options.selected_partition_entry_indices,
      known_entries,
      L"VSS GPT partition選択");
  if (!selected_entries) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        selected_entries.error());
  }

  std::vector<const clonecore::GptPartition*> partitions;
  partitions.reserve(source_gpt.partitions.size());
  for (const auto& partition : source_gpt.partitions) {
    partitions.push_back(&partition);
  }
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const auto* left, const auto* right) {
        return left->first_lba < right->first_lba;
      });

  PreparedSnapshotImagePlan plan = base_plan(read_only_source, options);
  std::vector<std::uint8_t> used(ntfs_bindings.size(), 0);
  for (const auto* partition : partitions) {
    if (!partition_entry_selected(options, partition->entry_index)) {
      continue;
    }
    if (partition->last_lba < partition->first_lba) {
      return clonecore::Result<PreparedSnapshotImagePlan>::failure(plan_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"VSS GPTパーティション範囲",
          L"GPTパーティション終端が開始位置より前です"));
    }
    const auto range = make_range(
        partition->first_lba,
        partition->last_lba - partition->first_lba + 1,
        source_gpt.logical_sector_size,
        read_only_source.size_bytes(),
        L"VSS GPTパーティション範囲");
    if (!range) {
      return clonecore::Result<PreparedSnapshotImagePlan>::failure(
          range.error());
    }
    if (partition->type_guid == clonecore::gpt_type_microsoft_reserved()) {
      const auto declared = find_manifest_partition(
          backup_manifest.value(),
          partition->entry_index,
          range.value(),
          imageformat::BackupPartitionRole::microsoft_reserved);
      if (!declared) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            declared.error());
      }
      ++plan.recreated_partition_count;
      continue;
    }
    if (partition->type_guid == clonecore::gpt_type_basic_data()) {
      const auto declared = find_manifest_partition(
          backup_manifest.value(),
          partition->entry_index,
          range.value(),
          backup_manifest.value().source.is_system_disk
              ? imageformat::BackupPartitionRole::windows_ntfs
              : imageformat::BackupPartitionRole::ntfs_data);
      if (!declared) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            declared.error());
      }
      const auto boot = read_boot_sector(read_only_source, range.value());
      if (!boot) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            boot.error());
      }
      const auto ntfs = clonecore::parse_ntfs_geometry(
          boot.value(),
          source_gpt.logical_sector_size,
          range.value().length);
      if (!ntfs ||
          ntfs.value().cluster_size() !=
              declared.value()->cluster_size) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            ntfs
                ? plan_error(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_INVALID_DATA,
                      L"VSS GPT NTFSクラスタ",
                      L"現在のNTFSクラスタサイズがマニフェストと一致しません")
                : ntfs.error());
      }
      const auto binding = find_unique_binding(
          partition->entry_index, ntfs_bindings, used);
      if (!binding) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            binding.error());
      }
      used[binding.value()] = 1;
      plan.workflow.volumes.push_back(VolumeRequest{
          .volume_guid_path =
              ntfs_bindings[binding.value()].volume_device_path,
          .file_system = L"NTFS",
      });
      plan.image_copy.volumes.push_back(SnapshotImageVolumePlan{
          .partition_entry_index = partition->entry_index,
          .disk_offset = range.value().offset,
          .partition_length = range.value().length,
      });
      ++plan.snapshot_partition_count;
      continue;
    }

    const auto boot = read_boot_sector(read_only_source, range.value());
    if (!boot) {
      return clonecore::Result<PreparedSnapshotImagePlan>::failure(
          boot.error());
    }
    if (partition->type_guid == clonecore::gpt_type_efi_system()) {
      const auto declared = find_manifest_partition(
          backup_manifest.value(),
          partition->entry_index,
          range.value(),
          imageformat::BackupPartitionRole::efi_system);
      if (!declared) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            declared.error());
      }
      const auto fat = clonecore::parse_fat32_geometry(
          boot.value(),
          source_gpt.logical_sector_size,
          range.value().length);
      if (!fat) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            fat.error());
      }
      if (fat.value().cluster_size() !=
          declared.value()->cluster_size) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            plan_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"VSS GPT EFIクラスタ",
                L"EFIクラスタサイズがマニフェストにありません"));
      }
    } else if (
        partition->type_guid == clonecore::gpt_type_windows_recovery()) {
      const auto declared = find_manifest_partition(
          backup_manifest.value(),
          partition->entry_index,
          range.value(),
          imageformat::BackupPartitionRole::recovery_ntfs);
      if (!declared) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            declared.error());
      }
      const auto ntfs = clonecore::parse_ntfs_geometry(
          boot.value(),
          source_gpt.logical_sector_size,
          range.value().length);
      if (!ntfs) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            ntfs.error());
      }
      if (ntfs.value().cluster_size() !=
          declared.value()->cluster_size) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            plan_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"VSS GPT回復クラスタ",
                L"回復NTFSクラスタサイズがマニフェストと一致しません"));
      }
    } else {
      return clonecore::Result<PreparedSnapshotImagePlan>::failure(plan_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"VSS GPTパーティション種別",
          L"未対応または不明なGPTパーティション種別を検出しました"));
    }
    plan.image_copy.raw_regions.push_back(VssSnapshotImageRawRegion{
        .disk_offset = range.value().offset,
        .length = range.value().length,
        .source_offset = range.value().offset,
        .source_reader = &read_only_source,
    });
    ++plan.raw_partition_count;
  }
  const auto all_used = reject_unused_bindings(used);
  if (!all_used) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        all_used.error());
  }
  if (plan.workflow.volumes.empty()) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(plan_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"VSS GPT Snapshot対象",
        L"VSS対象のBasic Data NTFSパーティションがありません"));
  }
  return clonecore::Result<PreparedSnapshotImagePlan>::success(
      std::move(plan));
}

clonecore::Result<PreparedSnapshotImagePlan>
prepare_mbr_snapshot_image_plan(
    const clonecore::MbrDisk& source_mbr,
    const clonecore::ISourceDiskReader& read_only_source,
    const std::span<const clonecore::VolumeBitmapBinding> ntfs_bindings,
    const SnapshotImagePlanOptions& options) {
  if (source_mbr.logical_sector_size !=
          read_only_source.logical_sector_size() ||
      source_mbr.sector_count >
          std::numeric_limits<std::uint64_t>::max() /
              source_mbr.logical_sector_size ||
      source_mbr.sector_count * source_mbr.logical_sector_size !=
          read_only_source.size_bytes()) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(plan_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"VSS MBR読取り元寸法",
        L"MBR解析結果と読取り専用ディスクの寸法が一致しません"));
  }
  const auto valid = validate_options(
      read_only_source, options, imageformat::PartitionTableStyle::mbr);
  if (!valid) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        valid.error());
  }
  const auto observed_mbr = clonecore::parse_mbr(read_only_source);
  if (!observed_mbr || !same_mbr(source_mbr, observed_mbr.value())) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        observed_mbr
            ? plan_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_INVALID_DATA,
                  L"VSS MBR再解析同一性",
                  L"指定MBRと現在の読取り元MBRが一致しません")
            : observed_mbr.error());
  }
  const auto backup_manifest =
      imageformat::inspect_backup_manifest_v1(options.manifest);
  if (!backup_manifest ||
      backup_manifest.value().partitions.size() !=
          source_mbr.partitions.size()) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        backup_manifest
            ? plan_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_INVALID_DATA,
                  L"VSS MBRマニフェスト件数",
                  L"MBRとマニフェストのパーティション件数が一致しません")
            : backup_manifest.error());
  }
  std::vector<std::uint32_t> known_entries;
  known_entries.reserve(source_mbr.partitions.size());
  for (const auto& partition : source_mbr.partitions) {
    known_entries.push_back(partition.table_index);
  }
  const auto selected_entries = validate_selected_partition_entries(
      options.selected_partition_entry_indices,
      known_entries,
      L"VSS MBR partition選択");
  if (!selected_entries) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        selected_entries.error());
  }

  std::vector<const clonecore::MbrPartition*> partitions;
  partitions.reserve(source_mbr.partitions.size());
  for (const auto& partition : source_mbr.partitions) {
    partitions.push_back(&partition);
  }
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const auto* left, const auto* right) {
        return left->first_lba < right->first_lba;
      });

  PreparedSnapshotImagePlan plan = base_plan(read_only_source, options);
  std::vector<std::uint8_t> used(ntfs_bindings.size(), 0);
  for (const auto* partition : partitions) {
    if (!partition_entry_selected(options, partition->table_index)) {
      continue;
    }
    const auto range = make_range(
        partition->first_lba,
        partition->sector_count,
        source_mbr.logical_sector_size,
        read_only_source.size_bytes(),
        L"VSS MBRパーティション範囲");
    if (!range) {
      return clonecore::Result<PreparedSnapshotImagePlan>::failure(
          range.error());
    }
    if (partition->type == 0x07) {
      const auto declared = find_manifest_partition(
          backup_manifest.value(),
          partition->table_index,
          range.value(),
          backup_manifest.value().source.is_system_disk
              ? imageformat::BackupPartitionRole::windows_ntfs
              : imageformat::BackupPartitionRole::ntfs_data);
      if (!declared) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            declared.error());
      }
      const auto boot = read_boot_sector(read_only_source, range.value());
      if (!boot) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            boot.error());
      }
      const auto ntfs = clonecore::parse_ntfs_geometry(
          boot.value(),
          source_mbr.logical_sector_size,
          range.value().length);
      if (!ntfs ||
          ntfs.value().cluster_size() !=
              declared.value()->cluster_size) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            ntfs
                ? plan_error(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_INVALID_DATA,
                      L"VSS MBR NTFSクラスタ",
                      L"現在のNTFSクラスタサイズがマニフェストと一致しません")
                : ntfs.error());
      }
      const auto binding = find_unique_binding(
          partition->table_index, ntfs_bindings, used);
      if (!binding) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            binding.error());
      }
      used[binding.value()] = 1;
      plan.workflow.volumes.push_back(VolumeRequest{
          .volume_guid_path =
              ntfs_bindings[binding.value()].volume_device_path,
          .file_system = L"NTFS",
      });
      plan.image_copy.volumes.push_back(SnapshotImageVolumePlan{
          .partition_entry_index = partition->table_index,
          .disk_offset = range.value().offset,
          .partition_length = range.value().length,
      });
      ++plan.snapshot_partition_count;
      continue;
    }

    const auto boot = read_boot_sector(read_only_source, range.value());
    if (!boot) {
      return clonecore::Result<PreparedSnapshotImagePlan>::failure(
          boot.error());
    }
    if (partition->type == 0x27) {
      const auto declared = find_manifest_partition(
          backup_manifest.value(),
          partition->table_index,
          range.value(),
          imageformat::BackupPartitionRole::recovery_ntfs);
      if (!declared) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            declared.error());
      }
      if (partition->active) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            plan_error(
                clonecore::ErrorCode::unsupported_layout,
                ERROR_NOT_SUPPORTED,
                L"VSS MBR Active回復区画",
                L"回復区画をBIOS起動対象として扱いません"));
      }
      const auto ntfs = clonecore::parse_ntfs_geometry(
          boot.value(),
          source_mbr.logical_sector_size,
          range.value().length);
      if (!ntfs) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            ntfs.error());
      }
      if (ntfs.value().cluster_size() !=
          declared.value()->cluster_size) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            plan_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"VSS MBR回復クラスタ",
                L"回復NTFSクラスタサイズがマニフェストと一致しません"));
      }
    } else if (partition->type == 0x0B || partition->type == 0x0C) {
      const auto declared = find_manifest_partition(
          backup_manifest.value(),
          partition->table_index,
          range.value(),
          imageformat::BackupPartitionRole::fat32_data);
      if (!declared) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            declared.error());
      }
      const auto fat = clonecore::parse_fat32_geometry(
          boot.value(),
          source_mbr.logical_sector_size,
          range.value().length);
      if (!fat) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            fat.error());
      }
      if (fat.value().cluster_size() !=
          declared.value()->cluster_size) {
        return clonecore::Result<PreparedSnapshotImagePlan>::failure(
            plan_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"VSS MBR FAT32クラスタ",
                L"FAT32クラスタサイズがマニフェストにありません"));
      }
    } else {
      return clonecore::Result<PreparedSnapshotImagePlan>::failure(plan_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"VSS MBRパーティション種別",
          L"未対応または不明なMBRパーティション種別を検出しました"));
    }
    plan.image_copy.raw_regions.push_back(VssSnapshotImageRawRegion{
        .disk_offset = range.value().offset,
        .length = range.value().length,
        .source_offset = range.value().offset,
        .source_reader = &read_only_source,
    });
    ++plan.raw_partition_count;
  }
  const auto all_used = reject_unused_bindings(used);
  if (!all_used) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(
        all_used.error());
  }
  if (plan.workflow.volumes.empty()) {
    return clonecore::Result<PreparedSnapshotImagePlan>::failure(plan_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"VSS MBR Snapshot対象",
        L"VSS対象の0x07 NTFSパーティションがありません"));
  }
  return clonecore::Result<PreparedSnapshotImagePlan>::success(
      std::move(plan));
}

}  // namespace ytec::vssrequester

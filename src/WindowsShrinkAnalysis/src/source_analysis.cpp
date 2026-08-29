#include "ytec/windowsshrink/source_analysis.h"

#include "ytec/bootrepair/offline_windows.h"
#include "ytec/clonecore/offline_gpt_clone.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/partition_snapshot.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ytec::windowsshrink {
namespace {

struct PartitionCandidate final {
  // Formal .tsumugi manifests use one-based partition numbers.  The value is
  // also used as the opaque correlation key while resolving Volume GUIDs.
  std::uint32_t table_index{};
  migrationcore::MigrationPartitionRole role{
      migrationcore::MigrationPartitionRole::data};
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
  bool active{};
  std::wstring name;
  std::array<std::byte, 16U> type_id{};
  std::array<std::byte, 16U> unique_id{};
};

struct VolumeFacts final {
  std::uint64_t used_bytes{};
  std::uint64_t cluster_size{};
  std::wstring label;
};

clonecore::Error analysis_error(
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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(analysis_error(
      code, native_code, std::move(operation), std::move(message)));
}

clonecore::Status validate_common(
    const diskmodel::DiskInfo& disk,
    const clonecore::ISourceDiskReader& reader,
    const ShrinkSourceAnalysisContext& context) {
  const auto identity = clonecore::validate_stable_identity(
      context.source_identity, context.source_identity, L"縮小移行コピー元");
  if (!identity) {
    return identity;
  }
  if (disk.disk_number != context.source_identity.disk_number ||
      disk.size_bytes != reader.size_bytes() ||
      disk.size_bytes != context.source_identity.size_bytes ||
      disk.logical_sector_size != reader.logical_sector_size() ||
      disk.logical_sector_size != context.source_identity.logical_sector_size ||
      disk.physical_sector_size != context.physical_sector_size ||
      context.created_utc.empty() || context.app_version.empty()) {
    return clonecore::Status::failure(analysis_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"縮小移行コピー元寸法",
        L"再識別ディスク、読取り専用ハンドル、または作成情報が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::uint64_t> checked_bytes(
    const std::uint64_t sectors,
    const std::uint32_t sector_size,
    const std::wstring_view operation) {
  if (sector_size == 0U ||
      sectors > (std::numeric_limits<std::uint64_t>::max)() / sector_size) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"セクター数からバイト数への変換がオーバーフローしました");
  }
  return clonecore::Result<std::uint64_t>::success(sectors * sector_size);
}

clonecore::Result<VolumeFacts> query_volume_facts(
    const std::wstring& volume_path,
    const std::uint64_t partition_size) {
  std::array<wchar_t, 128> label{};
  std::array<wchar_t, 32> file_system{};
  if (!GetVolumeInformationW(
          volume_path.c_str(),
          label.data(),
          static_cast<DWORD>(label.size()),
          nullptr,
          nullptr,
          nullptr,
          file_system.data(),
          static_cast<DWORD>(file_system.size()))) {
    return failure<VolumeFacts>(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"縮小移行NTFSボリューム情報",
        L"ボリューム情報を読み取れません。ロック、暗号化、または未対応形式の可能性があります");
  }
  if (_wcsicmp(file_system.data(), L"NTFS") != 0) {
    return failure<VolumeFacts>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小移行ファイルシステム",
        L"縮小移行の内容領域はNTFSだけに対応します");
  }
  ULARGE_INTEGER available{};
  ULARGE_INTEGER total{};
  ULARGE_INTEGER free{};
  if (!GetDiskFreeSpaceExW(
          volume_path.c_str(), &available, &total, &free) ||
      total.QuadPart == 0U || free.QuadPart > total.QuadPart ||
      total.QuadPart > partition_size) {
    return failure<VolumeFacts>(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"縮小移行NTFS使用量",
        L"ボリュームの総容量と空き容量を安全に取得できません");
  }
  DWORD sectors_per_cluster = 0;
  DWORD bytes_per_sector = 0;
  DWORD free_clusters = 0;
  DWORD total_clusters = 0;
  if (!GetDiskFreeSpaceW(
          volume_path.c_str(),
          &sectors_per_cluster,
          &bytes_per_sector,
          &free_clusters,
          &total_clusters) ||
      sectors_per_cluster == 0U || bytes_per_sector == 0U ||
      sectors_per_cluster >
          (std::numeric_limits<std::uint64_t>::max)() / bytes_per_sector) {
    return failure<VolumeFacts>(
        clonecore::ErrorCode::query_failed,
        GetLastError(),
        L"縮小移行NTFSクラスター",
        L"NTFSクラスター寸法を取得できません");
  }
  return clonecore::Result<VolumeFacts>::success(VolumeFacts{
      .used_bytes = total.QuadPart - free.QuadPart,
      .cluster_size =
          static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector,
      .label = label.data(),
  });
}

bool is_regular_non_reparse(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ==
          0U;
}

std::wstring volume_child(
    const std::wstring& root,
    const std::wstring_view relative) {
  std::wstring value = root;
  if (!value.ends_with(L'\\')) {
    value.push_back(L'\\');
  }
  value.append(relative);
  return value;
}

clonecore::Result<WindowsSourceVersion> inspect_windows_volume(
    const std::wstring& volume_path) {
  const auto verified = bootrepair::verify_offline_windows_amd64(volume_path);
  if (!verified) {
    return clonecore::Result<WindowsSourceVersion>::failure(verified.error());
  }
  const auto version = bootrepair::read_offline_windows_version_hive(
      volume_child(volume_path, L"Windows\\System32\\Config\\SOFTWARE"));
  if (!version) {
    return clonecore::Result<WindowsSourceVersion>::failure(version.error());
  }
  return clonecore::Result<WindowsSourceVersion>::success(
      WindowsSourceVersion{
          .major = version.value().major,
          .minor = 0,
          .build = version.value().build,
          .architecture = "AMD64",
      });
}

std::wstring widen_gpt_name(const std::u16string& name) {
  static_assert(sizeof(wchar_t) == sizeof(char16_t));
  return std::wstring(name.begin(), name.end());
}

std::array<std::byte, 16U> guid_bytes(const clonecore::GptGuid& guid) {
  return guid.bytes;
}

clonecore::Result<std::vector<std::byte>> read_partition_boot(
    const clonecore::ISourceDiskReader& reader,
    const PartitionCandidate& partition) {
  auto bytes = reader.read(partition.offset_bytes, reader.logical_sector_size());
  if (!bytes) {
    return bytes;
  }
  if (bytes.value().size() != reader.logical_sector_size()) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::io_failed,
        ERROR_HANDLE_EOF,
        L"縮小移行パーティション先頭読取り",
        L"ブートセクターを完全に読み取れません");
  }
  return bytes;
}

clonecore::Result<ShrinkSourceAnalysis> analyze_candidates(
    const diskmodel::DiskInfo& disk,
    const clonecore::ISourceDiskReader& reader,
    const migrationcore::MigrationPartitionStyle style,
    const std::vector<PartitionCandidate>& candidates,
    const std::vector<AnalyzedShrinkPartition>& fixed_partitions,
    const ShrinkSourceAnalysisContext& context) {
  std::vector<diskmodel::VolumePartitionLocation> locations;
  locations.reserve(candidates.size());
  for (const auto& partition : candidates) {
    const auto boot = read_partition_boot(reader, partition);
    if (!boot) {
      return clonecore::Result<ShrinkSourceAnalysis>::failure(boot.error());
    }
    const auto ntfs = clonecore::parse_ntfs_geometry(
        boot.value(), reader.logical_sector_size(), partition.size_bytes);
    if (ntfs) {
      locations.push_back(diskmodel::VolumePartitionLocation{
          .table_index = partition.table_index,
          .offset_bytes = partition.offset_bytes,
      });
    }
  }
  auto bindings = locations.empty()
      ? clonecore::Result<std::vector<clonecore::VolumeBitmapBinding>>::success(
            {})
      : diskmodel::query_windows_volume_bindings_by_offset(disk, locations);
  if (!bindings) {
    return clonecore::Result<ShrinkSourceAnalysis>::failure(bindings.error());
  }

  std::optional<std::uint32_t> windows_table_index;
  std::optional<WindowsSourceVersion> observed_windows;
  if (context.source_identity.is_system_disk) {
    for (const auto& binding : bindings.value()) {
      const std::wstring hive = volume_child(
          binding.volume_device_path,
          L"Windows\\System32\\Config\\SOFTWARE");
      if (!is_regular_non_reparse(hive)) {
        continue;
      }
      if (windows_table_index.has_value()) {
        return failure<ShrinkSourceAnalysis>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_DUP_NAME,
            L"縮小移行Windows領域特定",
            L"Windows領域が複数見つかったためコピー先の起動領域を一意に確定できません");
      }
      auto inspected = inspect_windows_volume(binding.volume_device_path);
      if (!inspected) {
        return clonecore::Result<ShrinkSourceAnalysis>::failure(
            inspected.error());
      }
      windows_table_index = binding.partition_entry_index;
      observed_windows = inspected.take_value();
    }
    if (!windows_table_index.has_value() || !observed_windows.has_value()) {
      return failure<ShrinkSourceAnalysis>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_FOUND,
          L"縮小移行Windows領域特定",
          L"対応するWindows 10/11 x64領域を1つに特定できません");
    }
    if (context.known_windows_version.has_value() &&
        (context.known_windows_version->major != observed_windows->major ||
         context.known_windows_version->build != observed_windows->build ||
         context.known_windows_version->architecture !=
             observed_windows->architecture)) {
      return failure<ShrinkSourceAnalysis>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REVISION_MISMATCH,
          L"縮小移行Windows版照合",
          L"現在Windowsの版と読み取り元ボリュームの版が一致しません");
    }
  }

  const auto snapshot = imageformat::capture_partition_snapshot_v1(
      reader,
      style == migrationcore::MigrationPartitionStyle::gpt
          ? imageformat::PartitionTableStyle::gpt
          : imageformat::PartitionTableStyle::mbr);
  if (!snapshot) {
    return clonecore::Result<ShrinkSourceAnalysis>::failure(snapshot.error());
  }

  ShrinkSourceAnalysis analysis{
      .source = context.source_identity,
      .physical_sector_size = context.physical_sector_size,
      .partition_style = style,
      .windows_version = observed_windows,
      .bitlocker_fully_decrypted = true,
      .created_utc = context.created_utc,
      .app_version = context.app_version,
      .partition_snapshot = snapshot.value(),
      .partitions = fixed_partitions,
  };
  for (const auto& candidate : candidates) {
    const auto boot = read_partition_boot(reader, candidate);
    if (!boot) {
      return clonecore::Result<ShrinkSourceAnalysis>::failure(boot.error());
    }
    const auto ntfs = clonecore::parse_ntfs_geometry(
        boot.value(), reader.logical_sector_size(), candidate.size_bytes);
    if (!ntfs) {
      analysis.partitions.push_back(AnalyzedShrinkPartition{
          .source_table_index = candidate.table_index,
          .role = candidate.role,
          .file_system = migrationcore::MigrationFileSystem::unsupported,
          .source_offset_bytes = candidate.offset_bytes,
          .source_size_bytes = candidate.size_bytes,
          .used_bytes = candidate.size_bytes,
          .active = candidate.active,
          .name = candidate.name,
          .type_id = candidate.type_id,
          .unique_id = candidate.unique_id,
      });
      continue;
    }
    const auto binding = std::find_if(
        bindings.value().begin(),
        bindings.value().end(),
        [&](const auto& value) {
          return value.partition_entry_index == candidate.table_index;
        });
    if (binding == bindings.value().end()) {
      return failure<ShrinkSourceAnalysis>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"縮小移行ボリューム対応",
          L"パーティションに対応するVolume GUIDがありません");
    }
    const auto facts =
        query_volume_facts(binding->volume_device_path, candidate.size_bytes);
    if (!facts) {
      return clonecore::Result<ShrinkSourceAnalysis>::failure(facts.error());
    }
    if (facts.value().cluster_size != ntfs.value().cluster_size()) {
      return failure<ShrinkSourceAnalysis>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"縮小移行NTFSクラスター照合",
          L"物理ディスクとVolume GUIDのNTFSクラスター寸法が一致しません");
    }
    migrationcore::MigrationPartitionRole role = candidate.role;
    if (context.source_identity.is_system_disk &&
        windows_table_index == candidate.table_index) {
      role = migrationcore::MigrationPartitionRole::windows;
    } else if (!context.source_identity.is_system_disk) {
      role = migrationcore::MigrationPartitionRole::data;
    }
    analysis.partitions.push_back(
        AnalyzedShrinkPartition{
            .source_table_index = candidate.table_index,
            .role = role,
            .file_system = migrationcore::MigrationFileSystem::ntfs,
            .source_offset_bytes = candidate.offset_bytes,
            .source_size_bytes = candidate.size_bytes,
            .used_bytes = facts.value().used_bytes,
            .cluster_size = facts.value().cluster_size,
            .active = candidate.active,
            .label = facts.value().label,
            .name = candidate.name,
            .type_id = candidate.type_id,
            .unique_id = candidate.unique_id,
        });
    // Even an apparently empty NTFS volume is captured.  Used-byte counts are
    // advisory sizing input; they are not proof that metadata and boot files
    // can be recreated without a filesystem archive.
    analysis.content_volumes.push_back(AnalyzedShrinkVolume{
        .source_table_index = candidate.table_index,
        .volume_guid_path = binding->volume_device_path,
    });
  }
  std::sort(
      analysis.partitions.begin(),
      analysis.partitions.end(),
      [](const auto& left, const auto& right) {
        return left.source_table_index < right.source_table_index;
      });
  return clonecore::Result<ShrinkSourceAnalysis>::success(
      std::move(analysis));
}

}  // namespace

clonecore::Result<ShrinkSourceAnalysis>
analyze_gpt_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::GptDisk& layout,
    const ShrinkSourceAnalysisContext& context) {
  const auto common = validate_common(source_disk, read_only_source, context);
  if (!common) {
    return clonecore::Result<ShrinkSourceAnalysis>::failure(common.error());
  }
  if (layout.logical_sector_size != read_only_source.logical_sector_size() ||
      layout.sector_count != read_only_source.size_bytes() /
          read_only_source.logical_sector_size()) {
    return failure<ShrinkSourceAnalysis>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"縮小移行GPT寸法",
        L"GPT解析結果と読取り元の寸法が一致しません");
  }
  std::vector<PartitionCandidate> candidates;
  std::vector<AnalyzedShrinkPartition> fixed;
  for (const auto& partition : layout.partitions) {
    if (partition.entry_index ==
        (std::numeric_limits<std::uint32_t>::max)()) {
      return failure<ShrinkSourceAnalysis>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"縮小移行GPT番号",
          L"GPTパーティション番号を正規化できません");
    }
    const std::uint32_t table_index = partition.entry_index + 1U;
    if (partition.last_lba < partition.first_lba) {
      return failure<ShrinkSourceAnalysis>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"縮小移行GPT範囲",
          L"GPTパーティション終端が開始位置より前です");
    }
    const auto offset = checked_bytes(
        partition.first_lba, layout.logical_sector_size, L"縮小移行GPT開始");
    const auto size = checked_bytes(
        partition.last_lba - partition.first_lba + 1U,
        layout.logical_sector_size,
        L"縮小移行GPT長");
    if (!offset || !size) {
      return clonecore::Result<ShrinkSourceAnalysis>::failure(
          offset ? size.error() : offset.error());
    }
    if (partition.type_guid == clonecore::gpt_type_efi_system()) {
      if (!context.source_identity.is_system_disk) {
        return failure<ShrinkSourceAnalysis>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"データ専用GPTのEFI領域",
            L"データ専用ディスクにEFI領域があるため役割を確定できません");
      }
      fixed.push_back(AnalyzedShrinkPartition{
          .source_table_index = table_index,
          .role = migrationcore::MigrationPartitionRole::efi_system,
          // A GPT type GUID is not filesystem proof.  This static system
          // region is carried as exact RAW until a read-only FAT probe exists.
          .file_system = migrationcore::MigrationFileSystem::unsupported,
          .source_offset_bytes = offset.value(),
          .source_size_bytes = size.value(),
          .used_bytes = size.value(),
          .name = widen_gpt_name(partition.name),
          .type_id = guid_bytes(partition.type_guid),
          .unique_id = guid_bytes(partition.unique_guid),
      });
    } else if (
        partition.type_guid == clonecore::gpt_type_microsoft_reserved()) {
      fixed.push_back(AnalyzedShrinkPartition{
          .source_table_index = table_index,
          .role = migrationcore::MigrationPartitionRole::microsoft_reserved,
          .file_system = migrationcore::MigrationFileSystem::none,
          .source_offset_bytes = offset.value(),
          .source_size_bytes = size.value(),
          .used_bytes = size.value(),
          .name = widen_gpt_name(partition.name),
          .type_id = guid_bytes(partition.type_guid),
          .unique_id = guid_bytes(partition.unique_guid),
      });
    } else if (
        partition.type_guid == clonecore::gpt_type_basic_data() ||
        partition.type_guid == clonecore::gpt_type_windows_recovery()) {
      if (!context.source_identity.is_system_disk &&
          partition.type_guid == clonecore::gpt_type_windows_recovery()) {
        return failure<ShrinkSourceAnalysis>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"データ専用GPTの回復領域",
            L"データ専用ディスクに回復領域があるため役割を確定できません");
      }
      candidates.push_back(PartitionCandidate{
          .table_index = table_index,
          .role = partition.type_guid == clonecore::gpt_type_windows_recovery()
              ? migrationcore::MigrationPartitionRole::recovery
              : migrationcore::MigrationPartitionRole::data,
          .offset_bytes = offset.value(),
          .size_bytes = size.value(),
          .name = widen_gpt_name(partition.name),
          .type_id = guid_bytes(partition.type_guid),
          .unique_id = guid_bytes(partition.unique_guid),
      });
    } else {
      fixed.push_back(AnalyzedShrinkPartition{
          .source_table_index = table_index,
          .role = migrationcore::MigrationPartitionRole::data,
          .file_system = migrationcore::MigrationFileSystem::unsupported,
          .source_offset_bytes = offset.value(),
          .source_size_bytes = size.value(),
          .used_bytes = size.value(),
          .name = widen_gpt_name(partition.name),
          .type_id = guid_bytes(partition.type_guid),
          .unique_id = guid_bytes(partition.unique_guid),
      });
    }
  }
  return analyze_candidates(
      source_disk,
      read_only_source,
      migrationcore::MigrationPartitionStyle::gpt,
      candidates,
      fixed,
      context);
}

clonecore::Result<ShrinkSourceAnalysis>
analyze_mbr_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::MbrDisk& layout,
    const ShrinkSourceAnalysisContext& context) {
  const auto common = validate_common(source_disk, read_only_source, context);
  if (!common) {
    return clonecore::Result<ShrinkSourceAnalysis>::failure(common.error());
  }
  if (layout.logical_sector_size != read_only_source.logical_sector_size() ||
      layout.sector_count != read_only_source.size_bytes() /
          read_only_source.logical_sector_size()) {
    return failure<ShrinkSourceAnalysis>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"縮小移行MBR寸法",
        L"MBR解析結果と読取り元の寸法が一致しません");
  }
  std::vector<PartitionCandidate> candidates;
  for (const auto& partition : layout.partitions) {
    if (partition.type == 0U || partition.type == 0x05U ||
        partition.type == 0x0FU || partition.type == 0x85U) {
      return failure<ShrinkSourceAnalysis>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"縮小移行MBR種別",
          L"空種別または拡張partitionを含むMBR layoutは安全に単一基本領域として扱えません");
    }
    const auto offset = checked_bytes(
        partition.first_lba, layout.logical_sector_size, L"縮小移行MBR開始");
    const auto size = checked_bytes(
        partition.sector_count, layout.logical_sector_size, L"縮小移行MBR長");
    if (!offset || !size) {
      return clonecore::Result<ShrinkSourceAnalysis>::failure(
          offset ? size.error() : offset.error());
    }
    candidates.push_back(PartitionCandidate{
        .table_index = static_cast<std::uint32_t>(partition.table_index) + 1U,
        .role = partition.type == 0x27U
            ? migrationcore::MigrationPartitionRole::recovery
            : partition.type == 0x07U && partition.active &&
                    context.source_identity.is_system_disk
                  ? migrationcore::MigrationPartitionRole::bios_system
                  : migrationcore::MigrationPartitionRole::data,
        .offset_bytes = offset.value(),
        .size_bytes = size.value(),
        .active = partition.active,
        .type_id = [] (const std::uint8_t type) {
          std::array<std::byte, 16U> value{};
          value[0] = static_cast<std::byte>(type);
          return value;
        }(partition.type),
    });
  }
  auto analysis = analyze_candidates(
      source_disk,
      read_only_source,
      migrationcore::MigrationPartitionStyle::mbr,
      candidates,
      {},
      context);
  if (analysis) {
    analysis.value().mbr_bootstrap = layout.bootstrap;
  }
  return analysis;
}

}  // namespace ytec::windowsshrink

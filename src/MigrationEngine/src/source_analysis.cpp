#include "ytec/migrationengine/source_analysis.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace ytec::migrationengine {
namespace {

std::string payload_name(const std::uint32_t table_index) {
  std::array<char, 32U> buffer{};
  (void)std::snprintf(
      buffer.data(), buffer.size(), "volume-%03u.wim", table_index);
  return buffer.data();
}

clonecore::Result<ShrinkSourceAnalysis> to_legacy_analysis(
    windowsshrink::ShrinkSourceAnalysis current) {
  imageformat::ShrinkImageManifest manifest{
      .source = current.source,
      .physical_sector_size = current.physical_sector_size,
      .partition_style = current.partition_style,
      .bitlocker_fully_decrypted = current.bitlocker_fully_decrypted,
      .created_utc = std::move(current.created_utc),
      .app_version = std::move(current.app_version),
      .mbr_bootstrap = current.mbr_bootstrap,
  };
  if (current.windows_version.has_value()) {
    manifest.windows_major = current.windows_version->major;
    manifest.windows_minor = current.windows_version->minor;
    manifest.windows_build = current.windows_version->build;
    manifest.windows_architecture = current.windows_version->architecture;
  }

  ShrinkSourceAnalysis legacy{.manifest = std::move(manifest)};
  legacy.manifest.partitions.reserve(current.partitions.size());
  legacy.content_volumes.reserve(current.content_volumes.size());
  for (const auto& partition : current.partitions) {
    if (partition.source_table_index == 0U) {
      return clonecore::Result<ShrinkSourceAnalysis>::failure(
          clonecore::Error{
              .code = clonecore::ErrorCode::invalid_data,
              .operation = L"旧縮小形式互換変換",
              .message = L"正規化済みパーティション番号が不正です",
          });
    }
    const std::uint32_t legacy_index = partition.source_table_index - 1U;
    const auto legacy_file_system =
        partition.role == migrationcore::MigrationPartitionRole::efi_system
        ? migrationcore::MigrationFileSystem::fat32
        : partition.file_system;
    const bool has_payload =
        legacy_file_system == migrationcore::MigrationFileSystem::ntfs &&
        partition.used_bytes != 0U;
    legacy.manifest.partitions.push_back(imageformat::ShrinkImagePartition{
        .source_table_index = legacy_index,
        .role = partition.role,
        .file_system = legacy_file_system,
        .source_size_bytes = partition.source_size_bytes,
        .used_bytes = has_payload ? partition.used_bytes : 0U,
        .cluster_size = legacy_file_system ==
                migrationcore::MigrationFileSystem::fat32
            ? 4096U
            : partition.cluster_size,
        .active = partition.active,
        .label = partition.label,
        .payload_file_name = has_payload ? payload_name(legacy_index)
                                         : std::string{},
    });
  }
  for (const auto& volume : current.content_volumes) {
    const auto partition = std::find_if(
        current.partitions.begin(),
        current.partitions.end(),
        [&](const auto& candidate) {
          return candidate.source_table_index == volume.source_table_index;
        });
    if (partition == current.partitions.end() || partition->used_bytes == 0U ||
        volume.source_table_index == 0U) {
      continue;
    }
    const std::uint32_t legacy_index = volume.source_table_index - 1U;
    legacy.content_volumes.push_back(AnalyzedShrinkVolume{
        .source_table_index = legacy_index,
        .volume_guid_path = volume.volume_guid_path,
        .payload_file_name = payload_name(legacy_index),
    });
  }
  return clonecore::Result<ShrinkSourceAnalysis>::success(std::move(legacy));
}

}  // namespace

clonecore::Result<ShrinkSourceAnalysis>
analyze_gpt_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::GptDisk& layout,
    const ShrinkSourceAnalysisContext& context) {
  auto current = windowsshrink::analyze_gpt_shrink_source_with_windows_apis(
      source_disk, read_only_source, layout, context);
  if (!current) {
    return clonecore::Result<ShrinkSourceAnalysis>::failure(current.error());
  }
  return to_legacy_analysis(current.take_value());
}

clonecore::Result<ShrinkSourceAnalysis>
analyze_mbr_shrink_source_with_windows_apis(
    const diskmodel::DiskInfo& source_disk,
    const clonecore::ISourceDiskReader& read_only_source,
    const clonecore::MbrDisk& layout,
    const ShrinkSourceAnalysisContext& context) {
  auto current = windowsshrink::analyze_mbr_shrink_source_with_windows_apis(
      source_disk, read_only_source, layout, context);
  if (!current) {
    return clonecore::Result<ShrinkSourceAnalysis>::failure(current.error());
  }
  return to_legacy_analysis(current.take_value());
}

}  // namespace ytec::migrationengine

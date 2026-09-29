#include "ytec/migrationengine/target_layout.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ytec::migrationengine {
namespace {

clonecore::Error layout_error(
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
  return clonecore::Result<T>::failure(layout_error(
      code, native_code, std::move(operation), std::move(message)));
}

migrationcore::ShrinkMigrationRequest migration_request(
    const imageformat::ShrinkImageManifest& manifest,
    const std::uint64_t target_size,
    const std::uint32_t target_sector) {
  migrationcore::ShrinkMigrationRequest request{
      .source_style = manifest.partition_style,
      .target_style = manifest.partition_style,
      .target_size_bytes = target_size,
      .target_logical_sector_size = target_sector,
      .source_is_windows_system = manifest.source.is_system_disk,
      .windows_is_amd64 = manifest.windows_architecture == "AMD64",
      .bitlocker_fully_decrypted = manifest.bitlocker_fully_decrypted,
  };
  request.source_partitions.reserve(manifest.partitions.size());
  for (const auto& partition : manifest.partitions) {
    request.source_partitions.push_back(
        migrationcore::ShrinkSourcePartition{
            .source_table_index = partition.source_table_index,
            .role = partition.role,
            .file_system = partition.file_system,
            .source_size_bytes = partition.source_size_bytes,
            .used_bytes = partition.used_bytes,
            .cluster_size = partition.cluster_size,
            .label = partition.label,
            .active = partition.active,
        });
  }
  return request;
}

std::u16string to_utf16_name(const std::wstring& value) {
  std::u16string result;
  result.reserve(value.size());
  for (const wchar_t character : value) {
    result.push_back(static_cast<char16_t>(character));
  }
  return result;
}

const clonecore::GptGuid& gpt_type_for(
    const migrationcore::MigrationPartitionRole role) {
  switch (role) {
    case migrationcore::MigrationPartitionRole::efi_system:
      return clonecore::gpt_type_efi_system();
    case migrationcore::MigrationPartitionRole::microsoft_reserved:
      return clonecore::gpt_type_microsoft_reserved();
    case migrationcore::MigrationPartitionRole::recovery:
      return clonecore::gpt_type_windows_recovery();
    case migrationcore::MigrationPartitionRole::windows:
    case migrationcore::MigrationPartitionRole::data:
    case migrationcore::MigrationPartitionRole::bios_system:
      return clonecore::gpt_type_basic_data();
  }
  return clonecore::gpt_type_basic_data();
}

clonecore::Result<clonecore::GptWritePlan> build_gpt(
    const migrationcore::ShrinkMigrationPlan& migration,
    const std::uint32_t sector_size,
    clonecore::IGuidGenerator& generator) {
  constexpr std::uint32_t entry_count = 128U;
  constexpr std::uint32_t entry_size = 128U;
  const std::uint64_t sector_count = migration.target_size_bytes / sector_size;
  const std::uint64_t entry_bytes =
      static_cast<std::uint64_t>(entry_count) * entry_size;
  const std::uint64_t entry_sectors =
      (entry_bytes + sector_size - 1U) / sector_size;
  if (sector_count <= 3U + 2U * entry_sectors) {
    return failure<clonecore::GptWritePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"縮小移行GPTメタデータ",
        L"GPTメタデータを配置できる容量がありません");
  }
  clonecore::GptDisk synthetic{
      .logical_sector_size = sector_size,
      .sector_count = sector_count,
      .first_usable_lba = 2U + entry_sectors,
      .last_usable_lba = sector_count - 2U - entry_sectors,
      .partition_entry_count = entry_count,
      .partition_entry_size = entry_size,
  };
  synthetic.partitions.reserve(migration.target_partitions.size());
  for (const auto& partition : migration.target_partitions) {
    if (partition.offset_bytes % sector_size != 0U ||
        partition.size_bytes % sector_size != 0U ||
        partition.target_number == 0U) {
      return failure<clonecore::GptWritePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"縮小移行GPTパーティション",
          L"パーティション番号、開始位置、または長さが非整列です");
    }
    synthetic.partitions.push_back(clonecore::GptPartition{
        .entry_index = partition.target_number - 1U,
        .type_guid = gpt_type_for(partition.role),
        .first_lba = partition.offset_bytes / sector_size,
        .last_lba =
            (partition.offset_bytes + partition.size_bytes) / sector_size - 1U,
        .attributes =
            partition.role == migrationcore::MigrationPartitionRole::recovery
                ? 0x8000000000000001ULL
                : 0U,
        .name = to_utf16_name(partition.label),
    });
  }
  return clonecore::make_gpt_write_plan(
      synthetic, migration.target_size_bytes, sector_size, generator);
}

clonecore::Result<clonecore::MbrWritePlan> build_mbr(
    const imageformat::ShrinkImageManifest& manifest,
    const migrationcore::ShrinkMigrationPlan& migration,
    const std::uint32_t sector_size,
    clonecore::IMbrSignatureGenerator& generator,
    const std::span<const std::uint32_t> disallowed) {
  if (sector_size != 512U || migration.target_partitions.size() > 4U ||
      migration.target_size_bytes / sector_size >
          (std::numeric_limits<std::uint32_t>::max)()) {
    return failure<clonecore::MbrWritePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小移行MBR寸法",
        L"MBR縮小移行は512バイト論理セクター、4パーティション以下に対応します");
  }
  clonecore::MbrDisk synthetic{
      .logical_sector_size = sector_size,
      .sector_count = migration.target_size_bytes / sector_size,
      .bootstrap = manifest.mbr_bootstrap,
  };
  synthetic.partitions.reserve(migration.target_partitions.size());
  constexpr std::array<std::byte, 3> chs{
      std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}};
  for (const auto& partition : migration.target_partitions) {
    const std::uint64_t first_lba = partition.offset_bytes / sector_size;
    const std::uint64_t count = partition.size_bytes / sector_size;
    if (partition.target_number == 0U ||
        partition.offset_bytes % sector_size != 0U ||
        partition.size_bytes % sector_size != 0U ||
        first_lba > (std::numeric_limits<std::uint32_t>::max)() ||
        count == 0U || count > (std::numeric_limits<std::uint32_t>::max)()) {
      return failure<clonecore::MbrWritePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"縮小移行MBRパーティション",
          L"MBRパーティション番号またはLBA範囲が不正です");
    }
    synthetic.partitions.push_back(clonecore::MbrPartition{
        .table_index =
            static_cast<std::uint8_t>(partition.target_number - 1U),
        .active = partition.active,
        .first_chs = chs,
        .type = partition.role ==
                migrationcore::MigrationPartitionRole::recovery
            ? static_cast<std::uint8_t>(0x27U)
            : static_cast<std::uint8_t>(0x07U),
        .last_chs = chs,
        .first_lba = static_cast<std::uint32_t>(first_lba),
        .sector_count = static_cast<std::uint32_t>(count),
    });
  }
  return clonecore::make_mbr_write_plan(
      synthetic,
      migration.target_size_bytes,
      sector_size,
      generator,
      disallowed,
      manifest.source.is_system_disk);
}

}  // namespace

clonecore::Result<ShrinkTargetLayout> build_shrink_target_layout(
    const imageformat::ShrinkImageManifest& source_manifest,
    const std::uint64_t target_size_bytes,
    const std::uint32_t target_logical_sector_size,
    clonecore::IGuidGenerator& guid_generator,
    clonecore::IMbrSignatureGenerator& signature_generator,
    const std::span<const std::uint32_t> disallowed_mbr_signatures) {
  const auto migration = migrationcore::plan_shrink_migration(
      migration_request(
          source_manifest, target_size_bytes, target_logical_sector_size));
  if (!migration) {
    return clonecore::Result<ShrinkTargetLayout>::failure(migration.error());
  }
  ShrinkTargetLayout result{
      .migration = migration.value(),
      .is_gpt = source_manifest.partition_style ==
          migrationcore::MigrationPartitionStyle::gpt,
  };
  if (result.is_gpt) {
    auto gpt = build_gpt(
        result.migration, target_logical_sector_size, guid_generator);
    if (!gpt) {
      return clonecore::Result<ShrinkTargetLayout>::failure(gpt.error());
    }
    result.gpt = gpt.take_value();
  } else {
    auto mbr = build_mbr(
        source_manifest,
        result.migration,
        target_logical_sector_size,
        signature_generator,
        disallowed_mbr_signatures);
    if (!mbr) {
      return clonecore::Result<ShrinkTargetLayout>::failure(mbr.error());
    }
    result.mbr = mbr.take_value();
  }
  return clonecore::Result<ShrinkTargetLayout>::success(std::move(result));
}

}  // namespace ytec::migrationengine

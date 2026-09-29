#include "ytec/imageformat/tsumugi_restore_layout.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::imageformat {
namespace {

constexpr std::uint64_t kMaximumInvalidationBytes = 1024U * 1024U;

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
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool flag_set(
    const TsumugiManifestPartitionFlags value,
    const TsumugiManifestPartitionFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

bool flag_set(
    const TsumugiManifestFlags value,
    const TsumugiManifestFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
}

class SnapshotReader final : public clonecore::ISourceDiskReader {
 public:
  explicit SnapshotReader(const PartitionSnapshot& snapshot) noexcept
      : snapshot_(&snapshot) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return snapshot_->source_disk_size;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return snapshot_->logical_sector_size;
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (length == 0U || offset > snapshot_->source_disk_size ||
        length > snapshot_->source_disk_size - offset) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_READ_FAULT,
          L"Tsumugiパーティション表snapshot読取り",
          L"要求範囲が元ディスク境界外です");
    }
    for (const auto& region : snapshot_->regions) {
      if (offset < region.disk_offset) {
        continue;
      }
      const std::uint64_t relative = offset - region.disk_offset;
      if (relative <= region.data.size() &&
          length <= region.data.size() - relative) {
        const auto first = region.data.begin() +
            static_cast<std::ptrdiff_t>(relative);
        return clonecore::Result<std::vector<std::byte>>::success(
            std::vector<std::byte>(
                first, first + static_cast<std::ptrdiff_t>(length)));
      }
    }
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_READ_FAULT,
        L"Tsumugiパーティション表snapshot読取り",
        L"保存対象外の領域をパーティション表として参照しようとしました");
  }

 private:
  const PartitionSnapshot* snapshot_{};
};

clonecore::Status validate_gpt_manifest_binding(
    const TsumugiManifest& manifest,
    const clonecore::GptDisk& source) {
  if (source.partitions.size() != manifest.partitions.size()) {
    return clonecore::Status::failure(layout_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi GPTマニフェスト照合",
        L"snapshotとマニフェストのパーティション数が一致しません"));
  }
  for (const auto& record : manifest.partitions) {
    const auto found = std::find_if(
        source.partitions.begin(), source.partitions.end(),
        [&](const clonecore::GptPartition& partition) {
          return static_cast<std::uint64_t>(partition.entry_index) + 1U ==
              record.source_table_index;
        });
    std::uint64_t offset{};
    std::uint64_t length{};
    if (found == source.partitions.end() ||
        found->last_lba < found->first_lba ||
        !checked_multiply(
            found->first_lba, source.logical_sector_size, offset) ||
        found->last_lba == std::numeric_limits<std::uint64_t>::max() ||
        !checked_multiply(
            found->last_lba - found->first_lba + 1U,
            source.logical_sector_size,
            length) ||
        offset != record.source_offset || length != record.source_size ||
        found->type_guid.bytes != record.type_id ||
        found->unique_guid.bytes != record.unique_id ||
        flag_set(record.flags, TsumugiManifestPartitionFlags::active)) {
      return clonecore::Status::failure(layout_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi GPTマニフェスト照合",
          L"snapshotとマニフェストの番号、範囲、GUID、または属性が一致しません"));
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_mbr_manifest_binding(
    const TsumugiManifest& manifest,
    const clonecore::MbrDisk& source) {
  if (source.partitions.size() != manifest.partitions.size()) {
    return clonecore::Status::failure(layout_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi MBRマニフェスト照合",
        L"snapshotとマニフェストのパーティション数が一致しません"));
  }
  for (const auto& record : manifest.partitions) {
    const auto found = std::find_if(
        source.partitions.begin(), source.partitions.end(),
        [&](const clonecore::MbrPartition& partition) {
          return static_cast<std::uint32_t>(partition.table_index) + 1U ==
              record.source_table_index;
        });
    std::uint64_t offset{};
    std::uint64_t length{};
    const bool active = flag_set(
        record.flags, TsumugiManifestPartitionFlags::active);
    if (found == source.partitions.end() ||
        !checked_multiply(
            found->first_lba, source.logical_sector_size, offset) ||
        !checked_multiply(
            found->sector_count, source.logical_sector_size, length) ||
        offset != record.source_offset || length != record.source_size ||
        record.type_id[0] != static_cast<std::byte>(found->type) ||
        active != found->active) {
      return clonecore::Status::failure(layout_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi MBRマニフェスト照合",
          L"snapshotとマニフェストの番号、範囲、種類、またはActive属性が一致しません"));
    }
  }
  return clonecore::success_status();
}

std::vector<clonecore::ByteRange> invalidation_ranges(
    const std::uint64_t target_size_bytes,
    const std::uint32_t sector_size) {
  const std::uint64_t half_aligned =
      (target_size_bytes / 2U / sector_size) * sector_size;
  const std::uint64_t length =
      std::min(kMaximumInvalidationBytes, half_aligned);
  if (length == 0U) {
    return {};
  }
  return {
      clonecore::ByteRange{.offset = 0U, .length = length},
      clonecore::ByteRange{
          .offset = target_size_bytes - length, .length = length},
  };
}

TsumugiRestoreLayoutWrite convert_gpt_write(
    clonecore::GptMetadataWrite write) {
  TsumugiRestoreLayoutWriteKind kind{};
  switch (write.kind) {
    case clonecore::GptMetadataKind::primary_entries:
      kind = TsumugiRestoreLayoutWriteKind::gpt_primary_entries;
      break;
    case clonecore::GptMetadataKind::backup_entries:
      kind = TsumugiRestoreLayoutWriteKind::gpt_backup_entries;
      break;
    case clonecore::GptMetadataKind::backup_header:
      kind = TsumugiRestoreLayoutWriteKind::gpt_backup_header;
      break;
    case clonecore::GptMetadataKind::protective_mbr:
      kind = TsumugiRestoreLayoutWriteKind::gpt_protective_mbr;
      break;
    case clonecore::GptMetadataKind::primary_header_commit:
      kind = TsumugiRestoreLayoutWriteKind::gpt_primary_header;
      break;
  }
  return TsumugiRestoreLayoutWrite{
      .kind = kind,
      .offset = write.offset,
      .bytes = std::move(write.bytes),
  };
}

clonecore::Result<std::wstring> from_utf8(const std::string_view value) {
  if (value.empty()) {
    return clonecore::Result<std::wstring>::success({});
  }
  if (value.size() > static_cast<std::size_t>(
                         (std::numeric_limits<int>::max)())) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"Tsumugi縮小レイアウトUTF-8",
        L"ラベルが変換可能な長さを超えています");
  }
  const int required = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (required <= 0) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"Tsumugi縮小レイアウトUTF-8",
        L"認証済みラベルをUTF-16へ変換できません");
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          required) != required) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"Tsumugi縮小レイアウトUTF-8",
        L"認証済みラベルを完全に変換できません");
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

std::u16string to_utf16_name(const std::wstring_view value) {
  std::u16string result;
  result.reserve(value.size());
  for (const wchar_t character : value) {
    result.push_back(static_cast<char16_t>(character));
  }
  return result;
}

clonecore::Result<migrationcore::MigrationPartitionStyle>
to_migration_style(const TsumugiManifestPartitionStyle style) {
  switch (style) {
    case TsumugiManifestPartitionStyle::mbr:
      return clonecore::Result<
          migrationcore::MigrationPartitionStyle>::success(
          migrationcore::MigrationPartitionStyle::mbr);
    case TsumugiManifestPartitionStyle::gpt:
      return clonecore::Result<
          migrationcore::MigrationPartitionStyle>::success(
          migrationcore::MigrationPartitionStyle::gpt);
  }
  return failure<migrationcore::MigrationPartitionStyle>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"Tsumugi縮小パーティション形式",
      L"MBRまたはGPT以外は縮小復元できません");
}

clonecore::Result<migrationcore::MigrationPartitionRole>
to_migration_role(
    const TsumugiManifestPartition& partition,
    const TsumugiManifestPartitionStyle source_style,
    const TsumugiManifestPartitionStyle target_style) {
  using Source = TsumugiManifestPartitionRole;
  using Target = migrationcore::MigrationPartitionRole;
  switch (partition.role) {
    case Source::efi_system:
      return clonecore::Result<Target>::success(Target::efi_system);
    case Source::microsoft_reserved:
      return clonecore::Result<Target>::success(Target::microsoft_reserved);
    case Source::bios_system:
      return clonecore::Result<Target>::success(Target::bios_system);
    case Source::windows:
      return clonecore::Result<Target>::success(Target::windows);
    case Source::recovery:
      return clonecore::Result<Target>::success(Target::recovery);
    case Source::data:
      return clonecore::Result<Target>::success(Target::data);
    case Source::other:
      if (partition.payload_encoding ==
              TsumugiManifestPayloadEncoding::exact_raw &&
          source_style == target_style) {
        return clonecore::Result<Target>::success(Target::data);
      }
      break;
  }
  return failure<Target>(
      clonecore::ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"Tsumugi縮小パーティション役割",
      L"その他の役割は同一形式の元サイズexact RAWとしてだけ保持できます");
}

migrationcore::MigrationFileSystem to_migration_file_system(
    const TsumugiManifestPartition& partition) noexcept {
  if (partition.payload_encoding ==
          TsumugiManifestPayloadEncoding::exact_raw &&
      partition.role != TsumugiManifestPartitionRole::efi_system &&
      partition.role != TsumugiManifestPartitionRole::microsoft_reserved) {
    return migrationcore::MigrationFileSystem::unsupported;
  }
  switch (partition.file_system) {
    case TsumugiManifestFileSystem::none:
      return migrationcore::MigrationFileSystem::none;
    case TsumugiManifestFileSystem::ntfs:
      return migrationcore::MigrationFileSystem::ntfs;
    case TsumugiManifestFileSystem::exfat:
      return migrationcore::MigrationFileSystem::exfat;
    case TsumugiManifestFileSystem::fat32:
      return migrationcore::MigrationFileSystem::fat32;
    case TsumugiManifestFileSystem::unknown:
      return migrationcore::MigrationFileSystem::unsupported;
  }
  return migrationcore::MigrationFileSystem::unsupported;
}

const clonecore::GptGuid& gpt_type_for(
    const migrationcore::MigrationPartitionRole role) noexcept {
  switch (role) {
    case migrationcore::MigrationPartitionRole::efi_system:
      return clonecore::gpt_type_efi_system();
    case migrationcore::MigrationPartitionRole::microsoft_reserved:
      return clonecore::gpt_type_microsoft_reserved();
    case migrationcore::MigrationPartitionRole::recovery:
      return clonecore::gpt_type_windows_recovery();
    case migrationcore::MigrationPartitionRole::bios_system:
    case migrationcore::MigrationPartitionRole::windows:
    case migrationcore::MigrationPartitionRole::data:
      return clonecore::gpt_type_basic_data();
  }
  return clonecore::gpt_type_basic_data();
}

clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>
make_gpt_metadata_plan(
    clonecore::GptWritePlan target,
    const std::uint64_t target_size_bytes,
    const std::uint32_t target_sector_size) {
  TsumugiWholeDiskRestoreLayoutPlan result{
      .style = PartitionTableStyle::gpt,
      .target_size_bytes = target_size_bytes,
      .logical_sector_size = target_sector_size,
      .invalidation_ranges =
          invalidation_ranges(target_size_bytes, target_sector_size),
      .target_layout = target.target_disk,
  };
  if (result.invalidation_ranges.size() != 2U ||
      result.invalidation_ranges.front().offset +
              result.invalidation_ranges.front().length >
          result.invalidation_ranges.back().offset) {
    return failure<TsumugiWholeDiskRestoreLayoutPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"Tsumugi縮小GPT無効化範囲",
        L"コピー先の両端を安全に無効化できません");
  }
  for (auto& write : target.writes) {
    auto converted = convert_gpt_write(std::move(write));
    if (converted.kind ==
            TsumugiRestoreLayoutWriteKind::gpt_primary_entries ||
        converted.kind ==
            TsumugiRestoreLayoutWriteKind::gpt_backup_entries) {
      result.staged_writes.push_back(std::move(converted));
    } else {
      result.commit_writes.push_back(std::move(converted));
    }
  }
  const std::array<TsumugiRestoreLayoutWriteKind, 3U> expected_commit{
      TsumugiRestoreLayoutWriteKind::gpt_backup_header,
      TsumugiRestoreLayoutWriteKind::gpt_protective_mbr,
      TsumugiRestoreLayoutWriteKind::gpt_primary_header,
  };
  std::sort(
      result.commit_writes.begin(), result.commit_writes.end(),
      [&](const auto& left, const auto& right) {
        const auto position = [&](const auto kind) {
          return std::find(
              expected_commit.begin(), expected_commit.end(), kind);
        };
        return position(left.kind) < position(right.kind);
      });
  if (result.staged_writes.size() != 2U ||
      result.commit_writes.size() != expected_commit.size() ||
      !std::equal(
          expected_commit.begin(), expected_commit.end(),
          result.commit_writes.begin(),
          [](const auto expected, const auto& actual) {
            return expected == actual.kind;
          })) {
    return failure<TsumugiWholeDiskRestoreLayoutPlan>(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小GPT書込み順序",
        L"仮配置と最終確定の順序を構成できません");
  }
  return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::success(
      std::move(result));
}

clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>
make_mbr_metadata_plan(
    clonecore::MbrWritePlan target,
    const std::uint64_t target_size_bytes,
    const std::uint32_t target_sector_size) {
  TsumugiWholeDiskRestoreLayoutPlan result{
      .style = PartitionTableStyle::mbr,
      .target_size_bytes = target_size_bytes,
      .logical_sector_size = target_sector_size,
      .invalidation_ranges =
          invalidation_ranges(target_size_bytes, target_sector_size),
      .target_layout = target.target_disk,
  };
  if (result.invalidation_ranges.size() != 2U ||
      result.invalidation_ranges.front().offset +
              result.invalidation_ranges.front().length >
          result.invalidation_ranges.back().offset) {
    return failure<TsumugiWholeDiskRestoreLayoutPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"Tsumugi縮小MBR無効化範囲",
        L"コピー先の両端を安全に無効化できません");
  }
  result.commit_writes.push_back(TsumugiRestoreLayoutWrite{
      .kind = TsumugiRestoreLayoutWriteKind::mbr_sector,
      .offset = 0U,
      .bytes = std::move(target.sector),
  });
  return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::success(
      std::move(result));
}

clonecore::Result<std::vector<clonecore::ByteRange>>
exact_metadata_ranges(
    const TsumugiWholeDiskRestoreLayoutPlan& metadata,
    const std::uint64_t payload_offset,
    const std::uint64_t payload_size) {
  std::vector<clonecore::ByteRange> ranges;
  ranges.reserve(
      metadata.staged_writes.size() + metadata.commit_writes.size());
  const auto append = [&](const TsumugiRestoreLayoutWrite& write) {
    ranges.push_back(clonecore::ByteRange{
        .offset = write.offset,
        .length = static_cast<std::uint64_t>(write.bytes.size()),
    });
  };
  for (const auto& write : metadata.staged_writes) {
    append(write);
  }
  for (const auto& write : metadata.commit_writes) {
    append(write);
  }
  std::sort(
      ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.offset < right.offset;
      });
  std::vector<clonecore::ByteRange> merged;
  for (const auto& range : ranges) {
    std::uint64_t end{};
    if (range.length == 0U ||
        range.offset % metadata.logical_sector_size != 0U ||
        range.length % metadata.logical_sector_size != 0U ||
        !checked_add(range.offset, range.length, end) ||
        end > metadata.target_size_bytes) {
      return failure<std::vector<clonecore::ByteRange>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小仮GPT退役範囲",
          L"仮GPTメタデータが空、未整列、または対象範囲外です");
    }
    if (merged.empty()) {
      merged.push_back(range);
      continue;
    }
    auto& previous = merged.back();
    const std::uint64_t previous_end = previous.offset + previous.length;
    if (range.offset > previous_end) {
      merged.push_back(range);
      continue;
    }
    previous.length = (std::max)(previous_end, end) - previous.offset;
  }
  std::uint64_t payload_end{};
  if (merged.size() != 2U || merged.front().offset != 0U ||
      !checked_add(payload_offset, payload_size, payload_end) ||
      merged.front().offset + merged.front().length > payload_offset ||
      payload_end > merged.back().offset ||
      merged.back().offset + merged.back().length !=
          metadata.target_size_bytes) {
    return failure<std::vector<clonecore::ByteRange>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小仮GPT退役境界",
        L"仮GPTの先頭・末尾メタデータをpayloadと分離できません");
  }
  return clonecore::Result<std::vector<clonecore::ByteRange>>::success(
      std::move(merged));
}

}  // namespace

clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>
make_tsumugi_whole_disk_restore_layout_plan_v1(
    const TsumugiManifest& manifest,
    const std::uint64_t target_size_bytes,
    const std::uint32_t target_sector_size,
    clonecore::IGuidGenerator& guid_generator,
    clonecore::IMbrSignatureGenerator& signature_generator,
    const std::span<const std::uint32_t> disallowed_mbr_signatures) {
  const auto canonical = build_tsumugi_manifest_v1(manifest);
  if (!canonical) {
    return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::failure(
        canonical.error());
  }
  if (manifest.mode == TsumugiManifestMode::shrink) {
    return failure<TsumugiWholeDiskRestoreLayoutPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugiディスク全体レイアウト",
        L"縮小画像は確定済み配置を使う縮小レイアウトエンジンが必要です");
  }
  if (target_sector_size != manifest.logical_sector_size ||
      target_size_bytes < manifest.source_disk_size ||
      target_size_bytes % target_sector_size != 0U) {
    return failure<TsumugiWholeDiskRestoreLayoutPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"Tsumugiディスク全体レイアウト寸法",
        L"通常・救出復元は元以上の容量と同じ論理セクターが必要です");
  }

  const auto snapshot =
      inspect_partition_snapshot_v1(manifest.partition_snapshot);
  if (!snapshot) {
    return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::failure(
        snapshot.error());
  }
  SnapshotReader reader(snapshot.value());
  TsumugiWholeDiskRestoreLayoutPlan result{
      .style = snapshot.value().style,
      .target_size_bytes = target_size_bytes,
      .logical_sector_size = target_sector_size,
      .invalidation_ranges =
          invalidation_ranges(target_size_bytes, target_sector_size),
  };
  if (result.invalidation_ranges.size() != 2U ||
      result.invalidation_ranges.front().offset +
              result.invalidation_ranges.front().length >
          result.invalidation_ranges.back().offset) {
    return failure<TsumugiWholeDiskRestoreLayoutPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"Tsumugiコピー先無効化範囲",
        L"先頭と末尾のパーティション表を安全に無効化できません");
  }

  if (snapshot.value().style == PartitionTableStyle::gpt) {
    auto parsed = clonecore::parse_gpt(reader);
    if (!parsed) {
      return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::failure(
          parsed.error());
    }
    const auto binding = validate_gpt_manifest_binding(manifest, parsed.value());
    if (!binding) {
      return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::failure(
          binding.error());
    }
    std::erase_if(
        parsed.value().partitions,
        [&](const clonecore::GptPartition& partition) {
          const auto record = std::find_if(
              manifest.partitions.begin(), manifest.partitions.end(),
              [&](const TsumugiManifestPartition& item) {
                return item.source_table_index ==
                    static_cast<std::uint64_t>(partition.entry_index) + 1U;
              });
          return record == manifest.partitions.end() ||
              !flag_set(
                  record->flags, TsumugiManifestPartitionFlags::selected);
        });
    auto target = clonecore::make_gpt_write_plan(
        parsed.value(), target_size_bytes, target_sector_size, guid_generator);
    if (!target) {
      return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::failure(
          target.error());
    }
    result.target_layout = target.value().target_disk;
    for (auto& write : target.value().writes) {
      auto converted = convert_gpt_write(std::move(write));
      if (converted.kind ==
              TsumugiRestoreLayoutWriteKind::gpt_primary_entries ||
          converted.kind ==
              TsumugiRestoreLayoutWriteKind::gpt_backup_entries) {
        result.staged_writes.push_back(std::move(converted));
      } else {
        result.commit_writes.push_back(std::move(converted));
      }
    }
    const std::array<TsumugiRestoreLayoutWriteKind, 3U> expected_commit{
        TsumugiRestoreLayoutWriteKind::gpt_backup_header,
        TsumugiRestoreLayoutWriteKind::gpt_protective_mbr,
        TsumugiRestoreLayoutWriteKind::gpt_primary_header,
    };
    std::sort(
        result.commit_writes.begin(), result.commit_writes.end(),
        [&](const auto& left, const auto& right) {
          const auto position = [&](const auto kind) {
            return std::find(
                expected_commit.begin(), expected_commit.end(), kind);
          };
          return position(left.kind) < position(right.kind);
        });
    if (result.staged_writes.size() != 2U ||
        result.commit_writes.size() != expected_commit.size() ||
        !std::equal(
            expected_commit.begin(), expected_commit.end(),
            result.commit_writes.begin(),
            [](const auto expected, const auto& actual) {
              return expected == actual.kind;
            })) {
      return failure<TsumugiWholeDiskRestoreLayoutPlan>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"Tsumugi GPT書込み順序",
          L"完全な仮配置または最終確定計画を構成できません");
    }
    return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::success(
        std::move(result));
  }

  if (snapshot.value().style != PartitionTableStyle::mbr) {
    return failure<TsumugiWholeDiskRestoreLayoutPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugiパーティション表形式",
        L"MBRまたはGPT以外は復元できません");
  }
  auto parsed = clonecore::parse_mbr(reader);
  if (!parsed) {
    return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::failure(
        parsed.error());
  }
  const auto binding = validate_mbr_manifest_binding(manifest, parsed.value());
  if (!binding) {
    return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::failure(
        binding.error());
  }
  std::erase_if(
      parsed.value().partitions,
      [&](const clonecore::MbrPartition& partition) {
        const auto record = std::find_if(
            manifest.partitions.begin(), manifest.partitions.end(),
            [&](const TsumugiManifestPartition& item) {
              return item.source_table_index ==
                  static_cast<std::uint32_t>(partition.table_index) + 1U;
            });
        return record == manifest.partitions.end() ||
            !flag_set(
                record->flags, TsumugiManifestPartitionFlags::selected);
      });
  const bool require_active = std::any_of(
      parsed.value().partitions.begin(), parsed.value().partitions.end(),
      [](const clonecore::MbrPartition& partition) {
        return partition.active;
      });
  auto target = clonecore::make_mbr_write_plan(
      parsed.value(),
      target_size_bytes,
      target_sector_size,
      signature_generator,
      disallowed_mbr_signatures,
      require_active);
  if (!target) {
    return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::failure(
        target.error());
  }
  result.target_layout = target.value().target_disk;
  result.commit_writes.push_back(TsumugiRestoreLayoutWrite{
      .kind = TsumugiRestoreLayoutWriteKind::mbr_sector,
      .offset = 0U,
      .bytes = std::move(target.value().sector),
  });
  return clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>::success(
      std::move(result));
}

clonecore::Result<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
    const TsumugiManifest& manifest,
    const std::uint64_t target_size_bytes,
    const std::uint32_t target_sector_size,
    const TsumugiManifestPartitionStyle target_style,
    const bool windows_is_amd64,
    clonecore::IGuidGenerator& guid_generator,
    clonecore::IMbrSignatureGenerator& signature_generator,
    const std::span<const std::uint32_t> disallowed_mbr_signatures) {
  const auto canonical = build_tsumugi_manifest_v1(manifest);
  if (!canonical) {
    return clonecore::Result<
        TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        canonical.error());
  }
  if (manifest.mode != TsumugiManifestMode::shrink) {
    return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小レイアウトモード",
        L"縮小マニフェストだけを縮小レイアウトへ渡せます");
  }
  const auto source_migration_style =
      to_migration_style(manifest.partition_style);
  const auto target_migration_style = to_migration_style(target_style);
  if (!source_migration_style || !target_migration_style) {
    return clonecore::Result<
        TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        source_migration_style ? target_migration_style.error()
                               : source_migration_style.error());
  }
  if (manifest.partition_style == TsumugiManifestPartitionStyle::gpt &&
      target_style == TsumugiManifestPartitionStyle::mbr) {
    return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小形式変換",
        L"GPTからMBRへの変換には対応していません");
  }
  const bool bitlocker_marked =
      flag_set(
          manifest.flags,
          TsumugiManifestFlags::bitlocker_source_was_unlocked) ||
      std::any_of(
          manifest.partitions.begin(), manifest.partitions.end(),
          [](const TsumugiManifestPartition& partition) {
            return flag_set(
                partition.flags,
                TsumugiManifestPartitionFlags::bitlocker_was_unlocked);
          });
  if (bitlocker_marked) {
    return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小BitLocker境界",
        L"現行の縮小captureが未保証のBitLocker印を持つ画像は復元計画へ進めません");
  }

  const auto snapshot =
      inspect_partition_snapshot_v1(manifest.partition_snapshot);
  if (!snapshot) {
    return clonecore::Result<
        TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        snapshot.error());
  }
  SnapshotReader reader(snapshot.value());
  std::optional<clonecore::GptDisk> source_gpt;
  std::optional<clonecore::MbrDisk> source_mbr;
  if (snapshot.value().style == PartitionTableStyle::gpt) {
    auto parsed = clonecore::parse_gpt(reader);
    if (!parsed) {
      return clonecore::Result<
          TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          parsed.error());
    }
    const auto binding = validate_gpt_manifest_binding(
        manifest, parsed.value());
    if (!binding) {
      return clonecore::Result<
          TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          binding.error());
    }
    source_gpt = parsed.take_value();
  } else if (snapshot.value().style == PartitionTableStyle::mbr) {
    auto parsed = clonecore::parse_mbr(reader);
    if (!parsed) {
      return clonecore::Result<
          TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          parsed.error());
    }
    const auto binding = validate_mbr_manifest_binding(
        manifest, parsed.value());
    if (!binding) {
      return clonecore::Result<
          TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          binding.error());
    }
    source_mbr = parsed.take_value();
  } else {
    return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小snapshot形式",
        L"認証済みsnapshotがMBR/GPTではありません");
  }

  migrationcore::ShrinkMigrationRequest request{
      .source_style = source_migration_style.value(),
      .target_style = target_migration_style.value(),
      .target_size_bytes = target_size_bytes,
      .target_logical_sector_size = target_sector_size,
      .source_is_windows_system = flag_set(
          manifest.flags,
          TsumugiManifestFlags::source_contains_windows),
      .windows_is_amd64 = windows_is_amd64,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation = flag_set(
          manifest.flags,
          TsumugiManifestFlags::automatic_surplus_allocation)
          ? migrationcore::ShrinkSurplusAllocation::automatic_proportional
          : migrationcore::ShrinkSurplusAllocation::leave_unallocated,
  };
  request.source_partitions.reserve(manifest.partitions.size());
  for (const auto& partition : manifest.partitions) {
    if (!flag_set(
            partition.flags,
            TsumugiManifestPartitionFlags::selected)) {
      continue;
    }
    if (partition.payload_encoding ==
            TsumugiManifestPayloadEncoding::exact_raw &&
        (manifest.partition_style != target_style ||
         manifest.logical_sector_size != target_sector_size)) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi縮小exact RAW変換",
          L"exact RAWを含む画像はパーティション形式または論理セクターを変換できません");
    }
    auto role = to_migration_role(
        partition, manifest.partition_style, target_style);
    if (!role) {
      return clonecore::Result<
          TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          role.error());
    }
    const auto& label_text = partition.label_utf8.empty()
        ? partition.name_utf8
        : partition.label_utf8;
    auto label = from_utf8(label_text);
    if (!label) {
      return clonecore::Result<
          TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          label.error());
    }
    request.source_partitions.push_back(
        migrationcore::ShrinkSourcePartition{
            .source_table_index = partition.source_table_index,
            .role = role.value(),
            .file_system = to_migration_file_system(partition),
            .source_size_bytes = partition.source_size,
            .used_bytes = partition.used_bytes,
            .minimum_target_bytes = partition.minimum_target_bytes,
            .cluster_size = partition.cluster_size,
            .label = label.take_value(),
            .active = flag_set(
                partition.flags,
                TsumugiManifestPartitionFlags::active),
        });
  }

  auto migration = migrationcore::plan_shrink_migration(request);
  if (!migration) {
    return clonecore::Result<
        TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        migration.error());
  }
  if (!migration.value().source_remains_unchanged ||
      migration.value().target_style != target_migration_style.value() ||
      migration.value().target_size_bytes != target_size_bytes) {
    return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小計画整合性",
        L"共通縮小計画がコピー元不変またはコピー先寸法を維持していません");
  }

  for (const auto& planned : migration.value().target_partitions) {
    if (!planned.source_table_index.has_value()) {
      continue;
    }
    const auto record = std::find_if(
        manifest.partitions.begin(), manifest.partitions.end(),
        [&](const TsumugiManifestPartition& partition) {
          return partition.source_table_index ==
                  planned.source_table_index.value() &&
              flag_set(
                  partition.flags,
                  TsumugiManifestPartitionFlags::selected);
        });
    if (record == manifest.partitions.end() ||
        planned.size_bytes < record->minimum_target_bytes ||
        (record->payload_encoding ==
             TsumugiManifestPayloadEncoding::exact_raw &&
         planned.action !=
             migrationcore::MigrationPartitionAction::copy_exact_raw)) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小payload配置照合",
          L"認証済み区画と共通縮小計画の識別、最小容量、またはRAW方針が一致しません");
    }
  }
  for (const auto& record : manifest.partitions) {
    if (!flag_set(
            record.flags,
            TsumugiManifestPartitionFlags::selected) ||
        record.role == TsumugiManifestPartitionRole::efi_system ||
        record.role ==
            TsumugiManifestPartitionRole::microsoft_reserved ||
        (manifest.partition_style == TsumugiManifestPartitionStyle::mbr &&
         target_style == TsumugiManifestPartitionStyle::gpt &&
         record.role == TsumugiManifestPartitionRole::bios_system)) {
      continue;
    }
    const auto count = static_cast<std::size_t>(std::count_if(
        migration.value().target_partitions.begin(),
        migration.value().target_partitions.end(),
        [&](const migrationcore::ShrinkPlannedPartition& partition) {
          return partition.source_table_index == record.source_table_index;
        }));
    if (count != 1U) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Tsumugi縮小payload一対一照合",
          L"選択済み内容区画のコピー先を一意に確定できません");
    }
  }

  TsumugiShrinkWholeDiskRestoreLayoutPlanV1 result{
      .migration = migration.take_value(),
  };
  if (target_style == TsumugiManifestPartitionStyle::gpt) {
    constexpr std::uint32_t entry_count = 128U;
    constexpr std::uint32_t entry_size = 128U;
    if ((target_sector_size != 512U && target_sector_size != 4096U) ||
        target_size_bytes == 0U ||
        target_size_bytes % target_sector_size != 0U) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi縮小GPT寸法",
          L"GPTコピー先の容量または論理セクターが対応条件外です");
    }
    const std::uint64_t sector_count =
        target_size_bytes / target_sector_size;
    const std::uint64_t entry_bytes =
        static_cast<std::uint64_t>(entry_count) * entry_size;
    const std::uint64_t entry_sectors =
        (entry_bytes + target_sector_size - 1U) / target_sector_size;
    if (sector_count <= 3U + 2U * entry_sectors) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"Tsumugi縮小GPTメタデータ",
          L"GPTメタデータを安全に配置できません");
    }
    clonecore::GptDisk synthetic{
        .logical_sector_size = target_sector_size,
        .sector_count = sector_count,
        .first_usable_lba = 2U + entry_sectors,
        .last_usable_lba = sector_count - 2U - entry_sectors,
        .partition_entry_count = entry_count,
        .partition_entry_size = entry_size,
    };
    synthetic.partitions.reserve(result.migration.target_partitions.size());
    for (const auto& partition : result.migration.target_partitions) {
      std::uint64_t end{};
      if (partition.target_number == 0U ||
          partition.target_number > entry_count ||
          partition.offset_bytes % target_sector_size != 0U ||
          partition.size_bytes == 0U ||
          partition.size_bytes % target_sector_size != 0U ||
          partition.offset_bytes >
              (std::numeric_limits<std::uint64_t>::max)() -
                  partition.size_bytes) {
        return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小GPT区画",
            L"GPT区画番号、offset、長さ、または終端が不正です");
      }
      end = partition.offset_bytes + partition.size_bytes;
      if (end > target_size_bytes) {
        return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_DISK_FULL,
            L"Tsumugi縮小GPT区画境界",
            L"GPT区画がコピー先ディスク範囲を超えています");
      }
      clonecore::GptGuid type = gpt_type_for(partition.role);
      std::uint64_t attributes =
          partition.role == migrationcore::MigrationPartitionRole::recovery
          ? 0x8000000000000001ULL
          : 0U;
      if (source_gpt.has_value() &&
          partition.source_table_index.has_value()) {
        const auto source_partition = std::find_if(
            source_gpt->partitions.begin(), source_gpt->partitions.end(),
            [&](const clonecore::GptPartition& candidate) {
              return static_cast<std::uint64_t>(candidate.entry_index) + 1U ==
                  partition.source_table_index.value();
            });
        if (source_partition == source_gpt->partitions.end()) {
          return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_NOT_FOUND,
              L"Tsumugi縮小GPT種類照合",
              L"認証済みsnapshotに対応するGPT区画がありません");
        }
        type = source_partition->type_guid;
        attributes = source_partition->attributes;
      }
      synthetic.partitions.push_back(clonecore::GptPartition{
          .entry_index = partition.target_number - 1U,
          .type_guid = type,
          .first_lba = partition.offset_bytes / target_sector_size,
          .last_lba = end / target_sector_size - 1U,
          .attributes = attributes,
          .name = to_utf16_name(partition.label),
      });
    }
    auto target = clonecore::make_gpt_write_plan(
        synthetic,
        target_size_bytes,
        target_sector_size,
        guid_generator);
    if (!target) {
      return clonecore::Result<
          TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          target.error());
    }
    auto metadata = make_gpt_metadata_plan(
        target.take_value(), target_size_bytes, target_sector_size);
    if (!metadata) {
      return clonecore::Result<
          TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          metadata.error());
    }
    result.metadata = metadata.take_value();
    return clonecore::Result<
        TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::success(
        std::move(result));
  }

  if (target_style != TsumugiManifestPartitionStyle::mbr ||
      !source_mbr.has_value() || target_sector_size != 512U ||
      result.migration.target_partitions.size() > 4U ||
      target_size_bytes / target_sector_size >
          (std::numeric_limits<std::uint32_t>::max)()) {
    return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小MBR寸法",
        L"MBR維持は512バイト論理セクター、4区画以下、32bit LBA範囲に限定します");
  }
  clonecore::MbrDisk synthetic{
      .logical_sector_size = target_sector_size,
      .sector_count = target_size_bytes / target_sector_size,
      .disk_signature = source_mbr->disk_signature,
      .bootstrap = source_mbr->bootstrap,
  };
  constexpr std::array<std::byte, 3U> chs{
      std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}};
  synthetic.partitions.reserve(result.migration.target_partitions.size());
  for (const auto& partition : result.migration.target_partitions) {
    if (!partition.source_table_index.has_value() ||
        partition.target_number == 0U || partition.target_number > 4U ||
        partition.offset_bytes % target_sector_size != 0U ||
        partition.size_bytes == 0U ||
        partition.size_bytes % target_sector_size != 0U) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小MBR区画",
          L"MBR区画の対応、番号、offset、または長さが不正です");
    }
    const std::uint64_t first_lba =
        partition.offset_bytes / target_sector_size;
    const std::uint64_t sector_count =
        partition.size_bytes / target_sector_size;
    if (first_lba > (std::numeric_limits<std::uint32_t>::max)() ||
        sector_count == 0U ||
        sector_count > (std::numeric_limits<std::uint32_t>::max)()) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi縮小MBR LBA",
          L"MBR区画が32bit LBA範囲を超えています");
    }
    const auto source_partition = std::find_if(
        source_mbr->partitions.begin(), source_mbr->partitions.end(),
        [&](const clonecore::MbrPartition& candidate) {
          return static_cast<std::uint32_t>(candidate.table_index) + 1U ==
              partition.source_table_index.value();
        });
    if (source_partition == source_mbr->partitions.end()) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Tsumugi縮小MBR種類照合",
          L"認証済みsnapshotに対応するMBR区画がありません");
    }
    std::uint8_t type = 0x07U;
    if (partition.action ==
        migrationcore::MigrationPartitionAction::copy_exact_raw) {
      type = source_partition->type;
    } else if (
        partition.role == migrationcore::MigrationPartitionRole::recovery) {
      type = 0x27U;
    } else if (
        partition.file_system == migrationcore::MigrationFileSystem::fat32) {
      type = 0x0CU;
    }
    if (type == 0U) {
      return failure<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi縮小MBR種類",
          L"空のMBRパーティション種類は使用できません");
    }
    synthetic.partitions.push_back(clonecore::MbrPartition{
        .table_index =
            static_cast<std::uint8_t>(partition.target_number - 1U),
        .active = partition.active,
        .first_chs = chs,
        .type = type,
        .last_chs = chs,
        .first_lba = static_cast<std::uint32_t>(first_lba),
        .sector_count = static_cast<std::uint32_t>(sector_count),
    });
  }
  auto target = clonecore::make_mbr_write_plan(
      synthetic,
      target_size_bytes,
      target_sector_size,
      signature_generator,
      disallowed_mbr_signatures,
      request.source_is_windows_system);
  if (!target) {
    return clonecore::Result<
        TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        target.error());
  }
  auto metadata = make_mbr_metadata_plan(
      target.take_value(), target_size_bytes, target_sector_size);
  if (!metadata) {
    return clonecore::Result<
        TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        metadata.error());
  }
  result.metadata = metadata.take_value();
  return clonecore::Result<
      TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::success(
      std::move(result));
}

clonecore::Result<std::vector<TsumugiShrinkConstructionLayoutPlanV1>>
make_tsumugi_shrink_construction_layout_plans_v1(
    const TsumugiShrinkWholeDiskRestoreLayoutPlanV1& final_plan,
    clonecore::IGuidGenerator& guid_generator) {
  const auto& migration = final_plan.migration;
  const auto& final_metadata = final_plan.metadata;
  const bool final_is_gpt =
      migration.target_style == migrationcore::MigrationPartitionStyle::gpt;
  const bool final_layout_type_matches = final_is_gpt
      ? final_metadata.style == PartitionTableStyle::gpt &&
          std::holds_alternative<clonecore::GptDisk>(
              final_metadata.target_layout)
      : final_metadata.style == PartitionTableStyle::mbr &&
          std::holds_alternative<clonecore::MbrDisk>(
              final_metadata.target_layout);
  const bool final_sector_supported =
      final_metadata.logical_sector_size == 512U ||
      final_metadata.logical_sector_size == 4096U;
  const bool final_geometry_matches = final_sector_supported &&
      final_layout_type_matches &&
      (final_is_gpt
          ? std::get<clonecore::GptDisk>(final_metadata.target_layout)
                    .logical_sector_size ==
                final_metadata.logical_sector_size &&
              std::get<clonecore::GptDisk>(final_metadata.target_layout)
                      .sector_count ==
                  final_metadata.target_size_bytes /
                      final_metadata.logical_sector_size
          : std::get<clonecore::MbrDisk>(final_metadata.target_layout)
                    .logical_sector_size ==
                final_metadata.logical_sector_size &&
              std::get<clonecore::MbrDisk>(final_metadata.target_layout)
                      .sector_count ==
                  final_metadata.target_size_bytes /
                      final_metadata.logical_sector_size);
  if (!migration.source_remains_unchanged ||
      migration.target_size_bytes == 0U ||
      final_metadata.target_size_bytes != migration.target_size_bytes ||
      (final_metadata.logical_sector_size != 512U &&
       final_metadata.logical_sector_size != 4096U) ||
      final_metadata.target_size_bytes %
              final_metadata.logical_sector_size !=
          0U ||
      migration.alignment_bytes == 0U ||
      migration.alignment_bytes % final_metadata.logical_sector_size != 0U ||
      !final_layout_type_matches || !final_geometry_matches) {
    return failure<
        std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小仮GPT最終計画照合",
        L"レビュー済み最終レイアウトの形式、寸法、整列、またはコピー元不変条件が一致しません");
  }

  std::set<std::uint32_t> target_numbers;
  std::set<std::uint32_t> source_indexes;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> final_ranges;
  std::vector<TsumugiShrinkConstructionLayoutPlanV1> result;
  std::set<std::array<std::byte, 16U>> construction_guids;
  std::uint32_t expected_target_number = 1U;
  for (const auto& partition : migration.target_partitions) {
    std::uint64_t end{};
    if (partition.target_number != expected_target_number++ ||
        partition.role > migrationcore::MigrationPartitionRole::data ||
        partition.file_system >
            migrationcore::MigrationFileSystem::unsupported ||
        partition.size_bytes == 0U ||
        partition.offset_bytes % final_metadata.logical_sector_size != 0U ||
        partition.offset_bytes % migration.alignment_bytes != 0U ||
        partition.size_bytes % final_metadata.logical_sector_size != 0U ||
        !checked_add(partition.offset_bytes, partition.size_bytes, end) ||
        end > final_metadata.target_size_bytes ||
        !target_numbers.insert(partition.target_number).second) {
      return failure<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小仮GPT最終区画照合",
          L"最終区画の番号、範囲、整列、または一意性が不正です");
    }
    final_ranges.emplace_back(partition.offset_bytes, end);
    if (partition.source_table_index.has_value() &&
        (*partition.source_table_index == 0U ||
         !source_indexes.insert(*partition.source_table_index).second)) {
      return failure<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小仮GPTコピー元対応",
          L"コピー元区画番号が0または最終区画間で重複しています");
    }

    std::optional<TsumugiShrinkConstructionPurposeV1> purpose;
    using Action = migrationcore::MigrationPartitionAction;
    using FileSystem = migrationcore::MigrationFileSystem;
    using Role = migrationcore::MigrationPartitionRole;
    switch (partition.action) {
      case Action::apply_file_image:
        if (!partition.source_table_index.has_value() ||
            migrationcore::classify_shrink_file_system(
                partition.file_system) !=
                migrationcore::ShrinkFileSystemDisposition::file_archive ||
            partition.role == Role::efi_system ||
            partition.role == Role::microsoft_reserved) {
          return failure<
              std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小仮GPT file image対応",
              L"file image区画のコピー元、役割、またはファイルシステムが不正です");
        }
        purpose = TsumugiShrinkConstructionPurposeV1::apply_file_image;
        break;
      case Action::create_empty_ntfs:
      case Action::create_empty_exfat:
      case Action::create_empty_fat32: {
        const bool file_system_matches =
            (partition.action == Action::create_empty_ntfs &&
             partition.file_system == FileSystem::ntfs) ||
            (partition.action == Action::create_empty_exfat &&
             partition.file_system == FileSystem::exfat) ||
            (partition.action == Action::create_empty_fat32 &&
             partition.file_system == FileSystem::fat32);
        if (!partition.source_table_index.has_value() ||
            !file_system_matches || partition.role == Role::efi_system ||
            partition.role == Role::microsoft_reserved) {
          return failure<
              std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小仮GPT空ファイルシステム対応",
              L"空ファイルシステム区画のコピー元、役割、またはactionが不正です");
        }
        purpose =
            TsumugiShrinkConstructionPurposeV1::recreate_empty_file_system;
        break;
      }
      case Action::create_fat32:
        if (!final_is_gpt || partition.source_table_index.has_value() ||
            partition.role != Role::efi_system ||
            partition.file_system != FileSystem::fat32 || partition.active) {
          return failure<
              std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小仮GPT生成ESP対応",
              L"生成ESPはコピー元なしのGPT FAT32非Active区画である必要があります");
        }
        purpose = TsumugiShrinkConstructionPurposeV1::prepare_efi_system;
        break;
      case Action::create_reserved:
        if (!final_is_gpt || partition.source_table_index.has_value() ||
            partition.role != Role::microsoft_reserved ||
            partition.file_system != FileSystem::none || partition.active) {
          return failure<
              std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小仮GPT生成MSR対応",
              L"生成MSRはコピー元なしのGPT予約領域である必要があります");
        }
        // MSR carries no filesystem or payload, so it must never be exposed
        // through a temporary Basic Data construction partition.
        continue;
      case Action::copy_exact_raw:
        if (!partition.source_table_index.has_value() ||
            migrationcore::classify_shrink_file_system(
                partition.file_system) !=
                migrationcore::ShrinkFileSystemDisposition::exact_raw_only ||
            partition.role == Role::efi_system ||
            partition.role == Role::microsoft_reserved) {
          return failure<
              std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小仮GPT exact RAW対応",
              L"exact RAW区画のコピー元、役割、またはファイルシステムが不正です");
        }
        // RAW restore must remain offline and is not a volume construction
        // operation.
        continue;
    }
    if (!purpose.has_value()) {
      return failure<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi縮小仮GPT action",
          L"最終区画actionを一時constructionへ対応付けられません");
    }

    constexpr std::uint32_t entry_count = 128U;
    constexpr std::uint32_t entry_size = 128U;
    const std::uint64_t sector_count =
        final_metadata.target_size_bytes /
        final_metadata.logical_sector_size;
    const std::uint64_t entry_bytes =
        static_cast<std::uint64_t>(entry_count) * entry_size;
    const std::uint64_t entry_sectors =
        (entry_bytes + final_metadata.logical_sector_size - 1U) /
        final_metadata.logical_sector_size;
    if (sector_count <= 3U + 2U * entry_sectors) {
      return failure<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"Tsumugi縮小仮GPT容量",
          L"安全な仮GPTメタデータを配置できません");
    }
    const std::wstring marker =
        L"YTEC-Tsumugi-INCOMPLETE-" +
        std::to_wstring(partition.target_number);
    if (marker.size() > 36U) {
      return failure<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小仮GPT印",
          L"未完了識別名がGPT名領域を超えています");
    }
    clonecore::GptDisk synthetic{
        .logical_sector_size = final_metadata.logical_sector_size,
        .sector_count = sector_count,
        .first_usable_lba = 2U + entry_sectors,
        .last_usable_lba = sector_count - 2U - entry_sectors,
        .partition_entry_count = entry_count,
        .partition_entry_size = entry_size,
        .partitions = {
            clonecore::GptPartition{
                .entry_index = 0U,
                .type_guid = clonecore::gpt_type_basic_data(),
                .first_lba = partition.offset_bytes /
                    final_metadata.logical_sector_size,
                .last_lba = end / final_metadata.logical_sector_size - 1U,
                .attributes =
                    kTsumugiConstructionNoDefaultDriveLetterAttribute,
                .name = to_utf16_name(marker),
            },
        },
    };
    auto generated = clonecore::make_gpt_write_plan(
        synthetic,
        final_metadata.target_size_bytes,
        final_metadata.logical_sector_size,
        guid_generator);
    if (!generated) {
      return clonecore::Result<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>::failure(
          generated.error());
    }
    if (!construction_guids
             .insert(generated.value().target_disk.disk_guid.bytes)
             .second ||
        generated.value().target_disk.partitions.size() != 1U ||
        !construction_guids
             .insert(
                 generated.value().target_disk.partitions.front()
                     .unique_guid.bytes)
             .second) {
      return failure<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"Tsumugi縮小仮GPT GUID",
          L"仮GPTのDisk／Partition GUIDが空、重複、または一意ではありません");
    }
    auto temporary = make_gpt_metadata_plan(
        generated.take_value(),
        final_metadata.target_size_bytes,
        final_metadata.logical_sector_size);
    if (!temporary) {
      return clonecore::Result<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>::failure(
          temporary.error());
    }
    const auto retirement = exact_metadata_ranges(
        temporary.value(), partition.offset_bytes, partition.size_bytes);
    if (!retirement) {
      return clonecore::Result<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>::failure(
          retirement.error());
    }
    result.push_back(TsumugiShrinkConstructionLayoutPlanV1{
        .purpose = *purpose,
        .source_table_index = partition.source_table_index,
        .final_target_number = partition.target_number,
        .target_offset = partition.offset_bytes,
        .target_size = partition.size_bytes,
        .temporary_metadata = temporary.take_value(),
        .retirement_ranges = retirement.value(),
    });
  }
  std::sort(final_ranges.begin(), final_ranges.end());
  for (std::size_t index = 1U; index < final_ranges.size(); ++index) {
    if (final_ranges[index].first < final_ranges[index - 1U].second) {
      return failure<
          std::vector<TsumugiShrinkConstructionLayoutPlanV1>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小仮GPT最終区画重複",
          L"レビュー済み最終区画の範囲が重複しています");
    }
  }
  return clonecore::Result<
      std::vector<TsumugiShrinkConstructionLayoutPlanV1>>::success(
      std::move(result));
}

}  // namespace ytec::imageformat

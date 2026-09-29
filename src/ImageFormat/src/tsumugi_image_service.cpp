#include "ytec/imageformat/tsumugi_image_service.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

clonecore::Error service_error(
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
  return clonecore::Result<T>::failure(service_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& value) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  value = left + right;
  return true;
}

bool all_zero(const Sha256Digest& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool partition_flag(
    const TsumugiManifestPartitionFlags value,
    const TsumugiManifestPartitionFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

bool chunk_is_zero(const TsumugiChunkFlags value) noexcept {
  return (static_cast<std::uint32_t>(value) & 1U) != 0U;
}

bool chunk_is_unreadable(const TsumugiChunkFlags value) noexcept {
  return (static_cast<std::uint32_t>(value) & 2U) != 0U;
}

clonecore::Status validate_storage(
    const TsumugiImageStorageFileSystem file_system,
    const std::wstring_view operation) {
  if (file_system != TsumugiImageStorageFileSystem::ntfs &&
      file_system != TsumugiImageStorageFileSystem::exfat) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        std::wstring(operation),
        L"単一.tsumugiファイルはNTFSまたはexFAT上だけで作成・復元できます"));
  }
  return clonecore::success_status();
}

TsumugiPayloadKind payload_kind(const TsumugiManifestMode mode) noexcept {
  switch (mode) {
    case TsumugiManifestMode::exact:
      return TsumugiPayloadKind::exact_disk;
    case TsumugiManifestMode::shrink:
      return TsumugiPayloadKind::shrink_disk;
    case TsumugiManifestMode::rescue:
      return TsumugiPayloadKind::rescue_disk;
  }
  return TsumugiPayloadKind::exact_disk;
}

struct LogicalRange final {
  std::uint64_t offset{};
  std::uint64_t length{};
  std::uint64_t source_offset{};
  bool source_offset_known{};
  TsumugiChunkFlags flags{TsumugiChunkFlags::none};
};

clonecore::Status validate_payload_coverage(
    const TsumugiManifest& manifest,
    const std::span<const LogicalRange> ranges) {
  std::map<std::uint32_t, std::uint64_t> next_offset;
  std::map<std::uint32_t, std::uint64_t> expected_end;
  for (const auto& partition : manifest.partitions) {
    if (!partition_flag(
            partition.flags, TsumugiManifestPartitionFlags::selected)) {
      continue;
    }
    std::uint64_t end{};
    if (!checked_add(
            partition.payload_logical_offset,
            partition.payload_logical_length,
            end)) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi payload範囲検証",
          L"パーティションpayload範囲がオーバーフローします"));
    }
    next_offset.emplace(
        partition.source_table_index,
        partition.payload_logical_offset);
    expected_end.emplace(partition.source_table_index, end);
  }
  if (next_offset.empty() || ranges.empty()) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi payload範囲検証",
        L"選択済みパーティションまたはpayloadチャンクがありません"));
  }
  for (const auto& range : ranges) {
    std::uint64_t range_end{};
    if (range.length == 0U ||
        !checked_add(range.offset, range.length, range_end)) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi payloadチャンク検証",
          L"チャンク範囲が空またはオーバーフローします"));
    }
    const TsumugiManifestPartition* owner = nullptr;
    for (const auto& partition : manifest.partitions) {
      if (!partition_flag(
              partition.flags, TsumugiManifestPartitionFlags::selected)) {
        continue;
      }
      const auto end = expected_end.at(partition.source_table_index);
      if (range.offset >= partition.payload_logical_offset &&
          range_end <= end) {
        if (owner != nullptr) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_DUP_NAME,
              L"Tsumugi payload所有領域検証",
              L"チャンクが複数パーティションに対応します"));
        }
        owner = &partition;
      }
    }
    if (owner == nullptr ||
        next_offset.at(owner->source_table_index) != range.offset) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi payload連続性検証",
          L"チャンクが選択領域外、重複、または明示的に表現されない穴を持ちます"));
    }
    const bool byte_stream_archive =
        manifest.mode == TsumugiManifestMode::shrink &&
        owner->payload_encoding ==
            TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
    if (!byte_stream_archive &&
        (range.offset % manifest.logical_sector_size != 0U ||
         range.length % manifest.logical_sector_size != 0U ||
         (range.source_offset_known &&
          range.source_offset % manifest.logical_sector_size != 0U))) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi exact RAWチャンク整列",
          L"exact RAWのpayload、長さ、Source offsetは論理セクター境界が必要です"));
    }
    next_offset[owner->source_table_index] = range_end;
    if (chunk_is_unreadable(range.flags) &&
        manifest.mode != TsumugiManifestMode::rescue) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi欠損マップ検証",
          L"読取り不能ゼロ埋めは救出モードだけです"));
    }
  }
  for (const auto& [index, end] : expected_end) {
    if (next_offset.at(index) != end) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_HANDLE_EOF,
          L"Tsumugi payload完全性検証",
          L"選択領域の全範囲がチャンクまたは明示的ゼロ埋めで表現されていません"));
    }
  }
  return clonecore::success_status();
}

std::vector<clonecore::ByteRange> unreadable_ranges(
    const std::span<const LogicalRange> ranges) {
  std::vector<clonecore::ByteRange> result;
  for (const auto& range : ranges) {
    if (!chunk_is_unreadable(range.flags)) {
      continue;
    }
    if (!result.empty() &&
        result.back().offset + result.back().length == range.offset) {
      result.back().length += range.length;
    } else {
      result.push_back({.offset = range.offset, .length = range.length});
    }
  }
  return result;
}

std::vector<LogicalRange> logical_ranges(
    const std::span<const TsumugiStreamBuildChunk> chunks) {
  std::vector<LogicalRange> ranges;
  ranges.reserve(chunks.size());
  for (const auto& chunk : chunks) {
    ranges.push_back({
        .offset = chunk.logical_offset,
        .length = chunk.logical_length,
        .source_offset = chunk.source_offset,
        .source_offset_known = true,
        .flags = chunk.flags,
    });
  }
  return ranges;
}

std::vector<LogicalRange> logical_ranges(
    const std::span<const TsumugiChunkRecord> records) {
  std::vector<LogicalRange> ranges;
  ranges.reserve(records.size());
  for (const auto& record : records) {
    ranges.push_back({
        .offset = record.logical_offset,
        .length = record.logical_length,
        .source_offset = 0U,
        .source_offset_known = false,
        .flags = record.flags,
    });
  }
  return ranges;
}

clonecore::Status validate_inspection(
    const TsumugiStreamInspection& container,
    const TsumugiManifest& manifest) {
  const bool encrypted =
      (container.header.required_features &
       static_cast<std::uint32_t>(TsumugiRequiredFeature::encrypted)) != 0U;
  if (container.header.payload_kind != payload_kind(manifest.mode) ||
      container.header.source_disk_size != manifest.source_disk_size ||
      container.header.logical_sector_size != manifest.logical_sector_size ||
      container.header.physical_sector_size != manifest.physical_sector_size ||
      !container.header_hash_verified ||
      (encrypted && !container.metadata_authenticated) ||
      !container.all_chunks_verified || !container.global_hash_verified) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugiコンテナとマニフェストの照合",
        L"種類、ディスク寸法、セクタ、または完全検証状態が一致しません"));
  }
  const auto ranges = logical_ranges(container.records);
  return validate_payload_coverage(manifest, ranges);
}

bool same_record(
    const TsumugiChunkRecord& left,
    const TsumugiChunkRecord& right) noexcept {
  return left.logical_offset == right.logical_offset &&
      left.logical_length == right.logical_length &&
      left.stored_offset == right.stored_offset &&
      left.stored_length == right.stored_length &&
      left.flags == right.flags && left.compression == right.compression &&
      left.nonce_counter == right.nonce_counter &&
      left.plaintext_hash == right.plaintext_hash &&
      left.authentication_tag == right.authentication_tag;
}

bool same_inspection(
    const TsumugiStreamInspection& left,
    const TsumugiStreamInspection& right) noexcept {
  if (!left.opened_file.identity_from_open_handle ||
      !right.opened_file.identity_from_open_handle ||
      left.opened_file != right.opened_file ||
      left.header.header_hash != right.header.header_hash ||
      left.header.image_id != right.header.image_id ||
      left.global_hash != right.global_hash ||
      left.manifest != right.manifest ||
      left.records.size() != right.records.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.records.size(); ++index) {
    if (!same_record(left.records[index], right.records[index])) {
      return false;
    }
  }
  return true;
}

const TsumugiManifestPartition* selected_partition(
    const TsumugiManifest& manifest,
    const std::uint32_t source_table_index) noexcept {
  const auto found = std::find_if(
      manifest.partitions.begin(),
      manifest.partitions.end(),
      [&](const auto& partition) {
        return partition.source_table_index == source_table_index &&
            partition_flag(
                partition.flags,
                TsumugiManifestPartitionFlags::selected);
      });
  return found == manifest.partitions.end() ? nullptr : &*found;
}

clonecore::Status validate_target_disk(
    const TsumugiRestoreDiskIdentity& disk,
    const TsumugiRestoreHost host,
    const std::uint32_t source_sector_size) {
  const bool running_windows_rejected =
      host == TsumugiRestoreHost::windows &&
      disk.is_running_windows_system_disk;
  const bool unsupported_target = disk.is_usb_memory ||
      disk.is_active_rescue_media || disk.is_dynamic_disk ||
      disk.is_storage_spaces || disk.is_windows_software_raid ||
      disk.has_unresolved_hardware_raid;
  const bool invalid_identity = all_zero(disk.stable_identity_hash) ||
      disk.disk_size == 0U ||
      disk.logical_sector_size != source_sector_size ||
      (disk.is_usb_attached && all_zero(disk.connection_instance_hash));
  if (running_windows_rejected || unsupported_target || invalid_identity) {
    return clonecore::Status::failure(service_error(
        running_windows_rejected || unsupported_target
            ? clonecore::ErrorCode::unsupported_layout
            : clonecore::ErrorCode::identity_mismatch,
        running_windows_rejected || unsupported_target
            ? ERROR_ACCESS_DENIED
            : ERROR_INVALID_DATA,
        L"Tsumugi復元先ディスク検証",
        running_windows_rejected
            ? L"稼働中Windows自身のシステムディスクはWindows上から復元できません。PEで画像と対象を改めて選択してください"
            : unsupported_target
                ? L"USBメモリ、起動中レスキュー媒体、Dynamic Disk、Storage Spaces、ソフトウェアRAID、または未解決RAIDは復元先にできません"
                : L"安定識別Hash、容量、論理セクタ、またはUSB接続セッションが復元計画と一致しません"));
  }
  return clonecore::success_status();
}

const TsumugiRestoreDiskIdentity& target_disk(
    const TsumugiRestoreTarget& target) {
  return std::visit(
      [](const auto& selected) -> const TsumugiRestoreDiskIdentity& {
        if constexpr (std::is_same_v<
                          std::decay_t<decltype(selected)>,
                          TsumugiWholeDiskRestoreTarget>) {
          return selected.disk;
        } else {
          return std::visit(
              [](const auto& individual)
                  -> const TsumugiRestoreDiskIdentity& {
                return individual.disk;
              },
              selected.target);
        }
      },
      target);
}

clonecore::Result<std::uint64_t> planned_payload_bytes(
    const TsumugiStreamInspection& container,
    const TsumugiManifestPartition* selected) {
  std::uint64_t total{};
  for (const auto& record : container.records) {
    std::uint64_t record_end{};
    std::uint64_t selected_end{};
    if (selected != nullptr &&
        (!checked_add(
             record.logical_offset, record.logical_length, record_end) ||
         !checked_add(
             selected->payload_logical_offset,
             selected->payload_logical_length,
             selected_end) ||
         record.logical_offset < selected->payload_logical_offset ||
         record_end > selected_end)) {
      continue;
    }
    if (!checked_add(total, record.logical_length, total)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi復元payload合計",
          L"復元payload総量がオーバーフローします");
    }
  }
  return clonecore::Result<std::uint64_t>::success(total);
}

bool same_target_identity(
    const TsumugiRestoreDiskIdentity& planned,
    const TsumugiRestoreDiskIdentity& current) noexcept {
  return planned.stable_identity_hash == current.stable_identity_hash &&
      planned.disk_size == current.disk_size &&
      planned.logical_sector_size == current.logical_sector_size &&
      planned.is_running_windows_system_disk ==
          current.is_running_windows_system_disk &&
      planned.is_usb_attached == current.is_usb_attached &&
      planned.is_usb_memory == current.is_usb_memory &&
      planned.is_active_rescue_media == current.is_active_rescue_media &&
      planned.is_dynamic_disk == current.is_dynamic_disk &&
      planned.is_storage_spaces == current.is_storage_spaces &&
      planned.is_windows_software_raid ==
          current.is_windows_software_raid &&
      planned.has_unresolved_hardware_raid ==
          current.has_unresolved_hardware_raid &&
      planned.connection_instance_hash == current.connection_instance_hash;
}

class RestoreTransactionGuard final {
 public:
  explicit RestoreTransactionGuard(
      ITsumugiRestoreTransaction& transaction) noexcept
      : transaction_(transaction) {}

  ~RestoreTransactionGuard() {
    if (begun_ && !committed_) {
      transaction_.abort();
    }
  }

  RestoreTransactionGuard(const RestoreTransactionGuard&) = delete;
  RestoreTransactionGuard& operator=(const RestoreTransactionGuard&) = delete;

  void mark_begun() noexcept { begun_ = true; }
  void mark_committed() noexcept { committed_ = true; }

 private:
  ITsumugiRestoreTransaction& transaction_;
  bool begun_{};
  bool committed_{};
};

class ShrinkRestoreTransactionGuard final {
 public:
  explicit ShrinkRestoreTransactionGuard(
      ITsumugiShrinkRestoreTransaction& transaction) noexcept
      : transaction_(transaction) {}

  ~ShrinkRestoreTransactionGuard() {
    if (begun_ && !committed_) {
      transaction_.abort();
    }
  }

  ShrinkRestoreTransactionGuard(const ShrinkRestoreTransactionGuard&) = delete;
  ShrinkRestoreTransactionGuard& operator=(
      const ShrinkRestoreTransactionGuard&) = delete;

  void mark_begun() noexcept { begun_ = true; }
  void mark_committed() noexcept { committed_ = true; }

 private:
  ITsumugiShrinkRestoreTransaction& transaction_;
  bool begun_{};
  bool committed_{};
};

bool manifest_flag(
    const TsumugiManifestFlags value,
    const TsumugiManifestFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

std::optional<migrationcore::MigrationPartitionRole> migration_role_for(
    const TsumugiManifestPartitionRole role) noexcept {
  using MigrationRole = migrationcore::MigrationPartitionRole;
  switch (role) {
    case TsumugiManifestPartitionRole::efi_system:
      return MigrationRole::efi_system;
    case TsumugiManifestPartitionRole::microsoft_reserved:
      return MigrationRole::microsoft_reserved;
    case TsumugiManifestPartitionRole::bios_system:
      return MigrationRole::bios_system;
    case TsumugiManifestPartitionRole::windows:
      return MigrationRole::windows;
    case TsumugiManifestPartitionRole::recovery:
      return MigrationRole::recovery;
    case TsumugiManifestPartitionRole::data:
      return MigrationRole::data;
    case TsumugiManifestPartitionRole::other:
      break;
  }
  return std::nullopt;
}

bool empty_file_system_action(
    const migrationcore::MigrationPartitionAction action) noexcept {
  using Action = migrationcore::MigrationPartitionAction;
  return action == Action::create_empty_ntfs ||
      action == Action::create_empty_exfat ||
      action == Action::create_empty_fat32;
}

bool generated_partition_exists(
    const migrationcore::ShrinkMigrationPlan& migration,
    const migrationcore::MigrationPartitionRole role,
    const migrationcore::MigrationPartitionAction action) noexcept {
  return std::count_if(
             migration.target_partitions.begin(),
             migration.target_partitions.end(),
             [&](const migrationcore::ShrinkPlannedPartition& partition) {
               return !partition.source_table_index.has_value() &&
                   partition.role == role && partition.action == action;
             }) == 1;
}

clonecore::Status validate_reviewed_layout_geometry(
    const TsumugiShrinkWholeDiskRestoreLayoutPlanV1& reviewed) {
  const auto& migration = reviewed.migration;
  const auto& metadata = reviewed.metadata;
  const bool gpt =
      migration.target_style == migrationcore::MigrationPartitionStyle::gpt;
  if (!migration.source_remains_unchanged ||
      migration.target_size_bytes == 0U ||
      metadata.target_size_bytes != migration.target_size_bytes ||
      metadata.logical_sector_size == 0U ||
      metadata.target_size_bytes % metadata.logical_sector_size != 0U ||
      migration.alignment_bytes == 0U ||
      (gpt
           ? metadata.style != PartitionTableStyle::gpt ||
               !std::holds_alternative<clonecore::GptDisk>(
                   metadata.target_layout)
           : metadata.style != PartitionTableStyle::mbr ||
               !std::holds_alternative<clonecore::MbrDisk>(
                   metadata.target_layout))) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小レビュー済みレイアウト",
        L"コピー元不変、対象寸法、論理セクター、形式、または最終メタデータが一致しません"));
  }

  std::set<std::uint32_t> target_numbers;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  for (const auto& partition : migration.target_partitions) {
    std::uint64_t end{};
    if (partition.target_number == 0U || partition.size_bytes == 0U ||
        partition.offset_bytes % metadata.logical_sector_size != 0U ||
        partition.size_bytes % metadata.logical_sector_size != 0U ||
        partition.offset_bytes % migration.alignment_bytes != 0U ||
        !checked_add(partition.offset_bytes, partition.size_bytes, end) ||
        end > migration.target_size_bytes ||
        !target_numbers.insert(partition.target_number).second) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小レビュー済み区画",
          L"最終区画の番号、整列、範囲、または一意性が不正です"));
    }
    ranges.emplace_back(partition.offset_bytes, end);
  }
  std::sort(ranges.begin(), ranges.end());
  for (std::size_t index = 1U; index < ranges.size(); ++index) {
    if (ranges[index].first < ranges[index - 1U].second) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小レビュー済み区画重複",
          L"最終区画の配置範囲が重複しています"));
    }
  }

  if (gpt) {
    const auto& disk = std::get<clonecore::GptDisk>(metadata.target_layout);
    if (disk.logical_sector_size != metadata.logical_sector_size ||
        disk.sector_count !=
            metadata.target_size_bytes / metadata.logical_sector_size ||
        disk.partitions.size() != migration.target_partitions.size()) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小レビュー済みGPT",
          L"最終GPTと移行区画の寸法または件数が一致しません"));
    }
    for (const auto& planned : migration.target_partitions) {
      if (planned.target_number > disk.partition_entry_count) {
        return clonecore::Status::failure(service_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小レビュー済みGPT番号",
            L"最終GPT区画番号がentry上限を超えています"));
      }
      const auto found = std::find_if(
          disk.partitions.begin(), disk.partitions.end(),
          [&](const clonecore::GptPartition& partition) {
            return static_cast<std::uint64_t>(partition.entry_index) + 1U ==
                planned.target_number;
          });
      const std::uint64_t first_lba =
          planned.offset_bytes / metadata.logical_sector_size;
      const std::uint64_t sector_count =
          planned.size_bytes / metadata.logical_sector_size;
      if (found == disk.partitions.end() ||
          found->first_lba != first_lba || sector_count == 0U ||
          found->last_lba != first_lba + sector_count - 1U) {
        return clonecore::Status::failure(service_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小レビュー済みGPT対応",
            L"最終GPT区画とレビュー済み移行配置が一致しません"));
      }
    }
  } else {
    const auto& disk = std::get<clonecore::MbrDisk>(metadata.target_layout);
    if (metadata.logical_sector_size != 512U ||
        disk.logical_sector_size != metadata.logical_sector_size ||
        disk.sector_count !=
            metadata.target_size_bytes / metadata.logical_sector_size ||
        disk.partitions.size() != migration.target_partitions.size()) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小レビュー済みMBR",
          L"最終MBRと移行区画の寸法または件数が一致しません"));
    }
    for (const auto& planned : migration.target_partitions) {
      if (planned.target_number > 4U) {
        return clonecore::Status::failure(service_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小レビュー済みMBR番号",
            L"最終MBR区画番号が基本区画上限を超えています"));
      }
      const auto found = std::find_if(
          disk.partitions.begin(), disk.partitions.end(),
          [&](const clonecore::MbrPartition& partition) {
            return static_cast<std::uint32_t>(partition.table_index) + 1U ==
                planned.target_number;
          });
      const std::uint64_t first_lba =
          planned.offset_bytes / metadata.logical_sector_size;
      const std::uint64_t sector_count =
          planned.size_bytes / metadata.logical_sector_size;
      if (found == disk.partitions.end() ||
          found->first_lba != first_lba ||
          found->sector_count != sector_count) {
        return clonecore::Status::failure(service_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小レビュー済みMBR対応",
            L"最終MBR区画とレビュー済み移行配置が一致しません"));
      }
    }
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<TsumugiShrinkPayloadBindingV1>>
make_shrink_payload_bindings(
    const TsumugiManifest& manifest,
    const TsumugiShrinkWholeDiskRestoreLayoutPlanV1& reviewed) {
  const auto canonical = build_tsumugi_manifest_v1(manifest);
  if (!canonical) {
    return clonecore::Result<
        std::vector<TsumugiShrinkPayloadBindingV1>>::failure(
        canonical.error());
  }
  if (manifest.mode != TsumugiManifestMode::shrink) {
    return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小payload binding",
        L"縮小マニフェストだけをレビュー済み縮小レイアウトへ対応付けられます");
  }
  const auto geometry = validate_reviewed_layout_geometry(reviewed);
  if (!geometry) {
    return clonecore::Result<
        std::vector<TsumugiShrinkPayloadBindingV1>>::failure(
        geometry.error());
  }
  const bool target_is_gpt = reviewed.migration.target_style ==
      migrationcore::MigrationPartitionStyle::gpt;
  const bool source_is_gpt = manifest.partition_style ==
      TsumugiManifestPartitionStyle::gpt;
  if ((!source_is_gpt && manifest.partition_style !=
                           TsumugiManifestPartitionStyle::mbr) ||
      (source_is_gpt && !target_is_gpt)) {
    return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小payload形式変換",
        L"レビュー済みレイアウトの形式変換方針がマニフェストと一致しません");
  }

  using Action = migrationcore::MigrationPartitionAction;
  using Role = migrationcore::MigrationPartitionRole;
  const bool generated_esp = generated_partition_exists(
      reviewed.migration, Role::efi_system, Action::create_fat32);
  const bool generated_msr = generated_partition_exists(
      reviewed.migration, Role::microsoft_reserved, Action::create_reserved);
  for (const auto& target : reviewed.migration.target_partitions) {
    if (target.source_table_index.has_value()) {
      continue;
    }
    const bool valid_generated = target_is_gpt &&
        ((target.role == Role::efi_system &&
          target.action == Action::create_fat32) ||
         (target.role == Role::microsoft_reserved &&
          target.action == Action::create_reserved));
    if (!valid_generated) {
      return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小生成区画",
          L"レビュー済みレイアウトに認証済みpayloadへ対応しない任意生成区画があります");
    }
  }

  std::vector<TsumugiShrinkPayloadBindingV1> bindings;
  std::set<std::uint32_t> consumed_target_sources;
  for (const auto& partition : manifest.partitions) {
    if (!partition_flag(
            partition.flags, TsumugiManifestPartitionFlags::selected)) {
      continue;
    }
    if (partition.payload_encoding ==
            TsumugiManifestPayloadEncoding::exact_raw &&
        (source_is_gpt != target_is_gpt ||
         manifest.logical_sector_size !=
             reviewed.metadata.logical_sector_size)) {
      return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi縮小exact RAW binding",
          L"exact RAW payloadを含む画像は形式または論理セクターを変換できません");
    }
    const auto expected_role = migration_role_for(partition.role);
    if (!expected_role.has_value()) {
      return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi縮小payload役割",
          L"選択済みpayloadの役割を縮小レイアウトへ対応付けられません");
    }
    std::vector<const migrationcore::ShrinkPlannedPartition*> matching;
    for (const auto& target : reviewed.migration.target_partitions) {
      if (target.source_table_index == partition.source_table_index) {
        matching.push_back(&target);
      }
    }
    if (matching.size() > 1U) {
      return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Tsumugi縮小payload重複対応",
          L"一つの選択済みpayloadに複数の最終区画が対応しています");
    }

    TsumugiShrinkPayloadBindingV1 binding{
        .source_table_index = partition.source_table_index,
        .source_partition_number = partition.source_partition_number,
    };
    const bool replace_esp = partition.role ==
        TsumugiManifestPartitionRole::efi_system;
    const bool replace_msr = partition.role ==
        TsumugiManifestPartitionRole::microsoft_reserved;
    const bool replace_bios = !source_is_gpt && target_is_gpt &&
        partition.role == TsumugiManifestPartitionRole::bios_system;
    if (replace_esp || replace_msr || replace_bios) {
      const bool authorized = matching.empty() && target_is_gpt &&
          (replace_esp ? source_is_gpt && generated_esp
                       : replace_msr ? source_is_gpt && generated_msr
                                     : generated_esp && generated_msr &&
                                           manifest_flag(
                                               manifest.flags,
                                               TsumugiManifestFlags::
                                                   source_contains_windows));
      if (!authorized) {
        return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"Tsumugi縮小再生成payload",
            L"ESP、MSR、またはBIOSシステムpayloadの省略理由を最終レイアウトで証明できません");
      }
      binding.disposition = replace_esp
          ? TsumugiShrinkPayloadDispositionV1::regenerate_efi_system
          : replace_msr
              ? TsumugiShrinkPayloadDispositionV1::
                    regenerate_microsoft_reserved
              : TsumugiShrinkPayloadDispositionV1::
                    replace_bios_system_with_uefi;
      bindings.push_back(binding);
      continue;
    }

    if (matching.size() != 1U ||
        matching.front()->role != expected_role.value() ||
        matching.front()->size_bytes < partition.minimum_target_bytes) {
      return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Tsumugi縮小payload一対一対応",
          L"選択済みpayloadの役割、最小容量、または一意な最終区画が一致しません");
    }
    const auto& target = *matching.front();
    const bool raw = partition.payload_encoding ==
        TsumugiManifestPayloadEncoding::exact_raw;
    const bool archive = partition.payload_encoding ==
        TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
    const bool restore_action =
        (raw && target.action == Action::copy_exact_raw) ||
        (archive && target.action == Action::apply_file_image);
    const bool recreate_empty = archive && partition.used_bytes == 0U &&
        empty_file_system_action(target.action);
    if ((!restore_action && !recreate_empty) ||
        !consumed_target_sources.insert(partition.source_table_index).second) {
      return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小payload action",
          L"payload形式とレビュー済み最終区画の復元actionが一致しません");
    }
    binding.disposition = recreate_empty
        ? TsumugiShrinkPayloadDispositionV1::recreate_empty_file_system
        : TsumugiShrinkPayloadDispositionV1::restore_to_reviewed_partition;
    binding.target_number = target.target_number;
    binding.target_offset = target.offset_bytes;
    binding.target_size = target.size_bytes;
    bindings.push_back(binding);
  }

  for (const auto& target : reviewed.migration.target_partitions) {
    if (target.source_table_index.has_value() &&
        !consumed_target_sources.contains(
            target.source_table_index.value())) {
      return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Tsumugi縮小最終区画対応",
          L"レビュー済み最終区画に対応する選択済みpayloadがありません");
    }
  }
  if (bindings.empty()) {
    return failure<std::vector<TsumugiShrinkPayloadBindingV1>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_NOT_FOUND,
        L"Tsumugi縮小payload binding",
        L"選択済みpayloadがありません");
  }
  return clonecore::Result<
      std::vector<TsumugiShrinkPayloadBindingV1>>::success(
      std::move(bindings));
}

std::vector<TsumugiRestorePartitionPlacement> canonical_placements(
    const std::span<const TsumugiShrinkPayloadBindingV1> bindings) {
  std::vector<TsumugiRestorePartitionPlacement> result;
  for (const auto& binding : bindings) {
    if (binding.target_number == 0U ||
        (binding.disposition != TsumugiShrinkPayloadDispositionV1::
             restore_to_reviewed_partition &&
         binding.disposition != TsumugiShrinkPayloadDispositionV1::
             recreate_empty_file_system)) {
      continue;
    }
    result.push_back(TsumugiRestorePartitionPlacement{
        .source_table_index = binding.source_table_index,
        .target_offset = binding.target_offset,
        .target_size = binding.target_size,
    });
  }
  return result;
}

bool same_placements(
    std::vector<TsumugiRestorePartitionPlacement> left,
    std::vector<TsumugiRestorePartitionPlacement> right) {
  const auto order = [](const auto& first, const auto& second) {
    return first.source_table_index < second.source_table_index;
  };
  std::sort(left.begin(), left.end(), order);
  std::sort(right.begin(), right.end(), order);
  return left.size() == right.size() &&
      std::equal(
          left.begin(), left.end(), right.begin(),
          [](const auto& first, const auto& second) {
            return first.source_table_index == second.source_table_index &&
                first.target_offset == second.target_offset &&
                first.target_size == second.target_size;
          });
}

clonecore::Status validate_whole_target(
    const TsumugiVerifiedImage& image,
    const TsumugiWholeDiskRestoreTarget& target,
    const TsumugiRestoreHost host) {
  auto status = validate_target_disk(
      target.disk, host, image.manifest.logical_sector_size);
  if (!status) {
    return status;
  }
  if (image.manifest.mode != TsumugiManifestMode::shrink) {
    if (target.reviewed_shrink_layout.has_value() ||
        !target.shrink_placements.empty() ||
        target.disk.disk_size < image.manifest.source_disk_size) {
      return clonecore::Status::failure(service_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"Tsumugi通常／救出のディスク全体復元",
          L"通常・救出画像は元以上の容量と元offsetの維持が必要です"));
    }
    return clonecore::success_status();
  }

  if (!target.reviewed_shrink_layout.has_value() ||
      target.reviewed_shrink_layout->migration.target_size_bytes !=
          target.disk.disk_size ||
      target.reviewed_shrink_layout->metadata.target_size_bytes !=
          target.disk.disk_size ||
      target.reviewed_shrink_layout->metadata.logical_sector_size !=
          target.disk.logical_sector_size) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小レビュー済み最終レイアウト",
        L"対象と一致するOK確認済み最終レイアウトが必要です"));
  }
  const auto bindings = make_shrink_payload_bindings(
      image.manifest, target.reviewed_shrink_layout.value());
  if (!bindings) {
    return clonecore::Status::failure(bindings.error());
  }
  const auto derived = canonical_placements(bindings.value());
  if (!target.shrink_placements.empty() &&
      !same_placements(target.shrink_placements, derived)) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小レビュー済み配置",
        L"任意placementは受理しません。最終レイアウト由来の配置だけを実行できます"));
  }
  return clonecore::success_status();
}

struct IndividualTargetView final {
  const TsumugiRestoreDiskIdentity* disk{};
  std::uint64_t offset{};
  std::uint64_t size{};
};

IndividualTargetView individual_target_view(
    const TsumugiIndividualPartitionRestoreTarget& target) {
  return std::visit(
      [](const auto& selected) {
        return IndividualTargetView{
            .disk = &selected.disk,
            .offset = selected.target_offset,
            .size = selected.target_size,
        };
      },
      target.target);
}

clonecore::Status validate_individual_target(
    const TsumugiVerifiedImage& image,
    const TsumugiIndividualPartitionRestoreTarget& target,
    const TsumugiRestoreHost host) {
  const auto* partition = selected_partition(
      image.manifest, target.source_table_index);
  const auto view = individual_target_view(target);
  if (partition == nullptr || view.disk == nullptr) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_NOT_FOUND,
        L"Tsumugi個別パーティション復元",
        L"画像内の選択済み復元元を確定できません"));
  }
  auto status = validate_target_disk(
      *view.disk, host, image.manifest.logical_sector_size);
  if (!status) {
    return status;
  }
  std::uint64_t end{};
  if (view.size < partition->minimum_target_bytes ||
      view.offset % view.disk->logical_sector_size != 0U ||
      view.size % view.disk->logical_sector_size != 0U ||
      !checked_add(view.offset, view.size, end) ||
      end > view.disk->disk_size) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"Tsumugi個別パーティション復元先",
        L"既存パーティションまたは未割当領域が必要容量・整列・範囲に合格しません"));
  }
  if (const auto* existing =
          std::get_if<TsumugiExistingPartitionRestoreTarget>(&target.target);
      existing != nullptr &&
      (existing->target_table_index == 0U ||
       existing->target_partition_number == 0U)) {
    return clonecore::Status::failure(service_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi既存パーティション識別",
        L"対象のテーブル番号とパーティション番号が必要です"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<std::vector<TsumugiStreamBuildChunk>>
make_tsumugi_rescue_chunks_v1(
    const TsumugiManifest& manifest,
    const clonecore::RescueRawCopyReport& rescue_report,
    const ITsumugiImageSourceSession& verified_staging_session,
    const std::uint32_t chunk_size) {
  const auto canonical_manifest = build_tsumugi_manifest_v1(manifest);
  if (!canonical_manifest) {
    return clonecore::Result<std::vector<TsumugiStreamBuildChunk>>::failure(
        canonical_manifest.error());
  }
  if (manifest.mode != TsumugiManifestMode::rescue ||
      (chunk_size != kImageChunkSize16MiB &&
       chunk_size != kImageChunkSize32MiB)) {
    return failure<std::vector<TsumugiStreamBuildChunk>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi救出チャンク計画",
        L"救出マニフェストと16MiBまたは32MiBのチャンク寸法が必要です");
  }
  if (verified_staging_session.size_bytes() != manifest.source_disk_size ||
      verified_staging_session.logical_sector_size() !=
          manifest.logical_sector_size ||
      verified_staging_session.source_model_hash() !=
          manifest.source_model_hash ||
      verified_staging_session.source_serial_hash() !=
          manifest.source_serial_hash ||
      verified_staging_session.source_state_hash() !=
          manifest.source_state_hash) {
    return failure<std::vector<TsumugiStreamBuildChunk>>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi救出一時Source識別",
        L"読戻し済み救出一時Sourceとマニフェストの寸法または識別Hashが一致しません");
  }

  std::uint64_t settled_bytes{};
  if (!checked_add(
          rescue_report.copied_source_bytes,
          rescue_report.zero_filled_bytes,
          settled_bytes) ||
      rescue_report.source_extent_bytes != manifest.source_disk_size ||
      settled_bytes != manifest.source_disk_size ||
      rescue_report.written_and_read_back_verified_bytes !=
          manifest.source_disk_size ||
      !rescue_report.layout_preserved_without_conversion ||
      !rescue_report.target_flushed ||
      !rescue_report.all_writes_read_back_verified ||
      rescue_report.partial_data_loss !=
          !rescue_report.missing_ranges.empty() ||
      rescue_report.byte_exact_copy !=
          rescue_report.missing_ranges.empty()) {
    return failure<std::vector<TsumugiStreamBuildChunk>>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Tsumugi救出完了証跡",
        L"救出結果が全範囲確定、全書込み読戻し、flush、または専用結果分類を証明していません");
  }

  std::uint64_t missing_bytes{};
  std::uint64_t previous_missing_end{};
  bool first_missing = true;
  for (const auto& missing : rescue_report.missing_ranges) {
    std::uint64_t missing_end{};
    if (missing.bytes.length == 0U ||
        missing.bytes.offset % manifest.logical_sector_size != 0U ||
        missing.bytes.length % manifest.logical_sector_size != 0U ||
        !checked_add(
            missing.bytes.offset, missing.bytes.length, missing_end) ||
        missing_end > manifest.source_disk_size ||
        (!first_missing && missing.bytes.offset < previous_missing_end) ||
        missing.first_lba !=
            missing.bytes.offset / manifest.logical_sector_size ||
        missing.sector_count !=
            missing.bytes.length / manifest.logical_sector_size ||
        missing.forward_attempts == 0U ||
        missing.reverse_attempts == 0U ||
        missing.sector_attempts == 0U ||
        !missing.zero_fill_read_back_verified ||
        !checked_add(missing_bytes, missing.bytes.length, missing_bytes)) {
      return failure<std::vector<TsumugiStreamBuildChunk>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi救出欠損証跡",
          L"欠損範囲、LBA、有限試行、ゼロ埋め読戻し、または並び順が不正です");
    }
    previous_missing_end = missing_end;
    first_missing = false;
  }
  if (missing_bytes != rescue_report.zero_filled_bytes ||
      rescue_report.exhausted_sector_count !=
          missing_bytes / manifest.logical_sector_size) {
    return failure<std::vector<TsumugiStreamBuildChunk>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi救出欠損集計",
        L"欠損Mapの合計と救出結果のゼロ埋め量または枯渇sector数が一致しません");
  }

  std::vector<TsumugiStreamBuildChunk> chunks;
  const auto append_range = [&](
                                std::uint64_t offset,
                                std::uint64_t length,
                                const clonecore::RescueMissingRange* missing)
      -> clonecore::Status {
    while (length != 0U) {
      if (chunks.size() >= kTsumugiMaximumChunkCount) {
        return clonecore::Status::failure(service_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_TOO_MANY_OPEN_FILES,
            L"Tsumugi救出チャンク件数",
            L"救出範囲の分割後チャンク数が形式上限を超えます"));
      }
      const std::uint64_t amount = (std::min)(
          length, static_cast<std::uint64_t>(chunk_size));
      TsumugiStreamBuildChunk chunk{
          .logical_offset = offset,
          .logical_length = amount,
          .source_offset = offset,
          .flags = missing == nullptr
              ? TsumugiChunkFlags::none
              : TsumugiChunkFlags::unreadable_zero_filled,
          .source = missing == nullptr ? &verified_staging_session : nullptr,
      };
      if (missing != nullptr) {
        chunk.rescue_read_evidence = TsumugiRescueReadEvidence{
            .forward_attempts = missing->forward_attempts,
            .reverse_attempts = missing->reverse_attempts,
            .sector_attempts = missing->sector_attempts,
            .zero_fill_read_back_verified =
                missing->zero_fill_read_back_verified,
            .forward_native_error = missing->forward_native_error,
            .reverse_native_error = missing->reverse_native_error,
            .sector_native_error = missing->sector_native_error,
        };
      }
      chunks.push_back(std::move(chunk));
      offset += amount;
      length -= amount;
    }
    return clonecore::success_status();
  };

  std::size_t missing_index = 0U;
  for (const auto& partition : manifest.partitions) {
    if (!partition_flag(
            partition.flags, TsumugiManifestPartitionFlags::selected)) {
      continue;
    }
    std::uint64_t partition_end{};
    if (!checked_add(
            partition.payload_logical_offset,
            partition.payload_logical_length,
            partition_end)) {
      return failure<std::vector<TsumugiStreamBuildChunk>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi救出payload範囲",
          L"選択済みpayload範囲が64bit上限を超えます");
    }
    std::uint64_t current = partition.payload_logical_offset;
    while (missing_index < rescue_report.missing_ranges.size()) {
      const auto& missing = rescue_report.missing_ranges[missing_index];
      std::uint64_t missing_end{};
      static_cast<void>(checked_add(
          missing.bytes.offset, missing.bytes.length, missing_end));
      if (missing_end <= current) {
        return failure<std::vector<TsumugiStreamBuildChunk>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Tsumugi救出欠損所有範囲",
            L"欠損範囲が選択済みパーティション外またはpayload間のgapにあります");
      }
      if (missing.bytes.offset >= partition_end) {
        break;
      }
      if (missing.bytes.offset < current || missing_end > partition_end) {
        return failure<std::vector<TsumugiStreamBuildChunk>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Tsumugi救出欠損所有範囲",
            L"欠損範囲が選択済みパーティション境界を横断しています");
      }
      auto status = append_range(
          current, missing.bytes.offset - current, nullptr);
      if (!status) {
        return clonecore::Result<
            std::vector<TsumugiStreamBuildChunk>>::failure(status.error());
      }
      status = append_range(
          missing.bytes.offset, missing.bytes.length, &missing);
      if (!status) {
        return clonecore::Result<
            std::vector<TsumugiStreamBuildChunk>>::failure(status.error());
      }
      current = missing_end;
      ++missing_index;
    }
    const auto status = append_range(current, partition_end - current, nullptr);
    if (!status) {
      return clonecore::Result<std::vector<TsumugiStreamBuildChunk>>::failure(
          status.error());
    }
  }
  if (missing_index != rescue_report.missing_ranges.size() || chunks.empty()) {
    return failure<std::vector<TsumugiStreamBuildChunk>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi救出欠損所有範囲",
        L"欠損範囲を全選択payloadへ一意に対応付けられません");
  }
  return clonecore::Result<std::vector<TsumugiStreamBuildChunk>>::success(
      std::move(chunks));
}

clonecore::Result<std::vector<TsumugiShrinkPayloadBindingV1>>
make_tsumugi_shrink_payload_bindings_v1(
    const TsumugiManifest& manifest,
    const TsumugiShrinkWholeDiskRestoreLayoutPlanV1& reviewed_layout) {
  return make_shrink_payload_bindings(manifest, reviewed_layout);
}

class TsumugiStagedImageV1::Impl final {
 public:
  Impl(
      TsumugiStagedFileV1&& staged_stream,
      TsumugiImageCreateReport create_report) noexcept
      : stream(std::move(staged_stream)),
        report(std::move(create_report)) {}

  TsumugiStagedFileV1 stream;
  TsumugiImageCreateReport report;
};

bool selected_tsumugi_creation_verification_passed(
    const TsumugiImageCreateReport& report) noexcept {
  if (!report.selected_verification_passed ||
      !report.stream.all_chunks_read_back_verified ||
      !report.stream.all_chunks_authenticated_and_hashed ||
      !report.stream.global_hash_read_back_verified ||
      !report.stream.final_metadata_read_back_verified) {
    return false;
  }
  switch (report.stream.verification_mode) {
    case TsumugiCreateVerificationMode::complete:
      return report.complete_verification_passed &&
          report.stream.final_complete_scan_performed;
    case TsumugiCreateVerificationMode::fast:
      return report.complete_verification_passed ==
          report.stream.final_complete_scan_performed;
  }
  return false;
}

clonecore::Result<TsumugiStagedImageV1> prepare_tsumugi_image_v1(
    const TsumugiImageCreateRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto storage = validate_storage(
      request.storage_file_system, L"Tsumugi画像作成先事前検査");
  if (!storage) {
    return clonecore::Result<TsumugiStagedImageV1>::failure(
        storage.error());
  }
  if (request.source_session == nullptr ||
      request.source_session->size_bytes() !=
          request.manifest.source_disk_size ||
      request.source_session->logical_sector_size() !=
          request.manifest.logical_sector_size ||
      request.source_session->source_model_hash() !=
          request.manifest.source_model_hash ||
      request.source_session->source_serial_hash() !=
          request.manifest.source_serial_hash ||
      request.source_session->source_state_hash() !=
          request.manifest.source_state_hash) {
    return failure<TsumugiStagedImageV1>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi画像作成元セッション",
        L"全チャンクを拘束するSnapshot／ディスク状態とマニフェストの識別情報が一致しません");
  }
  for (const auto& chunk : request.chunks) {
    const bool zero = chunk_is_zero(chunk.flags);
    if ((!zero && chunk.source != request.source_session) ||
        (zero && chunk.source != nullptr)) {
      return failure<TsumugiStagedImageV1>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi画像作成元チャンク拘束",
          L"異なるSnapshot／ディスクReaderの混在、またはゼロ埋めチャンクのSource指定を拒否しました");
    }
  }
  const auto manifest_bytes = build_tsumugi_manifest_v1(request.manifest);
  if (!manifest_bytes) {
    return clonecore::Result<TsumugiStagedImageV1>::failure(
        manifest_bytes.error());
  }
  const auto ranges = logical_ranges(request.chunks);
  const auto coverage = validate_payload_coverage(request.manifest, ranges);
  if (!coverage) {
    return clonecore::Result<TsumugiStagedImageV1>::failure(
        coverage.error());
  }

  const auto image_id = generate_tsumugi_salt();
  if (!image_id) {
    return clonecore::Result<TsumugiStagedImageV1>::failure(
        image_id.error());
  }
  bool weak = false;
  std::optional<TsumugiEncryptionSettings> encryption;
  if (request.encryption.has_value()) {
    const auto assessment =
        assess_tsumugi_password(request.encryption->password);
    if (!assessment.accepted) {
      return failure<TsumugiStagedImageV1>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PASSWORD,
          L"Tsumugi画像暗号パスワード",
          L"ASCII印字文8文字以上が必要です");
    }
    weak = assessment.weak;
    const auto salt = generate_tsumugi_salt();
    const auto nonce = generate_tsumugi_nonce();
    if (!salt || !nonce) {
      return clonecore::Result<TsumugiStagedImageV1>::failure(
          salt ? nonce.error() : salt.error());
    }
    encryption = TsumugiEncryptionSettings{
        .password = request.encryption->password,
        .argon2 = TsumugiArgon2Parameters{
            .salt = salt.value(),
        },
        .base_nonce = nonce.value(),
    };
  }

  auto written = prepare_verified_tsumugi_file_v1(
      TsumugiStreamBuildRequest{
          .final_path = request.final_path,
          .payload_kind = payload_kind(request.manifest.mode),
          .source_disk_size = request.manifest.source_disk_size,
          .logical_sector_size = request.manifest.logical_sector_size,
          .physical_sector_size = request.manifest.physical_sector_size,
          .chunk_size = request.chunk_size,
          .compression = request.compression,
          .verification_block_bytes = request.verification_block_bytes,
          .verification_mode = request.verification_mode,
          .image_id = image_id.value(),
          .manifest = manifest_bytes.value(),
          .chunks = request.chunks,
          .encryption = encryption,
          .replace_existing = request.replace_existing,
      },
      callbacks);
  if (!written) {
    return clonecore::Result<TsumugiStagedImageV1>::failure(
        written.error());
  }
  const bool selected_verification_passed =
      written.value().report().verification_mode == request.verification_mode &&
      written.value().report().all_chunks_read_back_verified &&
      written.value().report().all_chunks_authenticated_and_hashed &&
      written.value().report().global_hash_read_back_verified &&
      written.value().report().final_metadata_read_back_verified &&
      (request.verification_mode ==
               TsumugiCreateVerificationMode::fast ||
       written.value().report().final_complete_scan_performed);
  if (!selected_verification_passed) {
    return failure<TsumugiStagedImageV1>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"Tsumugi作成時検証結果",
        L"選択した作成時検証モードの必須証跡が揃っていません");
  }
  TsumugiImageCreateReport report{
      .stream = written.value().report(),
      .unreadable_ranges = unreadable_ranges(ranges),
      .encrypted = encryption.has_value(),
      .password_was_weak = weak,
      .selected_verification_passed = true,
      .complete_verification_passed =
          written.value().report().final_complete_scan_performed,
  };
  auto impl = std::make_unique<TsumugiStagedImageV1::Impl>(
      written.take_value(), std::move(report));
  return clonecore::Result<TsumugiStagedImageV1>::success(
      TsumugiStagedImageV1(std::move(impl)));
}

TsumugiStagedImageV1::TsumugiStagedImageV1(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

TsumugiStagedImageV1::~TsumugiStagedImageV1() {
  if (pending()) {
    static_cast<void>(abort_incomplete());
  }
}

TsumugiStagedImageV1::TsumugiStagedImageV1(
    TsumugiStagedImageV1&&) noexcept = default;

TsumugiStagedImageV1& TsumugiStagedImageV1::operator=(
    TsumugiStagedImageV1&&) noexcept = default;

const TsumugiImageCreateReport&
TsumugiStagedImageV1::report() const noexcept {
  static const TsumugiImageCreateReport empty{};
  return impl_ ? impl_->report : empty;
}

bool TsumugiStagedImageV1::pending() const noexcept {
  return impl_ && impl_->stream.pending();
}

clonecore::Result<TsumugiImageCreateReport>
TsumugiStagedImageV1::commit_verified() {
  if (!pending()) {
    return failure<TsumugiImageCreateReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi画像確定",
        L"確定または破棄済みの画像は再実行できません");
  }
  auto committed = impl_->stream.commit_verified();
  if (!committed) {
    return clonecore::Result<TsumugiImageCreateReport>::failure(
        committed.error());
  }
  impl_->report.stream = committed.take_value();
  return clonecore::Result<TsumugiImageCreateReport>::success(
      impl_->report);
}

clonecore::Status TsumugiStagedImageV1::abort_incomplete() noexcept {
  if (!impl_) {
    return clonecore::success_status();
  }
  return impl_->stream.abort_incomplete();
}

clonecore::Result<TsumugiImageCreateReport> create_tsumugi_image_v1(
    const TsumugiImageCreateRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto staged = prepare_tsumugi_image_v1(request, callbacks);
  if (!staged) {
    return clonecore::Result<TsumugiImageCreateReport>::failure(
        staged.error());
  }
  return staged.value().commit_verified();
}

clonecore::Result<TsumugiRescueImageCreateReport>
create_tsumugi_rescue_image_v1(
    const TsumugiRescueImageCreateRequest& request,
    const clonecore::DiskOperationCallbacks& image_callbacks) {
  if (request.failing_source == nullptr || request.staging == nullptr ||
      request.image.manifest.mode != TsumugiManifestMode::rescue ||
      !request.image.chunks.empty() ||
      request.image.source_session != nullptr ||
      request.staging->sealed_for_image_read()) {
    return failure<TsumugiRescueImageCreateReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi救出画像作成要求",
        L"救出Source、未封印の所有一時領域、救出マニフェスト、および空の派生フィールドが必要です");
  }
  const auto storage = validate_storage(
      request.image.storage_file_system, L"Tsumugi救出画像保存先事前検査");
  if (!storage) {
    return clonecore::Result<TsumugiRescueImageCreateReport>::failure(
        storage.error());
  }
  const auto canonical_manifest =
      build_tsumugi_manifest_v1(request.image.manifest);
  if (!canonical_manifest) {
    return clonecore::Result<TsumugiRescueImageCreateReport>::failure(
        canonical_manifest.error());
  }
  if (request.failing_source->size_bytes() !=
          request.image.manifest.source_disk_size ||
      request.failing_source->logical_sector_size() !=
          request.image.manifest.logical_sector_size ||
      request.staging->size_bytes() !=
          request.image.manifest.source_disk_size ||
      request.staging->logical_sector_size() !=
          request.image.manifest.logical_sector_size ||
      request.staging->source_model_hash() !=
          request.image.manifest.source_model_hash ||
      request.staging->source_serial_hash() !=
          request.image.manifest.source_serial_hash ||
      request.staging->source_state_hash() !=
          request.image.manifest.source_state_hash) {
    return failure<TsumugiRescueImageCreateReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi救出画像Source識別",
        L"故障Source、一時領域、およびマニフェストの寸法・sector・識別Hashが一致しません");
  }

  const auto fail_after_discard = [&](clonecore::Error primary) {
    const auto discarded = request.staging->discard_owned_staging();
    if (!discarded) {
      return failure<TsumugiRescueImageCreateReport>(
          discarded.error().code,
          discarded.error().native_code,
          L"Tsumugi救出一時領域破棄",
          L"処理失敗後に所有一時領域を破棄できませんでした: " +
              primary.message + L" / " + discarded.error().message);
    }
    return clonecore::Result<TsumugiRescueImageCreateReport>::failure(
        std::move(primary));
  };

  auto rescued = clonecore::execute_rescue_raw_copy(
      request.rescue_copy, *request.failing_source, *request.staging);
  if (!rescued) {
    return fail_after_discard(rescued.error());
  }
  clonecore::RescueRawCopyReport rescue_report = rescued.take_value();

  const auto sealed = request.staging->seal_for_image_read();
  if (!sealed) {
    return fail_after_discard(sealed.error());
  }
  if (!request.staging->sealed_for_image_read()) {
    return fail_after_discard(service_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        L"Tsumugi救出一時領域封印",
        L"一時領域を読取り専用として再識別できませんでした"));
  }

  auto chunks = make_tsumugi_rescue_chunks_v1(
      request.image.manifest,
      rescue_report,
      *request.staging,
      request.image.chunk_size);
  if (!chunks) {
    return fail_after_discard(chunks.error());
  }
  TsumugiImageCreateRequest image_request = request.image;
  image_request.chunks = chunks.take_value();
  image_request.source_session = request.staging;
  auto staged_image = prepare_tsumugi_image_v1(
      image_request, image_callbacks);
  if (!staged_image) {
    return fail_after_discard(staged_image.error());
  }

  const auto discarded = request.staging->discard_owned_staging();
  if (!discarded) {
    const auto aborted = staged_image.value().abort_incomplete();
    return failure<TsumugiRescueImageCreateReport>(
        discarded.error().code,
        discarded.error().native_code,
        L"Tsumugi救出一時領域の完成前破棄",
        aborted
            ? L"完成名を公開せず、所有一時領域の破棄失敗を報告します: " +
                discarded.error().message
            : L"所有一時領域と画像.partialの両方を破棄できませんでした: " +
                discarded.error().message + L" / " + aborted.error().message);
  }

  const auto destination_revalidated =
      request.staging->validate_image_destination_before_commit(
          staged_image.value().report().stream.image_length);
  if (!destination_revalidated) {
    const auto aborted = staged_image.value().abort_incomplete();
    return failure<TsumugiRescueImageCreateReport>(
        destination_revalidated.error().code,
        destination_revalidated.error().native_code,
        L"Tsumugi救出画像完成前保存先再識別",
        aborted
            ? L"完成名を公開せず、保存先再識別失敗を報告します: " +
                destination_revalidated.error().message
            : L"保存先再識別後に画像.partialも破棄できませんでした: " +
                destination_revalidated.error().message + L" / " +
                aborted.error().message);
  }

  auto committed = staged_image.value().commit_verified();
  if (!committed) {
    return clonecore::Result<TsumugiRescueImageCreateReport>::failure(
        committed.error());
  }
  return clonecore::Result<TsumugiRescueImageCreateReport>::success(
      TsumugiRescueImageCreateReport{
          .rescue = std::move(rescue_report),
          .image = committed.take_value(),
          .staging_sealed_for_image_read = true,
          .staging_discarded_before_final_commit = true,
          .staging_destination_revalidated_before_final_commit = true,
      });
}

clonecore::Result<TsumugiVerifiedImage> verify_tsumugi_image_v1(
    const TsumugiImageVerifyRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto storage = validate_storage(
      request.storage_file_system, L"Tsumugi画像検証元事前検査");
  if (!storage) {
    return clonecore::Result<TsumugiVerifiedImage>::failure(
        storage.error());
  }
  auto container = verify_tsumugi_file_v1(
      TsumugiStreamVerifyRequest{
          .image_path = request.image_path,
          .password = request.password,
          .verification_block_bytes = request.verification_block_bytes,
      },
      callbacks);
  if (!container) {
    return clonecore::Result<TsumugiVerifiedImage>::failure(
        container.error());
  }
  auto manifest = inspect_tsumugi_manifest_v1(container.value().manifest);
  if (!manifest) {
    return clonecore::Result<TsumugiVerifiedImage>::failure(
        manifest.error());
  }
  const auto cross = validate_inspection(container.value(), manifest.value());
  if (!cross) {
    return clonecore::Result<TsumugiVerifiedImage>::failure(cross.error());
  }
  const auto ranges = logical_ranges(container.value().records);
  auto missing = unreadable_ranges(ranges);
  const bool partial_loss = !missing.empty();
  return clonecore::Result<TsumugiVerifiedImage>::success({
      .container = container.take_value(),
      .manifest = manifest.take_value(),
      .unreadable_ranges = std::move(missing),
      .partial_loss = partial_loss,
  });
}

const TsumugiVerifiedImage& TsumugiRestorePlan::image() const noexcept {
  return image_;
}

const TsumugiRestoreTarget& TsumugiRestorePlan::target() const noexcept {
  return target_;
}

TsumugiRestoreHost TsumugiRestorePlan::host() const noexcept {
  return host_;
}

bool TsumugiRestorePlan::is_whole_disk_restore() const noexcept {
  return std::holds_alternative<TsumugiWholeDiskRestoreTarget>(target_);
}

bool TsumugiRestorePlan::requires_boot_repair_offer() const noexcept {
  return requires_boot_repair_offer_;
}

bool TsumugiRestorePlan::has_partial_loss() const noexcept {
  return image_.partial_loss;
}

std::uint64_t TsumugiRestorePlan::planned_payload_bytes() const noexcept {
  return planned_payload_bytes_;
}

std::span<const TsumugiShrinkPayloadBindingV1>
TsumugiRestorePlan::shrink_payload_bindings() const noexcept {
  return shrink_payload_bindings_;
}

clonecore::Result<TsumugiRestorePlan> prepare_tsumugi_restore_plan_v1(
    const TsumugiRestorePlanRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks) {
  auto image = verify_tsumugi_image_v1(request.image, callbacks);
  if (!image) {
    return clonecore::Result<TsumugiRestorePlan>::failure(image.error());
  }
  clonecore::Status target_status = clonecore::success_status();
  bool boot_offer = false;
  const TsumugiManifestPartition* selected_for_byte_count = nullptr;
  if (const auto* whole =
          std::get_if<TsumugiWholeDiskRestoreTarget>(&request.target);
      whole != nullptr) {
    target_status = validate_whole_target(
        image.value(), *whole, request.host);
  } else {
    const auto& individual =
        std::get<TsumugiIndividualPartitionRestoreTarget>(request.target);
    target_status = validate_individual_target(
        image.value(), individual, request.host);
    const auto* partition = selected_partition(
        image.value().manifest, individual.source_table_index);
    selected_for_byte_count = partition;
    boot_offer = partition != nullptr && partition_flag(
        partition->flags,
        TsumugiManifestPartitionFlags::contains_windows);
  }
  if (!target_status) {
    return clonecore::Result<TsumugiRestorePlan>::failure(
        target_status.error());
  }
  const auto bytes = planned_payload_bytes(
      image.value().container, selected_for_byte_count);
  if (!bytes) {
    return clonecore::Result<TsumugiRestorePlan>::failure(bytes.error());
  }
  TsumugiRestorePlan plan;
  plan.image_path_ = request.image.image_path;
  plan.storage_file_system_ = request.image.storage_file_system;
  plan.verification_block_bytes_ = request.image.verification_block_bytes;
  plan.host_ = request.host;
  plan.target_ = request.target;
  plan.image_ = image.take_value();
  if (const auto* whole =
          std::get_if<TsumugiWholeDiskRestoreTarget>(&plan.target_);
      whole != nullptr &&
      plan.image_.manifest.mode == TsumugiManifestMode::shrink) {
    const auto bindings = make_shrink_payload_bindings(
        plan.image_.manifest, whole->reviewed_shrink_layout.value());
    if (!bindings) {
      return clonecore::Result<TsumugiRestorePlan>::failure(
          bindings.error());
    }
    plan.shrink_payload_bindings_ = bindings.value();
    std::get<TsumugiWholeDiskRestoreTarget>(plan.target_)
        .shrink_placements = canonical_placements(
            plan.shrink_payload_bindings_);
  }
  plan.requires_boot_repair_offer_ = boot_offer;
  plan.planned_payload_bytes_ = bytes.value();
  return clonecore::Result<TsumugiRestorePlan>::success(std::move(plan));
}

clonecore::Result<TsumugiRestoreReport> execute_tsumugi_restore_plan_v1(
    TsumugiRestorePlan& plan,
    const std::optional<std::string_view> password,
    ITsumugiRestoreTransaction& transaction,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (tsumugi_manifest_requires_shrink_archive_adapter(
          plan.image_.manifest)) {
    return failure<TsumugiRestoreReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小WIM復元Adapter",
        L"縮小WIM payloadは通常のブロック復元へ渡せません。専用の検証済み展開Adapterが未接続です");
  }
  if (plan.consumed_ == nullptr || plan.consumed_->exchange(true)) {
    return failure<TsumugiRestoreReport>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_ALREADY_EXISTS,
        L"Tsumugi復元計画の単回実行",
        L"この復元計画は既に実行済みまたは無効です。画像と対象を改めて確認してください");
  }
  const auto& disk = target_disk(plan.target_);
  RestoreTransactionGuard transaction_guard(transaction);
  bool plan_matched = false;
  bool target_reidentified = false;
  std::uint64_t written_bytes = 0U;
  std::uint64_t written_chunks = 0U;
  std::uint64_t omitted_bytes = 0U;
  std::uint64_t omitted_chunks = 0U;
  std::set<std::uint32_t> regenerated_sources;
  auto restored = read_verified_tsumugi_file_v1(
      TsumugiStreamVerifyRequest{
          .image_path = plan.image_path_,
          .password = password,
          .verification_block_bytes = plan.verification_block_bytes_,
      },
      [&](const TsumugiChunkRecord& record,
          const std::span<const std::byte> plaintext) {
        const TsumugiManifestPartition* owner = nullptr;
        std::uint64_t record_end{};
        if (!checked_add(
                record.logical_offset, record.logical_length, record_end)) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"Tsumugi復元チャンク範囲",
              L"復元チャンク範囲がオーバーフローします"));
        }
        for (const auto& partition : plan.image_.manifest.partitions) {
          if (!partition_flag(
                  partition.flags,
                  TsumugiManifestPartitionFlags::selected)) {
            continue;
          }
          std::uint64_t end{};
          if (!checked_add(
                  partition.payload_logical_offset,
                  partition.payload_logical_length,
                  end)) {
            return clonecore::Status::failure(service_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_ARITHMETIC_OVERFLOW,
                L"Tsumugi復元パーティション範囲",
                L"認証済みパーティション範囲がオーバーフローします"));
          }
          if (record.logical_offset >= partition.payload_logical_offset &&
              record_end <= end) {
            owner = &partition;
            break;
          }
        }
        if (owner == nullptr) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Tsumugi復元チャンク対応",
              L"認証済みチャンクを選択済みパーティションに対応付けられません"));
        }

        std::uint64_t target_offset{};
        if (const auto* whole =
                std::get_if<TsumugiWholeDiskRestoreTarget>(&plan.target_);
            whole != nullptr) {
          if (plan.image_.manifest.mode == TsumugiManifestMode::shrink) {
            const auto binding = std::find_if(
                plan.shrink_payload_bindings_.begin(),
                plan.shrink_payload_bindings_.end(),
                [&](const TsumugiShrinkPayloadBindingV1& candidate) {
                  return candidate.source_table_index ==
                      owner->source_table_index;
                });
            if (binding == plan.shrink_payload_bindings_.end()) {
              return clonecore::Status::failure(service_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_NOT_FOUND,
                  L"Tsumugi縮小binding実行照合",
                  L"認証済みpayloadのレビュー済みbindingがありません"));
            }
            if (binding->disposition !=
                TsumugiShrinkPayloadDispositionV1::
                    restore_to_reviewed_partition) {
              if (!checked_add(
                      omitted_bytes,
                      record.logical_length,
                      omitted_bytes)) {
                return clonecore::Status::failure(service_error(
                    clonecore::ErrorCode::invalid_data,
                    ERROR_ARITHMETIC_OVERFLOW,
                    L"Tsumugi縮小再生成payload合計",
                    L"再生成するpayload容量がオーバーフローします"));
              }
              ++omitted_chunks;
              regenerated_sources.insert(owner->source_table_index);
              return clonecore::success_status();
            }
            const auto relative =
                record.logical_offset - owner->payload_logical_offset;
            std::uint64_t placement_end{};
            std::uint64_t write_end{};
            if (!checked_add(
                    binding->target_offset,
                    binding->target_size,
                    placement_end) ||
                !checked_add(
                    binding->target_offset, relative, target_offset) ||
                !checked_add(
                    target_offset, record.logical_length, write_end) ||
                write_end > placement_end) {
              return clonecore::Status::failure(service_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_DISK_FULL,
                  L"Tsumugi縮小配置実行範囲",
                  L"認証済みチャンクが検証済み配置範囲を超えます"));
            }
          } else {
            target_offset = record.logical_offset;
          }
        } else {
          const auto& individual =
              std::get<TsumugiIndividualPartitionRestoreTarget>(
                  plan.target_);
          if (individual.source_table_index != owner->source_table_index) {
            return clonecore::success_status();
          }
          const auto view = individual_target_view(individual);
          const auto relative =
              record.logical_offset - owner->payload_logical_offset;
          std::uint64_t target_end{};
          std::uint64_t write_end{};
          if (!checked_add(view.offset, view.size, target_end) ||
              !checked_add(view.offset, relative, target_offset) ||
              !checked_add(
                  target_offset, record.logical_length, write_end) ||
              write_end > target_end) {
            return clonecore::Status::failure(service_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DISK_FULL,
                L"Tsumugi個別復元実行範囲",
                L"認証済みチャンクが検証済み復元先範囲を超えます"));
          }
        }

        auto delivered = transaction.write_and_verify(
            TsumugiRestoreWrite{
                .stable_target_identity_hash =
                    disk.stable_identity_hash,
                .source_table_index = owner->source_table_index,
                .source_partition_number =
                    owner->source_partition_number,
                .source_payload_offset = record.logical_offset,
                .target_offset = target_offset,
                .length = record.logical_length,
                .zero_fill = chunk_is_zero(record.flags),
                .unreadable_zero_fill =
                    chunk_is_unreadable(record.flags),
            },
            plaintext);
        if (!delivered) {
          return delivered;
        }
        if (!checked_add(
                written_bytes, record.logical_length, written_bytes)) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"Tsumugi復元済み容量",
              L"復元済み容量がオーバーフローします"));
        }
        ++written_chunks;
        return clonecore::success_status();
      },
      callbacks,
      [&](const TsumugiStreamInspection& inspection) {
        if (!same_inspection(inspection, plan.image_.container)) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Tsumugi検討済み復元計画の同一ハンドル照合",
              L"実行時の認証済み画像が事前確認した画像と一致しません"));
        }
        auto current = transaction.begin(
            plan.image_, plan.target_, plan.host_);
        if (!current) {
          return clonecore::Status::failure(current.error());
        }
        transaction_guard.mark_begun();
        if (!same_target_identity(disk, current.value())) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"Tsumugi復元先の実行直前再識別",
              L"復元先の安定識別、容量、セクタ、または稼働中Windows状態が計画時から変わりました"));
        }
        plan_matched = true;
        target_reidentified = true;
        return clonecore::success_status();
      });
  if (!restored) {
    return clonecore::Result<TsumugiRestoreReport>::failure(
        restored.error());
  }
  std::uint64_t accounted_bytes{};
  const auto expected_regenerated = static_cast<std::size_t>(std::count_if(
      plan.shrink_payload_bindings_.begin(),
      plan.shrink_payload_bindings_.end(),
      [](const TsumugiShrinkPayloadBindingV1& binding) {
        return binding.disposition !=
            TsumugiShrinkPayloadDispositionV1::
                restore_to_reviewed_partition;
      }));
  if (!checked_add(written_bytes, omitted_bytes, accounted_bytes) ||
      accounted_bytes != plan.planned_payload_bytes_ ||
      regenerated_sources.size() != expected_regenerated) {
    return failure<TsumugiRestoreReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"Tsumugi復元payload完了照合",
        L"書込み済みと明示再生成済みのpayload合計が検討済み計画と一致しません");
  }
  const auto committed = transaction.commit();
  if (!committed) {
    return clonecore::Result<TsumugiRestoreReport>::failure(
        committed.error());
  }
  transaction_guard.mark_committed();
  return clonecore::Result<TsumugiRestoreReport>::success({
      .written_logical_bytes = written_bytes,
      .written_chunk_count = written_chunks,
      .intentionally_omitted_logical_bytes = omitted_bytes,
      .intentionally_omitted_chunk_count = omitted_chunks,
      .intentionally_regenerated_partitions = regenerated_sources.size(),
      .callbacks_started_after_complete_verification =
          restored.value().callbacks_started_after_complete_verification,
      .image_matched_prepared_plan = plan_matched,
      .target_reidentified_before_write = target_reidentified,
      .all_writes_read_back_verified = true,
      .final_layout_committed = true,
      .partial_loss = plan.image_.partial_loss,
  });
}

clonecore::Result<TsumugiShrinkRestoreReport>
execute_tsumugi_shrink_restore_plan_v1(
    TsumugiRestorePlan& plan,
    const std::optional<std::string_view> password,
    ITsumugiShrinkRestoreTransaction& transaction,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (plan.image_.manifest.mode != TsumugiManifestMode::shrink ||
      !tsumugi_manifest_requires_shrink_archive_adapter(
          plan.image_.manifest)) {
    return failure<TsumugiShrinkRestoreReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小WIM復元契約",
        L"この専用実行経路は単一WIM payloadを含む縮小画像だけを受け付けます");
  }
  if (plan.consumed_ == nullptr || plan.consumed_->exchange(true)) {
    return failure<TsumugiShrinkRestoreReport>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_ALREADY_EXISTS,
        L"Tsumugi縮小復元計画の単回実行",
        L"この縮小復元計画は既に実行済みまたは無効です");
  }

  const auto& disk = target_disk(plan.target_);
  ShrinkRestoreTransactionGuard transaction_guard(transaction);
  bool plan_matched = false;
  bool target_reidentified = false;
  std::optional<std::uint32_t> active_archive;
  std::uint64_t next_archive_offset{};
  std::uint64_t archive_bytes{};
  std::uint64_t archive_chunks{};
  std::uint64_t raw_bytes{};
  std::uint64_t raw_chunks{};
  std::uint64_t completed_archives{};
  std::uint64_t omitted_bytes{};
  std::uint64_t omitted_chunks{};
  std::set<std::uint32_t> regenerated_sources;
  std::set<std::uint32_t> verified_empty_sources;

  const auto target_view_for = [&](const std::uint32_t source_table_index)
      -> clonecore::Result<IndividualTargetView> {
    if (const auto* whole =
            std::get_if<TsumugiWholeDiskRestoreTarget>(&plan.target_);
        whole != nullptr) {
      const auto binding = std::find_if(
          plan.shrink_payload_bindings_.begin(),
          plan.shrink_payload_bindings_.end(),
          [source_table_index](
              const TsumugiShrinkPayloadBindingV1& candidate) {
            return candidate.source_table_index == source_table_index;
          });
      if (binding == plan.shrink_payload_bindings_.end() ||
          binding->disposition !=
              TsumugiShrinkPayloadDispositionV1::
                  restore_to_reviewed_partition) {
        return failure<IndividualTargetView>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_NOT_FOUND,
            L"Tsumugi縮小WIM binding実行照合",
            L"認証済みpayloadの書込み先bindingがレビュー済み計画にありません");
      }
      return clonecore::Result<IndividualTargetView>::success({
          .disk = &whole->disk,
          .offset = binding->target_offset,
          .size = binding->target_size,
      });
    }
    const auto& individual =
        std::get<TsumugiIndividualPartitionRestoreTarget>(plan.target_);
    if (individual.source_table_index != source_table_index) {
      return failure<IndividualTargetView>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_NOT_FOUND,
          L"Tsumugi縮小個別復元元照合",
          L"個別復元で選択していないpayloadです");
    }
    return clonecore::Result<IndividualTargetView>::success(
        individual_target_view(individual));
  };

  auto restored = read_verified_tsumugi_file_v1(
      TsumugiStreamVerifyRequest{
          .image_path = plan.image_path_,
          .password = password,
          .verification_block_bytes = plan.verification_block_bytes_,
      },
      [&](const TsumugiChunkRecord& record,
          const std::span<const std::byte> plaintext) {
        std::uint64_t record_end{};
        if (!checked_add(
                record.logical_offset, record.logical_length, record_end)) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"Tsumugi縮小payload範囲",
              L"認証済みpayload範囲がオーバーフローします"));
        }
        const TsumugiManifestPartition* owner = nullptr;
        for (const auto& partition : plan.image_.manifest.partitions) {
          if (!partition_flag(
                  partition.flags,
                  TsumugiManifestPartitionFlags::selected)) {
            continue;
          }
          std::uint64_t partition_end{};
          if (!checked_add(
                  partition.payload_logical_offset,
                  partition.payload_logical_length,
                  partition_end)) {
            return clonecore::Status::failure(service_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_ARITHMETIC_OVERFLOW,
                L"Tsumugi縮小パーティションpayload範囲",
                L"認証済みパーティションpayload範囲がオーバーフローします"));
          }
          if (record.logical_offset >= partition.payload_logical_offset &&
              record_end <= partition_end) {
            owner = &partition;
            break;
          }
        }
        if (owner == nullptr) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小payload対応",
              L"認証済みチャンクを選択済みパーティションに対応付けられません"));
        }

        if (const auto* individual =
                std::get_if<TsumugiIndividualPartitionRestoreTarget>(
                    &plan.target_);
            individual != nullptr &&
            individual->source_table_index != owner->source_table_index) {
          return clonecore::success_status();
        }
        if (std::holds_alternative<TsumugiWholeDiskRestoreTarget>(
                plan.target_)) {
          const auto binding = std::find_if(
              plan.shrink_payload_bindings_.begin(),
              plan.shrink_payload_bindings_.end(),
              [&](const TsumugiShrinkPayloadBindingV1& candidate) {
                return candidate.source_table_index ==
                    owner->source_table_index;
              });
          if (binding == plan.shrink_payload_bindings_.end()) {
            return clonecore::Status::failure(service_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_NOT_FOUND,
                L"Tsumugi縮小payload binding",
                L"選択済みpayloadのレビュー済みbindingがありません"));
          }
          if (binding->disposition !=
              TsumugiShrinkPayloadDispositionV1::
                  restore_to_reviewed_partition) {
            if (active_archive.has_value() ||
                !checked_add(
                    omitted_bytes,
                    record.logical_length,
                    omitted_bytes)) {
              return clonecore::Status::failure(service_error(
                  clonecore::ErrorCode::invalid_data,
                  active_archive.has_value()
                      ? ERROR_INVALID_DATA
                      : ERROR_ARITHMETIC_OVERFLOW,
                  L"Tsumugi縮小再生成payload",
                  active_archive.has_value()
                      ? L"WIM payloadの途中で再生成payloadへ切り替わりました"
                      : L"再生成するpayload容量がオーバーフローします"));
            }
            ++omitted_chunks;
            regenerated_sources.insert(owner->source_table_index);
            return clonecore::success_status();
          }
        }
        const auto view = target_view_for(owner->source_table_index);
        if (!view) {
          return clonecore::Status::failure(view.error());
        }
        const std::uint64_t relative =
            record.logical_offset - owner->payload_logical_offset;

        if (owner->payload_encoding ==
            TsumugiManifestPayloadEncoding::exact_raw) {
          if (active_archive.has_value()) {
            return clonecore::Status::failure(service_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_INVALID_DATA,
                L"Tsumugi縮小payload順序",
                L"WIM payloadの途中にRAW payloadが混在しています"));
          }
          std::uint64_t target_offset{};
          std::uint64_t target_end{};
          std::uint64_t write_end{};
          if (!checked_add(view.value().offset, view.value().size, target_end) ||
              !checked_add(view.value().offset, relative, target_offset) ||
              !checked_add(target_offset, record.logical_length, write_end) ||
              write_end > target_end) {
            return clonecore::Status::failure(service_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DISK_FULL,
                L"Tsumugi縮小RAW配置範囲",
                L"exact RAW payloadが検討済み配置範囲を超えます"));
          }
          auto written = transaction.write_exact_raw_and_verify(
              TsumugiRestoreWrite{
                  .stable_target_identity_hash = disk.stable_identity_hash,
                  .source_table_index = owner->source_table_index,
                  .source_partition_number = owner->source_partition_number,
                  .source_payload_offset = record.logical_offset,
                  .target_offset = target_offset,
                  .length = record.logical_length,
                  .zero_fill = chunk_is_zero(record.flags),
                  .unreadable_zero_fill = false,
              },
              plaintext);
          if (!written) {
            return written;
          }
          if (!checked_add(raw_bytes, record.logical_length, raw_bytes)) {
            return clonecore::Status::failure(service_error(
                clonecore::ErrorCode::invalid_data,
                ERROR_ARITHMETIC_OVERFLOW,
                L"Tsumugi縮小RAW復元済み容量",
                L"RAW復元済み容量がオーバーフローします"));
          }
          ++raw_chunks;
          return clonecore::success_status();
        }

        if (!active_archive.has_value()) {
          auto begun = transaction.begin_wim_archive(
              TsumugiShrinkArchiveTarget{
                  .stable_target_identity_hash = disk.stable_identity_hash,
                  .source_table_index = owner->source_table_index,
                  .source_partition_number = owner->source_partition_number,
                  .file_system = owner->file_system,
                  .payload_format_version = owner->payload_format_version,
                  .cluster_size = owner->cluster_size,
                  .target_offset = view.value().offset,
                  .target_size = view.value().size,
                  .archive_length = owner->payload_logical_length,
              });
          if (!begun) {
            return begun;
          }
          active_archive = owner->source_table_index;
          next_archive_offset = 0U;
        }
        if (*active_archive != owner->source_table_index ||
            relative != next_archive_offset ||
            chunk_is_unreadable(record.flags)) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小WIM連続性",
              L"WIM payloadが非連続、混在、または欠損指定です"));
        }
        auto appended = transaction.append_wim_archive(
            TsumugiShrinkArchiveChunk{
                .source_table_index = owner->source_table_index,
                .source_payload_offset = record.logical_offset,
                .archive_offset = relative,
                .length = record.logical_length,
                .zero_fill = chunk_is_zero(record.flags),
            },
            plaintext);
        if (!appended) {
          return appended;
        }
        if (!checked_add(
                next_archive_offset,
                record.logical_length,
                next_archive_offset) ||
            !checked_add(
                archive_bytes, record.logical_length, archive_bytes)) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"Tsumugi縮小WIM復元済み容量",
              L"WIM復元済み容量がオーバーフローします"));
        }
        ++archive_chunks;
        if (next_archive_offset == owner->payload_logical_length) {
          auto completed = transaction.complete_wim_archive_and_verify(
              owner->source_table_index);
          if (!completed) {
            return completed;
          }
          active_archive.reset();
          next_archive_offset = 0U;
          ++completed_archives;
        }
        return clonecore::success_status();
      },
      callbacks,
      [&](const TsumugiStreamInspection& inspection) {
        if (!same_inspection(inspection, plan.image_.container)) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Tsumugi縮小復元計画の同一ハンドル照合",
              L"実行時の認証済み画像が事前確認した縮小画像と一致しません"));
        }
        auto current = transaction.begin(
            plan.image_, plan.target_, plan.host_);
        if (!current) {
          return clonecore::Status::failure(current.error());
        }
        transaction_guard.mark_begun();
        if (!same_target_identity(disk, current.value())) {
          return clonecore::Status::failure(service_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"Tsumugi縮小復元先の実行直前再識別",
              L"復元先の安定識別、容量、セクタ、または稼働中Windows状態が計画時から変わりました"));
        }
        for (const auto& binding : plan.shrink_payload_bindings_) {
          if (binding.disposition !=
              TsumugiShrinkPayloadDispositionV1::
                  recreate_empty_file_system) {
            continue;
          }
          const auto* partition = selected_partition(
              plan.image_.manifest, binding.source_table_index);
          if (partition == nullptr || binding.target_number == 0U ||
              !verified_empty_sources
                   .insert(binding.source_table_index)
                   .second) {
            return clonecore::Status::failure(service_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"Tsumugi空ファイルシステムbinding",
                L"空ファイルシステム再作成対象を一意に確定できません"));
          }
          auto recreated =
              transaction.recreate_empty_file_system_and_verify(
                  TsumugiShrinkArchiveTarget{
                      .stable_target_identity_hash =
                          disk.stable_identity_hash,
                      .source_table_index = partition->source_table_index,
                      .source_partition_number =
                          partition->source_partition_number,
                      .file_system = partition->file_system,
                      .payload_format_version =
                          partition->payload_format_version,
                      .cluster_size = partition->cluster_size,
                      .target_offset = binding.target_offset,
                      .target_size = binding.target_size,
                      .archive_length = partition->payload_logical_length,
                  });
          if (!recreated) {
            return recreated;
          }
        }
        plan_matched = true;
        target_reidentified = true;
        return clonecore::success_status();
      });
  if (!restored) {
    return clonecore::Result<TsumugiShrinkRestoreReport>::failure(
        restored.error());
  }
  if (active_archive.has_value()) {
    return failure<TsumugiShrinkRestoreReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_HANDLE_EOF,
        L"Tsumugi縮小WIM完了照合",
        L"WIM payloadが完了境界まで到達しませんでした");
  }
  std::uint64_t restored_bytes{};
  std::uint64_t accounted_bytes{};
  const auto expected_regenerated = static_cast<std::size_t>(std::count_if(
      plan.shrink_payload_bindings_.begin(),
      plan.shrink_payload_bindings_.end(),
      [](const TsumugiShrinkPayloadBindingV1& binding) {
        return binding.disposition !=
            TsumugiShrinkPayloadDispositionV1::
                restore_to_reviewed_partition;
      }));
  const auto expected_empty = static_cast<std::size_t>(std::count_if(
      plan.shrink_payload_bindings_.begin(),
      plan.shrink_payload_bindings_.end(),
      [](const TsumugiShrinkPayloadBindingV1& binding) {
        return binding.disposition ==
            TsumugiShrinkPayloadDispositionV1::recreate_empty_file_system;
      }));
  if (!checked_add(archive_bytes, raw_bytes, restored_bytes) ||
      !checked_add(restored_bytes, omitted_bytes, accounted_bytes) ||
      accounted_bytes != plan.planned_payload_bytes_ ||
      regenerated_sources.size() != expected_regenerated ||
      verified_empty_sources.size() != expected_empty) {
    return failure<TsumugiShrinkRestoreReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小payload完了照合",
        L"WIM、RAW、および明示再生成payloadの合計が検討済み計画と一致しません");
  }
  const auto committed = transaction.commit();
  if (!committed) {
    return clonecore::Result<TsumugiShrinkRestoreReport>::failure(
        committed.error());
  }
  transaction_guard.mark_committed();
  return clonecore::Result<TsumugiShrinkRestoreReport>::success({
      .archive_logical_bytes = archive_bytes,
      .archive_chunk_count = archive_chunks,
      .exact_raw_logical_bytes = raw_bytes,
      .exact_raw_chunk_count = raw_chunks,
      .completed_archive_partitions = completed_archives,
      .completed_empty_file_system_partitions =
          verified_empty_sources.size(),
      .intentionally_omitted_logical_bytes = omitted_bytes,
      .intentionally_omitted_chunk_count = omitted_chunks,
      .intentionally_regenerated_partitions = regenerated_sources.size(),
      .callbacks_started_after_complete_verification =
          restored.value().callbacks_started_after_complete_verification,
      .image_matched_prepared_plan = plan_matched,
      .target_reidentified_before_write = target_reidentified,
      .all_payloads_verified_by_adapter = true,
      .final_layout_committed = true,
  });
}

}  // namespace ytec::imageformat

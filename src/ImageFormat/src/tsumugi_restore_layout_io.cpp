#include "ytec/imageformat/tsumugi_restore_layout_io.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

clonecore::Error io_error(
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
  return clonecore::Result<T>::failure(io_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
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

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0U &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  result = left * right;
  return true;
}

clonecore::Status cancelled(const std::wstring& operation) {
  return clonecore::Status::failure(io_error(
      clonecore::ErrorCode::cancelled,
      ERROR_CANCELLED,
      operation,
      L"パーティション表を公開しない安全な境界で取り消しました"));
}

void publish(
    const clonecore::DiskOperationCallbacks& callbacks,
    const clonecore::DiskOperationStage stage,
    const bool cancellation_allowed) {
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = stage,
          .cancellation_allowed = cancellation_allowed,
      });
}

clonecore::Status write_flush_verify(
    clonecore::ITargetDiskWriter& target,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes,
    const std::wstring& operation) {
  auto status = target.write_target(offset, bytes);
  if (!status) {
    return status;
  }
  status = target.flush_target();
  if (!status) {
    return status;
  }
  auto observed = target.read_back(offset, bytes.size());
  if (!observed) {
    return clonecore::Status::failure(observed.error());
  }
  if (observed.value().size() != bytes.size() ||
      !std::equal(bytes.begin(), bytes.end(), observed.value().begin())) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        operation,
        L"flush後に同じ対象ハンドルから読戻した内容が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_write(
    const TsumugiRestoreLayoutWrite& write,
    const std::uint64_t target_size,
    const std::uint32_t sector_size) {
  std::uint64_t end{};
  if (write.bytes.empty() || write.offset % sector_size != 0U ||
      write.bytes.size() % sector_size != 0U ||
      !checked_add(write.offset, write.bytes.size(), end) ||
      end > target_size) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi復元メタデータ範囲",
        L"書込み範囲が空、未整列、オーバーフロー、または対象境界外です"));
  }
  return clonecore::success_status();
}

clonecore::Error append_emergency_reinvalidation_failure(
    clonecore::Error primary,
    const clonecore::Status& emergency) {
  if (!emergency) {
    primary.message.append(
        L"。さらに公開途中メタデータの緊急再無効化にも失敗しました: ")
        .append(emergency.error().operation)
        .append(L" / ")
        .append(emergency.error().message);
  }
  return primary;
}

clonecore::Status reinvalidate_exact_metadata(
    const TsumugiWholeDiskRestoreLayoutPlan& plan,
    clonecore::ITargetDiskWriter& target) {
  std::optional<clonecore::Error> first_error;
  const auto clear = [&](const TsumugiRestoreLayoutWrite& write) {
    try {
      std::vector<std::byte> zeroes(write.bytes.size(), std::byte{0});
      const auto status = write_flush_verify(
          target,
          write.offset,
          zeroes,
          L"Tsumugi公開途中メタデータ緊急再無効化");
      if (!status && !first_error.has_value()) {
        first_error = status.error();
      }
    } catch (...) {
      if (!first_error.has_value()) {
        first_error = io_error(
            clonecore::ErrorCode::internal_error,
            ERROR_OUTOFMEMORY,
            L"Tsumugi公開途中メタデータ緊急再無効化",
            L"緊急ゼロ化bufferを確保できませんでした");
      }
    }
  };
  for (const auto& write : plan.staged_writes) {
    clear(write);
  }
  for (const auto& write : plan.commit_writes) {
    clear(write);
  }
  return first_error.has_value()
      ? clonecore::Status::failure(std::move(*first_error))
      : clonecore::success_status();
}

clonecore::Status validate_plan(
    const TsumugiWholeDiskRestoreLayoutPlan& plan,
    const clonecore::ITargetDiskWriter& target) {
  if (plan.target_size_bytes == 0U || plan.logical_sector_size == 0U ||
      target.size_bytes() != plan.target_size_bytes ||
      target.logical_sector_size() != plan.logical_sector_size ||
      plan.invalidation_ranges.size() != 2U) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi復元レイアウト対象照合",
        L"計画とロック済み対象の容量、セクター、または無効化範囲が一致しません"));
  }
  std::uint64_t first_end{};
  std::uint64_t second_end{};
  const auto& first = plan.invalidation_ranges[0];
  const auto& second = plan.invalidation_ranges[1];
  if (first.offset != 0U || first.length == 0U ||
      first.length > 1024U * 1024U ||
      first.length % plan.logical_sector_size != 0U ||
      second.length != first.length ||
      second.offset % plan.logical_sector_size != 0U ||
      !checked_add(first.offset, first.length, first_end) ||
      !checked_add(second.offset, second.length, second_end) ||
      first_end > second.offset || second_end != plan.target_size_bytes) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi復元レイアウト無効化",
        L"先頭・末尾の非重複かつ整列済み無効化範囲が必要です"));
  }
  for (const auto& write : plan.staged_writes) {
    const auto valid = validate_write(
        write, plan.target_size_bytes, plan.logical_sector_size);
    if (!valid) {
      return valid;
    }
  }
  for (const auto& write : plan.commit_writes) {
    const auto valid = validate_write(
        write, plan.target_size_bytes, plan.logical_sector_size);
    if (!valid) {
      return valid;
    }
  }
  std::vector<const TsumugiRestoreLayoutWrite*> writes;
  writes.reserve(plan.staged_writes.size() + plan.commit_writes.size());
  for (const auto& write : plan.staged_writes) {
    writes.push_back(&write);
  }
  for (const auto& write : plan.commit_writes) {
    writes.push_back(&write);
  }
  for (std::size_t left = 0U; left < writes.size(); ++left) {
    const std::uint64_t left_end =
        writes[left]->offset + writes[left]->bytes.size();
    for (std::size_t right = left + 1U; right < writes.size(); ++right) {
      const std::uint64_t right_end =
          writes[right]->offset + writes[right]->bytes.size();
      if (writes[left]->offset < right_end &&
          writes[right]->offset < left_end) {
        return clonecore::Status::failure(io_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_DATA,
            L"Tsumugi復元メタデータ重複",
            L"仮配置または最終確定の書込み範囲が重複しています"));
      }
    }
  }

  if (plan.style == PartitionTableStyle::gpt) {
    const std::array<TsumugiRestoreLayoutWriteKind, 2U> staged{
        TsumugiRestoreLayoutWriteKind::gpt_primary_entries,
        TsumugiRestoreLayoutWriteKind::gpt_backup_entries,
    };
    const std::array<TsumugiRestoreLayoutWriteKind, 3U> committed{
        TsumugiRestoreLayoutWriteKind::gpt_backup_header,
        TsumugiRestoreLayoutWriteKind::gpt_protective_mbr,
        TsumugiRestoreLayoutWriteKind::gpt_primary_header,
    };
    if (!std::holds_alternative<clonecore::GptDisk>(plan.target_layout) ||
        plan.staged_writes.size() != staged.size() ||
        plan.commit_writes.size() != committed.size() ||
        !std::equal(
            staged.begin(), staged.end(), plan.staged_writes.begin(),
            [](const auto expected, const auto& actual) {
              return expected == actual.kind;
            }) ||
        !std::equal(
            committed.begin(), committed.end(), plan.commit_writes.begin(),
            [](const auto expected, const auto& actual) {
              return expected == actual.kind;
            })) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"Tsumugi GPT復元書込み順序",
          L"GPT entry仮配置とbackup／protective／primary確定順が不正です"));
    }
    const auto& layout = std::get<clonecore::GptDisk>(plan.target_layout);
    if (layout.logical_sector_size != plan.logical_sector_size ||
        layout.sector_count !=
            plan.target_size_bytes / plan.logical_sector_size) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi GPT復元対象寸法",
          L"生成済みGPTと対象寸法が一致しません"));
    }
    std::uint64_t entry_bytes{};
    std::uint64_t rounded_entry_bytes{};
    if (layout.partition_entry_count == 0U ||
        layout.partition_entry_size == 0U ||
        !checked_multiply(
            layout.partition_entry_count,
            layout.partition_entry_size,
            entry_bytes) ||
        !checked_add(
            entry_bytes,
            plan.logical_sector_size - 1U,
            rounded_entry_bytes)) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"Tsumugi GPT配列寸法",
          L"GPT entry配列の寸法が空またはオーバーフローします"));
    }
    const std::uint64_t padded_entries =
        (rounded_entry_bytes / plan.logical_sector_size) *
        plan.logical_sector_size;
    if (padded_entries == 0U ||
        padded_entries > plan.target_size_bytes - plan.logical_sector_size ||
        plan.staged_writes[0].offset != 2ULL * plan.logical_sector_size ||
        plan.staged_writes[0].bytes.size() != padded_entries ||
        plan.staged_writes[1].offset !=
            plan.target_size_bytes - plan.logical_sector_size -
                padded_entries ||
        plan.staged_writes[1].bytes.size() != padded_entries ||
        plan.commit_writes[0].offset !=
            plan.target_size_bytes - plan.logical_sector_size ||
        plan.commit_writes[0].bytes.size() != plan.logical_sector_size ||
        plan.commit_writes[1].offset != 0U ||
        plan.commit_writes[1].bytes.size() != plan.logical_sector_size ||
        plan.commit_writes[2].offset != plan.logical_sector_size ||
        plan.commit_writes[2].bytes.size() != plan.logical_sector_size) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"Tsumugi GPTメタデータ配置",
          L"entry配列、backup header、protective MBR、またはprimary headerの位置が不正です"));
    }
    return clonecore::success_status();
  }

  if (plan.style != PartitionTableStyle::mbr ||
      !std::holds_alternative<clonecore::MbrDisk>(plan.target_layout) ||
      !plan.staged_writes.empty() || plan.commit_writes.size() != 1U ||
      plan.commit_writes[0].kind !=
          TsumugiRestoreLayoutWriteKind::mbr_sector) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi MBR復元書込み順序",
        L"MBRは仮配置なしの1セクター確定だけが必要です"));
  }
  const auto& layout = std::get<clonecore::MbrDisk>(plan.target_layout);
  if (layout.logical_sector_size != plan.logical_sector_size ||
      layout.sector_count !=
          plan.target_size_bytes / plan.logical_sector_size ||
      plan.logical_sector_size != 512U ||
      plan.commit_writes[0].offset != 0U ||
      plan.commit_writes[0].bytes.size() != plan.logical_sector_size) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi MBR復元対象寸法",
        L"生成済みMBRと対象寸法が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<clonecore::ByteRange>>
exact_metadata_ranges_for_io(
    const TsumugiWholeDiskRestoreLayoutPlan& metadata) {
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
          L"Tsumugi縮小仮GPT退役範囲照合",
          L"仮GPTメタデータ範囲が空、未整列、または対象外です");
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
  return clonecore::Result<std::vector<clonecore::ByteRange>>::success(
      std::move(merged));
}

bool same_ranges(
    const std::vector<clonecore::ByteRange>& left,
    const std::vector<clonecore::ByteRange>& right) noexcept {
  return left.size() == right.size() &&
      std::equal(
          left.begin(), left.end(), right.begin(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.offset == rhs.offset && lhs.length == rhs.length;
          });
}

clonecore::Status validate_shrink_transaction_plan(
    const TsumugiShrinkWholeDiskRestoreLayoutPlanV1& final_plan,
    const std::vector<TsumugiShrinkConstructionLayoutPlanV1>& constructions,
    const clonecore::ITargetDiskWriter& target) {
  const auto final_valid = validate_plan(final_plan.metadata, target);
  if (!final_valid) {
    return final_valid;
  }
  const auto& migration = final_plan.migration;
  const bool final_style_matches =
      (migration.target_style ==
           migrationcore::MigrationPartitionStyle::gpt &&
       final_plan.metadata.style == PartitionTableStyle::gpt) ||
      (migration.target_style ==
           migrationcore::MigrationPartitionStyle::mbr &&
       final_plan.metadata.style == PartitionTableStyle::mbr);
  if (!migration.source_remains_unchanged || !final_style_matches ||
      migration.target_size_bytes != final_plan.metadata.target_size_bytes ||
      migration.alignment_bytes == 0U ||
      migration.alignment_bytes % final_plan.metadata.logical_sector_size !=
          0U) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小I/O最終計画照合",
        L"コピー元不変条件、最終形式、容量、または整列が一致しません"));
  }

  std::set<std::uint32_t> expected_construction_targets;
  std::set<std::uint32_t> observed_construction_targets;
  std::set<std::uint32_t> source_indexes;
  std::set<std::uint32_t> target_numbers;
  std::set<std::array<std::byte, 16U>> construction_guids;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> target_ranges;
  std::uint32_t expected_target_number = 1U;
  for (const auto& partition : migration.target_partitions) {
    std::uint64_t partition_end{};
    if (partition.target_number != expected_target_number++ ||
        partition.role > migrationcore::MigrationPartitionRole::data ||
        partition.file_system >
            migrationcore::MigrationFileSystem::unsupported ||
        partition.size_bytes == 0U ||
        partition.offset_bytes % final_plan.metadata.logical_sector_size !=
            0U ||
        partition.offset_bytes % migration.alignment_bytes != 0U ||
        partition.size_bytes % final_plan.metadata.logical_sector_size != 0U ||
        !checked_add(
            partition.offset_bytes, partition.size_bytes, partition_end) ||
        partition_end > final_plan.metadata.target_size_bytes ||
        !target_numbers.insert(partition.target_number).second) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O最終区画範囲",
          L"最終区画の番号、範囲、整列、または一意性が不正です"));
    }
    target_ranges.emplace_back(partition.offset_bytes, partition_end);
    if (partition.source_table_index.has_value() &&
        (*partition.source_table_index == 0U ||
         !source_indexes.insert(*partition.source_table_index).second)) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/Oコピー元対応",
          L"コピー元区画番号が0または最終区画間で重複しています"));
    }

    using Action = migrationcore::MigrationPartitionAction;
    using FileSystem = migrationcore::MigrationFileSystem;
    using Role = migrationcore::MigrationPartitionRole;
    bool construction_required = false;
    bool action_valid = false;
    switch (partition.action) {
      case Action::apply_file_image:
        action_valid = true;
        construction_required = true;
        if (!partition.source_table_index.has_value() ||
            migrationcore::classify_shrink_file_system(
                partition.file_system) !=
                migrationcore::ShrinkFileSystemDisposition::file_archive ||
            partition.role == Role::efi_system ||
            partition.role == Role::microsoft_reserved) {
          return clonecore::Status::failure(io_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小I/O file image対応",
              L"file image区画のコピー元、役割、またはファイルシステムが不正です"));
        }
        break;
      case Action::create_empty_ntfs:
      case Action::create_empty_exfat:
      case Action::create_empty_fat32: {
        action_valid = true;
        construction_required = true;
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
          return clonecore::Status::failure(io_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小I/O空ファイルシステム対応",
              L"空ファイルシステム区画のコピー元、役割、またはactionが不正です"));
        }
        break;
      }
      case Action::create_fat32:
        action_valid = true;
        construction_required = true;
        if (migration.target_style !=
                migrationcore::MigrationPartitionStyle::gpt ||
            partition.source_table_index.has_value() ||
            partition.role != Role::efi_system ||
            partition.file_system != FileSystem::fat32 || partition.active) {
          return clonecore::Status::failure(io_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小I/O生成ESP対応",
              L"生成ESPはコピー元なしのGPT FAT32非Active区画である必要があります"));
        }
        break;
      case Action::create_reserved:
        action_valid = true;
        if (migration.target_style !=
                migrationcore::MigrationPartitionStyle::gpt ||
            partition.source_table_index.has_value() ||
            partition.role != Role::microsoft_reserved ||
            partition.file_system != FileSystem::none || partition.active) {
          return clonecore::Status::failure(io_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小I/O生成MSR対応",
              L"生成MSRはコピー元なしのGPT予約領域である必要があります"));
        }
        break;
      case Action::copy_exact_raw:
        action_valid = true;
        if (!partition.source_table_index.has_value() ||
            migrationcore::classify_shrink_file_system(
                partition.file_system) !=
                migrationcore::ShrinkFileSystemDisposition::exact_raw_only ||
            partition.role == Role::efi_system ||
            partition.role == Role::microsoft_reserved) {
          return clonecore::Status::failure(io_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小I/O exact RAW対応",
              L"exact RAW区画のコピー元、役割、またはファイルシステムが不正です"));
        }
        break;
    }
    if (!action_valid) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi縮小I/O action",
          L"最終区画actionを安全なconstruction方針へ対応付けられません"));
    }
    if (construction_required &&
        !expected_construction_targets.insert(partition.target_number).second) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"Tsumugi縮小I/O construction対応",
          L"同じ最終区画に複数のconstructionが必要とされています"));
    }
  }
  std::sort(target_ranges.begin(), target_ranges.end());
  for (std::size_t index = 1U; index < target_ranges.size(); ++index) {
    if (target_ranges[index].first < target_ranges[index - 1U].second) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O最終区画重複",
          L"レビュー済み最終区画の範囲が重複しています"));
    }
  }

  if (migration.target_style ==
      migrationcore::MigrationPartitionStyle::gpt) {
    const auto* final_gpt = std::get_if<clonecore::GptDisk>(
        &final_plan.metadata.target_layout);
    if (final_gpt == nullptr ||
        final_gpt->partitions.size() != migration.target_partitions.size()) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O最終GPT対応",
          L"レビュー済み区画と最終GPT entryの件数が一致しません"));
    }
    if (final_gpt->disk_guid.is_zero() ||
        !construction_guids.insert(final_gpt->disk_guid.bytes).second) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O最終GPT識別子",
          L"最終GPT Disk GUIDが空または重複しています"));
    }
    for (const auto& planned : migration.target_partitions) {
      const auto observed = std::find_if(
          final_gpt->partitions.begin(),
          final_gpt->partitions.end(),
          [&](const auto& partition) {
            return static_cast<std::uint64_t>(partition.entry_index) + 1U ==
                planned.target_number;
          });
      std::uint64_t observed_offset{};
      std::uint64_t observed_end_lba{};
      std::uint64_t observed_end{};
      std::uint64_t planned_end{};
      if (observed == final_gpt->partitions.end() ||
          observed != final_gpt->partitions.begin() +
              static_cast<std::ptrdiff_t>(planned.target_number - 1U) ||
          !checked_multiply(
              observed->first_lba,
              final_plan.metadata.logical_sector_size,
              observed_offset) ||
          !checked_add(observed->last_lba, 1U, observed_end_lba) ||
          !checked_multiply(
              observed_end_lba,
              final_plan.metadata.logical_sector_size,
              observed_end) ||
          !checked_add(
              planned.offset_bytes, planned.size_bytes, planned_end) ||
          observed_offset != planned.offset_bytes ||
          observed_end != planned_end || planned.active) {
        return clonecore::Status::failure(io_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小I/O最終GPT配置",
            L"最終GPT entryの番号、offset、長さ、またはActive相当条件が計画と一致しません"));
      }
      if (observed->unique_guid.is_zero() ||
          !construction_guids.insert(observed->unique_guid.bytes).second) {
        return clonecore::Status::failure(io_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_DUP_NAME,
            L"Tsumugi縮小I/O最終GPT区画識別子",
            L"最終GPT Partition GUIDが空または重複しています"));
      }

      const bool is_generated_esp =
          !planned.source_table_index.has_value() &&
          planned.role == migrationcore::MigrationPartitionRole::efi_system &&
          planned.action ==
              migrationcore::MigrationPartitionAction::create_fat32 &&
          observed->type_guid == clonecore::gpt_type_efi_system() &&
          observed->attributes == 0U;
      const bool is_generated_msr =
          !planned.source_table_index.has_value() &&
          planned.role ==
              migrationcore::MigrationPartitionRole::microsoft_reserved &&
          planned.action ==
              migrationcore::MigrationPartitionAction::create_reserved &&
          observed->type_guid ==
              clonecore::gpt_type_microsoft_reserved() &&
          observed->attributes == 0U;
      const bool is_source_content =
          planned.source_table_index.has_value() &&
          planned.role != migrationcore::MigrationPartitionRole::efi_system &&
          planned.role !=
              migrationcore::MigrationPartitionRole::microsoft_reserved &&
          planned.role !=
              migrationcore::MigrationPartitionRole::bios_system &&
          !observed->type_guid.is_zero();
      if (!is_generated_esp && !is_generated_msr && !is_source_content) {
        return clonecore::Status::failure(io_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小I/O最終GPT種類",
            L"最終GPTの生成領域またはコピー元対応領域の種類が不正です"));
      }
      if (planned.role == migrationcore::MigrationPartitionRole::windows &&
          observed->type_guid != clonecore::gpt_type_basic_data()) {
        return clonecore::Status::failure(io_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小I/O Windows GPT種類",
            L"Windows領域の最終GPT種類がMicrosoft Basic Dataではありません"));
      }
      if (planned.role == migrationcore::MigrationPartitionRole::recovery &&
          observed->type_guid != clonecore::gpt_type_windows_recovery()) {
        return clonecore::Status::failure(io_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小I/O回復GPT種類",
            L"回復領域の最終GPT種類がWindows Recoveryではありません"));
      }
    }
  } else {
    const auto* final_mbr = std::get_if<clonecore::MbrDisk>(
        &final_plan.metadata.target_layout);
    if (final_mbr == nullptr ||
        final_mbr->partitions.size() != migration.target_partitions.size()) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O最終MBR対応",
          L"レビュー済み区画と最終MBR entryの件数が一致しません"));
    }
    for (const auto& planned : migration.target_partitions) {
      const auto observed = std::find_if(
          final_mbr->partitions.begin(),
          final_mbr->partitions.end(),
          [&](const auto& partition) {
            return static_cast<std::uint32_t>(partition.table_index) + 1U ==
                planned.target_number;
          });
      std::uint64_t observed_offset{};
      std::uint64_t observed_size{};
      if (observed == final_mbr->partitions.end() ||
          observed != final_mbr->partitions.begin() +
              static_cast<std::ptrdiff_t>(planned.target_number - 1U) ||
          !planned.source_table_index.has_value() ||
          planned.target_number > 4U ||
          !checked_multiply(
              observed->first_lba,
              final_plan.metadata.logical_sector_size,
              observed_offset) ||
          !checked_multiply(
              observed->sector_count,
              final_plan.metadata.logical_sector_size,
              observed_size) ||
          observed_offset != planned.offset_bytes ||
          observed_size != planned.size_bytes ||
          observed->active != planned.active || observed->type == 0U ||
          planned.role == migrationcore::MigrationPartitionRole::efi_system ||
          planned.role ==
              migrationcore::MigrationPartitionRole::microsoft_reserved) {
        return clonecore::Status::failure(io_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi縮小I/O最終MBR配置",
            L"最終MBR entryの番号、対応、offset、長さ、種類、またはActiveが計画と一致しません"));
      }
      if (planned.action !=
          migrationcore::MigrationPartitionAction::copy_exact_raw) {
        const std::uint8_t expected_type =
            planned.role ==
                    migrationcore::MigrationPartitionRole::recovery
            ? 0x27U
            : planned.file_system ==
                    migrationcore::MigrationFileSystem::fat32
                ? 0x0CU
                : 0x07U;
        if (observed->type != expected_type) {
          return clonecore::Status::failure(io_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"Tsumugi縮小I/O最終MBR種類",
              L"最終MBR partition typeが計画した内容と一致しません"));
        }
      }
    }
  }
  if (constructions.size() != expected_construction_targets.size()) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi縮小I/O仮GPT件数",
        L"volume construction対象と仮GPTの件数が一致しません"));
  }

  for (const auto& construction : constructions) {
    if (construction.final_target_number == 0U ||
        !observed_construction_targets
             .insert(construction.final_target_number)
             .second ||
        !expected_construction_targets.contains(
            construction.final_target_number)) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O仮GPT識別",
          L"仮GPTの最終区画番号が不明、0、または重複しています"));
    }
    const auto placement = std::find_if(
        migration.target_partitions.begin(),
        migration.target_partitions.end(),
        [&](const auto& partition) {
          return partition.target_number ==
              construction.final_target_number;
        });
    std::optional<TsumugiShrinkConstructionPurposeV1> expected_purpose;
    if (placement != migration.target_partitions.end()) {
      using Action = migrationcore::MigrationPartitionAction;
      switch (placement->action) {
        case Action::apply_file_image:
          expected_purpose =
              TsumugiShrinkConstructionPurposeV1::apply_file_image;
          break;
        case Action::create_empty_ntfs:
        case Action::create_empty_exfat:
        case Action::create_empty_fat32:
          expected_purpose = TsumugiShrinkConstructionPurposeV1::
              recreate_empty_file_system;
          break;
        case Action::create_fat32:
          expected_purpose =
              TsumugiShrinkConstructionPurposeV1::prepare_efi_system;
          break;
        case Action::create_reserved:
        case Action::copy_exact_raw:
          break;
      }
    }
    if (placement == migration.target_partitions.end() ||
        !expected_purpose.has_value() ||
        construction.purpose != expected_purpose.value() ||
        placement->source_table_index != construction.source_table_index ||
        placement->offset_bytes != construction.target_offset ||
        placement->size_bytes != construction.target_size ||
        construction.target_size == 0U) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O仮GPT配置",
          L"仮GPTがレビュー済み最終配置と一致しません"));
    }
    const auto temporary_valid =
        validate_plan(construction.temporary_metadata, target);
    if (!temporary_valid) {
      return temporary_valid;
    }
    const auto* gpt = std::get_if<clonecore::GptDisk>(
        &construction.temporary_metadata.target_layout);
    std::uint64_t payload_end{};
    if (construction.temporary_metadata.style != PartitionTableStyle::gpt ||
        gpt == nullptr || gpt->partitions.size() != 1U ||
        !checked_add(
            construction.target_offset,
            construction.target_size,
            payload_end)) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O仮GPT構造",
          L"仮レイアウトは正確に1区画を持つGPTである必要があります"));
    }
    const auto& partition = gpt->partitions.front();
    std::uint64_t partition_offset{};
    std::uint64_t partition_end{};
    if (!checked_multiply(
            partition.first_lba,
            construction.temporary_metadata.logical_sector_size,
            partition_offset) ||
        !checked_add(
            partition.last_lba, 1U, partition_end) ||
        !checked_multiply(
            partition_end,
            construction.temporary_metadata.logical_sector_size,
            partition_end) ||
        partition.entry_index != 0U ||
        partition.type_guid != clonecore::gpt_type_basic_data() ||
        partition.unique_guid.is_zero() || gpt->disk_guid.is_zero() ||
        partition.attributes !=
            kTsumugiConstructionNoDefaultDriveLetterAttribute ||
        !partition.name.starts_with(u"YTEC-Tsumugi-INCOMPLETE-") ||
        partition_offset != construction.target_offset ||
        partition_end != payload_end ||
        !construction_guids.insert(gpt->disk_guid.bytes).second ||
        !construction_guids.insert(partition.unique_guid.bytes).second) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O仮GPT安全属性",
          L"仮GPTの種類、識別子、属性、印、またはpayload境界が不正です"));
    }
    const auto exact = exact_metadata_ranges_for_io(
        construction.temporary_metadata);
    if (!exact || !same_ranges(exact.value(), construction.retirement_ranges) ||
        construction.retirement_ranges.size() != 2U) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O仮GPT退役照合",
          L"退役範囲が仮GPTの正確なメタデータ範囲と一致しません"));
    }
    const auto& leading = construction.retirement_ranges.front();
    const auto& trailing = construction.retirement_ranges.back();
    std::uint64_t leading_end{};
    std::uint64_t trailing_end{};
    if (!checked_add(leading.offset, leading.length, leading_end) ||
        !checked_add(trailing.offset, trailing.length, trailing_end) ||
        leading.offset != 0U || leading_end > construction.target_offset ||
        payload_end > trailing.offset ||
        trailing_end != final_plan.metadata.target_size_bytes) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_DATA,
          L"Tsumugi縮小I/O仮GPT退役境界",
          L"退役範囲がpayloadと分離された対象両端ではありません"));
    }
  }
  return clonecore::success_status();
}

clonecore::Status clear_exact_ranges(
    clonecore::ITargetDiskWriter& target,
    const std::vector<clonecore::ByteRange>& ranges,
    const std::wstring& operation,
    std::uint64_t& verified_bytes) noexcept {
  std::optional<clonecore::Error> first_error;
  std::uint64_t local_verified{};
  for (const auto& range : ranges) {
    try {
      if (range.length >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::size_t>::max)())) {
        if (!first_error.has_value()) {
          first_error = io_error(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              operation,
              L"退役範囲をbuffer長として表現できません");
        }
        continue;
      }
      std::vector<std::byte> zeroes(
          static_cast<std::size_t>(range.length), std::byte{0});
      const auto status =
          write_flush_verify(target, range.offset, zeroes, operation);
      if (!status) {
        if (!first_error.has_value()) {
          first_error = status.error();
        }
        continue;
      }
      local_verified += range.length;
    } catch (...) {
      if (!first_error.has_value()) {
        first_error = io_error(
            clonecore::ErrorCode::internal_error,
            ERROR_OUTOFMEMORY,
            operation,
            L"退役用ゼロbufferを確保できませんでした");
      }
    }
  }
  if (first_error.has_value()) {
    return clonecore::Status::failure(std::move(*first_error));
  }
  verified_bytes += local_verified;
  return clonecore::success_status();
}

class TargetReaderView final : public clonecore::ISourceDiskReader {
 public:
  explicit TargetReaderView(
      const clonecore::ITargetDiskWriter& target) noexcept
      : target_(&target) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return target_->size_bytes();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return target_->logical_sector_size();
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    return target_->read_back(offset, length);
  }

 private:
  const clonecore::ITargetDiskWriter* target_{};
};

bool same_gpt_partition(
    const clonecore::GptPartition& left,
    const clonecore::GptPartition& right) noexcept {
  return left.entry_index == right.entry_index &&
      left.type_guid == right.type_guid &&
      left.unique_guid == right.unique_guid &&
      left.first_lba == right.first_lba &&
      left.last_lba == right.last_lba &&
      left.attributes == right.attributes && left.name == right.name;
}

bool same_gpt_disk(
    const clonecore::GptDisk& left,
    const clonecore::GptDisk& right) {
  if (left.logical_sector_size != right.logical_sector_size ||
      left.sector_count != right.sector_count ||
      left.disk_guid != right.disk_guid ||
      left.first_usable_lba != right.first_usable_lba ||
      left.last_usable_lba != right.last_usable_lba ||
      left.partition_entry_count != right.partition_entry_count ||
      left.partition_entry_size != right.partition_entry_size ||
      left.partitions.size() != right.partitions.size()) {
    return false;
  }
  return std::all_of(
      left.partitions.begin(),
      left.partitions.end(),
      [&](const clonecore::GptPartition& partition) {
        const auto found = std::find_if(
            right.partitions.begin(),
            right.partitions.end(),
            [&](const clonecore::GptPartition& candidate) {
              return candidate.entry_index == partition.entry_index;
            });
        return found != right.partitions.end() &&
            same_gpt_partition(partition, *found);
      });
}

bool same_mbr_partition(
    const clonecore::MbrPartition& left,
    const clonecore::MbrPartition& right) noexcept {
  return left.table_index == right.table_index &&
      left.active == right.active && left.first_chs == right.first_chs &&
      left.type == right.type && left.last_chs == right.last_chs &&
      left.first_lba == right.first_lba &&
      left.sector_count == right.sector_count;
}

bool same_mbr_disk(
    const clonecore::MbrDisk& left,
    const clonecore::MbrDisk& right) {
  if (left.logical_sector_size != right.logical_sector_size ||
      left.sector_count != right.sector_count ||
      left.disk_signature != right.disk_signature ||
      left.bootstrap != right.bootstrap ||
      left.partitions.size() != right.partitions.size()) {
    return false;
  }
  return std::all_of(
      left.partitions.begin(),
      left.partitions.end(),
      [&](const clonecore::MbrPartition& partition) {
        const auto found = std::find_if(
            right.partitions.begin(),
            right.partitions.end(),
            [&](const clonecore::MbrPartition& candidate) {
              return candidate.table_index == partition.table_index;
            });
        return found != right.partitions.end() &&
            same_mbr_partition(partition, *found);
      });
}

clonecore::Status verify_preserving_layout(
    const TsumugiRestoreTargetLayout& expected,
    const PartitionTableStyle style,
    const clonecore::ITargetDiskWriter& target,
    const std::wstring& operation) {
  TargetReaderView reader(target);
  if (style == PartitionTableStyle::gpt) {
    const auto observed = clonecore::parse_gpt(reader);
    if (!observed ||
        !std::holds_alternative<clonecore::GptDisk>(expected) ||
        !same_gpt_disk(
            std::get<clonecore::GptDisk>(expected), observed.value())) {
      return clonecore::Status::failure(
          observed
              ? io_error(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_CRC,
                    operation,
                    L"読戻したGPTが保持型計画と一致しません")
              : observed.error());
    }
    return clonecore::success_status();
  }
  const auto observed = clonecore::parse_mbr(reader);
  if (!observed ||
      !std::holds_alternative<clonecore::MbrDisk>(expected) ||
      !same_mbr_disk(
          std::get<clonecore::MbrDisk>(expected), observed.value())) {
    return clonecore::Status::failure(
        observed
            ? io_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  operation,
                  L"読戻したMBRが保持型計画と一致しません")
            : observed.error());
  }
  return clonecore::success_status();
}

clonecore::Status validate_preserving_plan(
    const TsumugiPreservingPartitionLayoutPlanV1& plan,
    const clonecore::ITargetDiskWriter& target) {
  std::uint64_t partition_end{};
  if (plan.target_size_bytes == 0U || plan.logical_sector_size == 0U ||
      target.size_bytes() != plan.target_size_bytes ||
      target.logical_sector_size() != plan.logical_sector_size ||
      plan.new_partition_number == 0U ||
      plan.new_partition_size == 0U ||
      plan.new_partition_offset % plan.logical_sector_size != 0U ||
      plan.new_partition_size % plan.logical_sector_size != 0U ||
      !checked_add(
          plan.new_partition_offset,
          plan.new_partition_size,
          partition_end) ||
      partition_end > plan.target_size_bytes ||
      plan.published_writes.empty() ||
      plan.published_writes.size() != plan.rollback_writes.size()) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi保持型レイアウト計画",
        L"対象寸法、新規区画、または公開／rollback書込みが不正です"));
  }
  for (const auto& write : plan.published_writes) {
    const auto valid = validate_write(
        write, plan.target_size_bytes, plan.logical_sector_size);
    if (!valid) {
      return valid;
    }
    const auto rollback = std::find_if(
        plan.rollback_writes.begin(),
        plan.rollback_writes.end(),
        [&](const TsumugiRestoreLayoutWrite& candidate) {
          return candidate.offset == write.offset &&
              candidate.bytes.size() == write.bytes.size();
        });
    if (rollback == plan.rollback_writes.end()) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"Tsumugi保持型rollback対応",
          L"公開書込みと同じ範囲のrollback bytesがありません"));
    }
  }
  for (const auto& write : plan.rollback_writes) {
    const auto valid = validate_write(
        write, plan.target_size_bytes, plan.logical_sector_size);
    if (!valid) {
      return valid;
    }
  }

  if (plan.style == PartitionTableStyle::gpt) {
    const std::array<TsumugiRestoreLayoutWriteKind, 4U> published{
        TsumugiRestoreLayoutWriteKind::gpt_backup_entries,
        TsumugiRestoreLayoutWriteKind::gpt_backup_header,
        TsumugiRestoreLayoutWriteKind::gpt_primary_entries,
        TsumugiRestoreLayoutWriteKind::gpt_primary_header,
    };
    const std::array<TsumugiRestoreLayoutWriteKind, 4U> rollback{
        TsumugiRestoreLayoutWriteKind::gpt_primary_entries,
        TsumugiRestoreLayoutWriteKind::gpt_primary_header,
        TsumugiRestoreLayoutWriteKind::gpt_backup_entries,
        TsumugiRestoreLayoutWriteKind::gpt_backup_header,
    };
    if (!std::holds_alternative<clonecore::GptDisk>(
            plan.original_layout) ||
        !std::holds_alternative<clonecore::GptDisk>(plan.target_layout) ||
        plan.published_writes.size() != published.size() ||
        plan.rollback_writes.size() != rollback.size() ||
        !std::equal(
            published.begin(),
            published.end(),
            plan.published_writes.begin(),
            [](const auto expected, const auto& actual) {
              return expected == actual.kind;
            }) ||
        !std::equal(
            rollback.begin(),
            rollback.end(),
            plan.rollback_writes.begin(),
            [](const auto expected, const auto& actual) {
              return expected == actual.kind;
            })) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"Tsumugi保持型GPT順序",
          L"backup先行公開またはprimary先行rollback順序が不正です"));
    }
    const auto& original =
        std::get<clonecore::GptDisk>(plan.original_layout);
    const auto& updated =
        std::get<clonecore::GptDisk>(plan.target_layout);
    if (updated.partitions.size() != original.partitions.size() + 1U ||
        updated.disk_guid != original.disk_guid ||
        updated.logical_sector_size != original.logical_sector_size ||
        updated.sector_count != original.sector_count ||
        updated.first_usable_lba != original.first_usable_lba ||
        updated.last_usable_lba != original.last_usable_lba ||
        updated.partition_entry_count != original.partition_entry_count ||
        updated.partition_entry_size != original.partition_entry_size ||
        !std::all_of(
            original.partitions.begin(),
            original.partitions.end(),
            [&](const clonecore::GptPartition& partition) {
              const auto found = std::find_if(
                  updated.partitions.begin(),
                  updated.partitions.end(),
                  [&](const clonecore::GptPartition& candidate) {
                    return candidate.entry_index == partition.entry_index;
                  });
              return found != updated.partitions.end() &&
                  same_gpt_partition(partition, *found);
            })) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi保持型GPT既存区画",
          L"Disk GUIDまたは既存partition entryが変更されています"));
    }
    const auto added = std::find_if(
        updated.partitions.begin(),
        updated.partitions.end(),
        [&](const clonecore::GptPartition& partition) {
          return partition.entry_index + 1U == plan.new_partition_number;
        });
    if (added == updated.partitions.end() ||
        added->first_lba * plan.logical_sector_size !=
            plan.new_partition_offset ||
        (added->last_lba - added->first_lba + 1U) *
                plan.logical_sector_size !=
            plan.new_partition_size) {
      return clonecore::Status::failure(io_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi保持型GPT新規区画",
          L"追加entryの番号または範囲がレビュー済み配置と一致しません"));
    }
    return clonecore::success_status();
  }

  if (plan.style != PartitionTableStyle::mbr ||
      !std::holds_alternative<clonecore::MbrDisk>(plan.original_layout) ||
      !std::holds_alternative<clonecore::MbrDisk>(plan.target_layout) ||
      plan.published_writes.size() != 1U ||
      plan.published_writes.front().kind !=
          TsumugiRestoreLayoutWriteKind::mbr_sector ||
      plan.rollback_writes.front().kind !=
          TsumugiRestoreLayoutWriteKind::mbr_sector) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi保持型MBR順序",
        L"MBRは公開・rollback各1sectorだけを許可します"));
  }
  const auto& original =
      std::get<clonecore::MbrDisk>(plan.original_layout);
  const auto& updated =
      std::get<clonecore::MbrDisk>(plan.target_layout);
  if (updated.partitions.size() != original.partitions.size() + 1U ||
      updated.disk_signature != original.disk_signature ||
      updated.bootstrap != original.bootstrap ||
      updated.logical_sector_size != original.logical_sector_size ||
      updated.sector_count != original.sector_count ||
      !std::all_of(
          original.partitions.begin(),
          original.partitions.end(),
          [&](const clonecore::MbrPartition& partition) {
            const auto found = std::find_if(
                updated.partitions.begin(),
                updated.partitions.end(),
                [&](const clonecore::MbrPartition& candidate) {
                  return candidate.table_index == partition.table_index;
                });
            return found != updated.partitions.end() &&
                same_mbr_partition(partition, *found);
          })) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi保持型MBR既存区画",
        L"disk signature、bootstrap、または既存entryが変更されています"));
  }
  const auto added = std::find_if(
      updated.partitions.begin(),
      updated.partitions.end(),
      [&](const clonecore::MbrPartition& partition) {
        return static_cast<std::uint32_t>(partition.table_index) + 1U ==
            plan.new_partition_number;
      });
  if (added == updated.partitions.end() ||
      static_cast<std::uint64_t>(added->first_lba) *
              plan.logical_sector_size !=
          plan.new_partition_offset ||
      static_cast<std::uint64_t>(added->sector_count) *
              plan.logical_sector_size !=
          plan.new_partition_size) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi保持型MBR新規区画",
        L"追加entryの番号または範囲がレビュー済み配置と一致しません"));
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<TsumugiRestoreLayoutPublicationInspectionV1>
inspect_tsumugi_whole_disk_restore_layout_publication_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& plan,
    clonecore::ITargetDiskWriter& target,
    const std::size_t verification_block_bytes) {
  const auto valid = validate_plan(plan, target);
  if (!valid) {
    return clonecore::Result<
        TsumugiRestoreLayoutPublicationInspectionV1>::failure(
        valid.error());
  }
  if (verification_block_bytes == 0U ||
      verification_block_bytes > 32U * 1024U * 1024U) {
    return failure<TsumugiRestoreLayoutPublicationInspectionV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi復元レイアウト公開状態",
        L"read-only再照合block上限が不正です");
  }
  const std::size_t block_limit = (std::max<std::size_t>)(
      plan.logical_sector_size,
      verification_block_bytes -
          (verification_block_bytes % plan.logical_sector_size));
  std::vector<const TsumugiRestoreLayoutWrite*> writes;
  writes.reserve(plan.staged_writes.size() + plan.commit_writes.size());
  for (const auto& write : plan.staged_writes) {
    writes.push_back(&write);
  }
  for (const auto& write : plan.commit_writes) {
    writes.push_back(&write);
  }

  std::uint64_t published{};
  bool observed_zero{};
  for (const auto* write : writes) {
    bool matches_zero = true;
    bool matches_expected = true;
    std::uint64_t completed{};
    while (completed < write->bytes.size()) {
      const auto amount = static_cast<std::size_t>((std::min<std::uint64_t>)(
          write->bytes.size() - completed, block_limit));
      auto observed = target.read_back(write->offset + completed, amount);
      if (!observed) {
        return clonecore::Result<
            TsumugiRestoreLayoutPublicationInspectionV1>::failure(
            observed.error());
      }
      if (observed.value().size() != amount) {
        return failure<TsumugiRestoreLayoutPublicationInspectionV1>(
            clonecore::ErrorCode::io_failed,
            ERROR_HANDLE_EOF,
            L"Tsumugi復元レイアウト公開状態読戻し",
            L"同一target handleのmetadata読戻し長が一致しません");
      }
      matches_zero = matches_zero && std::all_of(
          observed.value().begin(),
          observed.value().end(),
          [](const std::byte value) { return value == std::byte{0}; });
      matches_expected = matches_expected && std::equal(
          observed.value().begin(),
          observed.value().end(),
          write->bytes.begin() + static_cast<std::ptrdiff_t>(completed));
      completed += amount;
    }
    if (matches_zero == matches_expected ||
        (matches_expected && observed_zero)) {
      return failure<TsumugiRestoreLayoutPublicationInspectionV1>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"Tsumugi復元レイアウト公開状態照合",
          L"metadata rangeがzero/operation期待byteの既知prefixではなく、torn、foreign、曖昧、または順序外です");
    }
    if (matches_expected) {
      ++published;
    } else {
      observed_zero = true;
    }
  }

  const auto total = static_cast<std::uint64_t>(writes.size());
  return clonecore::Result<
      TsumugiRestoreLayoutPublicationInspectionV1>::success({
      .state = published == 0U
          ? TsumugiRestoreLayoutPublicationStateV1::all_zero
          : published == total
              ? TsumugiRestoreLayoutPublicationStateV1::all_final
              : TsumugiRestoreLayoutPublicationStateV1::known_write_prefix,
      .published_write_count = published,
      .total_write_count = total,
  });
}

TsumugiWholeDiskRestoreLayoutTransaction::
    TsumugiWholeDiskRestoreLayoutTransaction(
        TsumugiWholeDiskRestoreLayoutPlan plan,
        clonecore::ITargetDiskWriter& target) noexcept
    : plan_(std::move(plan)), target_(&target) {}

clonecore::Result<TsumugiRestoreLayoutIoReport>
TsumugiWholeDiskRestoreLayoutTransaction::prepare(
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (target_ == nullptr || prepared_ || committed_ || terminal_failure_) {
    return failure<TsumugiRestoreLayoutIoReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi復元レイアウト無効化開始",
        L"対象または単回トランザクション状態が不正です");
  }
  const auto valid = validate_plan(plan_, *target_);
  if (!valid) {
    terminal_failure_ = true;
    return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
        valid.error());
  }
  publish(callbacks, clonecore::DiskOperationStage::invalidating_target, true);
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    terminal_failure_ = true;
    return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
        cancelled(L"Tsumugiコピー先無効化前").error());
  }

  // Cancellation stays disabled until both old table locations are gone.
  publish(callbacks, clonecore::DiskOperationStage::invalidating_target, false);
  for (const auto& range : plan_.invalidation_ranges) {
    std::vector<std::byte> zeroes(
        static_cast<std::size_t>(range.length), std::byte{0});
    const auto status = write_flush_verify(
        *target_, range.offset, zeroes, L"Tsumugiコピー先表無効化");
    if (!status) {
      terminal_failure_ = true;
      report_.target_left_incomplete = true;
      return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
          status.error());
    }
    report_.invalidated_bytes += range.length;
  }
  prepared_ = true;
  report_.every_write_read_back_verified = true;
  report_.target_left_incomplete = true;
  return clonecore::Result<TsumugiRestoreLayoutIoReport>::success(report_);
}

clonecore::Result<TsumugiRestoreLayoutIoReport>
TsumugiWholeDiskRestoreLayoutTransaction::resume_prepared(
    const std::size_t verification_block_bytes) {
  if (target_ == nullptr || prepared_ || committed_ || terminal_failure_ ||
      verification_block_bytes == 0U ||
      verification_block_bytes > 32U * 1024U * 1024U) {
    return failure<TsumugiRestoreLayoutIoReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi復元レイアウト再開",
        L"対象、単回状態、または再照合block上限が不正です");
  }
  const auto valid = validate_plan(plan_, *target_);
  if (!valid) {
    terminal_failure_ = true;
    return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
        valid.error());
  }
  const std::size_t block_limit = (std::max<std::size_t>)(
      plan_.logical_sector_size,
      verification_block_bytes -
          (verification_block_bytes % plan_.logical_sector_size));
  const auto verify_zero = [&](const TsumugiRestoreLayoutWrite& write) {
    std::uint64_t completed = 0U;
    while (completed < write.bytes.size()) {
      const auto amount = static_cast<std::size_t>((std::min<std::uint64_t>)(
          write.bytes.size() - completed, block_limit));
      auto observed = target_->read_back(write.offset + completed, amount);
      if (!observed) {
        return clonecore::Status::failure(observed.error());
      }
      if (observed.value().size() != amount ||
          !std::all_of(
              observed.value().begin(),
              observed.value().end(),
              [](const std::byte value) { return value == std::byte{0}; })) {
        return clonecore::Status::failure(io_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"Tsumugi復元延期メタデータ再照合",
            L"GPT/MBRの延期範囲がzeroではないため同じ未完成レイアウトを再開できません"));
      }
      completed += amount;
    }
    return clonecore::success_status();
  };
  for (const auto& write : plan_.staged_writes) {
    const auto status = verify_zero(write);
    if (!status) {
      terminal_failure_ = true;
      return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
          status.error());
    }
  }
  for (const auto& write : plan_.commit_writes) {
    const auto status = verify_zero(write);
    if (!status) {
      terminal_failure_ = true;
      return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
          status.error());
    }
  }
  prepared_ = true;
  report_.every_write_read_back_verified = true;
  report_.target_left_incomplete = true;
  return clonecore::Result<TsumugiRestoreLayoutIoReport>::success(report_);
}

clonecore::Result<TsumugiRestoreLayoutIoReport>
TsumugiWholeDiskRestoreLayoutTransaction::
    reinvalidate_publication_prefix(
        const std::size_t verification_block_bytes) {
  if (target_ == nullptr || prepared_ || committed_ || terminal_failure_) {
    return failure<TsumugiRestoreLayoutIoReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi復元公開途中レイアウト再無効化",
        L"対象または単回transaction状態が不正です");
  }
  auto inspected =
      inspect_tsumugi_whole_disk_restore_layout_publication_v1(
          plan_, *target_, verification_block_bytes);
  if (!inspected || inspected.value().state !=
          TsumugiRestoreLayoutPublicationStateV1::known_write_prefix) {
    return inspected
        ? failure<TsumugiRestoreLayoutIoReport>(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"Tsumugi復元公開途中レイアウト再無効化",
              L"operation期待byteの既知publication prefixではありません")
        : clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
              inspected.error());
  }
  const auto reinvalidated = reinvalidate_exact_metadata(plan_, *target_);
  if (!reinvalidated) {
    terminal_failure_ = true;
    return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
        reinvalidated.error());
  }
  auto zero = inspect_tsumugi_whole_disk_restore_layout_publication_v1(
      plan_, *target_, verification_block_bytes);
  if (!zero || zero.value().state !=
          TsumugiRestoreLayoutPublicationStateV1::all_zero) {
    terminal_failure_ = true;
    return zero
        ? failure<TsumugiRestoreLayoutIoReport>(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"Tsumugi復元公開途中レイアウト再無効化読戻し",
              L"全metadata rangeのzero再無効化を証明できません")
        : clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
              zero.error());
  }
  prepared_ = true;
  report_.every_write_read_back_verified = true;
  report_.emergency_reinvalidation_verified = true;
  report_.target_left_incomplete = true;
  return clonecore::Result<TsumugiRestoreLayoutIoReport>::success(report_);
}

clonecore::Result<TsumugiRestoreLayoutIoReport>
TsumugiWholeDiskRestoreLayoutTransaction::commit(
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (target_ == nullptr || !prepared_ || committed_ || terminal_failure_) {
    return failure<TsumugiRestoreLayoutIoReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi復元レイアウト確定開始",
        L"無効化済みかつ未確定の単回トランザクションがありません");
  }
  for (const auto& write : plan_.staged_writes) {
    publish(
        callbacks, clonecore::DiskOperationStage::staging_partition_table,
        true);
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      terminal_failure_ = true;
      return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
          cancelled(L"Tsumugiパーティション表仮配置前").error());
    }
    const auto status = write_flush_verify(
        *target_, write.offset, write.bytes, L"Tsumugiパーティション表仮配置");
    if (!status) {
      terminal_failure_ = true;
      report_.every_write_read_back_verified = false;
      return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
          status.error());
    }
    report_.staged_metadata_bytes += write.bytes.size();
  }
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    terminal_failure_ = true;
    return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
        cancelled(L"Tsumugiパーティション表最終確定前").error());
  }

  // Once a valid backup header or MBR is exposed, cancellation is unsafe.
  publish(
      callbacks, clonecore::DiskOperationStage::committing_partition_table,
      false);
  metadata_may_be_recognizable_ = true;
  for (const auto& write : plan_.commit_writes) {
    const auto status = write_flush_verify(
        *target_, write.offset, write.bytes, L"Tsumugiパーティション表最終確定");
    if (!status) {
      const auto emergency = reinvalidate_exact_metadata(plan_, *target_);
      metadata_may_be_recognizable_ = !emergency.has_value();
      terminal_failure_ = true;
      report_.every_write_read_back_verified = false;
      report_.target_left_incomplete = emergency.has_value();
      report_.emergency_reinvalidation_verified = emergency.has_value();
      return clonecore::Result<TsumugiRestoreLayoutIoReport>::failure(
          append_emergency_reinvalidation_failure(
              status.error(), emergency));
    }
    report_.committed_metadata_bytes += write.bytes.size();
  }
  committed_ = true;
  metadata_may_be_recognizable_ = false;
  report_.primary_partition_table_committed = true;
  report_.target_left_incomplete = false;
  return clonecore::Result<TsumugiRestoreLayoutIoReport>::success(report_);
}

void TsumugiWholeDiskRestoreLayoutTransaction::abort() noexcept {
  if (target_ != nullptr && prepared_ && !committed_) {
    // A staged GPT entry array is deliberately not recognizable, but it is
    // still a durable publication prefix that must not strand the next
    // restart. Re-zero every exact metadata write when a publication prefix
    // (or an unreadable/torn publication state) exists, including cancellation
    // or failure between staged writes. An already-all-zero layout stays
    // read-only on ordinary payload failure.
    try {
      const auto state =
          inspect_tsumugi_whole_disk_restore_layout_publication_v1(
              plan_, *target_);
      if (!state || state.value().state !=
              TsumugiRestoreLayoutPublicationStateV1::all_zero) {
        const auto emergency = reinvalidate_exact_metadata(plan_, *target_);
        metadata_may_be_recognizable_ = !emergency.has_value();
        report_.emergency_reinvalidation_verified = emergency.has_value();
        report_.target_left_incomplete = emergency.has_value();
      } else {
        // The previous emergency write may have completed and flushed even
        // when its first readback was unavailable or corrupted.  Observing
        // every exact metadata range as zero now closes that proof.  Preserve
        // the result across repeated abort calls, while an ordinary prepared
        // transaction (which never exposed metadata) remains unclassified as
        // emergency reinvalidation.
        report_.emergency_reinvalidation_verified =
            report_.emergency_reinvalidation_verified ||
            metadata_may_be_recognizable_;
        metadata_may_be_recognizable_ = false;
        report_.target_left_incomplete = true;
      }
    } catch (...) {
      report_.target_left_incomplete = false;
      report_.emergency_reinvalidation_verified = false;
    }
    (void)target_->flush_target();
    if (!metadata_may_be_recognizable_) {
      report_.target_left_incomplete = true;
    }
  }
  terminal_failure_ = true;
}

const TsumugiRestoreLayoutIoReport&
TsumugiWholeDiskRestoreLayoutTransaction::report() const noexcept {
  return report_;
}

TsumugiPreservingPartitionLayoutTransactionV1::
    TsumugiPreservingPartitionLayoutTransactionV1(
        TsumugiPreservingPartitionLayoutPlanV1 plan,
        clonecore::ITargetDiskWriter& target) noexcept
    : plan_(std::move(plan)), target_(&target) {}

clonecore::Result<TsumugiPreservingPartitionLayoutIoReportV1>
TsumugiPreservingPartitionLayoutTransactionV1::prepare(
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (target_ == nullptr || prepared_ || committed_ || terminal_failure_) {
    return failure<TsumugiPreservingPartitionLayoutIoReportV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi保持型レイアウト準備",
        L"単回transactionの対象または状態が不正です");
  }
  const auto valid = validate_preserving_plan(plan_, *target_);
  if (!valid) {
    terminal_failure_ = true;
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutIoReportV1>::failure(
        valid.error());
  }
  publish(
      callbacks,
      clonecore::DiskOperationStage::staging_partition_table,
      true);
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    terminal_failure_ = true;
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutIoReportV1>::failure(
        cancelled(L"Tsumugi保持型metadata準備前").error());
  }
  for (const auto& rollback_write : plan_.rollback_writes) {
    auto current = target_->read_back(
        rollback_write.offset, rollback_write.bytes.size());
    if (!current || current.value().size() != rollback_write.bytes.size() ||
        !std::equal(
            rollback_write.bytes.begin(),
            rollback_write.bytes.end(),
            current.value().begin())) {
      terminal_failure_ = true;
      return clonecore::Result<
          TsumugiPreservingPartitionLayoutIoReportV1>::failure(
          current
              ? io_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"Tsumugi保持型metadata準備照合",
                    L"同じ対象ハンドルの既存metadataが計画後に変化しました")
              : current.error());
    }
  }
  const auto original = verify_preserving_layout(
      plan_.original_layout,
      plan_.style,
      *target_,
      L"Tsumugi保持型既存レイアウト準備照合");
  if (!original) {
    terminal_failure_ = true;
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutIoReportV1>::failure(
        original.error());
  }
  prepared_ = true;
  report_.every_write_read_back_verified = true;
  report_.original_layout_preserved_after_failure = true;
  return clonecore::Result<
      TsumugiPreservingPartitionLayoutIoReportV1>::success(report_);
}

clonecore::Status
TsumugiPreservingPartitionLayoutTransactionV1::rollback() noexcept {
  report_.rollback_attempted = true;
  std::optional<clonecore::Error> first_error;
  for (const auto& write : plan_.rollback_writes) {
    try {
      const auto status = write_flush_verify(
          *target_,
          write.offset,
          write.bytes,
          L"Tsumugi保持型metadata rollback");
      if (!status) {
        if (!first_error.has_value()) {
          first_error = status.error();
        }
        continue;
      }
      report_.rollback_metadata_bytes += write.bytes.size();
    } catch (...) {
      if (!first_error.has_value()) {
        first_error = io_error(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"Tsumugi保持型metadata rollback",
            L"rollback処理中に例外が発生しました");
      }
    }
  }
  if (!first_error.has_value()) {
    try {
      const auto restored = verify_preserving_layout(
          plan_.original_layout,
          plan_.style,
          *target_,
          L"Tsumugi保持型rollback最終照合");
      if (!restored) {
        first_error = restored.error();
      }
    } catch (...) {
      first_error = io_error(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"Tsumugi保持型rollback最終照合",
          L"既存レイアウトの最終照合中に例外が発生しました");
    }
  }
  const bool success = !first_error.has_value();
  rollback_required_ = !success;
  report_.rollback_read_back_verified = success;
  report_.original_layout_preserved_after_failure = success;
  report_.target_left_incomplete = !success;
  return success
      ? clonecore::success_status()
      : clonecore::Status::failure(std::move(*first_error));
}

clonecore::Result<TsumugiPreservingPartitionLayoutIoReportV1>
TsumugiPreservingPartitionLayoutTransactionV1::commit(
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (target_ == nullptr || !prepared_ || committed_ || terminal_failure_) {
    return failure<TsumugiPreservingPartitionLayoutIoReportV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi保持型レイアウト確定",
        L"準備済みかつ未確定の単回transactionがありません");
  }
  publish(
      callbacks,
      clonecore::DiskOperationStage::committing_partition_table,
      true);
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    terminal_failure_ = true;
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutIoReportV1>::failure(
        cancelled(L"Tsumugi保持型partition table公開前").error());
  }

  publish(
      callbacks,
      clonecore::DiskOperationStage::committing_partition_table,
      false);
  rollback_required_ = true;
  for (const auto& write : plan_.published_writes) {
    const auto status = write_flush_verify(
        *target_,
        write.offset,
        write.bytes,
        L"Tsumugi保持型partition table公開");
    if (!status) {
      clonecore::Error primary = status.error();
      const auto restored = rollback();
      if (!restored) {
        primary.message.append(
            L"。既存partition tableのrollbackにも失敗しました: ")
            .append(restored.error().operation)
            .append(L" / ")
            .append(restored.error().message);
      }
      terminal_failure_ = true;
      report_.every_write_read_back_verified = false;
      return clonecore::Result<
          TsumugiPreservingPartitionLayoutIoReportV1>::failure(
          std::move(primary));
    }
    report_.published_metadata_bytes += write.bytes.size();
  }
  const auto final = verify_preserving_layout(
      plan_.target_layout,
      plan_.style,
      *target_,
      L"Tsumugi保持型partition table最終照合");
  if (!final) {
    clonecore::Error primary = final.error();
    const auto restored = rollback();
    if (!restored) {
      primary.message.append(
          L"。既存partition tableのrollbackにも失敗しました: ")
          .append(restored.error().operation)
          .append(L" / ")
          .append(restored.error().message);
    }
    terminal_failure_ = true;
    report_.every_write_read_back_verified = false;
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutIoReportV1>::failure(
        std::move(primary));
  }
  rollback_required_ = false;
  committed_ = true;
  report_.every_write_read_back_verified = true;
  report_.partition_table_committed = true;
  report_.original_layout_preserved_after_failure = false;
  report_.target_left_incomplete = false;
  return clonecore::Result<
      TsumugiPreservingPartitionLayoutIoReportV1>::success(report_);
}

void TsumugiPreservingPartitionLayoutTransactionV1::abort() noexcept {
  if (target_ != nullptr && rollback_required_ && !committed_) {
    static_cast<void>(rollback());
  } else if (target_ != nullptr) {
    try {
      static_cast<void>(target_->flush_target());
    } catch (...) {
      report_.target_left_incomplete = true;
    }
  }
  terminal_failure_ = true;
}

const TsumugiPreservingPartitionLayoutIoReportV1&
TsumugiPreservingPartitionLayoutTransactionV1::report() const noexcept {
  return report_;
}

TsumugiShrinkRestoreLayoutTransactionV1::
    TsumugiShrinkRestoreLayoutTransactionV1(
        TsumugiShrinkWholeDiskRestoreLayoutPlanV1 final_plan,
        std::vector<TsumugiShrinkConstructionLayoutPlanV1>
            construction_plans,
        clonecore::ITargetDiskWriter& target)
    : final_plan_(std::move(final_plan)),
      construction_plans_(std::move(construction_plans)),
      construction_states_(
          construction_plans_.size(), ConstructionState::pending),
      target_(&target),
      final_transaction_(final_plan_.metadata, target) {
  refresh_report();
}

clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>
TsumugiShrinkRestoreLayoutTransactionV1::prepare(
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (target_ == nullptr || prepared_ || final_committed_ ||
      terminal_failure_) {
    return failure<TsumugiShrinkRestoreLayoutIoReportV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi縮小レイアウト無効化開始",
        L"対象または単回トランザクション状態が不正です");
  }
  const auto valid = validate_shrink_transaction_plan(
      final_plan_, construction_plans_, *target_);
  if (!valid) {
    terminal_failure_ = true;
    refresh_report();
    return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::failure(
        valid.error());
  }
  combined_plan_validated_ = true;
  const auto prepared = final_transaction_.prepare(callbacks);
  if (!prepared) {
    terminal_failure_ = true;
    refresh_report();
    return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::failure(
        prepared.error());
  }
  prepared_ = true;
  report_.every_temporary_write_read_back_verified = true;
  refresh_report();
  return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::success(
      report_);
}

clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>
TsumugiShrinkRestoreLayoutTransactionV1::publish_construction(
    const std::uint32_t final_target_number,
    const clonecore::DiskOperationCallbacks& callbacks) {
  const auto index = find_construction(final_target_number);
  if (!combined_plan_validated_ || !prepared_ || final_committed_ ||
      terminal_failure_ || active_construction_index_.has_value() ||
      !index.has_value() ||
      construction_states_[*index] != ConstructionState::pending) {
    return failure<TsumugiShrinkRestoreLayoutIoReportV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi縮小仮GPT公開開始",
        L"無効化済みで未公開の仮GPTが1件だけ必要です");
  }

  publish(
      callbacks,
      clonecore::DiskOperationStage::staging_partition_table,
      true);
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    terminal_failure_ = true;
    refresh_report();
    return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::failure(
        cancelled(L"Tsumugi縮小仮GPT公開前").error());
  }

  active_construction_index_ = *index;
  construction_states_[*index] = ConstructionState::active;
  active_construction_published_ = false;
  temporary_cleanup_unverified_ = false;
  const auto& plan = construction_plans_[*index];

  // Every temporary metadata write is inside one non-cancellable boundary.
  // A valid backup/primary header must never be left behind because a caller
  // toggled cancellation between individual writes.
  publish(
      callbacks,
      clonecore::DiskOperationStage::committing_partition_table,
      false);
  std::optional<clonecore::Error> publication_error;
  const auto apply = [&](const TsumugiRestoreLayoutWrite& write) {
    if (publication_error.has_value()) {
      return;
    }
    const auto status = write_flush_verify(
        *target_,
        write.offset,
        write.bytes,
        L"Tsumugi縮小仮GPT公開");
    if (!status) {
      publication_error = status.error();
      return;
    }
    report_.temporary_metadata_published_bytes += write.bytes.size();
  };
  for (const auto& write : plan.temporary_metadata.staged_writes) {
    apply(write);
  }
  for (const auto& write : plan.temporary_metadata.commit_writes) {
    apply(write);
  }
  if (publication_error.has_value()) {
    std::uint64_t retired_bytes{};
    const auto cleanup = clear_exact_ranges(
        *target_,
        plan.retirement_ranges,
        L"Tsumugi縮小仮GPT公開失敗後退役",
        retired_bytes);
    report_.every_temporary_write_read_back_verified = false;
    if (cleanup) {
      report_.temporary_metadata_retired_bytes += retired_bytes;
      construction_states_[*index] = ConstructionState::pending;
      active_construction_index_.reset();
      temporary_cleanup_unverified_ = false;
    } else {
      temporary_cleanup_unverified_ = true;
    }
    terminal_failure_ = true;
    refresh_report();
    return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::failure(
        append_emergency_reinvalidation_failure(
            std::move(*publication_error), cleanup));
  }

  active_construction_published_ = true;
  refresh_report();
  return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::success(
      report_);
}

clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>
TsumugiShrinkRestoreLayoutTransactionV1::retire_construction(
    const std::uint32_t final_target_number,
    const clonecore::DiskOperationCallbacks& callbacks) {
  const auto index = find_construction(final_target_number);
  if (!prepared_ || final_committed_ || terminal_failure_ ||
      !active_construction_index_.has_value() || !index.has_value() ||
      *active_construction_index_ != *index ||
      construction_states_[*index] != ConstructionState::active ||
      !active_construction_published_) {
    return failure<TsumugiShrinkRestoreLayoutIoReportV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi縮小仮GPT退役開始",
        L"指定した公開中の仮GPTがありません");
  }

  // The platform must already have dismounted/offlined the temporary volume.
  // Once erasure begins, cancellation cannot interrupt the exact metadata
  // retirement sequence.
  publish(
      callbacks,
      clonecore::DiskOperationStage::invalidating_target,
      false);
  std::uint64_t retired_bytes{};
  const auto status = clear_exact_ranges(
      *target_,
      construction_plans_[*index].retirement_ranges,
      L"Tsumugi縮小仮GPT退役",
      retired_bytes);
  if (!status) {
    report_.every_temporary_write_read_back_verified = false;
    temporary_cleanup_unverified_ = true;
    terminal_failure_ = true;
    refresh_report();
    return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::failure(
        status.error());
  }
  report_.temporary_metadata_retired_bytes += retired_bytes;
  construction_states_[*index] = ConstructionState::retired;
  active_construction_index_.reset();
  active_construction_published_ = false;
  temporary_cleanup_unverified_ = false;
  refresh_report();
  return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::success(
      report_);
}

clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>
TsumugiShrinkRestoreLayoutTransactionV1::commit_final(
    const clonecore::DiskOperationCallbacks& callbacks) {
  const bool all_retired = std::all_of(
      construction_states_.begin(),
      construction_states_.end(),
      [](const auto state) { return state == ConstructionState::retired; });
  if (!prepared_ || final_committed_ || terminal_failure_ ||
      active_construction_index_.has_value() ||
      temporary_cleanup_unverified_ || !all_retired) {
    return failure<TsumugiShrinkRestoreLayoutIoReportV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Tsumugi縮小最終レイアウト確定開始",
        L"全仮GPTの退役確認後でなければ最終レイアウトを公開できません");
  }
  const auto committed = final_transaction_.commit(callbacks);
  if (!committed) {
    terminal_failure_ = true;
    refresh_report();
    return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::failure(
        committed.error());
  }
  final_committed_ = true;
  refresh_report();
  return clonecore::Result<TsumugiShrinkRestoreLayoutIoReportV1>::success(
      report_);
}

void TsumugiShrinkRestoreLayoutTransactionV1::abort() noexcept {
  if (target_ != nullptr && active_construction_index_.has_value()) {
    const auto index = *active_construction_index_;
    std::uint64_t retired_bytes{};
    const auto cleanup = clear_exact_ranges(
        *target_,
        construction_plans_[index].retirement_ranges,
        L"Tsumugi縮小仮GPT中止退役",
        retired_bytes);
    if (cleanup) {
      report_.temporary_metadata_retired_bytes += retired_bytes;
      construction_states_[index] = active_construction_published_
          ? ConstructionState::retired
          : ConstructionState::pending;
      active_construction_index_.reset();
      active_construction_published_ = false;
      temporary_cleanup_unverified_ = false;
    } else {
      report_.every_temporary_write_read_back_verified = false;
      temporary_cleanup_unverified_ = true;
    }
  }
  final_transaction_.abort();
  terminal_failure_ = true;
  refresh_report();
}

const TsumugiShrinkRestoreLayoutIoReportV1&
TsumugiShrinkRestoreLayoutTransactionV1::report() const noexcept {
  return report_;
}

const TsumugiShrinkConstructionLayoutPlanV1*
TsumugiShrinkRestoreLayoutTransactionV1::active_construction_plan()
    const noexcept {
  if (!active_construction_index_.has_value() ||
      *active_construction_index_ >= construction_plans_.size()) {
    return nullptr;
  }
  return &construction_plans_[*active_construction_index_];
}

std::optional<std::size_t>
TsumugiShrinkRestoreLayoutTransactionV1::find_construction(
    const std::uint32_t final_target_number) const noexcept {
  const auto found = std::find_if(
      construction_plans_.begin(),
      construction_plans_.end(),
      [&](const auto& plan) {
        return plan.final_target_number == final_target_number;
      });
  if (found == construction_plans_.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(
      std::distance(construction_plans_.begin(), found));
}

void TsumugiShrinkRestoreLayoutTransactionV1::refresh_report() noexcept {
  report_.final_layout = final_transaction_.report();
  report_.construction_layout_count = construction_plans_.size();
  report_.published_construction_layouts = 0U;
  report_.retired_construction_layouts = 0U;
  for (const auto state : construction_states_) {
    if (state == ConstructionState::retired) {
      ++report_.published_construction_layouts;
      ++report_.retired_construction_layouts;
    } else if (state == ConstructionState::active &&
               active_construction_published_) {
      ++report_.published_construction_layouts;
    }
  }
  if (active_construction_index_.has_value()) {
    report_.active_final_target_number =
        construction_plans_[*active_construction_index_]
            .final_target_number;
    report_.active_source_table_index =
        construction_plans_[*active_construction_index_].source_table_index;
  } else {
    report_.active_final_target_number.reset();
    report_.active_source_table_index.reset();
  }
  report_.temporary_layout_active =
      active_construction_index_.has_value() &&
      active_construction_published_ && !temporary_cleanup_unverified_;
  report_.final_partition_table_committed =
      report_.final_layout.primary_partition_table_committed;
  report_.target_left_incomplete =
      !report_.final_partition_table_committed &&
      report_.final_layout.target_left_incomplete;
  report_.metadata_safely_withheld =
      prepared_ && !report_.final_partition_table_committed &&
      !active_construction_index_.has_value() &&
      !temporary_cleanup_unverified_ &&
      report_.final_layout.target_left_incomplete;
}

}  // namespace ytec::imageformat

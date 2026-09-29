#include "ytec/bootrepair/mbr2gpt_layout.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace ytec::bootrepair {
namespace {

using clonecore::Error;
using clonecore::ErrorCode;
using clonecore::Result;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kAlignmentBytes = kMiB;
constexpr std::uint64_t kMbr2GptFrontBytes = 16ULL * 1024ULL;
constexpr std::uint64_t kMbr2GptBackBytes = 16ULL * 1024ULL;
constexpr std::uint64_t kEsp512Bytes = 200ULL * kMiB;
constexpr std::uint64_t kEsp4KnBytes = 300ULL * kMiB;
constexpr std::uint64_t kMsrBytes = 16ULL * kMiB;
constexpr std::uint64_t kMinimumRecoveryBytes = 990ULL * kMiB;
constexpr std::uint64_t kRecoveryFreeBytes = 250ULL * kMiB;
constexpr std::uint32_t kGptPartitionEntryCount = 128U;
constexpr std::uint32_t kGptPartitionEntrySize = 128U;
constexpr std::uint64_t kRecoveryGptAttributes =
    0x8000000000000001ULL;

Error layout_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
      .operation = L"MBR→GPT別ターゲット再配置計画",
      .message = std::move(message),
  };
}

Result<std::uint64_t> checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    const std::wstring_view description) {
  if (left > (std::numeric_limits<std::uint64_t>::max)() - right) {
    return Result<std::uint64_t>::failure(layout_error(
        ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(description) + L"が64ビット範囲を超えています"));
  }
  return Result<std::uint64_t>::success(left + right);
}

Result<std::uint64_t> align_up(
    const std::uint64_t value,
    const std::uint64_t alignment) {
  if (alignment == 0U) {
    return Result<std::uint64_t>::failure(layout_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"配置アラインメントが0です"));
  }
  const std::uint64_t remainder = value % alignment;
  if (remainder == 0U) {
    return Result<std::uint64_t>::success(value);
  }
  return checked_add(
      value, alignment - remainder, L"パーティション境界");
}

std::uint64_t align_down(
    const std::uint64_t value,
    const std::uint64_t alignment) noexcept {
  return alignment == 0U ? 0U : value - (value % alignment);
}

bool role_requires_ntfs(const Mbr2GptSourceRole role) noexcept {
  return role == Mbr2GptSourceRole::windows ||
         role == Mbr2GptSourceRole::recovery ||
         role == Mbr2GptSourceRole::data;
}

bool is_in_place_precheck_candidate(
    const Mbr2GptRebuildRequest& request,
    const std::vector<Mbr2GptSourcePartition>& partitions) noexcept {
  if (!request.firmware_supports_uefi ||
      !request.windows_is_amd64 ||
      !request.bitlocker_fully_decrypted ||
      partitions.empty() ||
      partitions.size() > 3U) {
    return false;
  }
  std::size_t active_count = 0;
  for (const auto& partition : partitions) {
    if (!partition.primary ||
        partition.role == Mbr2GptSourceRole::unsupported) {
      return false;
    }
    if (partition.active) {
      ++active_count;
      if (partition.role != Mbr2GptSourceRole::system_reserved &&
          partition.role != Mbr2GptSourceRole::windows) {
        return false;
      }
    }
  }
  if (active_count != 1U) {
    return false;
  }
  const std::uint64_t required_front =
      kMbr2GptFrontBytes +
      2ULL * request.logical_sector_size;
  const std::uint64_t required_back =
      kMbr2GptBackBytes + request.logical_sector_size;
  const auto& first = partitions.front();
  const auto& last = partitions.back();
  if (first.offset_bytes < required_front ||
      last.offset_bytes > request.source_disk_size_bytes ||
      last.size_bytes >
          request.source_disk_size_bytes - last.offset_bytes) {
    return false;
  }
  return request.source_disk_size_bytes -
             (last.offset_bytes + last.size_bytes) >=
         required_back;
}

Result<std::vector<Mbr2GptSourcePartition>>
validate_and_sort_source(const Mbr2GptRebuildRequest& request) {
  if ((request.logical_sector_size != 512U &&
       request.logical_sector_size != 4096U) ||
      request.source_disk_size_bytes < 4ULL * 1024ULL * kMiB ||
      request.target_disk_size_bytes < 4ULL * 1024ULL * kMiB ||
      request.source_disk_size_bytes %
              request.logical_sector_size !=
          0U ||
      request.target_disk_size_bytes %
              request.logical_sector_size !=
          0U ||
      request.source_partitions.empty() ||
      request.source_partitions.size() > 128U) {
    return Result<std::vector<Mbr2GptSourcePartition>>::failure(
        layout_error(
            ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"対応セクター、4GiB以上の媒体、1～128区画が必要です"));
  }
  if (!request.firmware_supports_uefi ||
      !request.windows_is_amd64 ||
      !request.bitlocker_fully_decrypted) {
    return Result<std::vector<Mbr2GptSourcePartition>>::failure(
        layout_error(
            ErrorCode::unsupported_platform,
            ERROR_NOT_SUPPORTED,
            L"UEFI対応、Windows x64、BitLocker完全復号を確認できないため計画しません"));
  }

  std::vector<Mbr2GptSourcePartition> partitions =
      request.source_partitions;
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const auto& left, const auto& right) {
        return left.offset_bytes < right.offset_bytes;
      });
  std::uint64_t previous_end = 0;
  std::vector<std::uint32_t> numbers;
  numbers.reserve(partitions.size());
  std::size_t windows_count = 0;
  std::size_t recovery_count = 0;
  std::size_t system_reserved_count = 0;
  for (const auto& partition : partitions) {
    if (partition.number == 0U ||
        partition.size_bytes == 0U ||
        partition.offset_bytes % request.logical_sector_size != 0U ||
        partition.size_bytes % request.logical_sector_size != 0U ||
        partition.offset_bytes < previous_end ||
        partition.offset_bytes > request.source_disk_size_bytes ||
        partition.size_bytes >
            request.source_disk_size_bytes - partition.offset_bytes ||
        std::find(
            numbers.begin(), numbers.end(), partition.number) !=
            numbers.end()) {
      return Result<std::vector<Mbr2GptSourcePartition>>::failure(
          layout_error(
              ErrorCode::invalid_data,
              ERROR_INVALID_DATA,
              L"コピー元区画の番号、範囲、整列、重複が不正です"));
    }
    numbers.push_back(partition.number);
    previous_end = partition.offset_bytes + partition.size_bytes;
    if (partition.role == Mbr2GptSourceRole::unsupported ||
        !partition.primary) {
      return Result<std::vector<Mbr2GptSourcePartition>>::failure(
          layout_error(
              ErrorCode::unsupported_platform,
              ERROR_NOT_SUPPORTED,
              L"未知/OEM区画または拡張・論理区画を推測して再配置しません"));
    }
    if (role_requires_ntfs(partition.role) &&
        partition.file_system != Mbr2GptFileSystem::ntfs) {
      return Result<std::vector<Mbr2GptSourcePartition>>::failure(
          layout_error(
              ErrorCode::unsupported_platform,
              ERROR_NOT_SUPPORTED,
              L"Windows、回復、データ区画は確認済みNTFSだけを扱います"));
    }
    if (partition.role == Mbr2GptSourceRole::system_reserved &&
        partition.file_system != Mbr2GptFileSystem::ntfs &&
        partition.file_system != Mbr2GptFileSystem::fat32) {
      return Result<std::vector<Mbr2GptSourcePartition>>::failure(
          layout_error(
              ErrorCode::unsupported_platform,
              ERROR_NOT_SUPPORTED,
              L"システム予約区画のファイルシステムを確認できません"));
    }
    windows_count +=
        partition.role == Mbr2GptSourceRole::windows ? 1U : 0U;
    recovery_count +=
        partition.role == Mbr2GptSourceRole::recovery ? 1U : 0U;
    system_reserved_count +=
        partition.role == Mbr2GptSourceRole::system_reserved ? 1U : 0U;
  }
  if (windows_count != 1U || recovery_count > 1U ||
      system_reserved_count > 1U) {
    return Result<std::vector<Mbr2GptSourcePartition>>::failure(
        layout_error(
            ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Windows区画は1個、システム予約/回復区画は各0～1個でなければなりません"));
  }
  return Result<std::vector<Mbr2GptSourcePartition>>::success(
      std::move(partitions));
}

const clonecore::GptGuid& type_guid_for(
    const PlannedGptPartitionRole role) noexcept {
  switch (role) {
    case PlannedGptPartitionRole::efi_system:
      return clonecore::gpt_type_efi_system();
    case PlannedGptPartitionRole::microsoft_reserved:
      return clonecore::gpt_type_microsoft_reserved();
    case PlannedGptPartitionRole::windows:
    case PlannedGptPartitionRole::data:
      return clonecore::gpt_type_basic_data();
    case PlannedGptPartitionRole::recovery:
      return clonecore::gpt_type_windows_recovery();
  }
  return clonecore::gpt_type_basic_data();
}

std::u16string partition_name_for(
    const PlannedGptPartitionRole role) {
  switch (role) {
    case PlannedGptPartitionRole::efi_system:
      return u"EFI システム";
    case PlannedGptPartitionRole::microsoft_reserved:
      return u"Microsoft 予約";
    case PlannedGptPartitionRole::windows:
      return u"Windows";
    case PlannedGptPartitionRole::recovery:
      return u"Windows RE ツール";
    case PlannedGptPartitionRole::data:
      return u"データ";
  }
  return u"";
}

bool action_matches_role(
    const PlannedGptPartition& partition) noexcept {
  switch (partition.role) {
    case PlannedGptPartitionRole::efi_system:
      return partition.action ==
                 PlannedGptPartitionAction::create_fat32 &&
             partition.source_partition_number == 0U;
    case PlannedGptPartitionRole::microsoft_reserved:
      return partition.action ==
                 PlannedGptPartitionAction::create_reserved &&
             partition.source_partition_number == 0U;
    case PlannedGptPartitionRole::windows:
    case PlannedGptPartitionRole::data:
      return partition.action ==
                 PlannedGptPartitionAction::copy_source_contents &&
             partition.source_partition_number != 0U;
    case PlannedGptPartitionRole::recovery:
      return (partition.action ==
                  PlannedGptPartitionAction::copy_source_contents &&
              partition.source_partition_number != 0U) ||
             (partition.action ==
                  PlannedGptPartitionAction::create_and_stage_winre &&
              partition.source_partition_number == 0U);
  }
  return false;
}

}  // namespace

Result<Mbr2GptRebuildPlan> plan_mbr2gpt_rebuild_to_new_target(
    const Mbr2GptRebuildRequest& request) {
  auto source = validate_and_sort_source(request);
  if (!source) {
    return Result<Mbr2GptRebuildPlan>::failure(source.error());
  }
  const auto windows = std::find_if(
      source.value().begin(),
      source.value().end(),
      [](const auto& partition) {
        return partition.role == Mbr2GptSourceRole::windows;
      });
  const auto recovery = std::find_if(
      source.value().begin(),
      source.value().end(),
      [](const auto& partition) {
        return partition.role == Mbr2GptSourceRole::recovery;
      });
  const bool has_recovery = recovery != source.value().end();

  if (request.require_recovery_tools) {
    if (request.winre_state == WinReSourceState::missing ||
        request.winre_state == WinReSourceState::unknown ||
        request.winre_image_size_bytes == 0U) {
      return Result<Mbr2GptRebuildPlan>::failure(layout_error(
          ErrorCode::verification_failed,
          ERROR_NOT_FOUND,
          L"WinREイメージと登録状態を確認できないため、回復領域を推測作成しません"));
    }
    if (request.winre_state ==
            WinReSourceState::registered_partition &&
        (!has_recovery ||
         request.registered_winre_partition_number != recovery->number)) {
      return Result<Mbr2GptRebuildPlan>::failure(layout_error(
          ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"登録済みWinREが列挙した回復区画と一致しません"));
    }
  }

  const auto recovery_content_with_free = checked_add(
      request.winre_image_size_bytes,
      kRecoveryFreeBytes,
      L"WinREイメージと必要空き容量");
  if (!recovery_content_with_free) {
    return Result<Mbr2GptRebuildPlan>::failure(
        recovery_content_with_free.error());
  }
  const auto aligned_recovery = align_up(
      (std::max)(
          kMinimumRecoveryBytes,
          recovery_content_with_free.value()),
      kAlignmentBytes);
  if (!aligned_recovery) {
    return Result<Mbr2GptRebuildPlan>::failure(
        aligned_recovery.error());
  }
  const bool reuse_recovery_contents =
      request.require_recovery_tools &&
      request.winre_state ==
          WinReSourceState::registered_partition &&
      has_recovery &&
      recovery->size_bytes >= aligned_recovery.value();
  std::uint64_t recovery_size =
      request.require_recovery_tools ? aligned_recovery.value() : 0U;
  if (reuse_recovery_contents) {
    const auto aligned_existing =
        align_up(recovery->size_bytes, kAlignmentBytes);
    if (!aligned_existing) {
      return Result<Mbr2GptRebuildPlan>::failure(
          aligned_existing.error());
    }
    recovery_size =
        (std::max)(recovery_size, aligned_existing.value());
  }

  const std::uint64_t esp_size =
      request.logical_sector_size == 4096U
          ? kEsp4KnBytes
          : kEsp512Bytes;
  const auto windows_size =
      align_up(windows->size_bytes, kAlignmentBytes);
  if (!windows_size) {
    return Result<Mbr2GptRebuildPlan>::failure(
        windows_size.error());
  }
  auto required_partition_bytes = checked_add(
      esp_size, kMsrBytes, L"ESPとMSRの合計容量");
  if (required_partition_bytes) {
    required_partition_bytes = checked_add(
        required_partition_bytes.value(),
        windows_size.value(),
        L"ESP、MSR、Windowsの合計容量");
  }
  if (required_partition_bytes) {
    required_partition_bytes = checked_add(
        required_partition_bytes.value(),
        recovery_size,
        L"固定GPT区画の合計容量");
  }
  if (!required_partition_bytes) {
    return Result<Mbr2GptRebuildPlan>::failure(
        required_partition_bytes.error());
  }
  std::vector<Mbr2GptSourcePartition> data_partitions;
  for (const auto& partition : source.value()) {
    if (partition.role != Mbr2GptSourceRole::data) {
      continue;
    }
    const auto size = align_up(partition.size_bytes, kAlignmentBytes);
    if (!size) {
      return Result<Mbr2GptRebuildPlan>::failure(size.error());
    }
    const auto total = checked_add(
        required_partition_bytes.value(),
        size.value(),
        L"再配置先区画の合計容量");
    if (!total) {
      return Result<Mbr2GptRebuildPlan>::failure(total.error());
    }
    required_partition_bytes =
        Result<std::uint64_t>::success(total.value());
    data_partitions.push_back(partition);
  }

  if (request.target_disk_size_bytes <= 2U * kAlignmentBytes) {
    return Result<Mbr2GptRebuildPlan>::failure(layout_error(
        ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"GPTメタデータ用の先頭/末尾余白を確保できません"));
  }
  const std::uint64_t target_limit = align_down(
      request.target_disk_size_bytes - kAlignmentBytes,
      kAlignmentBytes);
  const std::uint64_t available_partition_bytes =
      target_limit - kAlignmentBytes;
  if (required_partition_bytes.value() >
      available_partition_bytes) {
    return Result<Mbr2GptRebuildPlan>::failure(layout_error(
        ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"ESP、MSR、Windows、回復、データ区画を安全に収容できません"));
  }
  const std::uint64_t extra_windows_bytes = align_down(
      available_partition_bytes - required_partition_bytes.value(),
      kAlignmentBytes);
  const auto expanded_windows_size = checked_add(
      windows_size.value(),
      extra_windows_bytes,
      L"拡張後Windows区画");
  if (!expanded_windows_size) {
    return Result<Mbr2GptRebuildPlan>::failure(
        expanded_windows_size.error());
  }

  Mbr2GptRebuildPlan plan;
  plan.alignment_bytes = kAlignmentBytes;
  plan.microsoft_in_place_precheck_passed =
      is_in_place_precheck_candidate(request, source.value());
  plan.system_partition_replaced_by_esp =
      std::any_of(
          source.value().begin(),
          source.value().end(),
          [](const auto& partition) {
            return partition.role ==
                   Mbr2GptSourceRole::system_reserved;
          });
  plan.recovery_partition_created =
      request.require_recovery_tools && !reuse_recovery_contents;
  plan.winre_registration_required =
      request.require_recovery_tools;

  const auto windows_position =
      static_cast<std::size_t>(
          std::distance(source.value().begin(), windows));
  const auto recovery_position =
      has_recovery
          ? static_cast<std::size_t>(
                std::distance(source.value().begin(), recovery))
          : source.value().size();
  plan.recovery_order_changed =
      request.require_recovery_tools &&
      (!has_recovery ||
       recovery_position != windows_position + 1U);

  std::uint64_t cursor = kAlignmentBytes;
  const auto append_partition =
      [&plan, &cursor](
          const PlannedGptPartitionRole role,
          const PlannedGptPartitionAction action,
          const std::uint32_t source_number,
          const std::uint64_t size) {
        plan.target_partitions.push_back(PlannedGptPartition{
            .target_number = static_cast<std::uint32_t>(
                plan.target_partitions.size() + 1U),
            .role = role,
            .action = action,
            .source_partition_number = source_number,
            .offset_bytes = cursor,
            .size_bytes = size,
        });
        cursor += size;
      };
  append_partition(
      PlannedGptPartitionRole::efi_system,
      PlannedGptPartitionAction::create_fat32,
      0U,
      esp_size);
  append_partition(
      PlannedGptPartitionRole::microsoft_reserved,
      PlannedGptPartitionAction::create_reserved,
      0U,
      kMsrBytes);
  append_partition(
      PlannedGptPartitionRole::windows,
      PlannedGptPartitionAction::copy_source_contents,
      windows->number,
      expanded_windows_size.value());
  if (request.require_recovery_tools) {
    append_partition(
        PlannedGptPartitionRole::recovery,
        reuse_recovery_contents
            ? PlannedGptPartitionAction::copy_source_contents
            : PlannedGptPartitionAction::create_and_stage_winre,
        reuse_recovery_contents ? recovery->number : 0U,
        recovery_size);
  }
  for (const auto& data : data_partitions) {
    const auto size = align_up(data.size_bytes, kAlignmentBytes);
    if (!size) {
      return Result<Mbr2GptRebuildPlan>::failure(size.error());
    }
    append_partition(
        PlannedGptPartitionRole::data,
        PlannedGptPartitionAction::copy_source_contents,
        data.number,
        size.value());
  }
  if (cursor > request.target_disk_size_bytes) {
    return Result<Mbr2GptRebuildPlan>::failure(layout_error(
        ErrorCode::internal_error,
        ERROR_ARITHMETIC_OVERFLOW,
        L"完成配置がコピー先容量を超えました"));
  }
  plan.unallocated_tail_bytes =
      request.target_disk_size_bytes - cursor;
  plan.notes = {
      L"コピー元MBRディスクは変更せず、別の空ターゲットだけを構築します",
      L"ESP/MSR/Windows/回復/データの順を固定し、未知区画は推測しません",
      L"BCDBoot、WinRE登録、GPT再読込み、コピー先単独UEFI起動の成功後だけ完成扱いにします",
      L"in-place候補表示は予備判定であり、Microsoft MBR2GPT /validateの代わりにはなりません",
  };
  return Result<Mbr2GptRebuildPlan>::success(std::move(plan));
}

Result<clonecore::GptWritePlan> make_mbr2gpt_gpt_metadata_plan(
    const Mbr2GptRebuildPlan& layout,
    const std::uint64_t target_disk_size_bytes,
    const std::uint32_t target_sector_size,
    clonecore::IGuidGenerator& guid_generator) {
  if ((target_sector_size != 512U &&
       target_sector_size != 4096U) ||
      target_disk_size_bytes < 4ULL * 1024ULL * kMiB ||
      target_disk_size_bytes % target_sector_size != 0U ||
      layout.alignment_bytes == 0U ||
      layout.target_partitions.size() < 3U ||
      layout.target_partitions.size() >
          kGptPartitionEntryCount ||
      !layout.source_disk_remains_unchanged) {
    return Result<clonecore::GptWritePlan>::failure(layout_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"GPTメタデータ化に必要な媒体寸法または安全条件が不正です"));
  }

  bool recovery_seen = false;
  bool data_seen = false;
  std::uint64_t previous_end = 0U;
  clonecore::GptDisk disk{
      .logical_sector_size = target_sector_size,
      .sector_count =
          target_disk_size_bytes / target_sector_size,
      .first_usable_lba = 0U,
      .last_usable_lba = 0U,
      .partition_entry_count = kGptPartitionEntryCount,
      .partition_entry_size = kGptPartitionEntrySize,
  };
  const std::uint64_t entry_bytes =
      static_cast<std::uint64_t>(kGptPartitionEntryCount) *
      kGptPartitionEntrySize;
  const std::uint64_t entry_sectors =
      (entry_bytes + target_sector_size - 1U) /
      target_sector_size;
  disk.first_usable_lba = 2U + entry_sectors;
  disk.last_usable_lba =
      disk.sector_count - 2U - entry_sectors;
  disk.partitions.reserve(layout.target_partitions.size());

  for (std::size_t index = 0;
       index < layout.target_partitions.size();
       ++index) {
    const auto& partition = layout.target_partitions[index];
    const auto required_role =
        index == 0U
            ? PlannedGptPartitionRole::efi_system
            : index == 1U
                  ? PlannedGptPartitionRole::microsoft_reserved
                  : index == 2U
                        ? PlannedGptPartitionRole::windows
                        : partition.role;
    const bool trailing_role_valid =
        index < 3U ||
        (partition.role == PlannedGptPartitionRole::recovery &&
         !recovery_seen && !data_seen) ||
        partition.role == PlannedGptPartitionRole::data;
    if (partition.target_number != index + 1U ||
        partition.role != required_role ||
        !trailing_role_valid ||
        (recovery_seen &&
         partition.role == PlannedGptPartitionRole::recovery) ||
        !action_matches_role(partition) ||
        partition.offset_bytes % target_sector_size != 0U ||
        partition.size_bytes == 0U ||
        partition.size_bytes % target_sector_size != 0U ||
        partition.offset_bytes < previous_end ||
        partition.offset_bytes > target_disk_size_bytes ||
        partition.size_bytes >
            target_disk_size_bytes - partition.offset_bytes) {
      return Result<clonecore::GptWritePlan>::failure(layout_error(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"GPT区画の順序、役割、動作、範囲を安全にメタデータ化できません"));
    }
    recovery_seen =
        recovery_seen ||
        partition.role == PlannedGptPartitionRole::recovery;
    data_seen =
        data_seen ||
        partition.role == PlannedGptPartitionRole::data;
    previous_end =
        partition.offset_bytes + partition.size_bytes;
    const std::uint64_t first_lba =
        partition.offset_bytes / target_sector_size;
    const std::uint64_t last_lba =
        previous_end / target_sector_size - 1U;
    if (first_lba < disk.first_usable_lba ||
        last_lba > disk.last_usable_lba) {
      return Result<clonecore::GptWritePlan>::failure(layout_error(
          ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"GPTヘッダーと区画配列の安全領域を確保できません"));
    }
    disk.partitions.push_back(clonecore::GptPartition{
        .entry_index = static_cast<std::uint32_t>(index),
        .type_guid = type_guid_for(partition.role),
        .first_lba = first_lba,
        .last_lba = last_lba,
        .attributes =
            partition.role == PlannedGptPartitionRole::recovery
                ? kRecoveryGptAttributes
                : 0U,
        .name = partition_name_for(partition.role),
    });
  }
  if (recovery_seen != layout.winre_registration_required) {
    return Result<clonecore::GptWritePlan>::failure(layout_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows RE登録要否と回復区画の有無が一致しません"));
  }
  return clonecore::make_gpt_write_plan(
      disk,
      target_disk_size_bytes,
      target_sector_size,
      guid_generator);
}

std::wstring_view planned_gpt_partition_role_name(
    const PlannedGptPartitionRole role) noexcept {
  switch (role) {
    case PlannedGptPartitionRole::efi_system:
      return L"EFIシステム";
    case PlannedGptPartitionRole::microsoft_reserved:
      return L"Microsoft予約";
    case PlannedGptPartitionRole::windows:
      return L"Windows";
    case PlannedGptPartitionRole::recovery:
      return L"回復ツール";
    case PlannedGptPartitionRole::data:
      return L"データ";
  }
  return L"不明";
}

}  // namespace ytec::bootrepair

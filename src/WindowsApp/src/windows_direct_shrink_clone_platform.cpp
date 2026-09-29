#include "ytec/windowsapp/windows_direct_shrink_clone_platform.h"

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/clone_boot_finalization.h"
#include "ytec/bootrepair/efi_boot_ownership.h"
#include "ytec/bootrepair/system_volume_mount.h"
#include "ytec/bootrepair/winre_diagnostic.h"
#include "ytec/bootrepair/winre_registration.h"
#include "ytec/clonecore/unique_handle.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/windowsdism/dism.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::size_t kHashBlockBytes = 4U * 1024U * 1024U;
constexpr std::uint32_t kDismTimeoutMilliseconds =
    6U * 60U * 60U * 1000U;
constexpr std::size_t kCheckpointBytes = static_cast<std::size_t>(
    kWindowsDirectShrinkCheckpointRecordBytes);
static_assert(kCheckpointBytes == 4096U);
constexpr std::uint64_t kMinimumDismScratchFreeBytes = 512ULL * kMiB;
constexpr std::uint64_t kTemporaryNoDefaultDriveLetter =
    0x8000000000000000ULL;
constexpr std::uint64_t kRecoveryGptAttributes =
    kTemporaryNoDefaultDriveLetter | 0x0000000000000001ULL;
constexpr std::array<std::byte, 8U> kWimSignature{
    static_cast<std::byte>('M'), static_cast<std::byte>('S'),
    static_cast<std::byte>('W'), static_cast<std::byte>('I'),
    static_cast<std::byte>('M'), std::byte{0}, std::byte{0}, std::byte{0},
};

clonecore::Error platform_error(
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
  return clonecore::Result<T>::failure(platform_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(platform_error(
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

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool equal_path(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
  if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
      right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return false;
  }
  return CompareStringOrdinal(
             left.data(),
             static_cast<int>(left.size()),
             right.data(),
             static_cast<int>(right.size()),
             TRUE) == CSTR_EQUAL;
}

void append_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

clonecore::Status write_flush_readback(
    clonecore::ITargetDiskWriter& writer,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes,
    const std::wstring_view operation) {
  auto status = writer.write_target(offset, bytes);
  if (status) {
    status = writer.flush_target();
  }
  if (!status) {
    return status;
  }
  auto observed = writer.read_back(offset, bytes.size());
  if (!observed) {
    return clonecore::Status::failure(observed.error());
  }
  if (observed.value().size() != bytes.size() ||
      !std::equal(bytes.begin(), bytes.end(), observed.value().begin())) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        std::wstring(operation),
        L"書込み直後の完全読戻しが一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status invalidate_initial_partition_metadata(
    clonecore::ITargetDiskWriter& writer) {
  if (writer.logical_sector_size() != 512U ||
      writer.size_bytes() < 2U * kMiB ||
      writer.size_bytes() % writer.logical_sector_size() != 0U) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小コピー先メタデータ無効化",
        L"512-byte論理セクターと2MiB以上の対象が必要です");
  }
  std::vector<std::byte> zeroes(static_cast<std::size_t>(kMiB), std::byte{0});
  auto status = write_flush_readback(
      writer, 0U, zeroes, L"直接縮小コピー先先頭無効化");
  if (!status) {
    return status;
  }
  return write_flush_readback(
      writer,
      writer.size_bytes() - kMiB,
      zeroes,
      L"直接縮小コピー先末尾無効化");
}

std::optional<std::size_t> gpt_write_order(
    const clonecore::GptMetadataKind kind) noexcept {
  switch (kind) {
    case clonecore::GptMetadataKind::primary_entries:
      return 0U;
    case clonecore::GptMetadataKind::backup_entries:
      return 1U;
    case clonecore::GptMetadataKind::backup_header:
      return 2U;
    case clonecore::GptMetadataKind::protective_mbr:
      return 3U;
    case clonecore::GptMetadataKind::primary_header_commit:
      return 4U;
  }
  return std::nullopt;
}

clonecore::Status validate_gpt_write_plan(
    const clonecore::GptWritePlan& plan,
    const std::uint64_t expected_size) {
  if (plan.target_disk.logical_sector_size != 512U ||
      expected_size % 512U != 0U ||
      plan.target_disk.sector_count != expected_size / 512U ||
      plan.writes.size() != 5U) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小GPT書込み計画",
        L"対象寸法、論理セクター、または固定5段階書込みが不正です");
  }
  std::array<bool, 5U> seen{};
  for (const auto& write : plan.writes) {
    const auto order = gpt_write_order(write.kind);
    std::uint64_t end{};
    if (!order || seen[*order] || write.bytes.empty() ||
        write.offset % 512U != 0U || write.bytes.size() % 512U != 0U ||
        !checked_add(write.offset, write.bytes.size(), end) ||
        end > expected_size) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小GPT書込み計画",
          L"重複、未整列、空、または範囲外のGPT書込みがあります");
    }
    seen[*order] = true;
  }
  if (!std::all_of(seen.begin(), seen.end(), [](const bool value) {
        return value;
      })) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小GPT書込み計画",
        L"固定GPT書込み種別が揃っていません");
  }
  return clonecore::success_status();
}

// Once the fixed checkpoint has been created at 64 KiB, broad first-MiB
// invalidation is forbidden. Invalidate only the five exact GPT metadata
// ranges represented by a validated plan so the durable incomplete marker is
// preserved across construction-layout transitions and abort cleanup.
clonecore::Status invalidate_exact_gpt_metadata(
    clonecore::ITargetDiskWriter& writer,
    const clonecore::GptWritePlan& reference_plan) {
  auto status = validate_gpt_write_plan(reference_plan, writer.size_bytes());
  if (!status) {
    return status;
  }
  for (const auto& write : reference_plan.writes) {
    std::vector<std::byte> zeroes(write.bytes.size(), std::byte{0});
    status = write_flush_readback(
        writer,
        write.offset,
        zeroes,
        L"直接縮小exact GPTメタデータ無効化");
    if (!status) {
      return status;
    }
  }
  return clonecore::success_status();
}

clonecore::Status publish_gpt_plan(
    clonecore::ITargetDiskWriter& writer,
    const clonecore::GptWritePlan& plan,
    const std::wstring_view operation) {
  auto valid = validate_gpt_write_plan(plan, writer.size_bytes());
  if (!valid) {
    return valid;
  }
  std::array<const clonecore::GptMetadataWrite*, 5U> ordered{};
  for (const auto& write : plan.writes) {
    ordered[*gpt_write_order(write.kind)] = &write;
  }
  for (const auto* write : ordered) {
    auto status = write_flush_readback(
        writer, write->offset, write->bytes, operation);
    if (!status) {
      static_cast<void>(invalidate_exact_gpt_metadata(writer, plan));
      return status;
    }
  }
  return clonecore::success_status();
}

clonecore::Status verify_gpt_plan(
    clonecore::ITargetDiskWriter& writer,
    const clonecore::GptWritePlan& plan) {
  auto valid = validate_gpt_write_plan(plan, writer.size_bytes());
  if (!valid) {
    return valid;
  }
  for (const auto& write : plan.writes) {
    auto observed = writer.read_back(write.offset, write.bytes.size());
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    if (observed.value().size() != write.bytes.size() ||
        !std::equal(
            write.bytes.begin(), write.bytes.end(), observed.value().begin())) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"直接縮小GPT完全読戻し",
          L"現在のGPTはこの操作が固定した一時または最終計画と一致しません");
    }
  }
  return clonecore::success_status();
}

clonecore::Status verify_mbr_sector(
    clonecore::ITargetDiskWriter& writer,
    const std::span<const std::byte> expected,
    const std::wstring_view operation) {
  if (writer.logical_sector_size() != 512U || expected.size() != 512U) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(operation),
        L"MBR sector0は512 bytesでなければなりません");
  }
  auto observed = writer.read_back(0U, expected.size());
  if (!observed) {
    return clonecore::Status::failure(observed.error());
  }
  if (observed.value().size() != expected.size() ||
      !std::equal(expected.begin(), expected.end(), observed.value().begin())) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        std::wstring(operation),
        L"現在のraw MBR sector0が不変計画と一致しません");
  }
  return clonecore::success_status();
}

class FixedMbrSignatureGenerator final
    : public clonecore::IMbrSignatureGenerator {
 public:
  explicit FixedMbrSignatureGenerator(const std::uint32_t signature) noexcept
      : signature_(signature) {}

  clonecore::Result<std::uint32_t> next_signature() override {
    return clonecore::Result<std::uint32_t>::success(signature_);
  }

 private:
  std::uint32_t signature_{};
};

clonecore::Result<clonecore::MbrWritePlan> build_final_mbr(
    const WindowsDirectShrinkClonePlan& plan) {
  const auto& binding = plan.mbr_preserve_binding();
  if (!binding.has_value() || binding->target_disk_signature == 0U ||
      plan.expected_target().logical_sector_size != 512U ||
      plan.expected_target().size_bytes % 512U != 0U ||
      plan.tasks().empty() || plan.tasks().size() > 4U) {
    return failure<clonecore::MbrWritePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小最終MBR計画",
        L"immutable raw MBR binding、512-byte対象、またはprimary partition件数が不正です");
  }

  clonecore::MbrDisk synthetic{
      .logical_sector_size = 512U,
      .sector_count = plan.expected_target().size_bytes / 512U,
      .disk_signature = binding->source_disk_signature,
      .bootstrap = binding->source_bootstrap,
  };
  synthetic.partitions.reserve(plan.tasks().size());
  std::array<bool, 4U> used{};
  for (const auto& task : plan.tasks()) {
    const std::uint64_t first_lba = task.target_offset_bytes / 512U;
    const std::uint64_t sector_count = task.target_size_bytes / 512U;
    std::uint64_t end_lba{};
    if (task.target_number == 0U || task.target_number > used.size() ||
        used[task.target_number - 1U] ||
        task.target_offset_bytes % 512U != 0U ||
        task.target_size_bytes == 0U || task.target_size_bytes % 512U != 0U ||
        first_lba > (std::numeric_limits<std::uint32_t>::max)() ||
        sector_count > (std::numeric_limits<std::uint32_t>::max)() ||
        !checked_add(first_lba, sector_count, end_lba) ||
        end_lba > synthetic.sector_count ||
        (task.role != migrationcore::MigrationPartitionRole::recovery &&
         task.role != migrationcore::MigrationPartitionRole::windows &&
         task.role != migrationcore::MigrationPartitionRole::bios_system &&
         task.role != migrationcore::MigrationPartitionRole::data)) {
      return failure<clonecore::MbrWritePlan>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"直接縮小最終MBR primary extent",
          L"partition番号、32bit LBA、extent、またはMBR roleが対応範囲外です");
    }
    for (const auto& existing : synthetic.partitions) {
      const std::uint64_t existing_end =
          static_cast<std::uint64_t>(existing.first_lba) +
          existing.sector_count;
      if (first_lba < existing_end && existing.first_lba < end_lba) {
        return failure<clonecore::MbrWritePlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小最終MBR primary重複",
            L"最終MBR primary partitionのextentが重複しています");
      }
    }
    used[task.target_number - 1U] = true;
    synthetic.partitions.push_back(clonecore::MbrPartition{
        .table_index = static_cast<std::uint8_t>(task.target_number - 1U),
        .active = task.active,
        .first_chs = {std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}},
        .type = task.kind ==
                WindowsDirectShrinkPartitionTaskKind::copy_exact_raw
            ? std::to_integer<std::uint8_t>(task.source_partition_type[0])
            : task.role == migrationcore::MigrationPartitionRole::recovery
                  ? static_cast<std::uint8_t>(0x27U)
                  : static_cast<std::uint8_t>(0x07U),
        .last_chs = {std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}},
        .first_lba = static_cast<std::uint32_t>(first_lba),
        .sector_count = static_cast<std::uint32_t>(sector_count),
    });
  }

  FixedMbrSignatureGenerator generator(binding->target_disk_signature);
  auto result = clonecore::make_mbr_write_plan(
      synthetic,
      plan.expected_target().size_bytes,
      512U,
      generator,
      {},
      plan.boot_finalization_required());
  if (!result) {
    return result;
  }
  const std::size_t active_count = static_cast<std::size_t>(std::count_if(
      result.value().target_disk.partitions.begin(),
      result.value().target_disk.partitions.end(),
      [](const clonecore::MbrPartition& partition) {
        return partition.active;
      }));
  if (result.value().sector.size() != 512U ||
      result.value().target_disk.disk_signature !=
          binding->target_disk_signature ||
      result.value().target_disk.bootstrap != binding->source_bootstrap ||
      active_count != (plan.boot_finalization_required() ? 1U : 0U)) {
    return failure<clonecore::MbrWritePlan>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"直接縮小最終MBR生成証跡",
        L"source bootstrap、fresh signature、sector0、またはActive件数が不変計画と一致しません");
  }
  return result;
}

clonecore::Result<std::vector<std::byte>> build_hidden_final_mbr_sector(
    const clonecore::MbrWritePlan& final_plan) {
  if (final_plan.sector.size() != 512U) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小hidden MBR sector0",
        L"最終MBR sector0が512 bytesではありません");
  }
  std::vector<std::byte> hidden = final_plan.sector;
  std::fill_n(hidden.begin(), 440U, std::byte{0});
  for (std::size_t index = 0U; index < 4U; ++index) {
    hidden[446U + index * 16U] = std::byte{0};
  }
  if (hidden[510] != std::byte{0x55} || hidden[511] != std::byte{0xAA} ||
      !std::all_of(hidden.begin(), hidden.begin() + 440, [](const auto value) {
        return value == std::byte{0};
      })) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"直接縮小hidden MBR生成証跡",
        L"zero bootstrap、inactive entries、または0x55AAを固定できません");
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(hidden));
}

std::u16string task_name(const WindowsDirectShrinkPartitionTask& task) {
  switch (task.role) {
    case migrationcore::MigrationPartitionRole::efi_system:
      return u"SYSTEM";
    case migrationcore::MigrationPartitionRole::microsoft_reserved:
      return u"MSR";
    case migrationcore::MigrationPartitionRole::windows:
      return u"Windows";
    case migrationcore::MigrationPartitionRole::recovery:
      return u"Recovery";
    case migrationcore::MigrationPartitionRole::bios_system:
      return u"System";
    case migrationcore::MigrationPartitionRole::data:
      break;
  }
  std::u16string result = u"Data ";
  const auto number = std::to_wstring(task.target_number);
  for (const wchar_t value : number) {
    result.push_back(static_cast<char16_t>(value));
  }
  return result;
}

clonecore::GptGuid task_type_guid(
    const WindowsDirectShrinkPartitionTask& task) noexcept {
  if (task.kind ==
      WindowsDirectShrinkPartitionTaskKind::copy_exact_raw) {
    return clonecore::GptGuid{.bytes = task.source_partition_type};
  }
  switch (task.role) {
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

std::uint64_t task_final_attributes(
    const WindowsDirectShrinkPartitionTask& task) noexcept {
  return task.role == migrationcore::MigrationPartitionRole::recovery
      ? kRecoveryGptAttributes
      : 0U;
}

void withhold_construction_partition_visibility(
    clonecore::GptPartition& partition) {
  partition.type_guid = clonecore::gpt_type_basic_data();
  partition.attributes = kTemporaryNoDefaultDriveLetter;
  partition.name = u"YTEC-INCOMPLETE";
}

class ReplayGuidGenerator final : public clonecore::IGuidGenerator {
 public:
  explicit ReplayGuidGenerator(std::vector<clonecore::GptGuid> values)
      : values_(std::move(values)) {}

  clonecore::Result<clonecore::GptGuid> next_guid() override {
    if (index_ >= values_.size()) {
      return failure<clonecore::GptGuid>(
          clonecore::ErrorCode::internal_error,
          ERROR_NO_MORE_ITEMS,
          L"直接縮小GPT GUID再利用",
          L"固定済みGPT GUID列を超えて要求されました");
    }
    return clonecore::Result<clonecore::GptGuid>::success(values_[index_++]);
  }

 private:
  std::vector<clonecore::GptGuid> values_;
  std::size_t index_{};
};

std::vector<clonecore::GptGuid> replay_guids(
    const clonecore::GptDisk& final_disk,
    const std::optional<clonecore::GptGuid>& staging_guid = std::nullopt) {
  std::vector<clonecore::GptGuid> values;
  values.reserve(final_disk.partitions.size() + 2U);
  values.push_back(final_disk.disk_guid);
  for (const auto& partition : final_disk.partitions) {
    values.push_back(partition.unique_guid);
  }
  if (staging_guid.has_value()) {
    values.push_back(*staging_guid);
  }
  return values;
}

clonecore::Result<clonecore::GptWritePlan> build_final_gpt(
    const WindowsDirectShrinkClonePlan& plan,
    clonecore::IGuidGenerator& generator) {
  constexpr std::uint32_t kEntryCount = 128U;
  constexpr std::uint32_t kEntrySize = 128U;
  const std::uint64_t target_size = plan.expected_target().size_bytes;
  const std::uint64_t sector_count = target_size / 512U;
  const std::uint64_t entry_sectors =
      (static_cast<std::uint64_t>(kEntryCount) * kEntrySize + 511U) / 512U;
  if (target_size == 0U || target_size % 512U != 0U ||
      sector_count <= 3U + 2U * entry_sectors) {
    return failure<clonecore::GptWritePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"直接縮小最終GPT寸法",
        L"最終GPTメタデータを安全に配置できません");
  }
  clonecore::GptDisk synthetic{
      .logical_sector_size = 512U,
      .sector_count = sector_count,
      .first_usable_lba = 2U + entry_sectors,
      .last_usable_lba = sector_count - 2U - entry_sectors,
      .partition_entry_count = kEntryCount,
      .partition_entry_size = kEntrySize,
  };
  synthetic.partitions.reserve(plan.tasks().size());
  std::array<bool, kEntryCount> used{};
  for (const auto& task : plan.tasks()) {
    std::uint64_t end{};
    if (task.target_number == 0U || task.target_number > kEntryCount ||
        used[task.target_number - 1U] || task.target_offset_bytes % 512U != 0U ||
        task.target_size_bytes == 0U || task.target_size_bytes % 512U != 0U ||
        !checked_add(task.target_offset_bytes, task.target_size_bytes, end)) {
      return failure<clonecore::GptWritePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小最終GPT区画",
          L"番号、整列、または容量が不正です");
    }
    for (const auto& existing : synthetic.partitions) {
      const std::uint64_t existing_begin = existing.first_lba * 512U;
      const std::uint64_t existing_end = (existing.last_lba + 1U) * 512U;
      if (task.target_offset_bytes < existing_end &&
          existing_begin < end) {
        return failure<clonecore::GptWritePlan>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小最終GPT区画重複",
            L"最終partition範囲が重複しています");
      }
    }
    used[task.target_number - 1U] = true;
    synthetic.partitions.push_back(clonecore::GptPartition{
        .entry_index = task.target_number - 1U,
        .type_guid = task_type_guid(task),
        .first_lba = task.target_offset_bytes / 512U,
        .last_lba = end / 512U - 1U,
        .attributes = task_final_attributes(task),
        .name = task_name(task),
    });
  }
  return clonecore::make_gpt_write_plan(
      synthetic, target_size, 512U, generator);
}

clonecore::Result<clonecore::GptWritePlan> build_temporary_gpt(
    const WindowsDirectShrinkClonePlan& plan,
    const clonecore::GptWritePlan& final_plan,
    const clonecore::GptGuid& staging_guid) {
  auto source = final_plan.target_disk;
  for (auto& partition : source.partitions) {
    const auto task = std::find_if(
        plan.tasks().begin(),
        plan.tasks().end(),
        [&](const WindowsDirectShrinkPartitionTask& candidate) {
          return candidate.target_number == partition.entry_index + 1U;
        });
    std::uint64_t construction_end{};
    if (task == plan.tasks().end() ||
        task->construction_size_bytes == 0U ||
        task->construction_size_bytes > task->target_size_bytes ||
        !checked_add(
            task->target_offset_bytes,
            task->construction_size_bytes,
            construction_end)) {
      return failure<clonecore::GptWritePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小一時construction GPT",
          L"最終entryとconstruction taskを一意に対応付けできません");
    }
    partition.last_lba = construction_end / 512U - 1U;
    withhold_construction_partition_visibility(partition);
  }
  if (plan.staging().archive_offset_bytes % 512U != 0U ||
      plan.staging().archive_capacity_bytes == 0U ||
      plan.staging().archive_capacity_bytes % 512U != 0U) {
    return failure<clonecore::GptWritePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小一時staging GPT",
        L"staging partitionが512-byte境界へ整列していません");
  }
  std::array<bool, 128U> used{};
  for (const auto& partition : source.partitions) {
    if (partition.entry_index >= used.size() ||
        used[partition.entry_index]) {
      return failure<clonecore::GptWritePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小一時staging GPT entry",
          L"最終GPTのentry indexが範囲外または重複しています");
    }
    used[partition.entry_index] = true;
  }
  const auto free_entry = std::find(used.begin(), used.end(), false);
  if (free_entry == used.end()) {
    return failure<clonecore::GptWritePlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小一時staging GPT entry",
        L"target-owned stagingを表す空GPT entryがありません");
  }
  const std::uint64_t first_lba =
      plan.staging().archive_offset_bytes / 512U;
  const std::uint64_t sector_count =
      plan.staging().archive_capacity_bytes / 512U;
  std::uint64_t last_lba{};
  if (!checked_add(first_lba, sector_count - 1U, last_lba) ||
      first_lba < source.first_usable_lba ||
      last_lba > source.last_usable_lba) {
    return failure<clonecore::GptWritePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小一時staging GPT範囲",
        L"target-owned stagingがGPT使用可能範囲に収まりません");
  }
  for (const auto& partition : source.partitions) {
    if (first_lba <= partition.last_lba &&
        partition.first_lba <= last_lba) {
      return failure<clonecore::GptWritePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小一時staging GPT重複",
          L"target-owned stagingがconstruction partitionと重複しています");
    }
  }
  source.partitions.push_back(clonecore::GptPartition{
      .entry_index = static_cast<std::uint32_t>(
          std::distance(used.begin(), free_entry)),
      .type_guid = clonecore::gpt_type_basic_data(),
      .first_lba = first_lba,
      .last_lba = last_lba,
      .attributes = kTemporaryNoDefaultDriveLetter,
      .name = u"YTEC-INCOMPLETE-STAGING",
  });
  // make_gpt_add_partition_plan() deliberately emits only the four GPT
  // updates needed to preserve an already-published disk.  The direct-shrink
  // temporary table is published after both metadata ends are invalidated,
  // so it must instead receive the complete five-write sequence including a
  // protective MBR.
  ReplayGuidGenerator replay(replay_guids(final_plan.target_disk, staging_guid));
  return clonecore::make_gpt_write_plan(
      source, plan.expected_target().size_bytes, 512U, replay);
}

clonecore::Result<clonecore::GptWritePlan> build_hidden_final_gpt(
    const WindowsDirectShrinkClonePlan& plan,
    const clonecore::GptWritePlan& final_plan) {
  auto source = final_plan.target_disk;
  for (auto& partition : source.partitions) {
    withhold_construction_partition_visibility(partition);
  }
  ReplayGuidGenerator replay(replay_guids(source));
  return clonecore::make_gpt_write_plan(
      source, plan.expected_target().size_bytes, 512U, replay);
}

const clonecore::GptPartition* find_gpt_partition(
    const clonecore::GptDisk& disk,
    const std::uint32_t target_number) noexcept {
  const auto found = std::find_if(
      disk.partitions.begin(),
      disk.partitions.end(),
      [&](const clonecore::GptPartition& partition) {
        return partition.entry_index + 1U == target_number;
      });
  return found == disk.partitions.end() ? nullptr : std::addressof(*found);
}

clonecore::Status validate_gpt_phase_relationships(
    const WindowsDirectShrinkClonePlan& plan,
    const clonecore::GptWritePlan& final_plan,
    const clonecore::GptWritePlan& temporary_plan,
    const clonecore::GptWritePlan& hidden_plan) {
  std::uint64_t checkpoint_end{};
  if (plan.checkpoint_offset_bytes() !=
          kWindowsDirectShrinkCheckpointOffsetBytes ||
      !checked_add(
          plan.checkpoint_offset_bytes(),
          kWindowsDirectShrinkCheckpointRecordBytes,
          checkpoint_end) ||
      checkpoint_end > plan.expected_target().size_bytes ||
      final_plan.target_disk.disk_guid != temporary_plan.target_disk.disk_guid ||
      final_plan.target_disk.disk_guid != hidden_plan.target_disk.disk_guid ||
      final_plan.target_disk.partitions.size() != plan.tasks().size() ||
      hidden_plan.target_disk.partitions.size() != plan.tasks().size() ||
      temporary_plan.target_disk.partitions.size() != plan.tasks().size() + 1U) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小GPT phase不変条件",
        L"固定checkpoint、同一fresh disk GUID、またはphase別partition件数が一致しません");
  }

  for (const auto* phase : {&final_plan, &temporary_plan, &hidden_plan}) {
    for (const auto& write : phase->writes) {
      std::uint64_t write_end{};
      if (!checked_add(write.offset, write.bytes.size(), write_end) ||
          (write.offset < checkpoint_end &&
           plan.checkpoint_offset_bytes() < write_end)) {
        return status_failure(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"直接縮小GPT／checkpoint分離",
            L"GPT metadata writeが固定checkpoint recordと重複しています");
      }
    }
  }

  for (const auto& task : plan.tasks()) {
    const auto* final = find_gpt_partition(
        final_plan.target_disk, task.target_number);
    const auto* temporary = find_gpt_partition(
        temporary_plan.target_disk, task.target_number);
    const auto* hidden = find_gpt_partition(
        hidden_plan.target_disk, task.target_number);
    if (final == nullptr || temporary == nullptr || hidden == nullptr ||
        final->unique_guid.is_zero() ||
        final->unique_guid != temporary->unique_guid ||
        final->unique_guid != hidden->unique_guid ||
        final->type_guid != task_type_guid(task) ||
        final->attributes != task_final_attributes(task) ||
        final->name != task_name(task) ||
        final->first_lba * 512ULL != task.target_offset_bytes ||
        (final->last_lba - final->first_lba + 1U) * 512ULL !=
            task.target_size_bytes ||
        temporary->type_guid != clonecore::gpt_type_basic_data() ||
        temporary->attributes != kTemporaryNoDefaultDriveLetter ||
        temporary->name != u"YTEC-INCOMPLETE" ||
        temporary->first_lba * 512ULL != task.target_offset_bytes ||
        (temporary->last_lba - temporary->first_lba + 1U) * 512ULL !=
            task.construction_size_bytes ||
        hidden->type_guid != clonecore::gpt_type_basic_data() ||
        hidden->attributes != kTemporaryNoDefaultDriveLetter ||
        hidden->name != u"YTEC-INCOMPLETE" ||
        hidden->first_lba * 512ULL != task.target_offset_bytes ||
        (hidden->last_lba - hidden->first_lba + 1U) * 512ULL !=
            task.target_size_bytes) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"直接縮小GPT construction/final対応",
          L"同一fresh partition GUID、construction非boot属性、final extent、または最終type/name/attrsが一致しません");
    }
  }

  std::size_t staging_count{};
  std::size_t temporary_esp_count{};
  for (const auto& partition : temporary_plan.target_disk.partitions) {
    temporary_esp_count +=
        partition.type_guid == clonecore::gpt_type_efi_system() ? 1U : 0U;
    if (partition.first_lba * 512ULL ==
            plan.staging().archive_offset_bytes &&
        (partition.last_lba - partition.first_lba + 1U) * 512ULL ==
            plan.staging().archive_capacity_bytes &&
        partition.type_guid == clonecore::gpt_type_basic_data() &&
        partition.attributes == kTemporaryNoDefaultDriveLetter &&
        partition.name == u"YTEC-INCOMPLETE-STAGING" &&
        !partition.unique_guid.is_zero()) {
      ++staging_count;
    }
  }
  const auto hidden_esp_count = static_cast<std::size_t>(std::count_if(
      hidden_plan.target_disk.partitions.begin(),
      hidden_plan.target_disk.partitions.end(),
      [](const clonecore::GptPartition& partition) {
        return partition.type_guid == clonecore::gpt_type_efi_system();
      }));
  if (staging_count != 1U || temporary_esp_count != 0U ||
      hidden_esp_count != 0U) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小GPT construction非boot証跡",
        L"exact stagingが一意でないかtemporary／hidden-finalにESP型が露出しています");
  }
  return clonecore::success_status();
}

clonecore::Result<imageformat::Sha256Digest> staging_identity_hash(
    const WindowsDirectShrinkClonePlan& plan,
    const imageformat::Sha256Digest& connection_instance_hash) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-STAGING-V2";
  auto target_hash = imageformat::
      hash_tsumugi_physical_restore_target_identity_v1(
          plan.expected_target());
  if (!target_hash) {
    return clonecore::Result<imageformat::Sha256Digest>::failure(
        target_hash.error());
  }
  std::vector<std::byte> bytes;
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_array(bytes, target_hash.value());
  append_array(bytes, connection_instance_hash);
  append_array(bytes, plan.final_layout_hash());
  append_u64(bytes, plan.checkpoint_offset_bytes());
  append_u64(bytes, kWindowsDirectShrinkCheckpointRecordBytes);
  append_u64(bytes, plan.staging().offset_bytes);
  append_u64(bytes, plan.staging().length_bytes);
  append_u64(bytes, plan.staging().control_reserve_bytes);
  append_u64(bytes, plan.staging().archive_offset_bytes);
  append_u64(bytes, plan.staging().archive_capacity_bytes);
  return imageformat::sha256(bytes);
}

bool source_partition_style_matches(
    const diskmodel::PartitionStyle observed,
    const migrationcore::MigrationPartitionStyle planned) noexcept {
  return (observed == diskmodel::PartitionStyle::gpt &&
          planned == migrationcore::MigrationPartitionStyle::gpt) ||
      (observed == diskmodel::PartitionStyle::mbr &&
       planned == migrationcore::MigrationPartitionStyle::mbr);
}

bool supported_initial_target_partition_table(
    const diskmodel::DiskInfo& target,
    const diskmodel::PartitionStyle normalized_style) noexcept {
  if (normalized_style == diskmodel::PartitionStyle::raw) {
    return target.partitions.empty();
  }
  if (normalized_style == diskmodel::PartitionStyle::gpt) {
    return std::all_of(
        target.partitions.begin(),
        target.partitions.end(),
        [](const diskmodel::PartitionInfo& partition) {
          return partition.number != 0U &&
              partition.style == diskmodel::PartitionStyle::gpt;
        });
  }
  if (normalized_style != diskmodel::PartitionStyle::mbr ||
      target.partitions.size() > 4U) {
    return false;
  }
  std::array<bool, 5U> seen{};
  for (const auto& partition : target.partitions) {
    const bool extended = equal_path(partition.type, L"0x05") ||
        equal_path(partition.type, L"0x0F") ||
        equal_path(partition.type, L"0x85");
    if (partition.number == 0U || partition.number > 4U ||
        seen[partition.number] ||
        partition.style != diskmodel::PartitionStyle::mbr || extended) {
      return false;
    }
    seen[partition.number] = true;
  }
  return true;
}

const WindowsDirectShrinkPartitionTask* find_source_task(
    const WindowsDirectShrinkClonePlan& plan,
    const std::uint32_t source_table_index,
    const std::uint32_t target_number) noexcept {
  const auto found = std::find_if(
      plan.tasks().begin(),
      plan.tasks().end(),
      [&](const WindowsDirectShrinkPartitionTask& task) {
        return task.source_table_index == source_table_index &&
            task.target_number == target_number;
      });
  return found == plan.tasks().end() ? nullptr : &*found;
}

bool mbr_partition_type_matches(
    const std::wstring_view observed,
    const std::uint8_t expected) noexcept {
  constexpr wchar_t kHex[] = L"0123456789ABCDEF";
  const std::array<wchar_t, 5U> canonical{
      L'0',
      L'x',
      kHex[(expected >> 4U) & 0x0FU],
      kHex[expected & 0x0FU],
      L'\0',
  };
  return observed.size() == 4U &&
      _wcsnicmp(observed.data(), canonical.data(), 4U) == 0;
}

bool unsupported_extended_mbr_type(const std::wstring_view type) noexcept {
  const auto hex = [](const wchar_t value) noexcept {
    return (value >= L'0' && value <= L'9') ||
        (value >= L'a' && value <= L'f') ||
        (value >= L'A' && value <= L'F');
  };
  return type.size() != 4U || type[0] != L'0' ||
      (type[1] != L'x' && type[1] != L'X') || !hex(type[2]) ||
      !hex(type[3]) || equal_path(type, L"0x00") ||
      equal_path(type, L"0x05") ||
      equal_path(type, L"0x0F") || equal_path(type, L"0x85");
}

clonecore::Status validate_mbr_to_gpt_source_bindings(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::DiskInfo& source) {
  using Disposition = WindowsDirectShrinkSourcePartitionDisposition;
  using Role = migrationcore::MigrationPartitionRole;

  if (all_zero(plan.source_partition_snapshot_hash()) ||
      source.partitions.empty() || source.partitions.size() > 4U ||
      plan.source_partition_mappings().size() != source.partitions.size()) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小production MBR source mapping",
        L"1～4個のprimary partitionと全source partitionの不変mappingが必要です");
  }

  std::size_t active_count{};
  std::size_t windows_count{};
  std::size_t bios_system_count{};
  std::size_t recovery_count{};
  std::size_t replaced_boot_count{};
  std::array<bool, 5U> seen_primary_numbers{};
  for (const auto& partition : source.partitions) {
    if (partition.number == 0U || partition.number > 4U ||
        seen_primary_numbers[partition.number] ||
        partition.style != diskmodel::PartitionStyle::mbr ||
        (!equal_path(partition.type, L"0x07") &&
         !equal_path(partition.type, L"0x27"))) {
      return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小production MBR source partition",
        L"MBR primary partitionは一意な1～4番の0x07または0x27だけに限定します");
    }
    seen_primary_numbers[partition.number] = true;
    if (partition.bootable) {
      ++active_count;
    }

    const WindowsDirectShrinkSourcePartitionMapping* mapping = nullptr;
    for (const auto& candidate : plan.source_partition_mappings()) {
      if (candidate.source_table_index != partition.number) {
        continue;
      }
      if (mapping != nullptr) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"直接縮小production MBR source mapping重複",
            L"一つのsource partitionへ複数のmappingがあります");
      }
      mapping = &candidate;
    }
    if (mapping == nullptr ||
        (equal_path(partition.type, L"0x27") &&
         mapping->role != Role::recovery) ||
        (equal_path(partition.type, L"0x07") &&
         mapping->role != Role::bios_system &&
         mapping->role != Role::windows && mapping->role != Role::data)) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小production MBR source role",
          L"source partition typeと不変role mappingが一致しません");
    }

    if (mapping->role == Role::windows) {
      ++windows_count;
    } else if (mapping->role == Role::bios_system) {
      ++bios_system_count;
    } else if (mapping->role == Role::recovery) {
      ++recovery_count;
    }

    switch (mapping->disposition) {
      case Disposition::transferred_to_target: {
        if (!mapping->selected || !mapping->target_number) {
          return status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"直接縮小production MBR transfer mapping",
              L"転送対象にはselectedとtarget numberが必要です");
        }
        const auto* task = find_source_task(
            plan, mapping->source_table_index, *mapping->target_number);
        if (task == nullptr || task->role != mapping->role) {
          return status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"直接縮小production MBR transfer task",
              L"source mappingとtarget taskを一意に対応付けできません");
        }
        break;
      }
      case Disposition::replaced_by_generated_uefi_boot:
        if (mapping->role != Role::bios_system || !mapping->selected ||
            !mapping->required || mapping->target_number) {
          return status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"直接縮小production BIOS boot置換",
              L"必須BIOS systemだけを生成ESP/MSRへ置換できます");
        }
        ++replaced_boot_count;
        break;
      case Disposition::omitted_unselected:
        if (mapping->selected || mapping->required || mapping->target_number ||
            partition.bootable) {
          return status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"直接縮小production MBR未選択領域",
              L"未選択・非必須・非activeのsourceだけを省略できます");
        }
        break;
      case Disposition::recreated_as_generated_system_partition:
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"直接縮小production MBR system再作成",
            L"MBR sourceにGPT system partition再作成mappingは指定できません");
    }

    if (partition.bootable &&
        !((mapping->role == Role::bios_system &&
           mapping->disposition ==
               Disposition::replaced_by_generated_uefi_boot) ||
          (mapping->role == Role::windows &&
           mapping->disposition == Disposition::transferred_to_target))) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"直接縮小production MBR active partition",
          L"一意なactive partitionはBIOS systemまたは転送Windowsでなければなりません");
    }
  }

  if (active_count != 1U || windows_count != 1U || bios_system_count > 1U ||
      recovery_count > 1U || replaced_boot_count != bios_system_count) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小production MBR起動layout",
        L"一意なactive/Windows、最大一つのBIOS system/Recovery、および完全なUEFI置換mappingが必要です");
  }
  return clonecore::success_status();
}

clonecore::Status validate_mbr_preserve_source_bindings(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::DiskInfo& source) {
  using Disposition = WindowsDirectShrinkSourcePartitionDisposition;
  using Role = migrationcore::MigrationPartitionRole;

  if (!plan.mbr_preserve_binding().has_value() ||
      all_zero(plan.source_partition_snapshot_hash()) ||
      source.partitions.empty() || source.partitions.size() > 4U ||
      plan.source_partition_mappings().size() != source.partitions.size()) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小production MBR preserve source mapping",
        L"raw MBR bindingと1～4個のprimary partition全件mappingが必要です");
  }

  std::array<bool, 5U> seen_primary_numbers{};
  std::size_t active_count{};
  std::size_t active_boot_role_count{};
  std::size_t windows_count{};
  std::size_t bios_system_count{};
  std::size_t recovery_count{};
  for (const auto& partition : source.partitions) {
    if (partition.number == 0U || partition.number > 4U ||
        seen_primary_numbers[partition.number] ||
        partition.style != diskmodel::PartitionStyle::mbr ||
        unsupported_extended_mbr_type(partition.type)) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"直接縮小production MBR preserve primary",
          L"MBR preserveは一意な1～4番の非extended primaryだけを扱います");
    }
    seen_primary_numbers[partition.number] = true;

    const WindowsDirectShrinkSourcePartitionMapping* mapping = nullptr;
    for (const auto& candidate : plan.source_partition_mappings()) {
      if (candidate.source_table_index != partition.number) {
        continue;
      }
      if (mapping != nullptr) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"直接縮小production MBR preserve mapping重複",
            L"一つのsource primaryへ複数mappingがあります");
      }
      mapping = &candidate;
    }
    const WindowsDirectShrinkPartitionTask* transferred_task = nullptr;
    if (mapping != nullptr &&
        mapping->disposition == Disposition::transferred_to_target &&
        mapping->target_number.has_value()) {
      transferred_task = find_source_task(
          plan, partition.number, *mapping->target_number);
    }
    const bool exact_raw = transferred_task != nullptr &&
        transferred_task->kind ==
            WindowsDirectShrinkPartitionTaskKind::copy_exact_raw;
    const bool exact_raw_type_matches = exact_raw &&
        mbr_partition_type_matches(
            partition.type,
            std::to_integer<std::uint8_t>(
                transferred_task->source_partition_type[0]));
    const bool role_matches_type = mapping != nullptr &&
        (exact_raw
             ? mapping->role == Role::data && exact_raw_type_matches &&
                   !partition.bootable
             : mapping->disposition == Disposition::omitted_unselected &&
                       mapping->role == Role::data && !partition.bootable
                 ? true
             : equal_path(partition.type, L"0x27")
                 ? mapping->role == Role::recovery
                 : equal_path(partition.type, L"0x07") &&
                     (mapping->role == Role::bios_system ||
                      mapping->role == Role::windows ||
                      mapping->role == Role::data));
    if (!role_matches_type) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小production MBR preserve role",
          L"source primary typeとimmutable role mappingが一致しません");
    }

    if (mapping->role == Role::windows) {
      ++windows_count;
    } else if (mapping->role == Role::bios_system) {
      ++bios_system_count;
    } else if (mapping->role == Role::recovery) {
      ++recovery_count;
    }
    if (partition.bootable) {
      ++active_count;
      if (mapping->role == Role::windows ||
          mapping->role == Role::bios_system) {
        ++active_boot_role_count;
      }
    }

    if (mapping->disposition == Disposition::transferred_to_target) {
      if (!mapping->selected || !mapping->target_number.has_value()) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"直接縮小production MBR preserve transfer",
            L"転送mappingにはselectedとtarget numberが必要です");
      }
      const auto* task = transferred_task;
      if (task == nullptr || task->role != mapping->role ||
          task->active != partition.bootable) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"直接縮小production MBR preserve task",
            L"source mappingとtarget taskのroleまたはActive属性が一致しません");
      }
    } else if (
        mapping->disposition == Disposition::omitted_unselected) {
      if (mapping->selected || mapping->required ||
          mapping->target_number.has_value() || partition.bootable ||
          (!equal_path(partition.type, L"0x07") &&
           !equal_path(partition.type, L"0x27") &&
           mapping->role != Role::data)) {
        return status_failure(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"直接縮小production MBR preserve省略",
            L"非required・非active・未選択primaryだけを省略できます");
      }
    } else {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"直接縮小production MBR preserve disposition",
          L"形式維持ではsource primaryの転送または安全な未選択省略だけを扱います");
    }
  }

  const bool system_valid = plan.boot_finalization_required() &&
      active_count == 1U && active_boot_role_count == 1U &&
      windows_count == 1U && bios_system_count <= 1U &&
      recovery_count <= 1U;
  const bool data_valid = !plan.boot_finalization_required() &&
      active_count == 0U && windows_count == 0U &&
      bios_system_count == 0U && recovery_count == 0U;
  if (!system_valid && !data_valid) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小production MBR preserve起動layout",
        L"systemは一意なActive/Windowsと最大一つのBIOS system/Recovery、data-onlyはActive/boot roleなしが必要です");
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> checkpoint_record(
    const WindowsDirectShrinkCheckpointEvidence& evidence) {
  constexpr std::string_view kMagic = "YTEC-DSC-CHK-V2";
  std::vector<std::byte> bytes;
  bytes.reserve(kCheckpointBytes);
  append_u32(bytes, static_cast<std::uint32_t>(kMagic.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kMagic.data()),
      reinterpret_cast<const std::byte*>(kMagic.data() + kMagic.size()));
  append_u8(bytes, static_cast<std::uint8_t>(evidence.phase));
  append_u64(bytes, evidence.revision);
  append_u64(bytes, evidence.completed_task_count);
  append_u64(bytes, evidence.verified_target_bytes);
  append_array(bytes, evidence.plan_hash);
  append_array(bytes, evidence.staging_identity_hash);
  append_array(bytes, evidence.aggregate_write_digest);
  if (bytes.size() > kCheckpointBytes) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::internal_error,
        ERROR_INSUFFICIENT_BUFFER,
        L"直接縮小checkpoint構築",
        L"固定checkpoint record上限を超えました");
  }
  bytes.resize(kCheckpointBytes, std::byte{0});
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

clonecore::Result<std::wstring> query_system_directory() {
  std::vector<wchar_t> buffer(32768U, L'\0');
  const UINT length = GetSystemDirectoryW(
      buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return clonecore::Result<std::wstring>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"直接縮小DISM System32取得",
            GetLastError()));
  }
  return clonecore::Result<std::wstring>::success(
      std::wstring(buffer.data(), length));
}

clonecore::Result<std::array<std::wstring, 2U>>
choose_unused_winre_drive_roots() {
  const DWORD drives = GetLogicalDrives();
  if (drives == 0U) {
    return clonecore::Result<std::array<std::wstring, 2U>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"直接縮小WinRE未使用drive取得",
            GetLastError()));
  }
  std::array<std::wstring, 2U> roots{};
  std::size_t count{};
  for (wchar_t letter = L'Z'; letter >= L'D' && count < roots.size();
       --letter) {
    if (letter == L'X') {
      continue;
    }
    const DWORD bit = 1UL << static_cast<unsigned int>(letter - L'A');
    if ((drives & bit) == 0U) {
      roots[count++] = std::wstring(1U, letter) + L":\\";
    }
  }
  if (count != roots.size()) {
    return failure<std::array<std::wstring, 2U>>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NO_MORE_FILES,
        L"直接縮小WinRE一時drive割当",
        L"Windows領域とRecovery領域へ割り当てる未使用drive文字が2個ありません");
  }
  return clonecore::Result<std::array<std::wstring, 2U>>::success(
      std::move(roots));
}

bool partition_matches_exactly(
    const diskmodel::PartitionInfo& partition,
    const std::uint32_t number,
    const std::uint64_t offset,
    const std::uint64_t size,
    const diskmodel::PartitionStyle style) noexcept {
  return partition.number == number && partition.offset_bytes == offset &&
      partition.size_bytes == size && partition.style == style;
}

clonecore::Status verify_native_construction_mbr(
    const diskmodel::DiskInfo& target,
    const WindowsDirectShrinkBootFinalizationRequest& request);

class WindowsDirectShrinkWinReTargetGuard final
    : public bootrepair::IWinReRegistrationTargetGuard {
 public:
  WindowsDirectShrinkWinReTargetGuard(
      WindowsDirectShrinkWinReFinalizationRequest request,
      bootrepair::ISystemVolumeMountApi& mount_api,
      std::wstring windows_drive_root,
      std::wstring recovery_drive_root)
      : request_(std::move(request)),
        mount_api_(mount_api),
        windows_drive_root_(std::move(windows_drive_root)),
        recovery_drive_root_(std::move(recovery_drive_root)),
        windows_location_({
            .disk_number = request_.expected_target_disk_number,
            .starting_offset = request_.expected_windows_partition_offset,
            .extent_length = request_.expected_windows_partition_size,
            .file_system = L"NTFS",
        }),
        recovery_location_({
            .disk_number = request_.expected_target_disk_number,
            .starting_offset = request_.expected_recovery_partition_offset,
            .extent_length = request_.expected_recovery_partition_size,
            .file_system = L"NTFS",
        }) {}

  clonecore::Status revalidate_disk_and_partitions() {
    auto inventory = diskmodel::make_windows_disk_inventory_provider();
    auto observed = diskmodel::reidentify_physical_clone(
        request_.expected_source,
        request_.expected_target,
        request_.confirmation,
        *inventory,
        false);
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    const auto& target = observed.value().target;
    const bool legacy_bios = request_.expected_partition_style ==
        migrationcore::MigrationPartitionStyle::mbr;
    const auto expected_style = legacy_bios
        ? diskmodel::PartitionStyle::mbr
        : diskmodel::PartitionStyle::gpt;
    const auto windows_count = static_cast<std::size_t>(std::count_if(
        target.partitions.begin(),
        target.partitions.end(),
        [&](const diskmodel::PartitionInfo& partition) {
          return partition_matches_exactly(
              partition,
              request_.expected_windows_partition_number,
              request_.expected_windows_partition_offset,
              request_.expected_windows_partition_size,
              expected_style);
        }));
    const auto recovery_count = static_cast<std::size_t>(std::count_if(
        target.partitions.begin(),
        target.partitions.end(),
        [&](const diskmodel::PartitionInfo& partition) {
          return partition_matches_exactly(
              partition,
              request_.expected_recovery_partition_number,
              request_.expected_recovery_partition_offset,
              request_.expected_recovery_partition_size,
              expected_style);
        }));
    if (target.disk_number != request_.expected_target_disk_number ||
        !target.offline.has_value() || target.offline.value() ||
        !target.read_only.has_value() || target.read_only.value() ||
        !target.removable.has_value() || target.removable.value() ||
        target.is_system_disk || target.logical_sector_size != 512U ||
        diskmodel::normalize_disk_partition_style(
            target.partition_style, target.partitions.size()) !=
            expected_style ||
        windows_count != 1U || recovery_count != 1U ||
        (legacy_bios && request_.expected_mbr_disk_signature == 0U) ||
        (!legacy_bios && request_.expected_mbr_disk_signature != 0U) ||
        request_.expected_windows_partition_number ==
            request_.expected_recovery_partition_number) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小WinRE対象再識別",
          L"安定識別済み対象のonline／writeable／fixed／非system／512-byte firmware別形式またはWindows／Recovery exact extentが変化しました");
    }
    if (legacy_bios) {
      return verify_native_construction_mbr(
          target,
          WindowsDirectShrinkBootFinalizationRequest{
              .expected_target_disk_number =
                  request_.expected_target_disk_number,
              .expected_windows_partition_number =
                  request_.expected_windows_partition_number,
              .expected_windows_partition_offset =
                  request_.expected_windows_partition_offset,
              .expected_windows_partition_size =
                  request_.expected_windows_partition_size,
              .expected_system_partition_number =
                  request_.expected_recovery_partition_number,
              .expected_system_partition_offset =
                  request_.expected_recovery_partition_offset,
              .expected_system_partition_size =
                  request_.expected_recovery_partition_size,
              .expected_mbr_disk_signature =
                  request_.expected_mbr_disk_signature,
              .firmware = bootrepair::BcdBootFirmware::bios,
          });
    }
    return clonecore::success_status();
  }

  clonecore::Status revalidate_target() override {
    auto status = revalidate_disk_and_partitions();
    if (!status) {
      return status;
    }
    auto windows = mount_api_.inspect(
        windows_drive_root_,
        request_.windows_volume_root,
        windows_location_);
    if (!windows) {
      return clonecore::Status::failure(windows.error());
    }
    auto recovery = mount_api_.inspect(
        recovery_drive_root_,
        request_.recovery_volume_root,
        recovery_location_);
    if (!recovery) {
      return clonecore::Status::failure(recovery.error());
    }
    return clonecore::success_status();
  }

 private:
  WindowsDirectShrinkWinReFinalizationRequest request_;
  bootrepair::ISystemVolumeMountApi& mount_api_;
  std::wstring windows_drive_root_;
  std::wstring recovery_drive_root_;
  bootrepair::BootRepairVolumeLocation windows_location_;
  bootrepair::BootRepairVolumeLocation recovery_location_;
};

clonecore::Result<WindowsDirectShrinkWinReFinalizationEvidence>
finalize_winre_with_windows_apis(
    const WindowsDirectShrinkWinReFinalizationRequest& request) {
  if (request.expected_windows_partition_number == 0U ||
      request.expected_recovery_partition_number == 0U ||
      request.expected_windows_partition_offset == 0U ||
      request.expected_windows_partition_size == 0U ||
      request.expected_recovery_partition_offset == 0U ||
      request.expected_recovery_partition_size == 0U ||
      request.windows_volume_root.empty() ||
      request.recovery_volume_root.empty() ||
      equal_path(request.windows_volume_root, request.recovery_volume_root)) {
    return failure<WindowsDirectShrinkWinReFinalizationEvidence>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"直接縮小WinRE production要求",
        L"一意なWindows／Recovery partitionとVolume GUID rootが必要です");
  }
  auto system_directory = query_system_directory();
  auto drive_roots = choose_unused_winre_drive_roots();
  auto mount_api = bootrepair::make_windows_system_volume_mount_api();
  if (!system_directory || !drive_roots || !mount_api) {
    return !system_directory
        ? clonecore::Result<
              WindowsDirectShrinkWinReFinalizationEvidence>::failure(
              system_directory.error())
        : !drive_roots
              ? clonecore::Result<
                    WindowsDirectShrinkWinReFinalizationEvidence>::failure(
                    drive_roots.error())
              : failure<WindowsDirectShrinkWinReFinalizationEvidence>(
                    clonecore::ErrorCode::internal_error,
                    ERROR_INVALID_HANDLE,
                    L"直接縮小WinRE mount依存",
                    L"一時Volume mount APIを作成できません");
  }

  WindowsDirectShrinkWinReTargetGuard guard(
      request,
      *mount_api,
      drive_roots.value()[0],
      drive_roots.value()[1]);
  auto status = guard.revalidate_disk_and_partitions();
  if (!status) {
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        status.error());
  }

  const bootrepair::BootRepairVolumeLocation windows_location{
      .disk_number = request.expected_target_disk_number,
      .starting_offset = request.expected_windows_partition_offset,
      .extent_length = request.expected_windows_partition_size,
      .file_system = L"NTFS",
  };
  const bootrepair::BootRepairVolumeLocation recovery_location{
      .disk_number = request.expected_target_disk_number,
      .starting_offset = request.expected_recovery_partition_offset,
      .extent_length = request.expected_recovery_partition_size,
      .file_system = L"NTFS",
  };
  auto windows_mount_result = bootrepair::TemporarySystemVolumeMount::acquire(
      bootrepair::TemporarySystemVolumeMountPlan{
          .firmware = bootrepair::BcdBootFirmware::uefi,
          .disk_number = request.expected_target_disk_number,
          .partition_number = request.expected_windows_partition_number,
          .volume_name = request.windows_volume_root,
          .temporary_root = drive_roots.value()[0],
          .expected_location = windows_location,
      },
      *mount_api);
  if (!windows_mount_result) {
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        windows_mount_result.error());
  }
  auto windows_mount = windows_mount_result.take_value();
  auto recovery_mount_result = bootrepair::TemporarySystemVolumeMount::acquire(
      bootrepair::TemporarySystemVolumeMountPlan{
          .firmware = bootrepair::BcdBootFirmware::uefi,
          .disk_number = request.expected_target_disk_number,
          .partition_number = request.expected_recovery_partition_number,
          .volume_name = request.recovery_volume_root,
          .temporary_root = drive_roots.value()[1],
          .expected_location = recovery_location,
      },
      *mount_api);
  if (!recovery_mount_result) {
    const auto release = windows_mount.release();
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        release ? recovery_mount_result.error() : release.error());
  }
  auto recovery_mount = recovery_mount_result.take_value();

  const auto release_mounts = [&]() -> std::optional<clonecore::Error> {
    const auto recovery_release = recovery_mount.release();
    const auto windows_release = windows_mount.release();
    if (!recovery_release) {
      auto error = recovery_release.error();
      if (!windows_release) {
        error.message += L" / Windows mount解放にも失敗: " +
            windows_release.error().message;
      }
      return error;
    }
    if (!windows_release) {
      return windows_release.error();
    }
    return std::nullopt;
  };

  status = guard.revalidate_target();
  if (!status) {
    const auto primary = status.error();
    const auto release = release_mounts();
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        release.value_or(primary));
  }
  const std::wstring offline_windows_directory =
      windows_mount.root() + L"Windows";
  const std::wstring candidate_directory =
      recovery_mount.root() + L"Recovery\\WindowsRE";
  const std::wstring candidate_path = candidate_directory + L"\\Winre.wim";
  auto candidate =
      bootrepair::observe_winre_registration_image_with_windows_apis(
          candidate_path);
  if (!candidate) {
    const auto primary = candidate.error();
    const auto release = release_mounts();
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        release.value_or(primary));
  }
  status = guard.revalidate_target();
  if (!status) {
    const auto primary = status.error();
    const auto release = release_mounts();
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        release.value_or(primary));
  }
  auto prior = bootrepair::inspect_winre_source_with_windows_apis(
      bootrepair::WinReDiagnosticRequest{
          .offline_windows_directory = offline_windows_directory,
          .trusted_system_directory = system_directory.value(),
          .expected_target_disk_number =
              request.expected_target_disk_number,
          .allow_mismatched_registered_location_as_cloned_source_stale = true,
      });
  if (!prior ||
      prior.value().source_state !=
          bootrepair::WinReSourceState::registered_partition ||
      !prior.value().microsoft_signature_verified ||
      !prior.value().read_only_command || prior.value().exit_code != 0U ||
      !prior.value().registered_location_reported ||
      !prior.value().registered_path_kind_reported ||
      prior.value().registered_location_matches_expected_disk ||
      !prior.value().
          registered_location_mismatch_classified_as_cloned_source_stale ||
      prior.value().registered_partition_number == 0U ||
      prior.value().registered_image_present ||
      prior.value().fallback_image_present ||
      prior.value().winre_image_size_bytes != 0U) {
    auto primary = prior
        ? platform_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_DATA,
              L"直接縮小WinRE cloned-source診断",
              L"foreign pathを開かない署名済み読取り専用診断で、コピー元由来の不一致登録を確定できません")
        : prior.error();
    const auto release = release_mounts();
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        release.value_or(primary));
  }

  auto trust = bootrepair::make_windows_authenticode_verifier();
  auto process = bootrepair::make_windows_process_runner();
  auto locker = bootrepair::make_windows_winre_registration_image_locker();
  auto diagnostic = bootrepair::make_windows_winre_diagnostic_service();
  if (!trust || !process || !locker || !diagnostic) {
    auto primary = platform_error(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_HANDLE,
        L"直接縮小WinRE transaction依存",
        L"署名検証、固定引数process、候補lock、または再診断serviceを作成できません");
    const auto release = release_mounts();
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        release.value_or(primary));
  }
  auto transaction = bootrepair::execute_winre_registration_transaction(
      bootrepair::WinReRegistrationRequest{
          .intent =
              bootrepair::WinReRegistrationIntent::register_verified_image,
          .prior_state_origin = bootrepair::
              WinReRegistrationPriorStateOrigin::cloned_source_stale,
          .offline_windows_directory = offline_windows_directory,
          .trusted_system_directory = system_directory.value(),
          .candidate_directory = candidate_directory,
          .rollback_candidate_directory = {},
          .expected_target_disk_number = request.expected_target_disk_number,
          .expected_target_partition_number =
              request.expected_recovery_partition_number,
          .expected_registered_path_kind =
              bootrepair::WinReRegisteredPathKind::recovery_windows_re,
          .reviewed_candidate = candidate.value(),
          .prior_diagnostic = prior.value(),
          .reviewed_rollback_image = std::nullopt,
      },
      *trust,
      *process,
      *locker,
      guard,
      *diagnostic);
  // execute_winre_registration_transaction() owns the candidate locks only
  // for the call. They are destroyed before either temporary mount is released.
  const auto release = release_mounts();
  if (release.has_value()) {
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(*release);
  }
  if (!transaction) {
    return clonecore::Result<
        WindowsDirectShrinkWinReFinalizationEvidence>::failure(
        transaction.error());
  }
  const auto& report = transaction.value();
  if (report.outcome != bootrepair::WinReRegistrationOutcome::completed ||
      !report.candidate_locked || !report.reagentc_signature_verified ||
      !report.cloned_source_registration_disabled ||
      !report.set_reimage_completed || !report.enable_completed ||
      !report.registration_verified || report.rollback_attempted ||
      report.primary_failure.has_value() || report.rollback_failure.has_value() ||
      report.target_revalidation_count < 4U ||
      !report.final_diagnostic.has_value()) {
    if (report.primary_failure.has_value()) {
      return clonecore::Result<
          WindowsDirectShrinkWinReFinalizationEvidence>::failure(
          *report.primary_failure);
    }
    if (report.rollback_failure.has_value()) {
      return clonecore::Result<
          WindowsDirectShrinkWinReFinalizationEvidence>::failure(
          *report.rollback_failure);
    }
    return failure<WindowsDirectShrinkWinReFinalizationEvidence>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"直接縮小WinRE transaction証跡",
        L"cloned-source解除、候補lock、固定set/enable、対象再識別、または最終再診断が不足しています");
  }
  const auto& final = *report.final_diagnostic;
  if (!final.microsoft_signature_verified || !final.read_only_command ||
      final.exit_code != 0U ||
      final.source_state != bootrepair::WinReSourceState::registered_partition ||
      !final.registered_location_reported ||
      !final.registered_path_kind_reported ||
      !final.registered_location_matches_expected_disk ||
      final.registered_partition_number !=
          request.expected_recovery_partition_number ||
      final.registered_path_kind !=
          bootrepair::WinReRegisteredPathKind::recovery_windows_re ||
      !final.registered_image_present ||
      final.winre_image_size_bytes != candidate.value().length) {
    return failure<WindowsDirectShrinkWinReFinalizationEvidence>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"直接縮小WinRE最終再診断",
        L"期待対象Recovery partition上の同じWinre.wimを読取り専用で再確認できません");
  }
  return clonecore::Result<
      WindowsDirectShrinkWinReFinalizationEvidence>::success({
      .registered_partition_number = final.registered_partition_number,
      .registered_image_size_bytes = final.winre_image_size_bytes,
      .microsoft_signed_reagentc = report.reagentc_signature_verified,
      .cloned_source_registration_disabled =
          report.cloned_source_registration_disabled,
      .candidate_identity_locked = report.candidate_locked,
      .fixed_setreimage_arguments = report.set_reimage_completed,
      .fixed_enable_arguments = report.enable_completed,
      .target_revalidated_before_each_mutation_and_diagnostic =
          report.target_revalidation_count >= 4U,
      .read_only_reinspection_completed = true,
      .registered_location_matches_expected_target =
          final.registered_location_matches_expected_disk,
      .registered_image_present = final.registered_image_present,
      .temporary_mounts_released = true,
  });
}

bool native_guid_equals(
    const GUID& observed,
    const clonecore::GptGuid& expected) noexcept {
  static_assert(sizeof(observed) == 16U);
  return std::memcmp(
             std::addressof(observed),
             expected.bytes.data(),
             expected.bytes.size()) == 0;
}

clonecore::Status verify_native_construction_gpt(
    const diskmodel::DiskInfo& target,
    const WindowsDirectShrinkBootFinalizationRequest& request) {
  if (target.device_path.empty()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        L"直接縮小construction GPT native照合",
        L"再識別済み対象のphysical device pathがありません");
  }
  clonecore::UniqueHandle handle(CreateFileW(
      target.device_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!handle) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"直接縮小construction GPT native open",
        GetLastError()));
  }

  constexpr DWORD kInitialBytes = 64U * 1024U;
  constexpr DWORD kMaximumBytes = 4U * 1024U * 1024U;
  DWORD buffer_size = kInitialBytes;
  DWORD bytes_returned{};
  std::vector<std::byte> bytes(buffer_size);
  while (DeviceIoControl(
             handle.get(),
             IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
             nullptr,
             0U,
             bytes.data(),
             buffer_size,
             &bytes_returned,
             nullptr) == FALSE) {
    const DWORD native_code = GetLastError();
    if ((native_code == ERROR_INSUFFICIENT_BUFFER ||
         native_code == ERROR_MORE_DATA) &&
        buffer_size < kMaximumBytes) {
      buffer_size = (std::min)(buffer_size * 2U, kMaximumBytes);
      bytes.resize(buffer_size);
      continue;
    }
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"直接縮小construction GPT native query",
        native_code));
  }

  constexpr std::size_t kHeaderBytes =
      offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
  if (bytes_returned < kHeaderBytes || bytes_returned > bytes.size()) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小construction GPT native length",
        L"partition layout応答長が不正です");
  }
  const auto* layout =
      reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(bytes.data());
  const std::size_t available_entries =
      (static_cast<std::size_t>(bytes_returned) - kHeaderBytes) /
      sizeof(PARTITION_INFORMATION_EX);
  if (layout->PartitionStyle != PARTITION_STYLE_GPT ||
      layout->PartitionCount > available_entries) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"直接縮小construction GPT native table",
        L"GPT形式またはpartition entry境界が期待と一致しません");
  }

  std::size_t windows_count{};
  std::size_t system_count{};
  std::size_t efi_type_count{};
  bool every_partition_hidden_basic_data = true;
  for (DWORD index = 0U; index < layout->PartitionCount; ++index) {
    const auto& partition = layout->PartitionEntry[index];
    if (partition.PartitionNumber == 0U ||
        partition.PartitionLength.QuadPart <= 0) {
      continue;
    }
    if (partition.PartitionStyle != PARTITION_STYLE_GPT) {
      every_partition_hidden_basic_data = false;
      continue;
    }
    if (native_guid_equals(
            partition.Gpt.PartitionType,
            clonecore::gpt_type_efi_system())) {
      ++efi_type_count;
    }
    const bool hidden_basic_data = native_guid_equals(
        partition.Gpt.PartitionType, clonecore::gpt_type_basic_data()) &&
        partition.Gpt.Attributes == kTemporaryNoDefaultDriveLetter;
    every_partition_hidden_basic_data &= hidden_basic_data;
    const auto offset = partition.StartingOffset.QuadPart < 0
        ? 0U
        : static_cast<std::uint64_t>(partition.StartingOffset.QuadPart);
    const auto length = partition.PartitionLength.QuadPart < 0
        ? 0U
        : static_cast<std::uint64_t>(partition.PartitionLength.QuadPart);
    if (partition.PartitionNumber ==
            request.expected_windows_partition_number &&
        offset == request.expected_windows_partition_offset &&
        length == request.expected_windows_partition_size &&
        hidden_basic_data) {
      ++windows_count;
    }
    if (partition.PartitionNumber ==
            request.expected_system_partition_number &&
        offset == request.expected_system_partition_offset &&
        length == request.expected_system_partition_size &&
        hidden_basic_data) {
      ++system_count;
    }
  }
  if (!every_partition_hidden_basic_data || efi_type_count != 0U ||
      windows_count != 1U || system_count != 1U) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"直接縮小construction GPT native visibility",
        L"全partitionのBasicData／NO_DRIVE_LETTER、ESP型0件、またはWindows／system exact extentを証明できません");
  }
  return clonecore::success_status();
}

clonecore::Status verify_native_construction_mbr(
    const diskmodel::DiskInfo& target,
    const WindowsDirectShrinkBootFinalizationRequest& request) {
  if (target.device_path.empty() || request.expected_mbr_disk_signature == 0U) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_NAME,
        L"直接縮小construction MBR native照合",
        L"再識別済み対象pathまたはimmutable fresh MBR signatureがありません");
  }
  clonecore::UniqueHandle handle(CreateFileW(
      target.device_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!handle) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        L"直接縮小construction MBR native open",
        GetLastError()));
  }

  constexpr DWORD kInitialBytes = 64U * 1024U;
  constexpr DWORD kMaximumBytes = 4U * 1024U * 1024U;
  DWORD buffer_size = kInitialBytes;
  DWORD bytes_returned{};
  std::vector<std::byte> bytes(buffer_size);
  while (DeviceIoControl(
             handle.get(),
             IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
             nullptr,
             0U,
             bytes.data(),
             buffer_size,
             &bytes_returned,
             nullptr) == FALSE) {
    const DWORD native_code = GetLastError();
    if ((native_code == ERROR_INSUFFICIENT_BUFFER ||
         native_code == ERROR_MORE_DATA) &&
        buffer_size < kMaximumBytes) {
      buffer_size = (std::min)(buffer_size * 2U, kMaximumBytes);
      bytes.resize(buffer_size);
      continue;
    }
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"直接縮小construction MBR native query",
        native_code));
  }

  constexpr std::size_t kHeaderBytes =
      offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
  if (bytes_returned < kHeaderBytes || bytes_returned > bytes.size()) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"直接縮小construction MBR native length",
        L"partition layout応答長が不正です");
  }
  const auto* layout =
      reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(bytes.data());
  const std::size_t available_entries =
      (static_cast<std::size_t>(bytes_returned) - kHeaderBytes) /
      sizeof(PARTITION_INFORMATION_EX);
  if (layout->PartitionStyle != PARTITION_STYLE_MBR ||
      layout->PartitionCount > available_entries ||
      layout->Mbr.Signature != request.expected_mbr_disk_signature) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"直接縮小construction MBR native table",
        L"MBR形式、fresh disk signature、またはpartition entry境界が期待と一致しません");
  }

  std::size_t windows_count{};
  std::size_t system_count{};
  bool every_partition_inactive_supported_primary = true;
  std::array<bool, 5U> seen_primary_numbers{};
  for (DWORD index = 0U; index < layout->PartitionCount; ++index) {
    const auto& partition = layout->PartitionEntry[index];
    if (partition.PartitionNumber == 0U ||
        partition.PartitionLength.QuadPart <= 0) {
      continue;
    }
    const bool supported = partition.PartitionStyle == PARTITION_STYLE_MBR &&
        partition.PartitionNumber <= 4U &&
        !seen_primary_numbers[partition.PartitionNumber];
    if (partition.PartitionNumber <= 4U) {
      seen_primary_numbers[partition.PartitionNumber] = true;
    }
    const bool supported_type = partition.PartitionStyle == PARTITION_STYLE_MBR &&
        (partition.Mbr.PartitionType == 0x07U ||
         partition.Mbr.PartitionType == 0x27U);
    every_partition_inactive_supported_primary &=
        supported && supported_type && !partition.Mbr.BootIndicator;
    const auto offset = partition.StartingOffset.QuadPart < 0
        ? 0U
        : static_cast<std::uint64_t>(partition.StartingOffset.QuadPart);
    const auto length = partition.PartitionLength.QuadPart < 0
        ? 0U
        : static_cast<std::uint64_t>(partition.PartitionLength.QuadPart);
    if (partition.PartitionNumber ==
            request.expected_windows_partition_number &&
        offset == request.expected_windows_partition_offset &&
        length == request.expected_windows_partition_size &&
        supported_type && !partition.Mbr.BootIndicator) {
      ++windows_count;
    }
    if (partition.PartitionNumber ==
            request.expected_system_partition_number &&
        offset == request.expected_system_partition_offset &&
        length == request.expected_system_partition_size &&
        supported_type && !partition.Mbr.BootIndicator) {
      ++system_count;
    }
  }
  if (!every_partition_inactive_supported_primary || windows_count != 1U ||
      system_count != 1U) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"直接縮小construction MBR native visibility",
        L"全primaryの0x07/0x27・Active=false、またはWindows/system exact extentを証明できません");
  }
  return clonecore::success_status();
}

clonecore::Status revalidate_direct_construction_boot_target(
    const WindowsDirectShrinkBootFinalizationRequest& request) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  auto observed = diskmodel::reidentify_physical_clone(
      request.expected_source,
      request.expected_target,
      request.confirmation,
      *inventory,
      false);
  if (!observed) {
    return clonecore::Status::failure(observed.error());
  }
  const auto& target = observed.value().target;
  const bool legacy_bios =
      request.firmware == bootrepair::BcdBootFirmware::bios;
  const auto expected_style = legacy_bios
      ? diskmodel::PartitionStyle::mbr
      : diskmodel::PartitionStyle::gpt;
  const auto windows_count = static_cast<std::size_t>(std::count_if(
      target.partitions.begin(),
      target.partitions.end(),
      [&](const diskmodel::PartitionInfo& partition) {
        return partition_matches_exactly(
            partition,
            request.expected_windows_partition_number,
            request.expected_windows_partition_offset,
            request.expected_windows_partition_size,
            expected_style);
      }));
  const auto system_count = static_cast<std::size_t>(std::count_if(
      target.partitions.begin(),
      target.partitions.end(),
      [&](const diskmodel::PartitionInfo& partition) {
        return partition_matches_exactly(
            partition,
            request.expected_system_partition_number,
            request.expected_system_partition_offset,
            request.expected_system_partition_size,
            expected_style);
      }));
  if (target.disk_number != request.expected_target_disk_number ||
      !target.offline.has_value() || target.offline.value() ||
      !target.read_only.has_value() || target.read_only.value() ||
      !target.removable.has_value() || target.removable.value() ||
      target.is_system_disk || target.logical_sector_size != 512U ||
      diskmodel::normalize_disk_partition_style(
          target.partition_style, target.partitions.size()) !=
          expected_style ||
      windows_count != 1U || system_count != 1U ||
      (!legacy_bios &&
       request.expected_windows_partition_number ==
           request.expected_system_partition_number) ||
      (legacy_bios && request.expected_mbr_disk_signature == 0U) ||
      (!legacy_bios && request.expected_mbr_disk_signature != 0U)) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"直接縮小construction起動対象再識別",
        L"安定識別済み対象のonline／writeable／fixed／非system／512-byte形式またはWindows／system exact extentが変化しました");
  }
  return legacy_bios
      ? verify_native_construction_mbr(target, request)
      : verify_native_construction_gpt(target, request);
}

clonecore::Result<WindowsDirectShrinkBootFinalizationEvidence>
finalize_bios_boot_with_windows_apis(
    const WindowsDirectShrinkBootFinalizationRequest& request) {
  auto status = revalidate_direct_construction_boot_target(request);
  auto drive_roots = choose_unused_winre_drive_roots();
  auto mount_api = bootrepair::make_windows_system_volume_mount_api();
  if (!status || !drive_roots || !mount_api) {
    return !status
        ? clonecore::Result<
              WindowsDirectShrinkBootFinalizationEvidence>::failure(
              status.error())
        : !drive_roots
              ? clonecore::Result<
                    WindowsDirectShrinkBootFinalizationEvidence>::failure(
                    drive_roots.error())
              : failure<WindowsDirectShrinkBootFinalizationEvidence>(
                    clonecore::ErrorCode::internal_error,
                    ERROR_INVALID_HANDLE,
                    L"直接縮小construction BIOS BCDBoot mount依存",
                    L"exact target Volume GUID mount APIがありません");
  }

  const bootrepair::BootRepairVolumeLocation windows_location{
      .disk_number = request.expected_target_disk_number,
      .starting_offset = request.expected_windows_partition_offset,
      .extent_length = request.expected_windows_partition_size,
      .file_system = L"NTFS",
  };
  const bootrepair::BootRepairVolumeLocation system_location{
      .disk_number = request.expected_target_disk_number,
      .starting_offset = request.expected_system_partition_offset,
      .extent_length = request.expected_system_partition_size,
      .file_system = L"NTFS",
  };
  auto windows_mount_result = bootrepair::TemporarySystemVolumeMount::acquire(
      bootrepair::TemporarySystemVolumeMountPlan{
          .firmware = bootrepair::BcdBootFirmware::bios,
          .disk_number = request.expected_target_disk_number,
          .partition_number = request.expected_windows_partition_number,
          .volume_name = request.windows_volume_root,
          .temporary_root = drive_roots.value()[0],
          .expected_location = windows_location,
      },
      *mount_api);
  if (!windows_mount_result) {
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(
        windows_mount_result.error());
  }
  auto windows_mount = windows_mount_result.take_value();

  const bool same_system_partition =
      request.expected_windows_partition_number ==
          request.expected_system_partition_number;
  std::optional<bootrepair::TemporarySystemVolumeMount> system_mount;
  if (!same_system_partition) {
    auto mounted = bootrepair::TemporarySystemVolumeMount::acquire(
        bootrepair::TemporarySystemVolumeMountPlan{
            .firmware = bootrepair::BcdBootFirmware::bios,
            .disk_number = request.expected_target_disk_number,
            .partition_number = request.expected_system_partition_number,
            .volume_name = request.system_volume_root,
            .temporary_root = drive_roots.value()[1],
            .expected_location = system_location,
        },
        *mount_api);
    if (!mounted) {
      const auto release = windows_mount.release();
      return clonecore::Result<
          WindowsDirectShrinkBootFinalizationEvidence>::failure(
          release ? mounted.error() : release.error());
    }
    system_mount.emplace(mounted.take_value());
  }

  const auto release_mounts = [&]() -> std::optional<clonecore::Error> {
    if (system_mount.has_value()) {
      const auto system_release = system_mount->release();
      if (!system_release) {
        static_cast<void>(windows_mount.release());
        return system_release.error();
      }
    }
    const auto windows_release = windows_mount.release();
    return windows_release
        ? std::nullopt
        : std::optional<clonecore::Error>(windows_release.error());
  };

  status = revalidate_direct_construction_boot_target(request);
  auto bcdboot = status
      ? bootrepair::execute_bcdboot_with_windows_apis(
            bootrepair::BcdBootRequest{
                .target_windows_directory = windows_mount.root() + L"Windows",
                .target_system_partition_root = same_system_partition
                    ? windows_mount.root()
                    : system_mount->root(),
                .firmware = bootrepair::BcdBootFirmware::bios,
                .store_policy =
                    bootrepair::BcdBootStorePolicy::rebuild_fresh,
            })
      : clonecore::Result<bootrepair::BcdBootReport>::failure(status.error());
  const auto release = release_mounts();
  if (release.has_value()) {
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(*release);
  }
  if (!bcdboot) {
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(bcdboot.error());
  }
  status = revalidate_direct_construction_boot_target(request);
  if (!status) {
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(status.error());
  }
  if (bcdboot.value().exit_code != 0U ||
      !bcdboot.value().microsoft_signature_verified ||
      !bcdboot.value().fresh_store_verified) {
    return failure<WindowsDirectShrinkBootFinalizationEvidence>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"直接縮小construction BIOS BCDBoot証跡",
        L"Microsoft署名、/f BIOS終了コード、またはfresh BCD store読戻し証跡が不足しています");
  }
  return clonecore::Result<
      WindowsDirectShrinkBootFinalizationEvidence>::success({
      .microsoft_signed_bcdboot = true,
      .fresh_bcd_store_read_back_verified = true,
      .construction_gpt_non_bootable_verified = false,
      .efi_ownership_safe_before_mount = false,
      .efi_ownership_revalidated_before_mutation = false,
      .microsoft_boot_namespace_read_back_verified = false,
      .temporary_mounts_released = true,
      .final_target_reidentified = true,
      .partition_layout_unchanged = true,
      .nvram_unchanged = true,
      .legacy_bios = true,
      .exact_target_volume_extents = true,
      .target_only_reconstruction = true,
  });
}

clonecore::Result<WindowsDirectShrinkBootFinalizationEvidence>
finalize_boot_with_windows_apis(
    const WindowsDirectShrinkBootFinalizationRequest& request) {
  const bool legacy_bios =
      request.firmware == bootrepair::BcdBootFirmware::bios;
  const bool same_system_partition =
      request.expected_windows_partition_number ==
          request.expected_system_partition_number;
  if ((request.firmware != bootrepair::BcdBootFirmware::uefi &&
       request.firmware != bootrepair::BcdBootFirmware::bios) ||
      request.expected_windows_partition_number == 0U ||
      request.expected_system_partition_number == 0U ||
      request.expected_windows_partition_offset == 0U ||
      request.expected_windows_partition_size == 0U ||
      request.expected_system_partition_offset == 0U ||
      request.expected_system_partition_size == 0U ||
      request.windows_volume_root.empty() ||
      request.system_volume_root.empty() ||
      (legacy_bios && request.expected_mbr_disk_signature == 0U) ||
      (!legacy_bios && request.expected_mbr_disk_signature != 0U) ||
      (!legacy_bios && same_system_partition) ||
      (same_system_partition !=
       equal_path(request.windows_volume_root, request.system_volume_root))) {
    return failure<WindowsDirectShrinkBootFinalizationEvidence>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"直接縮小construction BCDBoot要求",
        L"firmware別の一意なWindows/system partition、fresh MBR signature、およびexact Volume GUID rootが必要です");
  }
  if (legacy_bios) {
    return finalize_bios_boot_with_windows_apis(request);
  }
  auto status = revalidate_direct_construction_boot_target(request);
  auto drive_roots = choose_unused_winre_drive_roots();
  auto mount_api = bootrepair::make_windows_system_volume_mount_api();
  auto ownership_inspector =
      bootrepair::make_windows_efi_boot_ownership_inspector();
  if (!status || !drive_roots || !mount_api || !ownership_inspector) {
    return !status
        ? clonecore::Result<
              WindowsDirectShrinkBootFinalizationEvidence>::failure(
              status.error())
        : !drive_roots
              ? clonecore::Result<
                    WindowsDirectShrinkBootFinalizationEvidence>::failure(
                    drive_roots.error())
              : failure<WindowsDirectShrinkBootFinalizationEvidence>(
                    clonecore::ErrorCode::internal_error,
                    ERROR_INVALID_HANDLE,
                    L"直接縮小construction BCDBoot mount依存",
                    L"exact Volume GUID mountまたはEFI ownership検査APIがありません");
  }

  auto ownership_before =
      ownership_inspector->inspect_existing_esp_read_only(
          request.system_volume_root);
  if (!ownership_before ||
      ownership_before.value().state !=
          bootrepair::EfiBootOwnershipState::microsoft_only_or_empty ||
      !bootrepair::efi_boot_ownership_allows_microsoft_rebuild(
          ownership_before.value())) {
    return ownership_before
        ? failure<WindowsDirectShrinkBootFinalizationEvidence>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小construction EFI ownership事前診断",
              L"system volumeが空またはMicrosoft所有と一意に確認できません")
        : clonecore::Result<
              WindowsDirectShrinkBootFinalizationEvidence>::failure(
              ownership_before.error());
  }

  const bootrepair::BootRepairVolumeLocation windows_location{
      .disk_number = request.expected_target_disk_number,
      .starting_offset = request.expected_windows_partition_offset,
      .extent_length = request.expected_windows_partition_size,
      .file_system = L"NTFS",
  };
  const bootrepair::BootRepairVolumeLocation system_location{
      .disk_number = request.expected_target_disk_number,
      .starting_offset = request.expected_system_partition_offset,
      .extent_length = request.expected_system_partition_size,
      .file_system = L"FAT32",
  };
  auto windows_mount_result = bootrepair::TemporarySystemVolumeMount::acquire(
      bootrepair::TemporarySystemVolumeMountPlan{
          .firmware = bootrepair::BcdBootFirmware::uefi,
          .disk_number = request.expected_target_disk_number,
          .partition_number = request.expected_windows_partition_number,
          .volume_name = request.windows_volume_root,
          .temporary_root = drive_roots.value()[0],
          .expected_location = windows_location,
      },
      *mount_api);
  if (!windows_mount_result) {
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(
        windows_mount_result.error());
  }
  auto windows_mount = windows_mount_result.take_value();
  auto system_mount_result = bootrepair::TemporarySystemVolumeMount::acquire(
      bootrepair::TemporarySystemVolumeMountPlan{
          .firmware = bootrepair::BcdBootFirmware::uefi,
          .disk_number = request.expected_target_disk_number,
          .partition_number = request.expected_system_partition_number,
          .volume_name = request.system_volume_root,
          .temporary_root = drive_roots.value()[1],
          .expected_location = system_location,
      },
      *mount_api);
  if (!system_mount_result) {
    const auto release = windows_mount.release();
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(
        release ? system_mount_result.error() : release.error());
  }
  auto system_mount = system_mount_result.take_value();
  const auto release_mounts = [&]() -> std::optional<clonecore::Error> {
    const auto system_release = system_mount.release();
    const auto windows_release = windows_mount.release();
    if (!system_release) {
      return system_release.error();
    }
    if (!windows_release) {
      return windows_release.error();
    }
    return std::nullopt;
  };

  status = revalidate_direct_construction_boot_target(request);
  auto ownership_before_mutation = status
      ? ownership_inspector->inspect_existing_esp_read_only(
            request.system_volume_root)
      : clonecore::Result<bootrepair::EfiBootOwnershipEvidence>::failure(
            status.error());
  if (ownership_before_mutation &&
      (!bootrepair::equivalent_efi_boot_ownership(
           ownership_before.value(), ownership_before_mutation.value()) ||
       ownership_before_mutation.value().state !=
           bootrepair::EfiBootOwnershipState::microsoft_only_or_empty ||
       !bootrepair::efi_boot_ownership_allows_microsoft_rebuild(
           ownership_before_mutation.value()))) {
    ownership_before_mutation = clonecore::Result<
        bootrepair::EfiBootOwnershipEvidence>::failure(platform_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"直接縮小construction EFI ownership直前再照合",
        L"mount後・BCDBoot直前にEFI ownershipが変化しました"));
  }
  auto bcdboot = ownership_before_mutation
      ? bootrepair::execute_bcdboot_with_windows_apis(
            bootrepair::BcdBootRequest{
                .target_windows_directory = windows_mount.root() + L"Windows",
                .target_system_partition_root = system_mount.root(),
                .firmware = bootrepair::BcdBootFirmware::uefi,
                .store_policy =
                    bootrepair::BcdBootStorePolicy::rebuild_fresh,
            })
      : clonecore::Result<bootrepair::BcdBootReport>::failure(
            ownership_before_mutation.error());
  const auto release = release_mounts();
  if (release.has_value()) {
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(*release);
  }
  if (!bcdboot) {
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(
        bcdboot.error());
  }
  status = revalidate_direct_construction_boot_target(request);
  if (!status) {
    return clonecore::Result<
        WindowsDirectShrinkBootFinalizationEvidence>::failure(status.error());
  }
  auto ownership_after =
      ownership_inspector->inspect_existing_esp_read_only(
          request.system_volume_root);
  if (!ownership_after ||
      ownership_after.value().state !=
          bootrepair::EfiBootOwnershipState::microsoft_only_or_empty ||
      !bootrepair::efi_boot_ownership_allows_microsoft_rebuild(
          ownership_after.value()) ||
      !ownership_after.value().microsoft_namespace_present ||
      ownership_after.value().microsoft_signed_efi_loader_count == 0U) {
    return ownership_after
        ? failure<WindowsDirectShrinkBootFinalizationEvidence>(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"直接縮小construction EFI ownership事後読戻し",
              L"BCDBoot後のMicrosoft namespaceと署名済みEFI loaderを確認できません")
        : clonecore::Result<
              WindowsDirectShrinkBootFinalizationEvidence>::failure(
              ownership_after.error());
  }
  if (bcdboot.value().exit_code != 0U ||
      !bcdboot.value().microsoft_signature_verified ||
      !bcdboot.value().fresh_store_verified) {
    return failure<WindowsDirectShrinkBootFinalizationEvidence>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"直接縮小construction BCDBoot証跡",
        L"Microsoft署名、終了コード、または新規BCD store読戻し証跡が不足しています");
  }
  return clonecore::Result<
      WindowsDirectShrinkBootFinalizationEvidence>::success({
      .microsoft_signed_bcdboot = true,
      .fresh_bcd_store_read_back_verified = true,
      .construction_gpt_non_bootable_verified = true,
      .efi_ownership_safe_before_mount = true,
      .efi_ownership_revalidated_before_mutation = true,
      .microsoft_boot_namespace_read_back_verified = true,
      .temporary_mounts_released = true,
      .final_target_reidentified = true,
      .partition_layout_unchanged = true,
      // An explicit /s root is mandatory in BcdBootRequest and therefore the
      // direct construction flow never creates or reorders host NVRAM entries.
      .nvram_unchanged = true,
      .legacy_bios = false,
      .exact_target_volume_extents = true,
      .target_only_reconstruction = true,
  });
}

bool valid_snapshot_device_path(const std::wstring_view path) noexcept {
  constexpr std::wstring_view kPrefix =
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy";
  if (path.size() <= kPrefix.size() ||
      !equal_path(path.substr(0U, kPrefix.size()), kPrefix)) {
    return false;
  }
  return std::all_of(
      path.begin() + static_cast<std::ptrdiff_t>(kPrefix.size()),
      path.end(),
      [](const wchar_t value) { return value >= L'0' && value <= L'9'; });
}

struct FileIdentity final {
  std::uint64_t volume_serial{};
  std::array<std::byte, 16U> file_id{};

  [[nodiscard]] bool operator==(const FileIdentity&) const noexcept = default;
};

clonecore::Result<FileIdentity> query_file_identity(const HANDLE handle) {
  FILE_ID_INFO info{};
  if (!GetFileInformationByHandleEx(
          handle, FileIdInfo, &info, sizeof(info))) {
    return clonecore::Result<FileIdentity>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::query_failed,
            L"直接縮小所有file識別",
            GetLastError()));
  }
  FileIdentity result{.volume_serial = info.VolumeSerialNumber};
  static_assert(sizeof(info.FileId.Identifier) == result.file_id.size());
  std::memcpy(
      result.file_id.data(),
      info.FileId.Identifier,
      result.file_id.size());
  return clonecore::Result<FileIdentity>::success(result);
}

clonecore::Status validate_regular_file(
    const HANDLE handle,
    std::uint64_t& length,
    FileIdentity& identity) {
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(
          handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
      !GetFileInformationByHandleEx(
          handle, FileStandardInfo, &standard, sizeof(standard))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::query_failed,
        L"直接縮小WIM属性固定",
        GetLastError()));
  }
  if ((tag.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
      standard.DeletePending || standard.NumberOfLinks != 1U ||
      standard.EndOfFile.QuadPart <= 0) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_BAD_FORMAT,
        L"直接縮小WIM通常file固定",
        L"一意な通常fileではないDISM出力を拒否しました");
  }
  auto observed_identity = query_file_identity(handle);
  if (!observed_identity) {
    return clonecore::Status::failure(observed_identity.error());
  }
  length = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
  identity = observed_identity.take_value();
  return clonecore::success_status();
}

clonecore::Status mark_wim_handle_for_deletion(
    const HANDLE handle,
    const std::wstring_view operation) {
  FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
  if (!SetFileInformationByHandle(
          handle,
          FileDispositionInfo,
          &disposition,
          sizeof(disposition))) {
    return clonecore::Status::failure(clonecore::make_win32_error(
        clonecore::ErrorCode::io_failed,
        std::wstring(operation),
        GetLastError()));
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<std::byte>> read_handle_exact(
    const HANDLE handle,
    const std::uint64_t offset,
    const std::size_t length) {
  if (length > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()) ||
      offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        L"直接縮小WIM読取り範囲",
        L"Win32同期読取り範囲を超えています");
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"直接縮小WIM読取り位置",
            GetLastError()));
  }
  std::vector<std::byte> bytes(length);
  DWORD read{};
  if (length != 0U &&
      (!ReadFile(
           handle,
           bytes.data(),
           static_cast<DWORD>(length),
           &read,
           nullptr) ||
       read != length)) {
    const DWORD native_code = GetLastError();
    return clonecore::Result<std::vector<std::byte>>::failure(
        clonecore::make_win32_error(
            clonecore::ErrorCode::io_failed,
            L"直接縮小WIM完全読取り",
            native_code == ERROR_SUCCESS ? ERROR_HANDLE_EOF : native_code));
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(bytes));
}

clonecore::Result<imageformat::Sha256Digest> hash_file_handle(
    const HANDLE handle,
    const std::uint64_t length) {
  return imageformat::sha256_from_reader(
      length,
      kHashBlockBytes,
      [handle](const std::uint64_t offset, const std::size_t amount) {
        return read_handle_exact(handle, offset, amount);
      });
}

class WindowsDirectShrinkOwnedWimStore final
    : public IWindowsDirectShrinkOwnedWimStore {
 public:
  static clonecore::Result<std::unique_ptr<
      IWindowsDirectShrinkOwnedWimStore>>
  create(
      std::wstring root,
      const std::uint64_t capacity,
      const std::uint64_t maximum_archive,
      clonecore::DiskOperationCallbacks callbacks) {
    if (root.empty() || capacity < kMinimumDismScratchFreeBytes ||
        maximum_archive == 0U || maximum_archive > capacity) {
      return failure<std::unique_ptr<IWindowsDirectShrinkOwnedWimStore>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"直接縮小target-owned WIM容量",
          L"物理的に区切られたstaging容量とDISM scratch最小余白を保持できません");
    }
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return failure<std::unique_ptr<IWindowsDirectShrinkOwnedWimStore>>(
          clonecore::ErrorCode::unsupported_layout,
          attributes == INVALID_FILE_ATTRIBUTES ? GetLastError()
                                                 : ERROR_REPARSE_TAG_INVALID,
          L"直接縮小target-owned staging root",
          L"通常directoryのVolume GUID rootだけを使用できます");
    }
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr)) {
      return clonecore::Result<std::unique_ptr<
          IWindowsDirectShrinkOwnedWimStore>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::query_failed,
              L"直接縮小target-owned staging空き容量",
              GetLastError()));
    }
    if (available.QuadPart < kMinimumDismScratchFreeBytes) {
      return failure<std::unique_ptr<IWindowsDirectShrinkOwnedWimStore>>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"直接縮小format後staging空き容量",
          L"NTFS format後の実空き容量が512MiB DISM scratch最小余白を満たしません");
    }

    static std::atomic_uint64_t sequence{};
    std::wstring directory;
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
      directory = root;
      if (!directory.ends_with(L'\\')) {
        directory.push_back(L'\\');
      }
      directory += L"YTEC-DirectShrink-";
      directory += std::to_wstring(GetCurrentProcessId());
      directory.push_back(L'-');
      directory += std::to_wstring(GetTickCount64());
      directory.push_back(L'-');
      directory += std::to_wstring(sequence.fetch_add(1U));
      if (CreateDirectoryW(directory.c_str(), nullptr)) {
        break;
      }
      if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return clonecore::Result<std::unique_ptr<
            IWindowsDirectShrinkOwnedWimStore>>::failure(
            clonecore::make_win32_error(
                clonecore::ErrorCode::io_failed,
                L"直接縮小所有directory作成",
                GetLastError()));
      }
      directory.clear();
    }
    if (directory.empty()) {
      return failure<std::unique_ptr<IWindowsDirectShrinkOwnedWimStore>>(
          clonecore::ErrorCode::io_failed,
          ERROR_ALREADY_EXISTS,
          L"直接縮小所有directory作成",
          L"衝突しない所有directory名を確保できません");
    }
    clonecore::UniqueHandle directory_handle(CreateFileW(
        directory.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!directory_handle) {
      return clonecore::Result<std::unique_ptr<
          IWindowsDirectShrinkOwnedWimStore>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"直接縮小所有directory固定",
              GetLastError()));
    }
    auto directory_identity = query_file_identity(directory_handle.get());
    if (!directory_identity) {
      return clonecore::Result<std::unique_ptr<
          IWindowsDirectShrinkOwnedWimStore>>::failure(
          directory_identity.error());
    }
    std::wstring scratch = directory + L"\\scratch";
    if (!CreateDirectoryW(scratch.c_str(), nullptr)) {
      return clonecore::Result<std::unique_ptr<
          IWindowsDirectShrinkOwnedWimStore>>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"直接縮小DISM scratch作成",
              GetLastError()));
    }
    clonecore::UniqueHandle scratch_handle(CreateFileW(
        scratch.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    FILE_ATTRIBUTE_TAG_INFO scratch_tag{};
    if (!scratch_handle ||
        !GetFileInformationByHandleEx(
            scratch_handle.get(),
            FileAttributeTagInfo,
            &scratch_tag,
            sizeof(scratch_tag)) ||
        (scratch_tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (scratch_tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      const DWORD native_code = GetLastError();
      return failure<std::unique_ptr<IWindowsDirectShrinkOwnedWimStore>>(
          clonecore::ErrorCode::unsupported_layout,
          native_code == ERROR_SUCCESS ? ERROR_REPARSE_TAG_INVALID
                                       : native_code,
          L"直接縮小DISM scratch固定",
          L"作成直後のscratchが通常directoryではありません");
    }
    auto scratch_identity = query_file_identity(scratch_handle.get());
    if (!scratch_identity) {
      return clonecore::Result<std::unique_ptr<
          IWindowsDirectShrinkOwnedWimStore>>::failure(
              scratch_identity.error());
    }
    auto system = query_system_directory();
    auto trust = bootrepair::make_windows_authenticode_verifier();
    auto process = bootrepair::make_windows_process_runner(
        kDismTimeoutMilliseconds);
    if (!system || !trust || !process) {
      return !system
          ? clonecore::Result<std::unique_ptr<
                IWindowsDirectShrinkOwnedWimStore>>::failure(system.error())
          : failure<std::unique_ptr<IWindowsDirectShrinkOwnedWimStore>>(
                clonecore::ErrorCode::internal_error,
                ERROR_INVALID_HANDLE,
                L"直接縮小DISM依存",
                L"Authenticode verifierまたはprocess runnerを作成できません");
    }
    std::unique_ptr<IWindowsDirectShrinkOwnedWimStore> result(
        new WindowsDirectShrinkOwnedWimStore(
            std::move(directory),
            std::move(scratch),
            directory_identity.take_value(),
            scratch_identity.take_value(),
            capacity,
            maximum_archive,
            std::move(callbacks),
            system.take_value(),
            std::move(trust),
            std::move(process)));
    return clonecore::Result<std::unique_ptr<
        IWindowsDirectShrinkOwnedWimStore>>::success(std::move(result));
  }

  clonecore::Result<WindowsDirectShrinkOwnedWimEvidence>
  capture_and_seal(
      const std::uint32_t source_table_index,
      const std::wstring& snapshot_device_path,
      const std::uint64_t archive_upper_bound_bytes) override {
    cancellation_seen_during_external_process_ = false;
    if (current_.has_value() || source_table_index == 0U ||
        archive_upper_bound_bytes == 0U ||
        archive_upper_bound_bytes > maximum_archive_ ||
        !valid_snapshot_device_path(snapshot_device_path)) {
      return failure<WindowsDirectShrinkOwnedWimEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"直接縮小WIM capture要求",
          L"単回状態、partition番号、WIM上限、またはSnapshot device pathが不正です");
    }
    auto directory = revalidate_directory();
    if (!directory) {
      return clonecore::Result<WindowsDirectShrinkOwnedWimEvidence>::failure(
          directory.error());
    }
    std::wstring path = directory_ + L"\\volume-" +
        std::to_wstring(source_table_index) + L".wim";
    const DWORD existing = GetFileAttributesW(path.c_str());
    const DWORD native_code = existing == INVALID_FILE_ATTRIBUTES
        ? GetLastError()
        : ERROR_SUCCESS;
    if (existing != INVALID_FILE_ATTRIBUTES ||
        (native_code != ERROR_FILE_NOT_FOUND &&
         native_code != ERROR_PATH_NOT_FOUND)) {
      return failure<WindowsDirectShrinkOwnedWimEvidence>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_EXISTS,
          L"直接縮小WIM path所有確認",
          L"capture前からWIM pathが存在するため拒否しました");
    }
    std::wstring source_root = snapshot_device_path;
    source_root.push_back(L'\\');
    auto captured = windowsdism::execute_dism_capture(
        windowsdism::DismCaptureRequest{
            .source_root = source_root,
            .image_path = path,
            .scratch_directory = scratch_,
            .image_name = L"Y-TEC Tsumugi Direct Shrink",
        },
        system_directory_,
        *trust_,
        *process_,
        [this](const std::string_view) {
          if (clonecore::disk_operation_cancellation_requested(callbacks_)) {
            cancellation_seen_during_external_process_ = true;
          }
        });
    if (!captured || captured.value().exit_code != 0U ||
        !captured.value().microsoft_signature_verified) {
      auto primary = captured
          ? platform_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_DATA,
                L"直接縮小DISM capture証跡",
                L"Microsoft署名または正常終了を確認できません")
          : captured.error();
      return fail_capture_after_partial_cleanup(
          std::move(primary), path);
    }
    const bool cancel_after_seal =
        cancellation_seen_during_external_process_ ||
        clonecore::disk_operation_cancellation_requested(callbacks_);
    cancellation_seen_during_external_process_ = false;
    clonecore::UniqueHandle handle(CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      auto primary = clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"直接縮小WIM no-share固定",
          GetLastError());
      return fail_capture_after_partial_cleanup(
          std::move(primary), path);
    }
    std::uint64_t length{};
    FileIdentity identity{};
    auto regular = validate_regular_file(handle.get(), length, identity);
    if (!regular) {
      auto primary = regular.error();
      handle.reset();
      return fail_capture_after_partial_cleanup(
          std::move(primary), path);
    }
    if (length > archive_upper_bound_bytes || length > capacity_) {
      auto primary = platform_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_DISK_FULL,
          L"直接縮小WIM実長",
          L"DISM出力がレビュー済みWIM上限またはstaging容量を超えました");
      handle.reset();
      return fail_capture_after_partial_cleanup(
          std::move(primary), path);
    }
    if (!FlushFileBuffers(handle.get())) {
      auto primary = clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"直接縮小WIM flush",
          GetLastError());
      handle.reset();
      return fail_capture_after_partial_cleanup(
          std::move(primary), path);
    }
    auto signature = read_handle_exact(handle.get(), 0U, kWimSignature.size());
    auto hash = hash_file_handle(handle.get(), length);
    if (!signature || !hash || signature.value() !=
            std::vector<std::byte>(kWimSignature.begin(), kWimSignature.end())) {
      auto primary = !signature
          ? signature.error()
          : !hash
                ? hash.error()
                : platform_error(
                      clonecore::ErrorCode::invalid_data,
                      ERROR_BAD_FORMAT,
                      L"直接縮小WIM signature",
                      L"DISM出力が単一WIM headerではありません");
      handle.reset();
      return fail_capture_after_partial_cleanup(
          std::move(primary), path);
    }
    handle.reset();
    directory = revalidate_directory();
    if (!directory) {
      return fail_capture_after_partial_cleanup(
          directory.error(), path);
    }
    current_ = OwnedWim{
        .source_table_index = source_table_index,
        .path = std::move(path),
        .identity = identity,
        .length = length,
        .hash = hash.value(),
    };
    if (cancel_after_seal) {
      const auto cleanup = discard_exact(source_table_index, current_->hash);
      return cleanup
          ? failure<WindowsDirectShrinkOwnedWimEvidence>(
                clonecore::ErrorCode::cancelled,
                ERROR_CANCELLED,
                L"直接縮小DISM capture取消",
                L"DISM完了後の安全境界で取消し、exact所有WIMを破棄しました")
          : clonecore::Result<WindowsDirectShrinkOwnedWimEvidence>::failure(
                cleanup.error());
    }
    return clonecore::Result<WindowsDirectShrinkOwnedWimEvidence>::success({
        .source_table_index = source_table_index,
        .length = length,
        .hash = hash.take_value(),
        .sealed_without_write_or_delete_sharing = true,
        .flushed = true,
        .complete_read_back_hash_verified = true,
    });
  }

  clonecore::Status apply_locked_and_reverify(
      const std::uint32_t source_table_index,
      const imageformat::Sha256Digest& expected_hash,
      const std::wstring& target_volume_root) override {
    cancellation_seen_during_external_process_ = false;
    // DISM must be able to open the WIM for reading while this identity-bound
    // handle remains open.  Requesting delete access here would require DISM's own
    // handle to share delete access and can turn an otherwise read-only apply
    // into ERROR_SHARING_VIOLATION.  FILE_SHARE_READ still seals out every
    // writer and deleter while preserving the exact-file read lock.
    auto handle = reopen_current(
        source_table_index, expected_hash, false);
    if (!handle) {
      return clonecore::Status::failure(handle.error());
    }
    const DWORD target_attributes = GetFileAttributesW(target_volume_root.c_str());
    if (target_attributes == INVALID_FILE_ATTRIBUTES ||
        (target_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (target_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          target_attributes == INVALID_FILE_ATTRIBUTES
              ? GetLastError()
              : ERROR_REPARSE_TAG_INVALID,
          L"直接縮小WIM apply target",
          L"通常directoryのexact Volume rootだけへ適用できます");
    }
    auto applied = windowsdism::execute_dism_apply(
        windowsdism::DismApplyRequest{
            .image_path = current_->path,
            .target_root = target_volume_root,
            .scratch_directory = scratch_,
        },
        system_directory_,
        *trust_,
        *process_,
        [this](const std::string_view) {
          if (clonecore::disk_operation_cancellation_requested(callbacks_)) {
            cancellation_seen_during_external_process_ = true;
          }
        });
    if (!applied || applied.value().exit_code != 0U ||
        !applied.value().microsoft_signature_verified) {
      return applied
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_DATA,
                L"直接縮小DISM apply証跡",
                L"Microsoft署名または正常終了を確認できません")
          : clonecore::Status::failure(applied.error());
    }
    if (cancellation_seen_during_external_process_ ||
        clonecore::disk_operation_cancellation_requested(callbacks_)) {
      cancellation_seen_during_external_process_ = false;
      return status_failure(
          clonecore::ErrorCode::cancelled,
          ERROR_CANCELLED,
          L"直接縮小DISM apply取消",
          L"DISM完了後の安全境界で取消要求を確認しました");
    }
    auto after = hash_file_handle(handle.value().get(), current_->length);
    if (!after || after.value() != expected_hash) {
      return after
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"直接縮小WIM apply後Hash",
                L"DISM適用中に固定WIMが変化しました")
          : clonecore::Status::failure(after.error());
    }
    return revalidate_directory();
  }

  clonecore::Status discard_exact(
      const std::uint32_t source_table_index,
      const imageformat::Sha256Digest& expected_hash) override {
    auto handle = reopen_current(
        source_table_index, expected_hash, true);
    if (!handle) {
      return clonecore::Status::failure(handle.error());
    }
    auto deletion = mark_wim_handle_for_deletion(
        handle.value().get(), L"直接縮小WIM exact破棄");
    if (!deletion) {
      return deletion;
    }
    handle.value().reset();
    const std::wstring path = current_->path;
    current_.reset();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_FILE_EXISTS,
          L"直接縮小WIM破棄読戻し",
          L"exact所有WIMの削除を確認できません");
    }
    const DWORD native_code = GetLastError();
    if (native_code != ERROR_FILE_NOT_FOUND &&
        native_code != ERROR_PATH_NOT_FOUND) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"直接縮小WIM破棄読戻し",
          native_code));
    }
    return revalidate_directory();
  }

 private:
  struct OwnedWim final {
    std::uint32_t source_table_index{};
    std::wstring path;
    FileIdentity identity;
    std::uint64_t length{};
    imageformat::Sha256Digest hash{};
  };

  WindowsDirectShrinkOwnedWimStore(
      std::wstring directory,
      std::wstring scratch,
      FileIdentity directory_identity,
      FileIdentity scratch_identity,
      const std::uint64_t capacity,
      const std::uint64_t maximum_archive,
      clonecore::DiskOperationCallbacks callbacks,
      std::wstring system_directory,
      std::unique_ptr<bootrepair::IExecutableTrustVerifier> trust,
      std::unique_ptr<bootrepair::IProcessRunner> process)
      : directory_(std::move(directory)),
        scratch_(std::move(scratch)),
        directory_identity_(directory_identity),
        scratch_identity_(scratch_identity),
        capacity_(capacity),
        maximum_archive_(maximum_archive),
        callbacks_(std::move(callbacks)),
        system_directory_(std::move(system_directory)),
        trust_(std::move(trust)),
        process_(std::move(process)) {}

  clonecore::Result<WindowsDirectShrinkOwnedWimEvidence>
  fail_capture_after_partial_cleanup(
      clonecore::Error primary,
      const std::wstring& path) {
    const auto cleanup = discard_partial_wim_handle_bound(path);
    if (!cleanup) {
      auto cleanup_error = cleanup.error();
      cleanup_error.message =
          L"capture失敗: " + primary.message +
          L" / target-owned partial WIMのhandle固定破棄にも失敗: " +
          cleanup_error.message;
      return clonecore::Result<
          WindowsDirectShrinkOwnedWimEvidence>::failure(
          std::move(cleanup_error));
    }
    primary.message +=
        L" / target-owned partial WIMの不存在またはhandle固定破棄を確認済み";
    return clonecore::Result<WindowsDirectShrinkOwnedWimEvidence>::failure(
        std::move(primary));
  }

  clonecore::Status discard_partial_wim_handle_bound(
      const std::wstring& path) {
    auto directory = revalidate_directory();
    if (!directory) {
      return directory;
    }
    clonecore::UniqueHandle handle(CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle) {
      const DWORD native_code = GetLastError();
      if (native_code == ERROR_FILE_NOT_FOUND ||
          native_code == ERROR_PATH_NOT_FOUND) {
        return revalidate_directory();
      }
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"直接縮小partial WIM handle固定",
          native_code));
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !GetFileInformationByHandleEx(
            handle.get(), FileStandardInfo, &standard, sizeof(standard))) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"直接縮小partial WIM属性固定",
          GetLastError()));
    }
    if ((tag.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U ||
        standard.Directory != FALSE || standard.DeletePending ||
        standard.NumberOfLinks != 1U || standard.EndOfFile.QuadPart < 0) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"直接縮小partial WIM通常file固定",
          L"作成前に不存在を証明したpathが一意な通常fileではありません");
    }
    auto deletion = mark_wim_handle_for_deletion(
        handle.get(), L"直接縮小partial WIM handle固定破棄");
    if (!deletion) {
      return deletion;
    }
    handle.reset();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_FILE_EXISTS,
          L"直接縮小partial WIM破棄読戻し",
          L"target-owned partial WIMの削除を確認できません");
    }
    const DWORD native_code = GetLastError();
    if (native_code != ERROR_FILE_NOT_FOUND &&
        native_code != ERROR_PATH_NOT_FOUND) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::query_failed,
          L"直接縮小partial WIM破棄読戻し",
          native_code));
    }
    return revalidate_directory();
  }

  clonecore::Status revalidate_directory() const {
    clonecore::UniqueHandle handle(CreateFileW(
        directory_.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle) {
      return clonecore::Status::failure(clonecore::make_win32_error(
          clonecore::ErrorCode::io_failed,
          L"直接縮小所有directory再固定",
          GetLastError()));
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REPARSE_TAG_INVALID,
          L"直接縮小所有directory再照合",
          L"所有directoryが通常directoryではありません");
    }
    auto identity = query_file_identity(handle.get());
    if (!identity) {
      return clonecore::Status::failure(identity.error());
    }
    if (identity.value() != directory_identity_) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小所有directory再照合",
          L"staging directoryのfile identityが変化しました");
    }
    clonecore::UniqueHandle scratch_handle(CreateFileW(
        scratch_.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    FILE_ATTRIBUTE_TAG_INFO scratch_tag{};
    if (!scratch_handle ||
        !GetFileInformationByHandleEx(
            scratch_handle.get(),
            FileAttributeTagInfo,
            &scratch_tag,
            sizeof(scratch_tag)) ||
        (scratch_tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (scratch_tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_REPARSE_TAG_INVALID,
          L"直接縮小DISM scratch再固定",
          L"所有scratchが通常directoryではありません");
    }
    auto observed_scratch_identity =
        query_file_identity(scratch_handle.get());
    if (!observed_scratch_identity ||
        observed_scratch_identity.value() != scratch_identity_) {
      return observed_scratch_identity
          ? status_failure(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"直接縮小DISM scratch再照合",
                L"所有scratchのfile identityが変化しました")
          : clonecore::Status::failure(
                observed_scratch_identity.error());
    }
    return clonecore::success_status();
  }

  clonecore::Result<clonecore::UniqueHandle> reopen_current(
      const std::uint32_t source_table_index,
      const imageformat::Sha256Digest& expected_hash,
      const bool request_delete_access) const {
    if (!current_ || current_->source_table_index != source_table_index ||
        current_->hash != expected_hash || all_zero(expected_hash)) {
      return failure<clonecore::UniqueHandle>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_STATE,
          L"直接縮小WIM再固定要求",
          L"現在のexact所有WIMとpartition／Hashが一致しません");
    }
    auto directory = revalidate_directory();
    if (!directory) {
      return clonecore::Result<clonecore::UniqueHandle>::failure(
          directory.error());
    }
    const DWORD desired_access = GENERIC_READ |
        (request_delete_access ? DELETE : 0U);
    clonecore::UniqueHandle handle(CreateFileW(
        current_->path.c_str(),
        desired_access,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!handle) {
      return clonecore::Result<clonecore::UniqueHandle>::failure(
          clonecore::make_win32_error(
              clonecore::ErrorCode::io_failed,
              L"直接縮小WIM再固定",
              GetLastError()));
    }
    std::uint64_t length{};
    FileIdentity identity{};
    auto regular = validate_regular_file(handle.get(), length, identity);
    if (!regular) {
      return clonecore::Result<clonecore::UniqueHandle>::failure(
          regular.error());
    }
    if (identity != current_->identity || length != current_->length) {
      return failure<clonecore::UniqueHandle>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小WIM file identity再照合",
          L"offline期間後のWIM identityまたは実長が変化しました");
    }
    auto hash = hash_file_handle(handle.get(), length);
    if (!hash || hash.value() != expected_hash) {
      return hash
          ? failure<clonecore::UniqueHandle>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"直接縮小WIM再Hash",
                L"offline期間後のWIM内容が変化しました")
          : clonecore::Result<clonecore::UniqueHandle>::failure(hash.error());
    }
    return clonecore::Result<clonecore::UniqueHandle>::success(
        std::move(handle));
  }

  std::wstring directory_;
  std::wstring scratch_;
  FileIdentity directory_identity_;
  FileIdentity scratch_identity_;
  std::uint64_t capacity_{};
  std::uint64_t maximum_archive_{};
  clonecore::DiskOperationCallbacks callbacks_;
  std::wstring system_directory_;
  std::unique_ptr<bootrepair::IExecutableTrustVerifier> trust_;
  std::unique_ptr<bootrepair::IProcessRunner> process_;
  std::optional<OwnedWim> current_;
  bool cancellation_seen_during_external_process_{};
};

clonecore::Result<imageformat::Sha256Digest> preparation_digest(
    const WindowsDirectShrinkPartitionTask& task,
    const WindowsTsumugiShrinkFileSystemReadbackEvidence& evidence) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-PREPARED-FS-V2";
  std::vector<std::byte> bytes;
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_u32(bytes, task.target_number);
  append_u8(bytes, static_cast<std::uint8_t>(task.kind));
  append_u8(bytes, static_cast<std::uint8_t>(task.role));
  append_u64(bytes, task.target_offset_bytes);
  append_u64(bytes, task.construction_size_bytes);
  append_u64(bytes, evidence.directory_count);
  append_u64(bytes, evidence.regular_file_count);
  append_u64(bytes, evidence.regular_file_bytes_read);
  return imageformat::sha256(bytes);
}

clonecore::Result<imageformat::Sha256Digest> reserved_preparation_digest(
    const WindowsDirectShrinkPartitionTask& task,
    const imageformat::Sha256Digest& final_layout_hash) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-PREPARED-MSR-V1";
  std::vector<std::byte> bytes;
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_u32(bytes, task.target_number);
  append_u8(bytes, static_cast<std::uint8_t>(task.role));
  append_u64(bytes, task.target_offset_bytes);
  append_u64(bytes, task.construction_size_bytes);
  append_array(bytes, final_layout_hash);
  return imageformat::sha256(bytes);
}

clonecore::Result<imageformat::Sha256Digest> applied_digest(
    const WindowsDirectShrinkPartitionTask& task,
    const imageformat::Sha256Digest& archive_hash,
    const WindowsTsumugiShrinkFileSystemReadbackEvidence& evidence) {
  constexpr std::string_view kDomain =
      "YTEC-WINDOWS-DIRECT-SHRINK-APPLIED-NTFS-V1";
  std::vector<std::byte> bytes;
  append_u32(bytes, static_cast<std::uint32_t>(kDomain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(kDomain.data()),
      reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
  append_u32(bytes, task.source_table_index.value_or(0U));
  append_u32(bytes, task.target_number);
  append_u64(bytes, task.target_offset_bytes);
  append_u64(bytes, task.target_size_bytes);
  append_array(bytes, archive_hash);
  append_u64(bytes, evidence.directory_count);
  append_u64(bytes, evidence.regular_file_count);
  append_u64(bytes, evidence.regular_file_bytes_read);
  append_u64(bytes, evidence.reparse_point_count);
  return imageformat::sha256(bytes);
}

bool stable_identity_exactly_matches(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  return left.disk_number == right.disk_number && left.model == right.model &&
      left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.serial_suffix == right.serial_suffix &&
      left.device_instance_id == right.device_instance_id &&
      left.is_system_disk == right.is_system_disk;
}

bool checkpoint_exactly_matches(
    const WindowsDirectShrinkCheckpointEvidence& left,
    const WindowsDirectShrinkCheckpointEvidence& right) noexcept {
  return left.phase == right.phase && left.revision == right.revision &&
      left.plan_hash == right.plan_hash &&
      left.staging_identity_hash == right.staging_identity_hash &&
      left.record_hash == right.record_hash &&
      left.aggregate_write_digest == right.aggregate_write_digest &&
      stable_identity_exactly_matches(
          left.observed_target, right.observed_target) &&
      left.completed_task_count == right.completed_task_count &&
      left.verified_target_bytes == right.verified_target_bytes &&
      left.durable == right.durable && left.flushed == right.flushed &&
      left.read_back_verified == right.read_back_verified &&
      left.target_offline == right.target_offline &&
      left.final_layout_committed == right.final_layout_committed;
}

class WindowsDirectShrinkClonePlatform final
    : public IWindowsDirectShrinkClonePlatform {
 public:
  WindowsDirectShrinkClonePlatform(
      WindowsDirectShrinkClonePlan plan,
      WindowsDirectShrinkClonePlatformRequest request,
      WindowsDirectShrinkClonePlatformDependencies dependencies,
      clonecore::GptWritePlan final_gpt,
      clonecore::GptWritePlan temporary_gpt,
      clonecore::GptWritePlan hidden_final_gpt,
      std::optional<clonecore::MbrWritePlan> final_mbr,
      std::vector<std::byte> hidden_final_mbr_sector)
      : plan_(std::move(plan)),
        request_(std::move(request)),
        dependencies_(std::move(dependencies)),
        final_gpt_(std::move(final_gpt)),
        temporary_gpt_(std::move(temporary_gpt)),
        hidden_final_gpt_(std::move(hidden_final_gpt)),
        final_mbr_(std::move(final_mbr)),
        hidden_final_mbr_sector_(std::move(hidden_final_mbr_sector)) {}

  ~WindowsDirectShrinkClonePlatform() override {
    if (state_ != State::committed &&
        (state_ != State::aborted || !abort_cleanup_complete_)) {
      abort_keep_offline_incomplete();
    }
  }

  clonecore::Result<WindowsDirectShrinkCheckpointEvidence>
  begin_target_owned_staging(
      const WindowsDirectShrinkClonePlan& plan,
      const operationcore::Sha256Digest& operation_plan_hash) override {
    if (state_ != State::created ||
        plan.operation_plan().immutable_payload_hash !=
            plan_.operation_plan().immutable_payload_hash ||
        all_zero(operation_plan_hash)) {
      return failure<WindowsDirectShrinkCheckpointEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小production staging開始",
          L"単回状態または不変計画Hashが一致しません");
    }
    auto observed = dependencies_.target_io->observe_original_target(
        dependencies_.connection_instance_hash);
    if (!observed) {
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          observed.error());
    }
    auto identity = clonecore::validate_stable_identity(
        plan_.expected_target(),
        observed.value().physical.target_identity,
        L"直接縮小production初期対象");
    auto layout = imageformat::hash_tsumugi_physical_restore_target_layout_v1(
        observed.value().physical.target);
    if (!identity || !layout ||
        layout.value() != plan_.expected_target_layout_hash() ||
        observed.value().restore_identity.connection_instance_hash !=
            dependencies_.connection_instance_hash ||
        observed.value().restore_identity.is_running_windows_system_disk ||
        observed.value().restore_identity.is_active_rescue_media ||
        observed.value().restore_identity.is_dynamic_disk ||
        observed.value().restore_identity.is_storage_spaces ||
        observed.value().restore_identity.is_windows_software_raid ||
        observed.value().restore_identity.has_unresolved_hardware_raid) {
      return !identity
          ? clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
                identity.error())
          : !layout
                ? clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
                      layout.error())
                : failure<WindowsDirectShrinkCheckpointEvidence>(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_DEVICE_REINITIALIZATION_NEEDED,
                      L"直接縮小production初期再照合",
                      L"対象layout、connection instance、または安全分類がレビュー後に変化しました");
    }
    auto status = dependencies_.target_io->set_target_offline(true);
    if (!status) {
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          status.error());
    }
    auto opened = dependencies_.target_io->open_offline_target();
    if (!opened || !opened.value().target) {
      return opened
          ? failure<WindowsDirectShrinkCheckpointEvidence>(
                clonecore::ErrorCode::internal_error,
                ERROR_INVALID_HANDLE,
                L"直接縮小production target writer",
                L"offline target writerがありません")
          : clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
                opened.error());
    }
    const auto locked_identity = clonecore::validate_stable_identity(
        plan_.expected_target(),
        opened.value().observed.target_identity,
        L"直接縮小productionロック済み対象");
    if (!locked_identity ||
        !opened.value().observed.target.offline.value_or(false) ||
        opened.value().observed.target.read_only.value_or(true) ||
        opened.value().observed.target.removable.value_or(true) ||
        opened.value().observed.target.is_system_disk ||
        opened.value().observed.target.logical_sector_size != 512U) {
      return !locked_identity
          ? clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
                locked_identity.error())
          : failure<WindowsDirectShrinkCheckpointEvidence>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"直接縮小productionロック済み対象状態",
                L"offline writerの対象がwriteable／fixed／非system／512-byteではありません");
    }
    writer_ = std::move(opened.value().target);
    if (writer_->size_bytes() != plan_.expected_target().size_bytes ||
        writer_->logical_sector_size() != 512U) {
      writer_.reset();
      return failure<WindowsDirectShrinkCheckpointEvidence>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小production target writer寸法",
          L"固定したWriterの容量または論理セクターが計画と一致しません");
    }
    target_touched_ = true;
    status = invalidate_initial_partition_metadata(*writer_);
    if (status) {
      status = publish_gpt_plan(
          *writer_, temporary_gpt_, L"直接縮小一時GPT公開");
    }
    if (status) {
      status = dependencies_.target_io->notify_layout_changed();
    }
    if (status) {
      status = dependencies_.target_io->set_target_offline(false);
    }
    if (!status) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          status.error());
    }
    disk_online_ = true;
    const auto staging_partition = std::find_if(
        temporary_gpt_.target_disk.partitions.begin(),
        temporary_gpt_.target_disk.partitions.end(),
        [&](const clonecore::GptPartition& partition) {
          return partition.first_lba * 512ULL ==
                  plan_.staging().archive_offset_bytes &&
              (partition.last_lba - partition.first_lba + 1U) * 512ULL ==
                  plan_.staging().archive_capacity_bytes;
        });
    if (staging_partition == temporary_gpt_.target_disk.partitions.end()) {
      abort_keep_offline_incomplete();
      return failure<WindowsDirectShrinkCheckpointEvidence>(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_FOUND,
          L"直接縮小production staging entry",
          L"一時GPTからexact staging partitionを再取得できません");
    }
    auto staging = dependencies_.target_io->bind_online_volume(
        staging_partition->entry_index + 1U,
        plan_.staging().archive_offset_bytes,
        plan_.staging().archive_capacity_bytes);
    if (!staging) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          staging.error());
    }
    staging_volume_ = staging.value();
    status = dependencies_.target_io->format_volume(
        *staging_volume_,
        imageformat::TsumugiManifestFileSystem::ntfs,
        4096U);
    if (status) {
      auto verified = dependencies_.target_io->verify_volume_readback(
          *staging_volume_,
          imageformat::TsumugiManifestFileSystem::ntfs,
          4096U,
          false);
      if (!verified || !verified.value().file_system_metadata_verified) {
        status = verified
            ? status_failure(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"直接縮小staging NTFS読戻し",
                  L"format後の実filesystem metadataを確認できません")
            : clonecore::Status::failure(verified.error());
      }
    }
    if (status) {
      auto store = dependencies_.make_wim_store(
          staging_volume_->volume_device_path,
          plan_.staging().archive_capacity_bytes,
          plan_.maximum_archive_upper_bound_bytes(),
          request_.callbacks);
      if (!store || !store.value()) {
        status = store
            ? status_failure(
                  clonecore::ErrorCode::internal_error,
                  ERROR_INVALID_HANDLE,
                  L"直接縮小target-owned WIM store",
                  L"所有WIM storeがありません")
            : clonecore::Status::failure(store.error());
      } else {
        wim_store_ = store.take_value();
      }
    }
    const auto offline = dependencies_.target_io->dismount_and_offline_volume(
        *staging_volume_);
    disk_online_ = false;
    if (!status || !offline) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          status ? offline.error() : status.error());
    }
    auto staging_hash = staging_identity_hash(
        plan_, dependencies_.connection_instance_hash);
    if (!staging_hash) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          staging_hash.error());
    }
    WindowsDirectShrinkCheckpointEvidence initial{
        .phase = WindowsDirectShrinkCheckpointPhase::prepared,
        .revision = 1U,
        .plan_hash = operation_plan_hash,
        .staging_identity_hash = staging_hash.take_value(),
        .aggregate_write_digest = {},
        .observed_target = plan_.expected_target(),
        .completed_task_count = 0U,
        .verified_target_bytes = 0U,
        .durable = true,
        .flushed = true,
        .read_back_verified = true,
        .target_offline = true,
        .final_layout_committed = false,
    };
    auto persisted = persist_checkpoint(std::move(initial));
    if (!persisted) {
      abort_keep_offline_incomplete();
      return persisted;
    }
    state_ = State::begun;
    return persisted;
  }

  clonecore::Result<WindowsDirectShrinkTargetPreparationEvidence>
  prepare_non_archive_partitions_and_verify(
      const std::span<const WindowsDirectShrinkPartitionTask> tasks) override {
    if (state_ != State::begun || disk_online_ || !writer_ ||
        archive_.has_value()) {
      return failure<WindowsDirectShrinkTargetPreparationEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小非archive領域準備",
          L"開始済みかつarchive未処理の状態が必要です");
    }
    std::uint64_t count{};
    std::uint64_t verified_bytes{};
    imageformat::Sha256Digest digest{};
    const auto aggregate_digest = [&](
        const WindowsDirectShrinkPartitionTask& task,
        const imageformat::Sha256Digest& next)
        -> clonecore::Result<imageformat::Sha256Digest> {
      if (all_zero(digest)) {
        return clonecore::Result<imageformat::Sha256Digest>::success(next);
      }
      std::vector<std::byte> combined;
      constexpr std::string_view kDomain =
          "YTEC-WINDOWS-DIRECT-SHRINK-PREPARED-AGGREGATE-V2";
      append_u32(combined, static_cast<std::uint32_t>(kDomain.size()));
      combined.insert(
          combined.end(),
          reinterpret_cast<const std::byte*>(kDomain.data()),
          reinterpret_cast<const std::byte*>(kDomain.data() + kDomain.size()));
      append_array(combined, digest);
      append_array(combined, next);
      append_u32(combined, task.target_number);
      return imageformat::sha256(combined);
    };
    for (const auto& task : tasks) {
      if (task.kind == WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim ||
          task.kind == WindowsDirectShrinkPartitionTaskKind::copy_exact_raw) {
        continue;
      }
      if (task.kind ==
          WindowsDirectShrinkPartitionTaskKind::recreate_microsoft_reserved) {
        auto status = verify_gpt_plan(*writer_, temporary_gpt_);
        auto next_digest = status
            ? reserved_preparation_digest(task, plan_.final_layout_hash())
            : clonecore::Result<imageformat::Sha256Digest>::failure(
                  status.error());
        if (!next_digest) {
          abort_keep_offline_incomplete();
          return clonecore::Result<
              WindowsDirectShrinkTargetPreparationEvidence>::failure(
              next_digest.error());
        }
        auto aggregate = aggregate_digest(task, next_digest.value());
        if (!aggregate) {
          abort_keep_offline_incomplete();
          return clonecore::Result<
              WindowsDirectShrinkTargetPreparationEvidence>::failure(
              aggregate.error());
        }
        digest = aggregate.take_value();
        const auto verified =
            (std::min)(task.construction_size_bytes, kMiB);
        if (!checked_add(verified_bytes, verified, verified_bytes)) {
          abort_keep_offline_incomplete();
          return failure<WindowsDirectShrinkTargetPreparationEvidence>(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"直接縮小MSR検証容量",
              L"検証済み容量の集約がオーバーフローしました");
        }
        ++count;
        continue;
      }
      const bool efi = task.kind ==
          WindowsDirectShrinkPartitionTaskKind::recreate_efi_system;
      if (!efi &&
          task.kind != WindowsDirectShrinkPartitionTaskKind::create_empty_ntfs) {
        return failure<WindowsDirectShrinkTargetPreparationEvidence>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"直接縮小production非archive形式",
            L"production safe sliceは生成ESP、MSR、または空NTFSだけを非archiveとして扱います");
      }
      auto volume = online_bind_target(task);
      if (!volume) {
        return clonecore::Result<WindowsDirectShrinkTargetPreparationEvidence>::failure(
            volume.error());
      }
      auto status = dependencies_.target_io->format_volume(
          volume.value(),
          efi ? imageformat::TsumugiManifestFileSystem::fat32
              : imageformat::TsumugiManifestFileSystem::ntfs,
          4096U);
      auto readback = status
          ? dependencies_.target_io->verify_volume_readback(
                volume.value(),
                efi ? imageformat::TsumugiManifestFileSystem::fat32
                    : imageformat::TsumugiManifestFileSystem::ntfs,
                4096U,
                false)
          : clonecore::Result<WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
                status.error());
      const auto offline = dependencies_.target_io->dismount_and_offline_volume(
          volume.value());
      disk_online_ = false;
      if (!readback || !offline ||
          !readback.value().file_system_metadata_verified) {
        abort_keep_offline_incomplete();
        return !readback
            ? clonecore::Result<WindowsDirectShrinkTargetPreparationEvidence>::failure(
                  readback.error())
            : !offline
                  ? clonecore::Result<WindowsDirectShrinkTargetPreparationEvidence>::failure(
                        offline.error())
                  : failure<WindowsDirectShrinkTargetPreparationEvidence>(
                        clonecore::ErrorCode::verification_failed,
                        ERROR_CRC,
                         L"直接縮小生成filesystem読戻し",
                         L"実filesystem metadataを確認できません");
      }
      auto next_digest = preparation_digest(task, readback.value());
      if (!next_digest) {
        abort_keep_offline_incomplete();
        return clonecore::Result<WindowsDirectShrinkTargetPreparationEvidence>::failure(
            next_digest.error());
      }
      auto aggregate = aggregate_digest(task, next_digest.value());
      if (!aggregate) {
        abort_keep_offline_incomplete();
        return clonecore::Result<
            WindowsDirectShrinkTargetPreparationEvidence>::failure(
            aggregate.error());
      }
      digest = aggregate.take_value();
      const auto verified =
          (std::min)(task.construction_size_bytes, kMiB);
      if (!checked_add(verified_bytes, verified, verified_bytes)) {
        abort_keep_offline_incomplete();
        return failure<WindowsDirectShrinkTargetPreparationEvidence>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"直接縮小生成filesystem検証容量",
            L"検証済み容量の集約がオーバーフローしました");
      }
      ++count;
    }
    return clonecore::Result<WindowsDirectShrinkTargetPreparationEvidence>::success({
        .prepared_task_count = count,
        .verified_target_bytes = verified_bytes,
        .write_digest = digest,
        .every_write_flushed = true,
        .every_write_read_back = true,
        .target_offline = true,
        .final_layout_committed = false,
    });
  }

  clonecore::Result<WindowsDirectShrinkStagedArchiveEvidence>
  capture_ntfs_wim_to_owned_staging(
      const WindowsDirectShrinkPartitionTask& task,
      const vssrequester::SnapshotMapping& snapshot) override {
    if (state_ != State::begun || disk_online_ || !task.source_table_index ||
        task.kind != WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim ||
        archive_.has_value() || !wim_store_) {
      return failure<WindowsDirectShrinkStagedArchiveEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小production WIM capture",
          L"単回archive、NTFS task、またはWIM store状態が不正です");
    }
    auto staging = online_bind_staging();
    if (!staging) {
      return clonecore::Result<WindowsDirectShrinkStagedArchiveEvidence>::failure(
          staging.error());
    }
    auto captured = wim_store_->capture_and_seal(
        *task.source_table_index,
        snapshot.snapshot_device_path,
        task.archive_upper_bound_bytes);
    const auto offline = dependencies_.target_io->dismount_and_offline_volume(
        staging.value());
    disk_online_ = false;
    if (!captured || !offline) {
      abort_keep_offline_incomplete();
      return !captured
          ? clonecore::Result<WindowsDirectShrinkStagedArchiveEvidence>::failure(
                captured.error())
          : clonecore::Result<WindowsDirectShrinkStagedArchiveEvidence>::failure(
                offline.error());
    }
    if (captured.value().source_table_index != *task.source_table_index ||
        captured.value().length == 0U ||
        captured.value().length > task.archive_upper_bound_bytes ||
        all_zero(captured.value().hash) ||
        !captured.value().sealed_without_write_or_delete_sharing ||
        !captured.value().flushed ||
        !captured.value().complete_read_back_hash_verified) {
      abort_keep_offline_incomplete();
      return failure<WindowsDirectShrinkStagedArchiveEvidence>(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"直接縮小production WIM証跡",
          L"実長、Hash、no-share seal、flush、または完全読戻しが不足しています");
    }
    archive_ = ActiveArchive{
        .source_table_index = *task.source_table_index,
        .target_number = task.target_number,
        .length = captured.value().length,
        .hash = captured.value().hash,
    };
    return clonecore::Result<WindowsDirectShrinkStagedArchiveEvidence>::success({
        .source_table_index = *task.source_table_index,
        .target_number = task.target_number,
        .snapshot_id = snapshot.snapshot_id,
        .snapshot_device_path = snapshot.snapshot_device_path,
        .archive_length = captured.value().length,
        .archive_hash = captured.value().hash,
        .sealed_no_write_delete_sharing = true,
        .flushed = true,
        .complete_read_back_hash_verified = true,
        .target_offline = true,
    });
  }

  clonecore::Result<WindowsDirectShrinkAppliedPartitionEvidence>
  apply_staged_ntfs_wim_and_verify(
      const WindowsDirectShrinkPartitionTask& task,
      const WindowsDirectShrinkStagedArchiveEvidence& archive) override {
    if (state_ != State::begun || disk_online_ ||
        !task.source_table_index || !archive_ ||
        archive_->source_table_index != *task.source_table_index ||
        archive_->target_number != task.target_number ||
        archive_->length != archive.archive_length ||
        archive_->hash != archive.archive_hash) {
      return failure<WindowsDirectShrinkAppliedPartitionEvidence>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_STATE,
          L"直接縮小production WIM apply",
          L"現在固定中のexact WIMとtask証跡が一致しません");
    }
    auto staging = online_bind_staging();
    if (!staging) {
      return clonecore::Result<WindowsDirectShrinkAppliedPartitionEvidence>::failure(
          staging.error());
    }
    auto target = dependencies_.target_io->bind_online_volume(
        task.target_number,
        task.target_offset_bytes,
        task.construction_size_bytes);
    if (!target) {
      const auto offline =
          dependencies_.target_io->dismount_and_offline_volume(
              staging.value());
      disk_online_ = false;
      if (!offline) {
        abort_keep_offline_incomplete();
        return clonecore::Result<
            WindowsDirectShrinkAppliedPartitionEvidence>::failure(
                offline.error());
      }
      return clonecore::Result<WindowsDirectShrinkAppliedPartitionEvidence>::failure(
          target.error());
    }
    auto status = dependencies_.target_io->format_volume(
        target.value(),
        imageformat::TsumugiManifestFileSystem::ntfs,
        4096U);
    if (status) {
      status = wim_store_->apply_locked_and_reverify(
          *task.source_table_index,
          archive.archive_hash,
          target.value().volume_device_path);
    }
    auto readback = status
        ? dependencies_.target_io->verify_volume_readback(
              target.value(),
              imageformat::TsumugiManifestFileSystem::ntfs,
              4096U,
              true)
        : clonecore::Result<WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
              status.error());
    const auto offline = dependencies_.target_io->dismount_and_offline_volume(
        target.value());
    disk_online_ = false;
    if (!offline) {
      abort_keep_offline_incomplete();
      return clonecore::Result<
          WindowsDirectShrinkAppliedPartitionEvidence>::failure(
              offline.error());
    }
    if (!readback ||
        !readback.value().file_system_metadata_verified ||
        !readback.value().namespace_fully_enumerated ||
        !readback.value().every_regular_file_read_to_eof) {
      return !readback
          ? clonecore::Result<WindowsDirectShrinkAppliedPartitionEvidence>::failure(
                readback.error())
          : failure<WindowsDirectShrinkAppliedPartitionEvidence>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"直接縮小production NTFS完全読戻し",
                L"filesystem metadata、完全namespace、または通常file EOF読戻しが不足しています");
    }
    auto digest = applied_digest(task, archive.archive_hash, readback.value());
    if (!digest) {
      return clonecore::Result<WindowsDirectShrinkAppliedPartitionEvidence>::failure(
          digest.error());
    }
    std::uint64_t readback_with_metadata{};
    if (!checked_add(
            readback.value().regular_file_bytes_read,
            1U,
            readback_with_metadata)) {
      return failure<WindowsDirectShrinkAppliedPartitionEvidence>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"直接縮小NTFS読戻し容量",
          L"通常file読戻し容量とmetadata証跡の集約がオーバーフローしました");
    }
    const std::uint64_t verified = (std::max<std::uint64_t>)(
        1U,
        (std::min)(task.construction_size_bytes, readback_with_metadata));
    archive_->applied = true;
    return clonecore::Result<WindowsDirectShrinkAppliedPartitionEvidence>::success({
        .source_table_index = *task.source_table_index,
        .target_number = task.target_number,
        .verified_target_bytes = verified,
        .archive_hash = archive.archive_hash,
        .target_write_digest = digest.take_value(),
        .every_write_flushed = true,
        .every_write_read_back = true,
        .file_system_metadata_verified = true,
        .target_offline = true,
    });
  }

  clonecore::Result<WindowsDirectShrinkExactRawPartitionEvidence>
  copy_exact_raw_and_verify(
      const WindowsDirectShrinkPartitionTask& task,
      const clonecore::ISourceDiskReader& read_only_source) override {
    constexpr std::size_t kChunkBytes = 4U * 1024U * 1024U;
    if (state_ != State::begun || disk_online_ || !writer_ ||
        archive_.has_value() || !task.source_table_index.has_value() ||
        task.kind !=
            WindowsDirectShrinkPartitionTaskKind::copy_exact_raw ||
        task.role != migrationcore::MigrationPartitionRole::data ||
        task.source_size_bytes == 0U ||
        task.construction_size_bytes != task.source_size_bytes ||
        task.target_size_bytes != task.source_size_bytes ||
        read_only_source.logical_sector_size() != 512U ||
        read_only_source.size_bytes() != plan_.expected_source().size_bytes ||
        writer_->logical_sector_size() != 512U ||
        writer_->size_bytes() != plan_.expected_target().size_bytes) {
      return failure<WindowsDirectShrinkExactRawPartitionEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小exact RAW初期条件",
          L"read-only source、offline target、元区画と同一容量のRAW task、または512-byte sectorを証明できません");
    }
    std::uint64_t source_end{};
    std::uint64_t target_end{};
    if (!checked_add(
            task.source_offset_bytes,
            task.source_size_bytes,
            source_end) ||
        !checked_add(
            task.target_offset_bytes,
            task.target_size_bytes,
            target_end) ||
        source_end > read_only_source.size_bytes() ||
        target_end > writer_->size_bytes() ||
        task.source_offset_bytes % 512U != 0U ||
        task.target_offset_bytes % 512U != 0U ||
        task.source_size_bytes % 512U != 0U) {
      return failure<WindowsDirectShrinkExactRawPartitionEvidence>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"直接縮小exact RAW extent",
          L"source/target extentの終端、整列、または不変容量が不正です");
    }
    const auto source_reader = [&](
        const std::uint64_t relative,
        const std::size_t length) {
      if (clonecore::disk_operation_cancellation_requested(
              request_.callbacks)) {
        return clonecore::Result<std::vector<std::byte>>::failure({
            .code = clonecore::ErrorCode::cancelled,
            .native_code = ERROR_CANCELLED,
            .operation = L"直接縮小exact RAW source Hash",
            .message = L"読取り専用区画のHash境界で取消しました",
        });
      }
      return read_only_source.read(task.source_offset_bytes + relative, length);
    };
    auto source_hash_before = imageformat::sha256_from_reader(
        task.source_size_bytes, kChunkBytes, source_reader);
    if (!source_hash_before || all_zero(source_hash_before.value())) {
      return source_hash_before
          ? failure<WindowsDirectShrinkExactRawPartitionEvidence>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"直接縮小exact RAW source Hash",
                L"コピー前source SHA-256が無効です")
          : clonecore::Result<
                WindowsDirectShrinkExactRawPartitionEvidence>::failure(
                source_hash_before.error());
    }

    std::uint64_t copied{};
    std::uint64_t chunks{};
    while (copied < task.source_size_bytes) {
      if (clonecore::disk_operation_cancellation_requested(
              request_.callbacks)) {
        abort_keep_offline_incomplete();
        return failure<WindowsDirectShrinkExactRawPartitionEvidence>(
            clonecore::ErrorCode::cancelled,
            ERROR_CANCELLED,
            L"直接縮小exact RAW取消",
            L"次のRAW chunk書込み前に取消し、targetをoffline未完了で保持しました");
      }
      const auto remaining = task.source_size_bytes - copied;
      const auto length = static_cast<std::size_t>((std::min<std::uint64_t>)(
          remaining, kChunkBytes));
      auto source = read_only_source.read(
          task.source_offset_bytes + copied, length);
      if (!source || source.value().size() != length) {
        abort_keep_offline_incomplete();
        return source
            ? failure<WindowsDirectShrinkExactRawPartitionEvidence>(
                  clonecore::ErrorCode::io_failed,
                  ERROR_HANDLE_EOF,
                  L"直接縮小exact RAW source読取り",
                  L"読取り専用sourceからchunk全体を読み取れません")
            : clonecore::Result<
                  WindowsDirectShrinkExactRawPartitionEvidence>::failure(
                  source.error());
      }
      auto status = write_flush_readback(
          *writer_,
          task.target_offset_bytes + copied,
          source.value(),
          L"直接縮小exact RAW chunk書込み・flush・読戻し");
      if (!status) {
        abort_keep_offline_incomplete();
        return clonecore::Result<
            WindowsDirectShrinkExactRawPartitionEvidence>::failure(
            status.error());
      }
      copied += length;
      ++chunks;
      clonecore::report_disk_operation_progress(
          request_.callbacks,
          clonecore::DiskOperationProgress{
              .stage = clonecore::DiskOperationStage::copying_data,
              .partition_index = task.source_table_index,
              .total_read_bytes = task.source_size_bytes,
              .total_write_bytes = task.target_size_bytes,
              .total_verify_bytes = task.target_size_bytes,
              .read_bytes = copied,
              .written_bytes = copied,
              .verified_bytes = copied,
              .cancellation_allowed = true,
              .pause_allowed = true,
          });
      if (clonecore::disk_operation_control_at_safe_boundary(
              request_.callbacks,
              clonecore::DiskOperationSafeBoundary{
                  .kind = clonecore::DiskOperationSafeBoundaryKind::
                      verified_chunk,
                  .stage = clonecore::DiskOperationStage::copying_data,
                  .partition_index = task.source_table_index,
                  .completed_bytes = copied,
                  .completed_units = chunks,
              }) == clonecore::DiskOperationControlDecision::
                  cancel_operation) {
        abort_keep_offline_incomplete();
        return failure<WindowsDirectShrinkExactRawPartitionEvidence>(
            clonecore::ErrorCode::cancelled,
            ERROR_CANCELLED,
            L"直接縮小exact RAW安全境界",
            L"検証済みchunk境界で取消し、targetをoffline未完了で保持しました");
      }
    }

    auto target_hash = imageformat::sha256_from_reader(
        task.target_size_bytes,
        kChunkBytes,
        [&](const std::uint64_t relative, const std::size_t length) {
          return writer_->read_back(task.target_offset_bytes + relative, length);
        });
    auto source_hash_after = imageformat::sha256_from_reader(
        task.source_size_bytes, kChunkBytes, source_reader);
    if (!target_hash || !source_hash_after ||
        target_hash.value() != source_hash_before.value() ||
        source_hash_after.value() != source_hash_before.value()) {
      abort_keep_offline_incomplete();
      return !target_hash
          ? clonecore::Result<
                WindowsDirectShrinkExactRawPartitionEvidence>::failure(
                target_hash.error())
          : !source_hash_after
                ? clonecore::Result<
                      WindowsDirectShrinkExactRawPartitionEvidence>::failure(
                      source_hash_after.error())
                : failure<WindowsDirectShrinkExactRawPartitionEvidence>(
                      clonecore::ErrorCode::verification_failed,
                      ERROR_CRC,
                      L"直接縮小exact RAW全体Hash",
                      L"コピー前後sourceまたはtarget全体SHA-256が一致しません");
    }
    return clonecore::Result<
        WindowsDirectShrinkExactRawPartitionEvidence>::success({
        .source_table_index = *task.source_table_index,
        .target_number = task.target_number,
        .verified_target_bytes = task.target_size_bytes,
        .verified_chunk_count = chunks,
        .source_sha256 = source_hash_before.take_value(),
        .target_sha256 = target_hash.value(),
        .target_write_digest = target_hash.take_value(),
        .source_reader_read_only = true,
        .source_extent_exact = true,
        .every_write_flushed = true,
        .every_chunk_read_back = true,
        .complete_target_hash_verified = true,
        .target_offline = true,
    });
  }

  clonecore::Status discard_exact_staged_archive(
      const WindowsDirectShrinkStagedArchiveEvidence& archive) override {
    if (state_ != State::begun || disk_online_ || !archive_ ||
        archive_->source_table_index != archive.source_table_index ||
        archive_->target_number != archive.target_number ||
        archive_->length != archive.archive_length ||
        archive_->hash != archive.archive_hash) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_STATE,
          L"直接縮小production WIM破棄",
          L"適用済みexact WIM証跡が現在の所有状態と一致しません");
    }
    auto staging = online_bind_staging();
    if (!staging) {
      return clonecore::Status::failure(staging.error());
    }
    auto status = wim_store_->discard_exact(
        archive.source_table_index, archive.archive_hash);
    const auto offline = dependencies_.target_io->dismount_and_offline_volume(
        staging.value());
    disk_online_ = false;
    if (!status || !offline) {
      abort_keep_offline_incomplete();
      return status ? offline : status;
    }
    archive_.reset();
    return clonecore::success_status();
  }

  clonecore::Result<WindowsDirectShrinkCheckpointEvidence>
  persist_prepared_partitions_checkpoint(
      const WindowsDirectShrinkCheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) override {
    return advance_checkpoint(
        previous,
        WindowsDirectShrinkCheckpointPhase::applying,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
  }

  clonecore::Result<WindowsDirectShrinkCheckpointEvidence>
  persist_progress_checkpoint(
      const WindowsDirectShrinkCheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) override {
    return advance_checkpoint(
        previous,
        WindowsDirectShrinkCheckpointPhase::applying,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
  }

  clonecore::Result<WindowsDirectShrinkBootEvidence>
  finalize_boot_from_staged_layout_and_verify(
      const WindowsDirectShrinkClonePlan& plan) override {
    if (state_ != State::commit_ready || disk_online_ ||
        !final_extents_prepared_ || boot_finalized_ ||
        !current_checkpoint_ ||
        current_checkpoint_->phase !=
            WindowsDirectShrinkCheckpointPhase::commit_ready ||
        plan.final_layout_hash() != plan_.final_layout_hash()) {
      return failure<WindowsDirectShrinkBootEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小production起動最終化",
          L"commit-ready checkpoint、hidden-final extent、単回offline状態、および同じ不変計画が必要です");
    }
    auto status = verify_hidden_partition_layout();
    if (status) {
      status = verify_checkpoint(*current_checkpoint_);
    }
    if (status) {
      status = revalidate_mbr_source_and_signature(
          L"直接縮小BIOS BCDBoot直前raw MBR再照合");
    }
    if (!status) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkBootEvidence>::failure(
          status.error());
    }
    if (!plan.boot_finalization_required()) {
      boot_finalized_ = true;
      return clonecore::Result<WindowsDirectShrinkBootEvidence>::success({
          .required = false,
          .completed = true,
          .boot_files_read_back_verified = true,
          .recovery_configuration_verified = true,
          .target_offline = true,
          .target_only_reconstruction = true,
          .exact_target_volume_extents = true,
          .legacy_bios = false,
          .real_boot_not_claimed = true,
      });
    }
    const bool legacy_bios = is_mbr_mode();
    const WindowsDirectShrinkPartitionTask* windows = nullptr;
    const WindowsDirectShrinkPartitionTask* system = nullptr;
    const WindowsDirectShrinkPartitionTask* recovery = nullptr;
    for (const auto& task : plan.tasks()) {
      if (task.role == migrationcore::MigrationPartitionRole::windows) {
        if (windows != nullptr) {
          return failure<WindowsDirectShrinkBootEvidence>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production Windows起動領域",
              L"Windows領域が一意ではありません");
        }
        windows = &task;
      } else if (
          (!legacy_bios &&
           task.role == migrationcore::MigrationPartitionRole::efi_system) ||
          (legacy_bios &&
           task.role == migrationcore::MigrationPartitionRole::bios_system)) {
        if (system != nullptr) {
          return failure<WindowsDirectShrinkBootEvidence>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production construction system領域",
              L"firmware別system領域が一意ではありません");
        }
        system = &task;
      } else if (
          task.role == migrationcore::MigrationPartitionRole::recovery) {
        if (recovery != nullptr) {
          return failure<WindowsDirectShrinkBootEvidence>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production WinRE領域",
              L"回復領域が一意ではありません");
        }
        recovery = &task;
      }
    }
    if (legacy_bios && system == nullptr) {
      system = windows;
    }
    const std::size_t active_count = static_cast<std::size_t>(std::count_if(
        plan.tasks().begin(),
        plan.tasks().end(),
        [](const WindowsDirectShrinkPartitionTask& task) {
          return task.active;
        }));
    if (windows == nullptr || system == nullptr ||
        (!legacy_bios && windows->target_number == system->target_number) ||
        (legacy_bios && (!system->active || active_count != 1U)) ||
        !dependencies_.finalize_boot ||
        (recovery != nullptr && !dependencies_.finalize_winre)) {
      return failure<WindowsDirectShrinkBootEvidence>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"直接縮小production起動依存",
          L"一意なWindows／firmware別system領域、exactly one Active、BCDBoot finalizer、またはWinRE finalizerがありません");
    }

    auto windows_volume = online_bind_target(*windows, true);
    if (!windows_volume) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkBootEvidence>::failure(
          windows_volume.error());
    }
    WindowsTsumugiShrinkVolumeBinding system_volume = windows_volume.value();
    if (system->target_number != windows->target_number) {
      auto bound_system = dependencies_.target_io->bind_online_volume(
          system->target_number,
          system->target_offset_bytes,
          system->target_size_bytes);
      if (!bound_system ||
          bound_system.value().disk_number !=
              windows_volume.value().disk_number ||
          equal_path(
              bound_system.value().volume_device_path,
              windows_volume.value().volume_device_path)) {
        static_cast<void>(dependencies_.target_io->set_target_offline(true));
        disk_online_ = false;
        abort_keep_offline_incomplete();
        return bound_system
            ? failure<WindowsDirectShrinkBootEvidence>(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_DEVICE_REINITIALIZATION_NEEDED,
                  L"直接縮小production construction system volume拘束",
                  L"Windowsとsystem領域が同じ再識別済み対象の異なるexact Volume GUIDではありません")
            : clonecore::Result<WindowsDirectShrinkBootEvidence>::failure(
                  bound_system.error());
      }
      system_volume = bound_system.take_value();
    }
    std::optional<WindowsTsumugiShrinkVolumeBinding> recovery_volume;
    if (recovery != nullptr) {
      auto bound = dependencies_.target_io->bind_online_volume(
          recovery->target_number,
          recovery->target_offset_bytes,
          recovery->target_size_bytes);
      if (!bound ||
          bound.value().disk_number != windows_volume.value().disk_number) {
        static_cast<void>(dependencies_.target_io->set_target_offline(true));
        disk_online_ = false;
        abort_keep_offline_incomplete();
        return bound
            ? failure<WindowsDirectShrinkBootEvidence>(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_DEVICE_REINITIALIZATION_NEEDED,
                  L"直接縮小production WinRE volume拘束",
                  L"Windowsと回復領域が同じ再識別済み対象にありません")
            : clonecore::Result<WindowsDirectShrinkBootEvidence>::failure(
                  bound.error());
      }
      recovery_volume = bound.take_value();
    }

    auto finalized = dependencies_.finalize_boot(
        WindowsDirectShrinkBootFinalizationRequest{
            .expected_source = plan_.expected_source(),
            .expected_target = plan_.expected_target(),
            .confirmation = request_.confirmation,
            .expected_target_disk_number =
                windows_volume.value().disk_number,
            .expected_windows_partition_number = windows->target_number,
            .expected_windows_partition_offset = windows->target_offset_bytes,
            .expected_windows_partition_size = windows->target_size_bytes,
            .expected_system_partition_number = system->target_number,
            .expected_system_partition_offset = system->target_offset_bytes,
            .expected_system_partition_size = system->target_size_bytes,
            .expected_mbr_disk_signature = legacy_bios
                ? plan_.mbr_preserve_binding()->target_disk_signature
                : 0U,
            .windows_volume_root =
                windows_volume.value().volume_device_path,
            .system_volume_root = system_volume.volume_device_path,
            .firmware = legacy_bios
                ? bootrepair::BcdBootFirmware::bios
                : bootrepair::BcdBootFirmware::uefi,
        });
    const bool firmware_evidence_valid = finalized &&
        (legacy_bios
             ? finalized.value().legacy_bios &&
                   !finalized.value().construction_gpt_non_bootable_verified &&
                   !finalized.value().efi_ownership_safe_before_mount &&
                   !finalized.value().
                       efi_ownership_revalidated_before_mutation &&
                   !finalized.value().
                       microsoft_boot_namespace_read_back_verified
             : !finalized.value().legacy_bios &&
                   finalized.value().construction_gpt_non_bootable_verified &&
                   finalized.value().efi_ownership_safe_before_mount &&
                   finalized.value().
                       efi_ownership_revalidated_before_mutation &&
                   finalized.value().
                       microsoft_boot_namespace_read_back_verified);
    if (!finalized || !finalized.value().microsoft_signed_bcdboot ||
        !finalized.value().fresh_bcd_store_read_back_verified ||
        !firmware_evidence_valid ||
        !finalized.value().temporary_mounts_released ||
        !finalized.value().final_target_reidentified ||
        !finalized.value().partition_layout_unchanged ||
        !finalized.value().nvram_unchanged ||
        !finalized.value().exact_target_volume_extents ||
        !finalized.value().target_only_reconstruction) {
      static_cast<void>(dependencies_.target_io->set_target_offline(true));
      disk_online_ = false;
      abort_keep_offline_incomplete();
      return finalized
          ? failure<WindowsDirectShrinkBootEvidence>(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"直接縮小production BCDBoot証跡",
                L"Microsoft署名、新規BCD、firmware別nonboot construction、exact target volume、target-only、mount解放、対象再識別、layout不変、またはNVRAM不変の証跡が不足しています")
          : clonecore::Result<WindowsDirectShrinkBootEvidence>::failure(
                finalized.error());
    }

    bool recovery_verified = recovery == nullptr;
    if (recovery != nullptr) {
      auto winre = dependencies_.finalize_winre(
          WindowsDirectShrinkWinReFinalizationRequest{
              .expected_source = plan_.expected_source(),
              .expected_target = plan_.expected_target(),
              .confirmation = request_.confirmation,
              .expected_target_disk_number =
                  windows_volume.value().disk_number,
              .expected_windows_partition_number = windows->target_number,
              .expected_windows_partition_offset =
                  windows->target_offset_bytes,
              .expected_windows_partition_size =
                  windows->target_size_bytes,
              .expected_recovery_partition_number = recovery->target_number,
              .expected_recovery_partition_offset =
                  recovery->target_offset_bytes,
              .expected_recovery_partition_size =
                  recovery->target_size_bytes,
              .expected_partition_style = plan_.partition_style(),
              .expected_mbr_disk_signature = legacy_bios
                  ? plan_.mbr_preserve_binding()->target_disk_signature
                  : 0U,
              .windows_volume_root =
                  windows_volume.value().volume_device_path,
              .recovery_volume_root =
                  recovery_volume->volume_device_path,
          });
      recovery_verified = winre.has_value() &&
          winre.value().registered_partition_number == recovery->target_number &&
          winre.value().registered_image_size_bytes != 0U &&
          winre.value().microsoft_signed_reagentc &&
          winre.value().cloned_source_registration_disabled &&
          winre.value().candidate_identity_locked &&
          winre.value().fixed_setreimage_arguments &&
          winre.value().fixed_enable_arguments &&
          winre.value().
              target_revalidated_before_each_mutation_and_diagnostic &&
          winre.value().read_only_reinspection_completed &&
          winre.value().registered_location_matches_expected_target &&
          winre.value().registered_image_present &&
          winre.value().temporary_mounts_released;
      if (!recovery_verified) {
        static_cast<void>(dependencies_.target_io->set_target_offline(true));
        disk_online_ = false;
        abort_keep_offline_incomplete();
        return winre
            ? failure<WindowsDirectShrinkBootEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"直接縮小production WinRE証跡",
                  L"署名済みREAgentC固定引数、登録先、Winre.wim、または読取り専用再診断の証跡が不足しています")
            : clonecore::Result<WindowsDirectShrinkBootEvidence>::failure(
                  winre.error());
      }
    }
    const auto offline =
        dependencies_.target_io->dismount_and_offline_volume(
            windows_volume.value());
    disk_online_ = false;
    if (!offline) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkBootEvidence>::failure(
          offline.error());
    }
    status = verify_hidden_partition_layout();
    if (status) {
      status = verify_checkpoint(*current_checkpoint_);
    }
    if (!status) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkBootEvidence>::failure(
          status.error());
    }
    boot_finalized_ = true;
    return clonecore::Result<WindowsDirectShrinkBootEvidence>::success({
        .required = true,
        .completed = true,
        .boot_files_read_back_verified = true,
        .recovery_configuration_verified = recovery_verified,
        .target_offline = true,
        .target_only_reconstruction = true,
        .exact_target_volume_extents = true,
        .legacy_bios = legacy_bios,
        .real_boot_not_claimed = true,
    });
  }

  clonecore::Result<WindowsDirectShrinkCheckpointEvidence>
  seal_commit_ready_checkpoint(
      const WindowsDirectShrinkCheckpointEvidence& previous,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) override {
    auto result = advance_checkpoint(
        previous,
        WindowsDirectShrinkCheckpointPhase::commit_ready,
        completed_task_count,
        verified_target_bytes,
        aggregate_write_digest);
    if (result) {
      state_ = State::commit_ready;
    }
    return result;
  }

  clonecore::Result<WindowsDirectShrinkCheckpointEvidence>
  prepare_final_extents_keep_incomplete_and_verify(
      const WindowsDirectShrinkClonePlan& plan,
      const WindowsDirectShrinkCheckpointEvidence& expected) override {
    if (state_ != State::commit_ready || disk_online_ ||
        final_extents_prepared_ || boot_finalized_ ||
        final_layout_published_ || archive_.has_value() ||
        plan.final_layout_hash() != plan_.final_layout_hash() ||
        expected.phase != WindowsDirectShrinkCheckpointPhase::commit_ready) {
      return failure<WindowsDirectShrinkCheckpointEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小production hidden-final extent準備",
          L"単回commit-ready、archive破棄、offline、および同じ不変計画が必要です");
    }
    auto status = reidentify_current_target();
    if (status) {
      status = verify_gpt_plan(*writer_, temporary_gpt_);
    }
    if (status) {
      status = verify_checkpoint(expected);
    }
    if (status) {
      status = revalidate_mbr_source_and_signature(
          L"直接縮小hidden MBR公開前raw MBR再照合");
    }
    if (status) {
      // No archive can remain at this boundary. Release every object tied to
      // the soon-to-be-removed staging extent before changing the GPT.
      wim_store_.reset();
      staging_volume_.reset();
      status = invalidate_exact_gpt_metadata(*writer_, temporary_gpt_);
    }
    if (status) {
      status = is_mbr_mode()
          ? write_flush_readback(
                *writer_,
                0U,
                hidden_final_mbr_sector_,
                L"直接縮小nonboot hidden-final MBR公開")
          : publish_gpt_plan(
                *writer_,
                hidden_final_gpt_,
                L"直接縮小nonboot hidden-final GPT公開");
    }
    if (status) {
      status = verify_hidden_partition_layout();
    }
    if (status) {
      // Even with no extension, staging removal and the final extents must be
      // visible to the volume binding layer before boot reconstruction.
      status = dependencies_.target_io->notify_layout_changed();
    }

    std::uint64_t extended_count{};
    for (const auto& task : plan_.tasks()) {
      if (!status || task.construction_size_bytes == task.target_size_bytes) {
        continue;
      }
      auto volume = online_bind_target(task, true);
      if (!volume) {
        status = clonecore::Status::failure(volume.error());
        break;
      }
      auto extended = dependencies_.target_io->
          extend_ntfs_volume_to_exact_extent_and_verify(
              volume.value(), task.construction_size_bytes, 4096U);
      auto readback = extended
          ? dependencies_.target_io->verify_volume_readback(
                volume.value(),
                imageformat::TsumugiManifestFileSystem::ntfs,
                4096U,
                true)
          : clonecore::Result<
                WindowsTsumugiShrinkFileSystemReadbackEvidence>::failure(
                extended.error());
      const auto offline =
          dependencies_.target_io->dismount_and_offline_volume(volume.value());
      disk_online_ = false;
      if (!extended || !readback || !offline ||
          extended.value().previous_file_system_bytes !=
              task.construction_size_bytes ||
          extended.value().final_file_system_bytes != task.target_size_bytes ||
          extended.value().final_partition_extent_bytes !=
              task.target_size_bytes ||
          !extended.value().exact_single_extent_reverified ||
          !extended.value().ntfs_sector_count_reverified ||
          !extended.value().flushed ||
          !readback.value().file_system_metadata_verified ||
          !readback.value().namespace_fully_enumerated ||
          !readback.value().every_regular_file_read_to_eof) {
        status = !extended
            ? clonecore::Status::failure(extended.error())
            : !readback
                  ? clonecore::Status::failure(readback.error())
                  : !offline
                        ? offline
                        : status_failure(
                              clonecore::ErrorCode::verification_failed,
                              ERROR_CRC,
                              L"直接縮小NTFS hidden-final伸長証跡",
                              L"旧・新寸法、exact extent、NTFS sector、flush、または全namespace読戻しが不足しています");
        break;
      }
      if (plan_.surplus_allocation() ==
          migrationcore::ShrinkSurplusAllocation::
              selected_data_partition) {
        if (!task.source_table_index.has_value() ||
            task.source_table_index !=
                plan_.surplus_target_source_table_index() ||
            task.role != migrationcore::MigrationPartitionRole::data ||
            plan_.staging().final_growth_owner_target_number !=
                task.target_number ||
            targeted_surplus_source_table_index_.has_value()) {
          status = status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_REINITIALIZATION_NEEDED,
              L"直接縮小指定NTFS余剰所有者",
              L"レビュー済みsource table index、target number、data役割、または単一所有者が一致しません");
          break;
        }
        targeted_surplus_source_table_index_ = task.source_table_index;
        targeted_surplus_target_number_ = task.target_number;
        targeted_surplus_previous_file_system_bytes_ =
            extended.value().previous_file_system_bytes;
        targeted_surplus_final_file_system_bytes_ =
            extended.value().final_file_system_bytes;
        targeted_surplus_owner_verified_ = true;
        targeted_surplus_exact_size_verified_ =
            extended.value().final_file_system_bytes ==
                task.target_size_bytes &&
            extended.value().final_partition_extent_bytes ==
                task.target_size_bytes &&
            extended.value().exact_single_extent_reverified &&
            extended.value().ntfs_sector_count_reverified;
        targeted_surplus_readback_verified_ =
            readback.value().file_system_metadata_verified &&
            readback.value().namespace_fully_enumerated &&
            readback.value().every_regular_file_read_to_eof;
      }
      ++extended_count;
    }
    if (status && extended_count != plan_.ntfs_extension_task_count()) {
      status = status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"直接縮小NTFS hidden-final伸長件数",
          L"計画した全NTFS伸長を完了していません");
    }
    const bool targeted_surplus = plan_.surplus_allocation() ==
        migrationcore::ShrinkSurplusAllocation::selected_data_partition;
    if (status && targeted_surplus &&
        (!targeted_surplus_source_table_index_.has_value() ||
         targeted_surplus_source_table_index_ !=
             plan_.surplus_target_source_table_index() ||
         !targeted_surplus_target_number_.has_value() ||
         targeted_surplus_target_number_ !=
             plan_.staging().final_growth_owner_target_number ||
         !targeted_surplus_owner_verified_ ||
         !targeted_surplus_exact_size_verified_ ||
         !targeted_surplus_readback_verified_)) {
      status = status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"直接縮小指定NTFS余剰の最終証跡",
          L"指定所有者、exact filesystem寸法、または全namespace読戻し証跡が不足しています");
    }
    if (status) {
      status = reidentify_current_target();
    }
    if (status) {
      status = verify_hidden_partition_layout();
    }
    if (status) {
      status = verify_checkpoint(expected);
    }
    if (status) {
      status = revalidate_mbr_source_and_signature(
          L"直接縮小最終MBR公開前raw MBR再照合");
    }
    if (!status) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          status.error());
    }
    prepared_extension_count_ = extended_count;
    final_extents_prepared_ = true;
    // Core synthesizes the successful non-required boot evidence for data-only
    // plans and therefore does not call the boot finalizer in that branch.
    boot_finalized_ = !plan_.boot_finalization_required();
    return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::success(
        expected);
  }

  clonecore::Result<WindowsDirectShrinkCheckpointEvidence>
  revalidate_before_final_commit(
      const WindowsDirectShrinkClonePlan& plan,
      const WindowsDirectShrinkCheckpointEvidence& expected) override {
    if (state_ != State::commit_ready || disk_online_ ||
        !final_extents_prepared_ || !boot_finalized_ ||
        final_layout_published_ || archive_.has_value() ||
        plan.final_layout_hash() != plan_.final_layout_hash()) {
      return failure<WindowsDirectShrinkCheckpointEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小production最終commit前再照合",
          L"commit-ready、hidden-final extent、boot最終化、archive破棄、または不変計画が成立していません");
    }
    auto status = reidentify_current_target();
    if (status) {
      status = verify_hidden_partition_layout();
    }
    if (status) {
      status = verify_checkpoint(expected);
    }
    if (status) {
      status = revalidate_mbr_source_and_signature(
          L"直接縮小最終commit前raw MBR再照合");
    }
    if (!status) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          status.error());
    }
    return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::success(
        expected);
  }

  clonecore::Result<WindowsDirectShrinkFinalCommitEvidence>
  commit_final_layout_last(
      const WindowsDirectShrinkClonePlan& plan,
      const WindowsDirectShrinkCheckpointEvidence& commit_ready) override {
    if (state_ != State::commit_ready || disk_online_ ||
        !final_extents_prepared_ || !boot_finalized_ ||
        final_layout_published_ || archive_.has_value() ||
        plan.final_layout_hash() != plan_.final_layout_hash()) {
      return failure<WindowsDirectShrinkFinalCommitEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小production最終commit",
          L"commit-ready、hidden-final extent、boot最終化、archive破棄、または不変計画が成立していません");
    }
    // Allocate the cleanup buffer before the irreversible final-publication
    // boundary. No allocation or fallible evidence construction is needed
    // after the final GPT or final MBR sector0 has been read back.
    std::vector<std::byte> checkpoint_zeroes(
        kCheckpointBytes, std::byte{0});
    auto status = reidentify_current_target();
    if (status) {
      status = verify_checkpoint(commit_ready);
    }
    if (status) {
      status = verify_hidden_partition_layout();
    }
    if (status) {
      status = revalidate_mbr_source_and_signature(
          L"直接縮小sector0-last直前raw MBR再照合");
    }
    if (status && is_mbr_mode()) {
      status = write_flush_readback(
          *writer_,
          0U,
          final_mbr_->sector,
          L"直接縮小visible final MBR sector0最終公開");
    } else if (status) {
      status = invalidate_exact_gpt_metadata(*writer_, hidden_final_gpt_);
      if (status) {
        status = publish_gpt_plan(
            *writer_, final_gpt_, L"直接縮小visible final GPT最終公開");
      }
    }
    if (!status) {
      abort_keep_offline_incomplete();
      return clonecore::Result<WindowsDirectShrinkFinalCommitEvidence>::failure(
          status.error());
    }

    // This latch is the irreversible safe boundary: final GPT metadata or the
    // single final MBR sector0 and its complete readback are proven while target
    // remains offline. Do not add a second fallible readback before this latch:
    // a transient failure there could otherwise invalidate an already proven
    // final layout. Nothing after this point may invalidate it or report failure.
    final_layout_published_ = true;
    bool checkpoint_retired = false;
    try {
      const auto retired = write_flush_readback(
          *writer_,
          plan_.checkpoint_offset_bytes(),
          checkpoint_zeroes,
          L"直接縮小checkpoint退役");
      checkpoint_retired = retired.has_value();
    } catch (...) {
      checkpoint_retired = false;
    }
    if (checkpoint_retired) {
      current_checkpoint_.reset();
      current_checkpoint_record_.clear();
    }
    state_ = State::committed;
    abort_cleanup_complete_ = true;
    return clonecore::Result<WindowsDirectShrinkFinalCommitEvidence>::success({
        .committed_layout_hash = plan_.final_layout_hash(),
        .aggregate_write_digest = commit_ready.aggregate_write_digest,
        .source_reidentified = true,
        .source_layout_unchanged = true,
        .target_reidentified = true,
        .staging_identity_reverified = true,
        .checkpoint_reverified = true,
        .staging_removed = true,
        .checkpoint_retired = checkpoint_retired,
        .checkpoint_retirement_pending = !checkpoint_retired,
        .construction_layout_non_bootable = true,
        .checkpoint_retained_through_extensions_and_boot = true,
        .boot_completed_before_final_layout_publication = true,
        .final_layout_published_before_checkpoint_retirement = true,
        .hidden_final_layout_published_and_read_back = true,
        .extended_ntfs_partition_count = prepared_extension_count_,
        .every_required_ntfs_extension_verified = true,
        .targeted_surplus_source_table_index =
            targeted_surplus_source_table_index_,
        .targeted_surplus_target_number = targeted_surplus_target_number_,
        .targeted_surplus_previous_file_system_bytes =
            targeted_surplus_previous_file_system_bytes_,
        .targeted_surplus_final_file_system_bytes =
            targeted_surplus_final_file_system_bytes_,
        .targeted_surplus_owner_verified =
            targeted_surplus_owner_verified_,
        .targeted_surplus_exact_size_verified =
            targeted_surplus_exact_size_verified_,
        .targeted_surplus_readback_verified =
            targeted_surplus_readback_verified_,
        .every_write_flushed = true,
        .every_write_read_back = true,
        .primary_layout_committed_last = true,
        .target_offline = true,
        .final_partition_style = plan_.partition_style(),
        .source_mbr_sector0_unchanged = is_mbr_mode(),
        .source_mbr_bootstrap_unchanged = is_mbr_mode(),
        .target_mbr_signature_collision_free = is_mbr_mode(),
        .final_mbr_sector0_read_back_verified = is_mbr_mode(),
        .final_mbr_disk_signature = is_mbr_mode()
            ? final_mbr_->target_disk.disk_signature
            : 0U,
        .final_mbr_active_partition_count = is_mbr_mode()
            ? static_cast<std::uint32_t>(std::count_if(
                  final_mbr_->target_disk.partitions.begin(),
                  final_mbr_->target_disk.partitions.end(),
                  [](const clonecore::MbrPartition& partition) {
                    return partition.active;
                  }))
            : 0U,
    });
  }

  void abort_keep_offline_incomplete() noexcept override {
    if (state_ == State::committed ||
        (state_ == State::aborted && abort_cleanup_complete_)) {
      return;
    }
    // Factory construction is deliberately a pre-VSS pure preflight.  If no
    // target byte/state was ever touched, destruction must likewise perform
    // no target transition.
    if (!target_touched_) {
      state_ = State::aborted;
      abort_cleanup_complete_ = true;
      return;
    }
    // A successfully published and read-back final GPT is complete. A later
    // checkpoint-cleanup problem must never turn it back into an invalid disk.
    if (final_layout_published_) {
      bool offline = !disk_online_;
      try {
        if (!offline && dependencies_.target_io) {
          offline = dependencies_.target_io->set_target_offline(true).has_value();
        }
      } catch (...) {
        offline = false;
      }
      if (offline) {
        disk_online_ = false;
        state_ = State::committed;
        abort_cleanup_complete_ = true;
      } else {
        state_ = State::aborted;
        abort_cleanup_complete_ = false;
      }
      return;
    }
    bool safely_offline = false;
    try {
      if (dependencies_.target_io) {
        const auto offline =
            dependencies_.target_io->set_target_offline(true);
        safely_offline = offline.has_value();
      }
      if (safely_offline) {
        disk_online_ = false;
      }
      if (safely_offline && writer_) {
        abort_cleanup_complete_ =
            invalidate_exact_gpt_metadata(*writer_, temporary_gpt_).has_value();
      }
    } catch (...) {
      safely_offline = false;
      abort_cleanup_complete_ = false;
    }
    state_ = State::aborted;
  }

 private:
  enum class State : std::uint8_t {
    created,
    begun,
    commit_ready,
    committed,
    aborted,
  };

  struct ActiveArchive final {
    std::uint32_t source_table_index{};
    std::uint32_t target_number{};
    std::uint64_t length{};
    imageformat::Sha256Digest hash{};
    bool applied{};
  };

  [[nodiscard]] bool is_mbr_mode() const noexcept {
    return final_mbr_.has_value();
  }

  clonecore::Status verify_hidden_partition_layout() {
    if (!writer_) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_HANDLE,
          L"直接縮小hidden layout読戻し",
          L"offline target writerがありません");
    }
    return is_mbr_mode()
        ? verify_mbr_sector(
              *writer_,
              hidden_final_mbr_sector_,
              L"直接縮小nonboot hidden MBR読戻し")
        : verify_gpt_plan(*writer_, hidden_final_gpt_);
  }

  clonecore::Status revalidate_mbr_source_and_signature(
      const std::wstring_view operation) {
    if (!is_mbr_mode()) {
      return clonecore::success_status();
    }
    const auto& binding = plan_.mbr_preserve_binding();
    if (!binding.has_value() || !dependencies_.observe_mbr_safety) {
      return status_failure(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_HANDLE,
          std::wstring(operation),
          L"immutable MBR bindingまたはsource/signature observerがありません");
    }
    auto observed = dependencies_.observe_mbr_safety(
        plan_.expected_source(), plan_.expected_target(), false);
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    if (observed.value().source_sector0_hash != binding->source_sector0_hash ||
        observed.value().source_bootstrap != binding->source_bootstrap ||
        observed.value().source_disk_signature !=
            binding->source_disk_signature ||
        binding->target_disk_signature == 0U ||
        std::find(
            observed.value().connected_mbr_signatures_excluding_target.begin(),
            observed.value().connected_mbr_signatures_excluding_target.end(),
            binding->target_disk_signature) !=
            observed.value().connected_mbr_signatures_excluding_target.end()) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          std::wstring(operation),
          L"source raw MBR sector0/bootstrapまたはfresh target signatureの非衝突性が計画後に変化しました");
    }
    return clonecore::success_status();
  }

  clonecore::Result<WindowsDirectShrinkCheckpointEvidence>
  persist_checkpoint(WindowsDirectShrinkCheckpointEvidence evidence) {
    if (disk_online_ || !writer_ || evidence.revision == 0U ||
        evidence.completed_task_count > plan_.tasks().size() ||
        evidence.verified_target_bytes >
            plan_.operation_plan().expected_work_bytes) {
      return failure<WindowsDirectShrinkCheckpointEvidence>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"直接縮小checkpoint永続化",
          L"Writer、revision、task件数、または検証容量が不正です");
    }
    evidence.record_hash = {};
    auto record = checkpoint_record(evidence);
    if (!record) {
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          record.error());
    }
    auto hash = imageformat::sha256(record.value());
    if (!hash) {
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          hash.error());
    }
    evidence.record_hash = hash.take_value();
    auto status = write_flush_readback(
        *writer_,
        plan_.checkpoint_offset_bytes(),
        record.value(),
        L"直接縮小checkpoint書込み");
    if (!status) {
      return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::failure(
          status.error());
    }
    current_checkpoint_ = evidence;
    current_checkpoint_record_ = record.take_value();
    return clonecore::Result<WindowsDirectShrinkCheckpointEvidence>::success(
        std::move(evidence));
  }

  clonecore::Result<WindowsDirectShrinkCheckpointEvidence>
  advance_checkpoint(
      const WindowsDirectShrinkCheckpointEvidence& previous,
      const WindowsDirectShrinkCheckpointPhase phase,
      const std::uint64_t completed_task_count,
      const std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) {
    if ((state_ != State::begun && state_ != State::commit_ready) ||
        !current_checkpoint_ || archive_.has_value() ||
        previous.record_hash != current_checkpoint_->record_hash ||
        previous.revision != current_checkpoint_->revision ||
        completed_task_count < previous.completed_task_count ||
        verified_target_bytes < previous.verified_target_bytes ||
        completed_task_count > plan_.tasks().size() ||
        verified_target_bytes > plan_.operation_plan().expected_work_bytes) {
      return failure<WindowsDirectShrinkCheckpointEvidence>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_STATE,
          L"直接縮小checkpoint遷移",
          L"現在record、単調進捗、またはarchive破棄境界が一致しません");
    }
    auto next = previous;
    next.phase = phase;
    ++next.revision;
    next.completed_task_count = completed_task_count;
    next.verified_target_bytes = verified_target_bytes;
    next.aggregate_write_digest = aggregate_write_digest;
    next.record_hash = {};
    return persist_checkpoint(std::move(next));
  }

  clonecore::Status verify_checkpoint(
      const WindowsDirectShrinkCheckpointEvidence& expected) {
    if (!writer_ || !current_checkpoint_ ||
        !checkpoint_exactly_matches(expected, *current_checkpoint_) ||
        current_checkpoint_record_.size() != kCheckpointBytes) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小checkpoint memory再照合",
          L"期待recordと現在の単回所有checkpointが一致しません");
    }
    auto observed = writer_->read_back(
        plan_.checkpoint_offset_bytes(), current_checkpoint_record_.size());
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    auto hash = imageformat::sha256(observed.value());
    if (observed.value() != current_checkpoint_record_ || !hash ||
        hash.value() != expected.record_hash) {
      return !hash
          ? clonecore::Status::failure(hash.error())
          : status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"直接縮小checkpoint完全読戻し",
                L"target-owned checkpoint byteまたはHashが変化しました");
    }
    return clonecore::success_status();
  }

  clonecore::Result<WindowsTsumugiShrinkVolumeBinding> online_bind_staging() {
    if (disk_online_ || !staging_volume_) {
      return failure<WindowsTsumugiShrinkVolumeBinding>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_STATE,
          L"直接縮小staging volume再接続",
          L"初期staging bindingがありません");
    }
    auto status = dependencies_.target_io->set_target_offline(false);
    if (!status) {
      return clonecore::Result<WindowsTsumugiShrinkVolumeBinding>::failure(
          status.error());
    }
    disk_online_ = true;
    auto bound = dependencies_.target_io->bind_online_volume(
        staging_volume_->final_target_number,
        plan_.staging().archive_offset_bytes,
        plan_.staging().archive_capacity_bytes);
    if (!bound ||
        !equal_path(
            bound.value().volume_device_path,
            staging_volume_->volume_device_path)) {
      static_cast<void>(dependencies_.target_io->set_target_offline(true));
      disk_online_ = false;
      return bound
          ? failure<WindowsTsumugiShrinkVolumeBinding>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"直接縮小staging Volume GUID再照合",
                L"同じ一時GPT extentが別のVolume GUIDへ対応しました")
          : clonecore::Result<WindowsTsumugiShrinkVolumeBinding>::failure(
                bound.error());
    }
    return bound;
  }

  clonecore::Result<WindowsTsumugiShrinkVolumeBinding> online_bind_target(
      const WindowsDirectShrinkPartitionTask& task,
      const bool final_extent = false) {
    if (disk_online_) {
      return failure<WindowsTsumugiShrinkVolumeBinding>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"直接縮小target volume再接続",
          L"同時に一つのtarget disk online区間だけを開始できます");
    }
    auto status = dependencies_.target_io->set_target_offline(false);
    if (!status) {
      return clonecore::Result<WindowsTsumugiShrinkVolumeBinding>::failure(
          status.error());
    }
    disk_online_ = true;
    auto bound = dependencies_.target_io->bind_online_volume(
        task.target_number,
        task.target_offset_bytes,
        final_extent ? task.target_size_bytes : task.construction_size_bytes);
    if (!bound) {
      static_cast<void>(dependencies_.target_io->set_target_offline(true));
      disk_online_ = false;
    }
    return bound;
  }

  clonecore::Status reidentify_current_target() {
    if (!dependencies_.reidentify_confirmed) {
      return status_failure(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_HANDLE,
          L"直接縮小production再識別",
          L"confirmed reidentifierがありません");
    }
    auto observed = dependencies_.reidentify_confirmed(
        plan_.expected_source(),
        plan_.expected_target(),
        request_.confirmation);
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    auto status = clonecore::validate_clone_identities(
        plan_.expected_source(),
        observed.value().source_identity,
        plan_.expected_target(),
        observed.value().target_identity,
        request_.confirmation,
        false);
    if (!status) {
      return status;
    }
    auto source_layout =
        imageformat::hash_tsumugi_physical_restore_target_layout_v1(
            observed.value().source);
    if (!source_layout) {
      return clonecore::Status::failure(source_layout.error());
    }
    const auto source_style = diskmodel::normalize_disk_partition_style(
        observed.value().source.partition_style,
        observed.value().source.partitions.size());
    if (!source_partition_style_matches(
            source_style, plan_.source_partition_style()) ||
        source_layout.value() != plan_.expected_source_layout_hash() ||
        observed.value().source.is_system_disk !=
            plan_.expected_source().is_system_disk ||
        !observed.value().source.offline.has_value() ||
        observed.value().source.offline.value() ||
        !observed.value().source.read_only.has_value() ||
        observed.value().source.read_only.value() ||
        !observed.value().source.removable.has_value() ||
        observed.value().source.removable.value() ||
        observed.value().source.logical_sector_size != 512U) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小production source layout再照合",
          L"最終commit前のsource形式、layout Hash、system属性、または固定online状態が不変計画と一致しません");
    }
    if (is_mbr_mode()) {
      status = validate_mbr_preserve_source_bindings(
          plan_, observed.value().source);
      if (!status) {
        return status;
      }
    } else if (plan_.partition_style_choice() ==
               migrationcore::DirectClonePartitionStyleChoice::mbr_to_gpt) {
      status = validate_mbr_to_gpt_source_bindings(
          plan_, observed.value().source);
      if (!status) {
        return status;
      }
    }
    if (!observed.value().target.offline.value_or(false) ||
        observed.value().target.read_only.value_or(true) ||
        observed.value().target.removable.value_or(true) ||
        observed.value().target.is_system_disk ||
        observed.value().target.logical_sector_size != 512U) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"直接縮小production対象状態再照合",
          L"最終commit前の対象がoffline／writeable／fixed／非system／512-byteではありません");
    }
    return clonecore::success_status();
  }

  WindowsDirectShrinkClonePlan plan_;
  WindowsDirectShrinkClonePlatformRequest request_;
  WindowsDirectShrinkClonePlatformDependencies dependencies_;
  clonecore::GptWritePlan final_gpt_;
  clonecore::GptWritePlan temporary_gpt_;
  clonecore::GptWritePlan hidden_final_gpt_;
  std::optional<clonecore::MbrWritePlan> final_mbr_;
  std::vector<std::byte> hidden_final_mbr_sector_;
  std::unique_ptr<clonecore::ITargetDiskWriter> writer_;
  std::unique_ptr<IWindowsDirectShrinkOwnedWimStore> wim_store_;
  std::optional<WindowsTsumugiShrinkVolumeBinding> staging_volume_;
  std::optional<ActiveArchive> archive_;
  std::optional<WindowsDirectShrinkCheckpointEvidence> current_checkpoint_;
  std::vector<std::byte> current_checkpoint_record_;
  State state_{State::created};
  bool disk_online_{};
  bool target_touched_{};
  bool final_extents_prepared_{};
  bool boot_finalized_{};
  bool final_layout_published_{};
  std::uint64_t prepared_extension_count_{};
  std::optional<std::uint32_t>
      targeted_surplus_source_table_index_;
  std::optional<std::uint32_t> targeted_surplus_target_number_;
  std::uint64_t targeted_surplus_previous_file_system_bytes_{};
  std::uint64_t targeted_surplus_final_file_system_bytes_{};
  bool targeted_surplus_owner_verified_{};
  bool targeted_surplus_exact_size_verified_{};
  bool targeted_surplus_readback_verified_{};
  bool abort_cleanup_complete_{};
};

clonecore::Status validate_factory_inputs(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const WindowsDirectShrinkClonePlatformRequest& request,
    const WindowsDirectShrinkClonePlatformDependencies& dependencies) {
  auto status = clonecore::validate_clone_identities(
      plan.expected_source(),
      observed.source_identity,
      plan.expected_target(),
      observed.target_identity,
      request.confirmation,
      false);
  if (!status) {
    return status;
  }
  auto source_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.source);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          observed.target);
  if (!source_layout || !target_layout) {
    return clonecore::Status::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  const auto source_class =
      imageformat::classify_tsumugi_physical_restore_target(observed.source);
  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(observed.target);
  const auto source_style = diskmodel::normalize_disk_partition_style(
      observed.source.partition_style, observed.source.partitions.size());
  const auto target_style = diskmodel::normalize_disk_partition_style(
      observed.target.partition_style, observed.target.partitions.size());
  const bool preserve_gpt =
      plan.partition_style_choice() ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      plan.source_partition_style() ==
          migrationcore::MigrationPartitionStyle::gpt &&
      plan.partition_style() == migrationcore::MigrationPartitionStyle::gpt &&
      source_style == diskmodel::PartitionStyle::gpt;
  const bool preserve_mbr =
      plan.partition_style_choice() ==
          migrationcore::DirectClonePartitionStyleChoice::preserve &&
      plan.source_partition_style() ==
          migrationcore::MigrationPartitionStyle::mbr &&
      plan.partition_style() == migrationcore::MigrationPartitionStyle::mbr &&
      source_style == diskmodel::PartitionStyle::mbr &&
      plan.mbr_preserve_binding().has_value() &&
      plan.mbr_preserve_binding()->target_disk_signature != 0U &&
      plan.mbr_preserve_binding()->target_disk_signature !=
          plan.mbr_preserve_binding()->source_disk_signature;
  const bool target_only_mbr_to_gpt =
      plan.partition_style_choice() ==
          migrationcore::DirectClonePartitionStyleChoice::mbr_to_gpt &&
      plan.source_partition_style() ==
          migrationcore::MigrationPartitionStyle::mbr &&
      plan.partition_style() == migrationcore::MigrationPartitionStyle::gpt &&
      source_style == diskmodel::PartitionStyle::mbr &&
      plan.boot_finalization_required() &&
      plan.expected_source().is_system_disk && observed.source.is_system_disk;
  if (!request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token != L"OK" ||
      source_layout.value() != plan.expected_source_layout_hash() ||
      target_layout.value() != plan.expected_target_layout_hash() ||
      observed.source.is_system_disk !=
          plan.expected_source().is_system_disk ||
      !observed.source.offline.has_value() ||
      observed.source.offline.value() ||
      !observed.source.read_only.has_value() ||
      observed.source.read_only.value() ||
      !observed.source.removable.has_value() ||
      observed.source.removable.value() ||
      !observed.target.offline.has_value() ||
      !observed.target.read_only.has_value() ||
      observed.target.read_only.value() ||
      !observed.target.removable.has_value() ||
      observed.target.removable.value() ||
      observed.target.is_system_disk ||
      observed.source.logical_sector_size != 512U ||
      observed.target.logical_sector_size != 512U ||
      (!preserve_gpt && !preserve_mbr && !target_only_mbr_to_gpt) ||
      (target_style != diskmodel::PartitionStyle::raw &&
       target_style != diskmodel::PartitionStyle::gpt &&
       target_style != diskmodel::PartitionStyle::mbr) ||
      !supported_initial_target_partition_table(
          observed.target, target_style) ||
      source_class.dynamic_disk || source_class.storage_spaces ||
      source_class.software_raid ||
      source_class.unresolved_hardware_raid ||
      source_class.unsupported_virtual || target_class.dynamic_disk ||
      target_class.storage_spaces || target_class.software_raid ||
      target_class.unresolved_hardware_raid ||
      target_class.unsupported_virtual ||
      diskmodel::disk_health_operation_advice(observed.source.health, true) !=
          diskmodel::DiskHealthOperationAdvice::proceed ||
      diskmodel::disk_health_operation_advice(observed.target.health, false) ==
          diskmodel::DiskHealthOperationAdvice::block_target ||
      (plan.partition_style() != migrationcore::MigrationPartitionStyle::gpt &&
       plan.partition_style() != migrationcore::MigrationPartitionStyle::mbr) ||
      (plan.surplus_allocation() !=
           migrationcore::ShrinkSurplusAllocation::leave_unallocated &&
       plan.surplus_allocation() !=
           migrationcore::ShrinkSurplusAllocation::automatic_proportional &&
       plan.surplus_allocation() !=
           migrationcore::ShrinkSurplusAllocation::selected_data_partition) ||
      plan.target_is_active_rescue_media() || plan.tasks().empty() ||
      plan.archive_task_count() == 0U ||
      plan.maximum_archive_upper_bound_bytes() == 0U ||
      plan.maximum_archive_upper_bound_bytes() !=
          plan.staging().archive_capacity_bytes ||
      plan.staging().archive_capacity_bytes <
          kWindowsDirectShrinkStagingFileSystemReserveBytes ||
      all_zero(dependencies.connection_instance_hash) ||
      !dependencies.target_io || !dependencies.guid_generator ||
      !dependencies.make_wim_store || !dependencies.reidentify_confirmed ||
      (preserve_mbr && !dependencies.observe_mbr_safety)) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小production factory安全範囲",
        L"レビュー済み形式維持/変換intent、layout、固定512-byte基本disk、GPT/MBR target、NTFS、1GiB staging余白、confirmed identity、および全production依存が必要です");
  }
  if (preserve_mbr) {
    status = validate_mbr_preserve_source_bindings(plan, observed.source);
    if (!status) {
      return status;
    }
  } else if (target_only_mbr_to_gpt) {
    status = validate_mbr_to_gpt_source_bindings(plan, observed.source);
    if (!status) {
      return status;
    }
  }
  std::size_t efi_count{};
  std::size_t msr_count{};
  std::size_t windows_count{};
  std::size_t recovery_count{};
  std::size_t bios_system_count{};
  std::size_t active_count{};
  std::size_t active_boot_role_count{};
  std::uint64_t extension_count{};
  bool growth_owner_matched = false;
  bool selected_data_growth_owner_matched = false;
  for (const auto& task : plan.tasks()) {
    std::uint64_t construction_end{};
    std::uint64_t final_end{};
    if (task.target_number == 0U ||
        (preserve_mbr && task.target_number > 4U) ||
        task.target_offset_bytes % kMiB != 0U ||
        task.construction_size_bytes == 0U ||
        task.construction_size_bytes > task.target_size_bytes ||
        task.construction_size_bytes % 512U != 0U ||
        task.target_size_bytes % 512U != 0U ||
        (task.active && !preserve_mbr) ||
        !checked_add(
            task.target_offset_bytes,
            task.construction_size_bytes,
            construction_end) ||
        !checked_add(
            task.target_offset_bytes, task.target_size_bytes, final_end) ||
        final_end > plan.expected_target().size_bytes ||
        (task.kind ==
                 WindowsDirectShrinkPartitionTaskKind::copy_exact_raw &&
         (!task.source_table_index.has_value() ||
          task.source_size_bytes == 0U ||
          task.construction_size_bytes != task.source_size_bytes ||
          task.target_size_bytes != task.source_size_bytes ||
          all_zero(task.source_partition_type) ||
          !task.original_volume_guid_path.empty() ||
          task.archive_upper_bound_bytes != 0U)) ||
        (task.kind !=
                 WindowsDirectShrinkPartitionTaskKind::copy_exact_raw &&
         !all_zero(task.source_partition_type))) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"直接縮小production partition寸法",
          L"construction/final寸法、整列、範囲、またはActive属性が未対応です");
    }
    if (task.active) {
      ++active_count;
      if (task.role == migrationcore::MigrationPartitionRole::windows ||
          task.role == migrationcore::MigrationPartitionRole::bios_system) {
        ++active_boot_role_count;
      }
    }
    switch (task.role) {
      case migrationcore::MigrationPartitionRole::efi_system:
        ++efi_count;
        if (task.kind !=
                WindowsDirectShrinkPartitionTaskKind::recreate_efi_system ||
            task.construction_size_bytes != task.target_size_bytes) {
          return status_failure(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production ESP task",
              L"ESPは固定容量の生成FAT32でなければ開始しません");
        }
        break;
      case migrationcore::MigrationPartitionRole::microsoft_reserved:
        ++msr_count;
        if (task.kind != WindowsDirectShrinkPartitionTaskKind::
                recreate_microsoft_reserved ||
            task.construction_size_bytes != task.target_size_bytes) {
          return status_failure(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production MSR task",
              L"MSRは固定容量のmetadata-only生成領域でなければ開始しません");
        }
        break;
      case migrationcore::MigrationPartitionRole::windows:
        ++windows_count;
        if (task.kind !=
            WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
          return status_failure(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production Windows task",
              L"Windowsは非空NTFS WIM適用として証明できなければ開始しません");
        }
        break;
      case migrationcore::MigrationPartitionRole::recovery:
        ++recovery_count;
        if (task.kind !=
            WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim) {
          return status_failure(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production recovery task",
              L"回復領域は非空NTFS WIM適用として証明できなければ開始しません");
        }
        break;
      case migrationcore::MigrationPartitionRole::data:
        if (task.kind !=
                WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim &&
            task.kind !=
                WindowsDirectShrinkPartitionTaskKind::create_empty_ntfs &&
            task.kind !=
                WindowsDirectShrinkPartitionTaskKind::copy_exact_raw) {
          return status_failure(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production data task",
              L"data領域はNTFS WIM適用、空NTFS生成、または元サイズexact RAWに限定します");
        }
        break;
      case migrationcore::MigrationPartitionRole::bios_system:
        ++bios_system_count;
        if (!preserve_mbr ||
            (task.kind !=
                 WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim &&
             task.kind !=
                 WindowsDirectShrinkPartitionTaskKind::create_empty_ntfs)) {
          return status_failure(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"直接縮小production BIOS system task",
              L"MBR形式維持のNTFS WIM適用または空NTFS生成だけを扱います");
        }
        break;
    }
    if (task.construction_size_bytes < task.target_size_bytes) {
      ++extension_count;
      if (plan.staging().final_growth_owner_target_number ==
          task.target_number) {
        growth_owner_matched =
            plan.staging().offset_bytes == construction_end &&
            plan.staging().length_bytes ==
                task.target_size_bytes - task.construction_size_bytes;
      }
      if (plan.surplus_allocation() ==
              migrationcore::ShrinkSurplusAllocation::
                  selected_data_partition &&
          plan.surplus_target_source_table_index().has_value() &&
          task.source_table_index ==
              plan.surplus_target_source_table_index() &&
          task.role == migrationcore::MigrationPartitionRole::data &&
          (task.kind ==
               WindowsDirectShrinkPartitionTaskKind::apply_ntfs_wim ||
           task.kind ==
               WindowsDirectShrinkPartitionTaskKind::create_empty_ntfs) &&
          plan.staging().final_growth_owner_target_number ==
              task.target_number) {
        selected_data_growth_owner_matched = growth_owner_matched;
      }
    }
  }
  const bool common_system_roles_valid = plan.boot_finalization_required() &&
      windows_count == 1U && recovery_count <= 1U &&
      dependencies.finalize_boot &&
      (recovery_count == 0U || dependencies.finalize_winre);
  const bool gpt_system_roles_valid = common_system_roles_valid &&
      !preserve_mbr && efi_count == 1U && msr_count == 1U &&
      bios_system_count == 0U && active_count == 0U;
  const bool mbr_system_roles_valid = common_system_roles_valid &&
      preserve_mbr && efi_count == 0U && msr_count == 0U &&
      bios_system_count <= 1U && active_count == 1U &&
      active_boot_role_count == 1U;
  const bool system_roles_valid =
      gpt_system_roles_valid || mbr_system_roles_valid;
  const bool data_roles_valid = !plan.boot_finalization_required() &&
      efi_count == 0U && windows_count == 0U && recovery_count == 0U &&
      bios_system_count == 0U && active_count == 0U &&
      (preserve_mbr ? msr_count == 0U : msr_count <= 1U);
  const bool leave_valid = plan.surplus_allocation() ==
          migrationcore::ShrinkSurplusAllocation::leave_unallocated &&
      extension_count == 0U &&
      !plan.staging().final_growth_owner_target_number.has_value();
  const bool automatic_valid = plan.surplus_allocation() ==
          migrationcore::ShrinkSurplusAllocation::automatic_proportional &&
      extension_count != 0U && growth_owner_matched;
  bool selected_mapping_valid = false;
  if (plan.surplus_target_source_table_index().has_value()) {
    const auto mapping = std::find_if(
        plan.source_partition_mappings().begin(),
        plan.source_partition_mappings().end(),
        [&plan](const WindowsDirectShrinkSourcePartitionMapping& value) {
          return value.source_table_index ==
              *plan.surplus_target_source_table_index();
        });
    selected_mapping_valid =
        mapping != plan.source_partition_mappings().end() &&
        mapping->role == migrationcore::MigrationPartitionRole::data &&
        mapping->requested && mapping->selected &&
        mapping->disposition == WindowsDirectShrinkSourcePartitionDisposition::
            transferred_to_target &&
        mapping->target_number ==
            plan.staging().final_growth_owner_target_number;
  }
  const bool selected_data_valid = plan.surplus_allocation() ==
          migrationcore::ShrinkSurplusAllocation::selected_data_partition &&
      plan.surplus_target_source_table_index().has_value() &&
      extension_count == 1U && growth_owner_matched &&
      selected_data_growth_owner_matched && selected_mapping_valid;
  if ((!system_roles_valid && !data_roles_valid) ||
      (!leave_valid && !automatic_valid && !selected_data_valid) ||
      extension_count != plan.ntfs_extension_task_count()) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"直接縮小production role／余剰契約",
        L"GPT/MBR system/data役割、Boot/WinRE依存、staging所有、またはNTFS伸長件数が不変計画と一致しません");
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<std::unique_ptr<IWindowsDirectShrinkClonePlatform>>
make_windows_direct_shrink_clone_platform_with_dependencies(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const WindowsDirectShrinkClonePlatformRequest& request,
    WindowsDirectShrinkClonePlatformDependencies dependencies) {
  auto valid = validate_factory_inputs(plan, observed, request, dependencies);
  if (!valid) {
    return clonecore::Result<std::unique_ptr<
        IWindowsDirectShrinkClonePlatform>>::failure(valid.error());
  }
  auto final_gpt = build_final_gpt(plan, *dependencies.guid_generator);
  if (!final_gpt) {
    return clonecore::Result<std::unique_ptr<
        IWindowsDirectShrinkClonePlatform>>::failure(final_gpt.error());
  }
  auto staging_guid = dependencies.guid_generator->next_guid();
  if (!staging_guid || staging_guid.value().is_zero()) {
    return clonecore::Result<std::unique_ptr<
        IWindowsDirectShrinkClonePlatform>>::failure(
        staging_guid
            ? platform_error(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_INVALID_DATA,
                  L"直接縮小staging partition GUID",
                  L"空のstaging GUIDは使用できません")
            : staging_guid.error());
  }
  auto temporary_gpt = build_temporary_gpt(
      plan, final_gpt.value(), staging_guid.value());
  if (!temporary_gpt) {
    return clonecore::Result<std::unique_ptr<
        IWindowsDirectShrinkClonePlatform>>::failure(temporary_gpt.error());
  }
  auto hidden_final_gpt = build_hidden_final_gpt(plan, final_gpt.value());
  if (!hidden_final_gpt) {
    return clonecore::Result<std::unique_ptr<
        IWindowsDirectShrinkClonePlatform>>::failure(
        hidden_final_gpt.error());
  }
  auto final_valid = validate_gpt_write_plan(
      final_gpt.value(), plan.expected_target().size_bytes);
  auto temporary_valid = validate_gpt_write_plan(
      temporary_gpt.value(), plan.expected_target().size_bytes);
  auto hidden_valid = validate_gpt_write_plan(
      hidden_final_gpt.value(), plan.expected_target().size_bytes);
  auto phase_relationships =
      final_valid && temporary_valid && hidden_valid
      ? validate_gpt_phase_relationships(
            plan,
            final_gpt.value(),
            temporary_gpt.value(),
            hidden_final_gpt.value())
      : clonecore::Status::failure(
            !final_valid
                ? final_valid.error()
                : !temporary_valid ? temporary_valid.error()
                                   : hidden_valid.error());
  if (!phase_relationships) {
    return clonecore::Result<std::unique_ptr<
        IWindowsDirectShrinkClonePlatform>>::failure(
        phase_relationships.error());
  }
  std::optional<clonecore::MbrWritePlan> final_mbr;
  std::vector<std::byte> hidden_final_mbr_sector;
  if (plan.partition_style() == migrationcore::MigrationPartitionStyle::mbr) {
    auto built_mbr = build_final_mbr(plan);
    if (!built_mbr) {
      return clonecore::Result<std::unique_ptr<
          IWindowsDirectShrinkClonePlatform>>::failure(built_mbr.error());
    }
    auto hidden_mbr = build_hidden_final_mbr_sector(built_mbr.value());
    if (!hidden_mbr) {
      return clonecore::Result<std::unique_ptr<
          IWindowsDirectShrinkClonePlatform>>::failure(hidden_mbr.error());
    }
    final_mbr.emplace(built_mbr.take_value());
    hidden_final_mbr_sector = hidden_mbr.take_value();
  }
  std::unique_ptr<IWindowsDirectShrinkClonePlatform> result =
      std::make_unique<WindowsDirectShrinkClonePlatform>(
          plan,
          request,
          std::move(dependencies),
          final_gpt.take_value(),
          temporary_gpt.take_value(),
          hidden_final_gpt.take_value(),
          std::move(final_mbr),
          std::move(hidden_final_mbr_sector));
  return clonecore::Result<std::unique_ptr<
      IWindowsDirectShrinkClonePlatform>>::success(std::move(result));
}

clonecore::Result<std::unique_ptr<IWindowsDirectShrinkClonePlatform>>
make_windows_direct_shrink_clone_platform(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const WindowsDirectShrinkClonePlatformRequest& request) {
  WindowsTsumugiShrinkRestorePlatformRequest io_request{
      .expected_target = plan.expected_target(),
      .confirmation = request.confirmation,
      .expected_target_layout_hash = plan.expected_target_layout_hash(),
      .target_is_active_rescue_media = plan.target_is_active_rescue_media(),
      .callbacks = request.callbacks,
  };
  auto io = make_windows_tsumugi_shrink_restore_platform_io(io_request);
  auto guid = clonecore::make_windows_guid_generator();
  auto connection =
      imageformat::make_tsumugi_connection_instance_hash_with_windows_apis();
  if (!io || !guid || !connection) {
    return !io
        ? clonecore::Result<std::unique_ptr<
              IWindowsDirectShrinkClonePlatform>>::failure(io.error())
        : !connection
              ? clonecore::Result<std::unique_ptr<
                    IWindowsDirectShrinkClonePlatform>>::failure(
                    connection.error())
              : failure<std::unique_ptr<IWindowsDirectShrinkClonePlatform>>(
                    clonecore::ErrorCode::internal_error,
                    ERROR_INVALID_HANDLE,
                    L"直接縮小production GUID generator",
                    L"Windows GUID generatorを作成できません");
  }
  WindowsDirectShrinkClonePlatformDependencies dependencies{
      .target_io = io.take_value(),
      .guid_generator = std::move(guid),
      .make_wim_store =
          [](const std::wstring& root,
             const std::uint64_t capacity,
             const std::uint64_t maximum_archive,
             const clonecore::DiskOperationCallbacks& callbacks) {
            return WindowsDirectShrinkOwnedWimStore::create(
                root, capacity, maximum_archive, callbacks);
          },
      .connection_instance_hash = connection.take_value(),
      .reidentify_confirmed =
          [](const clonecore::StableDiskIdentity& source,
             const clonecore::StableDiskIdentity& target,
             const clonecore::TargetConfirmation& confirmation) {
            auto inventory = diskmodel::make_windows_disk_inventory_provider();
            return diskmodel::reidentify_physical_clone(
                source,
                target,
                confirmation,
                *inventory,
                false);
          },
      .finalize_boot = finalize_boot_with_windows_apis,
      .finalize_winre = finalize_winre_with_windows_apis,
      .observe_mbr_safety =
          observe_windows_direct_shrink_mbr_safety_with_windows_apis,
  };
  return make_windows_direct_shrink_clone_platform_with_dependencies(
      plan, observed, request, std::move(dependencies));
}

}  // namespace ytec::windowsapp

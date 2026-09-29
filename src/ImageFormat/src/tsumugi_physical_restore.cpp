#include "ytec/imageformat/tsumugi_physical_restore.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_restore_layout.h"
#include "ytec/imageformat/tsumugi_restore_layout_io.h"
#include "ytec/imageformat/tsumugi_restore_transaction.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

clonecore::Error restore_error(
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
  return clonecore::Result<T>::failure(restore_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool all_zero(const Sha256Digest& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool has_embedded_null(const std::wstring_view value) noexcept {
  return value.find(L'\0') != std::wstring_view::npos;
}

void append_u32(
    std::vector<std::byte>& output,
    const std::uint32_t value) {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(
    std::vector<std::byte>& output,
    const std::uint64_t value) {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_domain(
    std::vector<std::byte>& output,
    const std::string_view domain) {
  output.insert(
      output.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  output.push_back(std::byte{0});
}

clonecore::Status append_utf16(
    std::vector<std::byte>& output,
    const std::wstring_view value,
    const std::wstring_view operation,
    const std::size_t maximum_characters = 32767U) {
  static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
  if (value.size() > maximum_characters || has_embedded_null(value) ||
      value.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        std::wstring(operation),
        L"識別文字列が安全な上限を超えるかNULを含みます"));
  }
  append_u32(output, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t code_unit : value) {
    const auto value16 = static_cast<std::uint16_t>(code_unit);
    output.push_back(static_cast<std::byte>(value16 & 0xffU));
    output.push_back(static_cast<std::byte>((value16 >> 8U) & 0xffU));
  }
  return clonecore::success_status();
}

clonecore::Status append_ascii(
    std::vector<std::byte>& output,
    const std::string_view value,
    const std::wstring_view operation) {
  if (value.size() > 1024U ||
      value.find('\0') != std::string_view::npos) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        std::wstring(operation),
        L"シリアル識別子が安全な上限を超えるかNULを含みます"));
  }
  append_u32(output, static_cast<std::uint32_t>(value.size()));
  output.insert(
      output.end(),
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()));
  return clonecore::success_status();
}

clonecore::Result<Sha256Digest> stable_identity_hash(
    const clonecore::StableDiskIdentity& identity) {
  const auto valid = clonecore::validate_stable_identity(
      identity, identity, L"Tsumugi復元先");
  if (!valid) {
    return clonecore::Result<Sha256Digest>::failure(valid.error());
  }
  constexpr std::string_view domain =
      "YTEC-TSUMUGI-RESTORE-TARGET-IDENTITY-V1";
  std::vector<std::byte> material;
  material.reserve(160U);
  append_u32(material, static_cast<std::uint32_t>(domain.size()));
  material.insert(
      material.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  const auto model = append_utf16(
      material, identity.model, L"Tsumugi復元先識別Hash");
  const auto serial = append_ascii(
      material, identity.serial_suffix, L"Tsumugi復元先識別Hash");
  const auto instance = append_utf16(
      material, identity.device_instance_id, L"Tsumugi復元先識別Hash");
  if (!model || !serial || !instance) {
    return clonecore::Result<Sha256Digest>::failure(
        !model ? model.error() : !serial ? serial.error() : instance.error());
  }
  append_u64(material, identity.size_bytes);
  append_u32(material, identity.logical_sector_size);
  material.push_back(identity.is_system_disk ? std::byte{1} : std::byte{0});
  return sha256(material);
}

bool is_dynamic_partition(const diskmodel::PartitionInfo& partition) {
  return _wcsicmp(partition.type.c_str(), L"0x42") == 0 ||
      _wcsicmp(
          partition.type.c_str(),
          L"{5808C8AA-7E8F-42E0-85D2-E1E90434CFB3}") == 0 ||
      _wcsicmp(
          partition.type.c_str(),
          L"{AF9B60A0-1431-4F62-BC68-3311714A69AD}") == 0;
}

clonecore::Result<TsumugiRestoreDiskIdentity> make_restore_identity(
    const diskmodel::ReidentifiedPhysicalTarget& observed,
    const TsumugiPhysicalRestoreTargetClass& target_class,
    const bool active_rescue_media,
    const Sha256Digest& connection_token) {
  auto hash = stable_identity_hash(observed.target_identity);
  if (!hash) {
    return clonecore::Result<TsumugiRestoreDiskIdentity>::failure(
        hash.error());
  }
  if (target_class.usb_attached && all_zero(connection_token)) {
    return failure<TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi USB接続Session",
        L"USB接続ディスクには現在の接続だけに有効な識別値が必要です");
  }
  return clonecore::Result<TsumugiRestoreDiskIdentity>::success({
      .stable_identity_hash = hash.take_value(),
      .disk_size = observed.target_identity.size_bytes,
      .logical_sector_size = observed.target_identity.logical_sector_size,
      .is_running_windows_system_disk =
          observed.target_identity.is_system_disk,
      .is_usb_attached = target_class.usb_attached,
      .is_usb_memory = target_class.usb_memory,
      .is_active_rescue_media = active_rescue_media,
      .is_dynamic_disk = target_class.dynamic_disk,
      .is_storage_spaces = target_class.storage_spaces,
      .is_windows_software_raid = target_class.software_raid,
      .has_unresolved_hardware_raid =
          target_class.unresolved_hardware_raid,
      .connection_instance_hash = connection_token,
  });
}

bool same_restore_identity(
    const TsumugiRestoreDiskIdentity& left,
    const TsumugiRestoreDiskIdentity& right) noexcept {
  return left.stable_identity_hash == right.stable_identity_hash &&
      left.disk_size == right.disk_size &&
      left.logical_sector_size == right.logical_sector_size &&
      left.is_running_windows_system_disk ==
          right.is_running_windows_system_disk &&
      left.is_usb_attached == right.is_usb_attached &&
      left.is_usb_memory == right.is_usb_memory &&
      left.is_active_rescue_media == right.is_active_rescue_media &&
      left.is_dynamic_disk == right.is_dynamic_disk &&
      left.is_storage_spaces == right.is_storage_spaces &&
      left.is_windows_software_raid == right.is_windows_software_raid &&
      left.has_unresolved_hardware_raid ==
          right.has_unresolved_hardware_raid &&
      left.connection_instance_hash == right.connection_instance_hash;
}

bool contains_windows(const TsumugiManifest& manifest) noexcept {
  return (static_cast<std::uint32_t>(manifest.flags) &
          static_cast<std::uint32_t>(
              TsumugiManifestFlags::source_contains_windows)) != 0U;
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

bool partition_selected(
    const TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              TsumugiManifestPartitionFlags::selected)) != 0U;
}

const TsumugiManifestPartition* selected_source_partition(
    const TsumugiManifest& manifest,
    const std::uint32_t source_table_index) noexcept {
  const auto found = std::find_if(
      manifest.partitions.begin(),
      manifest.partitions.end(),
      [&](const TsumugiManifestPartition& partition) {
        return partition.source_table_index == source_table_index &&
            partition_selected(partition);
      });
  return found == manifest.partitions.end() ? nullptr : &*found;
}

bool selected_partition_contains_windows(
    const TsumugiManifest& manifest,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection)
    noexcept {
  const auto* partition = selected_source_partition(
      manifest, selection.source_table_index);
  return partition != nullptr &&
      (static_cast<std::uint32_t>(partition->flags) &
       static_cast<std::uint32_t>(
           TsumugiManifestPartitionFlags::contains_windows)) != 0U;
}

std::vector<diskmodel::PartitionInfo> ordered_partitions(
    const diskmodel::DiskInfo& target) {
  auto partitions = target.partitions;
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const diskmodel::PartitionInfo& left,
         const diskmodel::PartitionInfo& right) {
        if (left.number != right.number) {
          return left.number < right.number;
        }
        if (left.offset_bytes != right.offset_bytes) {
          return left.offset_bytes < right.offset_bytes;
        }
        return left.size_bytes < right.size_bytes;
      });
  return partitions;
}

bool same_service_individual_target(
    const TsumugiIndividualPartitionRestoreTarget& target,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection)
    noexcept {
  if (target.source_table_index != selection.source_table_index) {
    return false;
  }
  if (const auto* physical = std::get_if<
          TsumugiPhysicalExistingPartitionRestoreSelection>(
          &selection.target);
      physical != nullptr) {
    const auto* service =
        std::get_if<TsumugiExistingPartitionRestoreTarget>(&target.target);
    return service != nullptr &&
        physical->target_table_index == service->target_table_index &&
        physical->target_partition_number ==
            service->target_partition_number &&
        physical->target_offset == service->target_offset &&
        physical->target_size == service->target_size;
  }
  const auto* physical = std::get_if<
      TsumugiPhysicalUnallocatedRestoreSelection>(&selection.target);
  const auto* service =
      std::get_if<TsumugiUnallocatedRestoreTarget>(&target.target);
  return physical != nullptr && service != nullptr &&
      physical->target_offset == service->target_offset &&
      physical->target_size == service->target_size;
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

TsumugiRestoreLayoutWriteKind convert_gpt_kind(
    const clonecore::GptMetadataKind kind) {
  switch (kind) {
    case clonecore::GptMetadataKind::primary_entries:
      return TsumugiRestoreLayoutWriteKind::gpt_primary_entries;
    case clonecore::GptMetadataKind::backup_entries:
      return TsumugiRestoreLayoutWriteKind::gpt_backup_entries;
    case clonecore::GptMetadataKind::backup_header:
      return TsumugiRestoreLayoutWriteKind::gpt_backup_header;
    case clonecore::GptMetadataKind::primary_header_commit:
      return TsumugiRestoreLayoutWriteKind::gpt_primary_header;
    case clonecore::GptMetadataKind::protective_mbr:
      return TsumugiRestoreLayoutWriteKind::gpt_protective_mbr;
  }
  return TsumugiRestoreLayoutWriteKind::gpt_protective_mbr;
}

clonecore::Result<TsumugiPreservingPartitionLayoutPlanV1>
make_preserving_partition_plan(
    const TsumugiManifest& manifest,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection,
    clonecore::ITargetDiskWriter& target,
    clonecore::IGuidGenerator& guid_generator) {
  const auto* source = selected_source_partition(
      manifest, selection.source_table_index);
  const auto* unallocated = std::get_if<
      TsumugiPhysicalUnallocatedRestoreSelection>(&selection.target);
  if (source == nullptr || unallocated == nullptr ||
      target.logical_sector_size() == 0U ||
      unallocated->target_offset % target.logical_sector_size() != 0U ||
      unallocated->target_size % target.logical_sector_size() != 0U) {
    return failure<TsumugiPreservingPartitionLayoutPlanV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi未割当保持型計画",
        L"復元元、未割当配置、または論理セクターが不正です");
  }

  TargetReaderView reader(target);
  TsumugiPreservingPartitionLayoutPlanV1 result{
      .target_size_bytes = target.size_bytes(),
      .logical_sector_size = target.logical_sector_size(),
      .new_partition_offset = unallocated->target_offset,
      .new_partition_size = unallocated->target_size,
  };
  if (manifest.partition_style == TsumugiManifestPartitionStyle::gpt) {
    auto original = clonecore::parse_gpt(reader);
    if (!original) {
      return clonecore::Result<
          TsumugiPreservingPartitionLayoutPlanV1>::failure(
          original.error());
    }
    clonecore::GptGuid type;
    type.bytes = source->type_id;
    auto generated = clonecore::make_gpt_add_partition_plan(
        original.value(),
        clonecore::GptAddPartitionRequest{
            .first_lba =
                unallocated->target_offset / target.logical_sector_size(),
            .sector_count =
                unallocated->target_size / target.logical_sector_size(),
            .type_guid = type,
            .attributes = 0U,
            .name = u"Y-TEC restored",
        },
        guid_generator);
    if (!generated) {
      return clonecore::Result<
          TsumugiPreservingPartitionLayoutPlanV1>::failure(
          generated.error());
    }
    const auto append = [&](const clonecore::GptMetadataKind kind,
                            std::vector<TsumugiRestoreLayoutWrite>& output,
                            const bool capture_current)
        -> clonecore::Status {
      const auto found = std::find_if(
          generated.value().writes.begin(),
          generated.value().writes.end(),
          [&](const clonecore::GptMetadataWrite& write) {
            return write.kind == kind;
          });
      if (found == generated.value().writes.end() ||
          kind == clonecore::GptMetadataKind::protective_mbr) {
        return clonecore::Status::failure(restore_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Tsumugi保持型GPT metadata",
            L"必要なGPT metadata書込みを一意に確定できません"));
      }
      std::vector<std::byte> bytes = found->bytes;
      if (capture_current) {
        auto captured = target.read_back(found->offset, found->bytes.size());
        if (!captured || captured.value().size() != found->bytes.size()) {
          return clonecore::Status::failure(
              captured
                  ? restore_error(
                        clonecore::ErrorCode::verification_failed,
                        ERROR_CRC,
                        L"Tsumugi保持型GPT rollback capture",
                        L"既存GPT metadataを同じ範囲から取得できません")
                  : captured.error());
        }
        bytes = captured.take_value();
      }
      output.push_back(TsumugiRestoreLayoutWrite{
          .kind = convert_gpt_kind(kind),
          .offset = found->offset,
          .bytes = std::move(bytes),
      });
      return clonecore::success_status();
    };
    for (const auto kind : {
             clonecore::GptMetadataKind::backup_entries,
             clonecore::GptMetadataKind::backup_header,
             clonecore::GptMetadataKind::primary_entries,
             clonecore::GptMetadataKind::primary_header_commit,
         }) {
      const auto status = append(kind, result.published_writes, false);
      if (!status) {
        return clonecore::Result<
            TsumugiPreservingPartitionLayoutPlanV1>::failure(
            status.error());
      }
    }
    for (const auto kind : {
             clonecore::GptMetadataKind::primary_entries,
             clonecore::GptMetadataKind::primary_header_commit,
             clonecore::GptMetadataKind::backup_entries,
             clonecore::GptMetadataKind::backup_header,
         }) {
      const auto status = append(kind, result.rollback_writes, true);
      if (!status) {
        return clonecore::Result<
            TsumugiPreservingPartitionLayoutPlanV1>::failure(
            status.error());
      }
    }
    const auto added = std::find_if(
        generated.value().target_disk.partitions.begin(),
        generated.value().target_disk.partitions.end(),
        [&](const clonecore::GptPartition& partition) {
          return partition.first_lba ==
                  unallocated->target_offset /
                      target.logical_sector_size() &&
              partition.last_lba - partition.first_lba + 1U ==
                  unallocated->target_size /
                      target.logical_sector_size();
        });
    if (added == generated.value().target_disk.partitions.end()) {
      return failure<TsumugiPreservingPartitionLayoutPlanV1>(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"Tsumugi保持型GPT追加entry",
          L"生成済みGPTから新規entryを確定できません");
    }
    result.style = PartitionTableStyle::gpt;
    result.new_partition_number = added->entry_index + 1U;
    result.original_layout = original.take_value();
    result.target_layout = generated.take_value().target_disk;
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutPlanV1>::success(
        std::move(result));
  }

  if (manifest.partition_style != TsumugiManifestPartitionStyle::mbr ||
      target.logical_sector_size() != 512U ||
      unallocated->target_offset / 512U >
          (std::numeric_limits<std::uint32_t>::max)() ||
      unallocated->target_size / 512U >
          (std::numeric_limits<std::uint32_t>::max)()) {
    return failure<TsumugiPreservingPartitionLayoutPlanV1>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi保持型MBR計画",
        L"MBR未割当復元は512-byte sectorと32-bit LBA範囲が必要です");
  }
  auto original = clonecore::parse_mbr(reader);
  if (!original) {
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutPlanV1>::failure(original.error());
  }
  auto generated = clonecore::make_mbr_add_partition_plan(
      original.value(),
      clonecore::MbrAddPartitionRequest{
          .first_lba = static_cast<std::uint32_t>(
              unallocated->target_offset / 512U),
          .sector_count = static_cast<std::uint32_t>(
              unallocated->target_size / 512U),
          .type = static_cast<std::uint8_t>(source->type_id[0]),
      });
  if (!generated) {
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutPlanV1>::failure(
        generated.error());
  }
  auto captured = target.read_back(0U, generated.value().sector.size());
  if (!captured || captured.value().size() != generated.value().sector.size()) {
    return clonecore::Result<
        TsumugiPreservingPartitionLayoutPlanV1>::failure(
        captured
            ? restore_error(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Tsumugi保持型MBR rollback capture",
                  L"既存MBR sectorを取得できません")
            : captured.error());
  }
  const auto added = std::find_if(
      generated.value().target_disk.partitions.begin(),
      generated.value().target_disk.partitions.end(),
      [&](const clonecore::MbrPartition& partition) {
        return partition.first_lba == unallocated->target_offset / 512U &&
            partition.sector_count == unallocated->target_size / 512U;
      });
  if (added == generated.value().target_disk.partitions.end()) {
    return failure<TsumugiPreservingPartitionLayoutPlanV1>(
        clonecore::ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"Tsumugi保持型MBR追加entry",
        L"生成済みMBRから新規entryを確定できません");
  }
  result.style = PartitionTableStyle::mbr;
  result.new_partition_number =
      static_cast<std::uint32_t>(added->table_index) + 1U;
  result.published_writes.push_back({
      .kind = TsumugiRestoreLayoutWriteKind::mbr_sector,
      .offset = 0U,
      .bytes = generated.value().sector,
  });
  result.rollback_writes.push_back({
      .kind = TsumugiRestoreLayoutWriteKind::mbr_sector,
      .offset = 0U,
      .bytes = captured.take_value(),
  });
  result.original_layout = original.take_value();
  result.target_layout = generated.take_value().target_disk;
  return clonecore::Result<
      TsumugiPreservingPartitionLayoutPlanV1>::success(std::move(result));
}

clonecore::Error append_offline_failure(
    clonecore::Error primary,
    const clonecore::Status& offline) {
  if (!offline) {
    primary.message +=
        L"。復元先offline状態の維持確認にも失敗しました: " +
        offline.error().operation;
  }
  return primary;
}

class LockedPhysicalRestoreSession final
    : public ITsumugiRestoreTargetSession {
 public:
  LockedPhysicalRestoreSession(
      std::unique_ptr<clonecore::ITargetDiskWriter> target,
      TsumugiRestoreDiskIdentity identity,
      std::vector<std::uint32_t> disallowed_mbr_signatures,
      std::optional<TsumugiPhysicalIndividualPartitionRestoreSelection>
          individual_partition,
      const TsumugiRestoreHost host,
      TsumugiPhysicalRestoreLockedTargetRevalidator
          revalidate_locked_target,
      clonecore::DiskOperationCallbacks callbacks)
      : target_(std::move(target)),
        identity_(std::move(identity)),
        disallowed_mbr_signatures_(std::move(disallowed_mbr_signatures)),
        individual_partition_(std::move(individual_partition)),
        host_(host),
        revalidate_locked_target_(std::move(revalidate_locked_target)),
        callbacks_(std::move(callbacks)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return target_ ? target_->size_bytes() : 0U;
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return target_ ? target_->logical_sector_size() : 0U;
  }

  [[nodiscard]] clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (!target_) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_HANDLE,
          L"Tsumugiロック済み復元先書込み",
          L"対象ハンドルがありません"));
    }
    return target_->write_target(offset, bytes);
  }

  [[nodiscard]] clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!target_) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_HANDLE,
          L"Tsumugiロック済み復元先読戻し",
          L"対象ハンドルがありません");
    }
    return target_->read_back(offset, length);
  }

  [[nodiscard]] clonecore::Status flush_target() override {
    if (!target_) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_HANDLE,
          L"Tsumugiロック済み復元先flush",
          L"対象ハンドルがありません"));
    }
    return target_->flush_target();
  }

  [[nodiscard]] clonecore::Result<TsumugiRestoreDiskIdentity>
  reidentify_locked_target() const override {
    if (!target_ || !revalidate_locked_target_ ||
        identity_.disk_size != target_->size_bytes() ||
        identity_.logical_sector_size != target_->logical_sector_size() ||
        all_zero(identity_.stable_identity_hash)) {
      return failure<TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"Tsumugiロック済み復元先再識別",
          L"安定識別と同じ対象ハンドルの容量またはセクターが一致しません");
    }
    auto current = revalidate_locked_target_();
    if (!current) {
      return current;
    }
    if (!same_restore_identity(identity_, current.value())) {
      return failure<TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"Tsumugi書込み直前復元先Session",
          L"完全イメージ検証の直後に対象識別または接続Sessionが変化しました");
    }
    return current;
  }

  [[nodiscard]] clonecore::Status prepare_layout(
      const TsumugiVerifiedImage& image,
      const TsumugiRestoreTarget& target,
      const TsumugiRestoreHost host) override {
    if (host != host_ || layout_transaction_ ||
        preserving_partition_transaction_ ||
        individual_existing_prepared_ ||
        (image.manifest.mode != TsumugiManifestMode::exact &&
         image.manifest.mode != TsumugiManifestMode::rescue)) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi物理復元レイアウト",
          L"復元Sessionの実行環境、単回状態、または画像modeが一致しません"));
    }

    if (individual_partition_.has_value()) {
      const auto* individual =
          std::get_if<TsumugiIndividualPartitionRestoreTarget>(&target);
      if (individual == nullptr ||
          !same_service_individual_target(
              *individual, individual_partition_.value())) {
        return clonecore::Status::failure(restore_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"Tsumugi個別パーティション復元レイアウト",
            L"レビュー済みの復元元または復元先と実行計画が一致しません"));
      }
      if (std::holds_alternative<
              TsumugiPhysicalExistingPartitionRestoreSelection>(
              individual_partition_->target)) {
        individual_existing_prepared_ = true;
        return clonecore::success_status();
      }
      auto guid_generator = clonecore::make_windows_guid_generator();
      if (!guid_generator) {
        return clonecore::Status::failure(restore_error(
            clonecore::ErrorCode::internal_error,
            ERROR_NOT_ENOUGH_MEMORY,
            L"Tsumugi保持型partition GUID生成器",
            L"新規partition識別子の生成器を初期化できません"));
      }
      auto preserving_plan = make_preserving_partition_plan(
          image.manifest,
          individual_partition_.value(),
          *target_,
          *guid_generator);
      if (!preserving_plan) {
        return clonecore::Status::failure(preserving_plan.error());
      }
      preserving_partition_transaction_ = std::make_unique<
          TsumugiPreservingPartitionLayoutTransactionV1>(
          preserving_plan.take_value(), *target_);
      const auto prepared =
          preserving_partition_transaction_->prepare(callbacks_);
      if (!prepared) {
        return clonecore::Status::failure(prepared.error());
      }
      return clonecore::success_status();
    }

    const auto* whole = std::get_if<TsumugiWholeDiskRestoreTarget>(&target);
    if (whole == nullptr || !whole->shrink_placements.empty()) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Tsumugi物理復元レイアウト",
          L"この経路は通常／救出のディスク全体または既存パーティション復元だけに対応します"));
    }
    auto guid_generator = clonecore::make_windows_guid_generator();
    auto signature_generator =
        clonecore::make_windows_mbr_signature_generator();
    if (!guid_generator || !signature_generator) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::internal_error,
          ERROR_NOT_ENOUGH_MEMORY,
          L"Tsumugi物理復元識別子生成器",
          L"ディスク識別子生成器を初期化できません"));
    }
    auto plan = make_tsumugi_whole_disk_restore_layout_plan_v1(
        image.manifest,
        size_bytes(),
        logical_sector_size(),
        *guid_generator,
        *signature_generator,
        disallowed_mbr_signatures_);
    if (!plan) {
      return clonecore::Status::failure(plan.error());
    }
    layout_transaction_ =
        std::make_unique<TsumugiWholeDiskRestoreLayoutTransaction>(
            plan.take_value(), *target_);
    const auto prepared = layout_transaction_->prepare(callbacks_);
    if (!prepared) {
      return clonecore::Status::failure(prepared.error());
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status commit_layout() override {
    if (individual_existing_prepared_) {
      auto current = reidentify_locked_target();
      if (!current) {
        return clonecore::Status::failure(current.error());
      }
      individual_existing_prepared_ = false;
      return clonecore::success_status();
    }
    if (preserving_partition_transaction_) {
      const auto committed =
          preserving_partition_transaction_->commit(callbacks_);
      if (!committed) {
        return clonecore::Status::failure(committed.error());
      }
      if (!committed.value().every_write_read_back_verified ||
          !committed.value().partition_table_committed ||
          committed.value().target_left_incomplete) {
        return clonecore::Status::failure(restore_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"Tsumugi保持型partition確定検証",
            L"新規partition tableの読戻しまたは確定証跡が不足しています"));
      }
      return clonecore::success_status();
    }
    if (!layout_transaction_) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_STATE,
          L"Tsumugi物理復元確定",
          L"無効化済みレイアウトがありません"));
    }
    const auto committed = layout_transaction_->commit(callbacks_);
    if (!committed) {
      return clonecore::Status::failure(committed.error());
    }
    if (!committed.value().every_write_read_back_verified ||
        !committed.value().primary_partition_table_committed ||
        committed.value().target_left_incomplete) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"Tsumugi物理復元確定検証",
          L"レイアウトの読戻しまたは最終確定証跡が不足しています"));
    }
    return clonecore::success_status();
  }

  void abort_layout() noexcept override {
    if (layout_transaction_) {
      layout_transaction_->abort();
    }
    if (preserving_partition_transaction_) {
      preserving_partition_transaction_->abort();
    }
    individual_existing_prepared_ = false;
  }

 private:
  std::unique_ptr<clonecore::ITargetDiskWriter> target_;
  TsumugiRestoreDiskIdentity identity_;
  std::vector<std::uint32_t> disallowed_mbr_signatures_;
  std::optional<TsumugiPhysicalIndividualPartitionRestoreSelection>
      individual_partition_;
  TsumugiRestoreHost host_{TsumugiRestoreHost::windows};
  TsumugiPhysicalRestoreLockedTargetRevalidator revalidate_locked_target_;
  clonecore::DiskOperationCallbacks callbacks_;
  std::unique_ptr<TsumugiWholeDiskRestoreLayoutTransaction>
      layout_transaction_;
  std::unique_ptr<TsumugiPreservingPartitionLayoutTransactionV1>
      preserving_partition_transaction_;
  bool individual_existing_prepared_{};
};

}  // namespace

clonecore::Result<Sha256Digest> hash_tsumugi_source_model_v1(
    const std::wstring_view model) {
  if (model.empty() || model == L"未取得" || has_embedded_null(model)) {
    return failure<Sha256Digest>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"TsumugiモデルHash",
        L"コピー元モデルがありません");
  }
  std::vector<std::byte> material;
  material.reserve(32U + model.size() * sizeof(wchar_t));
  append_domain(material, "YTEC-TSUMUGI-SOURCE-MODEL-V1");
  const auto appended = append_utf16(
      material, model, L"TsumugiモデルHash");
  if (!appended) {
    return clonecore::Result<Sha256Digest>::failure(appended.error());
  }
  return sha256(material);
}

clonecore::Result<Sha256Digest>
hash_tsumugi_physical_restore_target_identity_v1(
    const clonecore::StableDiskIdentity& identity) {
  return stable_identity_hash(identity);
}

clonecore::Result<Sha256Digest> hash_tsumugi_source_serial_v1(
    const std::string_view serial_suffix,
    const std::wstring_view device_instance_id) {
  if (serial_suffix.empty() && device_instance_id.empty()) {
    return failure<Sha256Digest>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"TsumugiシリアルHash",
        L"シリアル末尾またはデバイス識別子がありません");
  }
  std::vector<std::byte> material;
  append_domain(material, "YTEC-TSUMUGI-SOURCE-SERIAL-V1");
  clonecore::Status appended = clonecore::success_status();
  if (!serial_suffix.empty()) {
    material.push_back(std::byte{1});
    appended = append_ascii(
        material, serial_suffix, L"TsumugiシリアルHash");
  } else {
    material.push_back(std::byte{2});
    appended = append_utf16(
        material, device_instance_id, L"Tsumugiデバイス識別Hash");
  }
  if (!appended) {
    return clonecore::Result<Sha256Digest>::failure(appended.error());
  }
  return sha256(material);
}

clonecore::Result<Sha256Digest>
hash_tsumugi_physical_restore_target_layout_v1(
    const diskmodel::DiskInfo& target) {
  if (target.size_bytes == 0U || target.logical_sector_size == 0U ||
      target.partitions.size() > 128U) {
    return failure<Sha256Digest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi復元先レイアウトHash",
        L"容量、セクター、またはパーティション数が不正です");
  }
  constexpr std::string_view domain =
      "YTEC-TSUMUGI-RESTORE-TARGET-LAYOUT-V1";
  std::vector<std::byte> material;
  material.reserve(256U + target.partitions.size() * 160U);
  append_u32(material, static_cast<std::uint32_t>(domain.size()));
  material.insert(
      material.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  append_u64(material, target.size_bytes);
  append_u64(material, target.sector_count);
  append_u32(material, target.logical_sector_size);
  append_u32(material, target.physical_sector_size);
  append_u32(
      material,
      static_cast<std::uint32_t>(
          diskmodel::normalize_disk_partition_style(
              target.partition_style, target.partitions.size())));

  auto partitions = target.partitions;
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const diskmodel::PartitionInfo& left,
         const diskmodel::PartitionInfo& right) {
        if (left.number != right.number) {
          return left.number < right.number;
        }
        if (left.offset_bytes != right.offset_bytes) {
          return left.offset_bytes < right.offset_bytes;
        }
        return left.size_bytes < right.size_bytes;
      });
  append_u32(material, static_cast<std::uint32_t>(partitions.size()));
  for (const auto& partition : partitions) {
    append_u32(material, partition.number);
    append_u64(material, partition.offset_bytes);
    append_u64(material, partition.size_bytes);
    append_u32(material, static_cast<std::uint32_t>(partition.style));
    material.push_back(partition.bootable ? std::byte{1} : std::byte{0});
    const auto type = append_utf16(
        material,
        partition.type,
        L"Tsumugi復元先レイアウトHash",
        1024U);
    const auto identifier = append_utf16(
        material,
        partition.identifier,
        L"Tsumugi復元先レイアウトHash",
        1024U);
    const auto name = append_utf16(
        material,
        partition.name,
        L"Tsumugi復元先レイアウトHash",
        1024U);
    if (!type || !identifier || !name) {
      return clonecore::Result<Sha256Digest>::failure(
          !type ? type.error()
                : !identifier ? identifier.error() : name.error());
    }
  }
  return sha256(material);
}

TsumugiPhysicalRestoreTargetClass
classify_tsumugi_physical_restore_target(
    const diskmodel::DiskInfo& target) {
  const bool dynamic = std::any_of(
      target.partitions.begin(), target.partitions.end(),
      is_dynamic_partition);
  const bool storage_spaces =
      _wcsicmp(target.bus_type.c_str(), L"Storage Spaces") == 0;
  const bool raid = _wcsicmp(target.bus_type.c_str(), L"RAID") == 0 ||
      target.bus_type.starts_with(L"Unknown(") ||
      _wcsicmp(target.bus_type.c_str(), L"iSCSI") == 0;
  const bool virtual_disk =
      _wcsicmp(target.bus_type.c_str(), L"Virtual") == 0 ||
      _wcsicmp(
          target.bus_type.c_str(), L"File-backed virtual") == 0;
  return TsumugiPhysicalRestoreTargetClass{
      .usb_attached = _wcsicmp(target.bus_type.c_str(), L"USB") == 0,
      .usb_memory =
          _wcsicmp(target.bus_type.c_str(), L"USB") == 0 &&
          target.removable.value_or(true),
      .dynamic_disk = dynamic,
      .storage_spaces = storage_spaces,
      .software_raid = dynamic,
      .unresolved_hardware_raid = raid,
      .unsupported_virtual = virtual_disk,
  };
}

clonecore::Status validate_tsumugi_physical_restore_target(
    const diskmodel::DiskInfo& target,
    const TsumugiPhysicalRestoreTargetClass& target_class,
    const bool target_is_active_rescue_media) {
  if (!target.offline.has_value() || !target.read_only.has_value() ||
      !target.removable.has_value()) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"Tsumugi復元先属性",
        L"offline、read-only、removable属性をすべて確認できません"));
  }
  if (target.is_system_disk || target.read_only.value() ||
      target_class.usb_memory || target_is_active_rescue_media ||
      target_class.dynamic_disk || target_class.storage_spaces ||
      target_class.software_raid ||
      target_class.unresolved_hardware_raid ||
      target_class.unsupported_virtual ||
      diskmodel::disk_health_operation_advice(target.health, false) ==
          diskmodel::DiskHealthOperationAdvice::block_target ||
      target.partition_style == diskmodel::PartitionStyle::unknown) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi復元先安全分類",
        L"実行中システム、USBメモリ、起動中レスキュー媒体、Dynamic Disk、Storage Spaces、RAID、仮想ディスク、SMART/NVMe注意・異常、または不明形式は復元先にできません"));
  }
  return clonecore::success_status();
}

clonecore::Status
validate_tsumugi_physical_individual_partition_selection_v1(
    const TsumugiManifest& manifest,
    const diskmodel::DiskInfo& target,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection) {
  const auto* source = selected_source_partition(
      manifest, selection.source_table_index);
  if (source == nullptr || target.logical_sector_size == 0U ||
      target.logical_sector_size != manifest.logical_sector_size) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi個別復元元／セクター照合",
        L"選択済み復元元または復元先の論理セクターを確定できません"));
  }

  const auto validate_geometry = [&](const std::uint64_t offset,
                                     const std::uint64_t size) {
    std::uint64_t end{};
    return size >= source->minimum_target_bytes &&
        offset % target.logical_sector_size == 0U &&
        size % target.logical_sector_size == 0U &&
        checked_add(offset, size, end) && end <= target.size_bytes;
  };

  if (const auto* existing = std::get_if<
          TsumugiPhysicalExistingPartitionRestoreSelection>(
          &selection.target);
      existing != nullptr) {
    const auto partitions = ordered_partitions(target);
    if (existing->target_table_index == 0U ||
        existing->target_table_index > partitions.size() ||
        existing->target_partition_number == 0U ||
        !validate_geometry(
            existing->target_offset, existing->target_size)) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"Tsumugi既存パーティション復元先",
          L"既存復元先の番号、必要容量、整列、または範囲が不正です"));
    }
    const auto& observed = partitions[existing->target_table_index - 1U];
    if (observed.number != existing->target_partition_number ||
        observed.offset_bytes != existing->target_offset ||
        observed.size_bytes != existing->target_size) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"Tsumugi既存パーティション再照合",
          L"レビュー後に既存復元先の番号または配置が変化しました"));
    }
    return clonecore::success_status();
  }

  const auto& unallocated =
      std::get<TsumugiPhysicalUnallocatedRestoreSelection>(
          selection.target);
  std::uint64_t unallocated_end{};
  const bool same_style =
      (manifest.partition_style == TsumugiManifestPartitionStyle::gpt &&
       target.partition_style == diskmodel::PartitionStyle::gpt) ||
      (manifest.partition_style == TsumugiManifestPartitionStyle::mbr &&
       target.partition_style == diskmodel::PartitionStyle::mbr);
  const auto is_extended = [](const diskmodel::PartitionInfo& partition) {
    return _wcsicmp(partition.type.c_str(), L"0x05") == 0 ||
        _wcsicmp(partition.type.c_str(), L"0x0F") == 0 ||
        _wcsicmp(partition.type.c_str(), L"0x85") == 0;
  };
  const bool entry_available =
      target.partition_style == diskmodel::PartitionStyle::gpt
      ? target.partitions.size() < 128U
      : target.logical_sector_size == 512U &&
          target.partitions.size() < 4U &&
          std::none_of(
              target.partitions.begin(),
              target.partitions.end(),
              is_extended);
  if (!same_style || !entry_available ||
      unallocated.target_size != source->source_size ||
      !validate_geometry(
          unallocated.target_offset, unallocated.target_size) ||
      !checked_add(
          unallocated.target_offset,
          unallocated.target_size,
          unallocated_end)) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"Tsumugi未割当復元先",
        L"未割当復元先には同じGPT/MBR形式、空きentry、元区画と同じ容量、整列、範囲が必要です"));
  }
  for (const auto& partition : target.partitions) {
    std::uint64_t partition_end{};
    if (!checked_add(
            partition.offset_bytes,
            partition.size_bytes,
            partition_end) ||
        (unallocated.target_offset < partition_end &&
         partition.offset_bytes < unallocated_end)) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi未割当復元先再照合",
          L"指定範囲が既存パーティションと重なるか配置を検証できません"));
    }
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<
    TsumugiPhysicalIndividualPartitionRestoreSelection>>
find_tsumugi_physical_unallocated_restore_candidates_v1(
    const TsumugiManifest& manifest,
    const diskmodel::DiskInfo& target,
    const std::uint32_t source_table_index) {
  const auto* source = selected_source_partition(
      manifest, source_table_index);
  constexpr std::uint64_t alignment = 1024ULL * 1024ULL;
  if (source == nullptr || target.logical_sector_size == 0U ||
      alignment % target.logical_sector_size != 0U ||
      source->source_size == 0U ||
      source->source_size % target.logical_sector_size != 0U) {
    return failure<std::vector<
        TsumugiPhysicalIndividualPartitionRestoreSelection>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi未割当候補",
        L"復元元または対象セクターから安全な候補を計算できません");
  }
  const bool same_style =
      (manifest.partition_style == TsumugiManifestPartitionStyle::gpt &&
       target.partition_style == diskmodel::PartitionStyle::gpt) ||
      (manifest.partition_style == TsumugiManifestPartitionStyle::mbr &&
       target.partition_style == diskmodel::PartitionStyle::mbr);
  if (!same_style) {
    return clonecore::Result<std::vector<
        TsumugiPhysicalIndividualPartitionRestoreSelection>>::success({});
  }
  const auto is_extended = [](const diskmodel::PartitionInfo& partition) {
    return _wcsicmp(partition.type.c_str(), L"0x05") == 0 ||
        _wcsicmp(partition.type.c_str(), L"0x0F") == 0 ||
        _wcsicmp(partition.type.c_str(), L"0x85") == 0;
  };
  if ((target.partition_style == diskmodel::PartitionStyle::gpt &&
       target.partitions.size() >= 128U) ||
      (target.partition_style == diskmodel::PartitionStyle::mbr &&
       (target.logical_sector_size != 512U ||
        target.partitions.size() >= 4U ||
        std::any_of(
            target.partitions.begin(),
            target.partitions.end(),
            is_extended)))) {
    return clonecore::Result<std::vector<
        TsumugiPhysicalIndividualPartitionRestoreSelection>>::success({});
  }

  const auto align_up = [](const std::uint64_t value) {
    const auto remainder = value % alignment;
    if (remainder == 0U) {
      return std::optional<std::uint64_t>(value);
    }
    std::uint64_t aligned{};
    if (!checked_add(value, alignment - remainder, aligned)) {
      return std::optional<std::uint64_t>{};
    }
    return std::optional<std::uint64_t>(aligned);
  };
  auto partitions = target.partitions;
  std::sort(
      partitions.begin(),
      partitions.end(),
      [](const auto& left, const auto& right) {
        return left.offset_bytes < right.offset_bytes;
      });
  const std::uint64_t last_allowed =
      target.partition_style == diskmodel::PartitionStyle::gpt &&
          target.size_bytes > alignment
      ? target.size_bytes - alignment
      : target.size_bytes;
  std::uint64_t cursor = alignment;
  std::vector<TsumugiPhysicalIndividualPartitionRestoreSelection> result;
  const auto consider_gap = [&](const std::uint64_t gap_end) {
    const auto start = align_up(cursor);
    std::uint64_t end{};
    if (!start.has_value() || *start >= gap_end ||
        !checked_add(*start, source->source_size, end) || end > gap_end) {
      return;
    }
    TsumugiPhysicalIndividualPartitionRestoreSelection candidate{
        .source_table_index = source_table_index,
        .target = TsumugiPhysicalUnallocatedRestoreSelection{
            .target_offset = *start,
            .target_size = source->source_size,
        },
    };
    if (validate_tsumugi_physical_individual_partition_selection_v1(
            manifest, target, candidate)) {
      result.push_back(std::move(candidate));
    }
  };
  for (const auto& partition : partitions) {
    if (partition.offset_bytes > cursor) {
      consider_gap(std::min(partition.offset_bytes, last_allowed));
    }
    std::uint64_t partition_end{};
    if (!checked_add(
            partition.offset_bytes, partition.size_bytes, partition_end)) {
      return failure<std::vector<
          TsumugiPhysicalIndividualPartitionRestoreSelection>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi未割当候補",
          L"既存partition範囲がオーバーフローします");
    }
    cursor = std::max(cursor, partition_end);
  }
  if (cursor < last_allowed) {
    consider_gap(last_allowed);
  }
  return clonecore::Result<std::vector<
      TsumugiPhysicalIndividualPartitionRestoreSelection>>::success(
      std::move(result));
}

clonecore::Result<TsumugiPhysicalRestoreReport>
execute_tsumugi_physical_restore_v1(
    const TsumugiPhysicalRestoreRequest& request,
    const TsumugiPhysicalRestoreDependencies& dependencies) {
  if (!request.administrator ||
      !request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token != L"OK" ||
      all_zero(request.expected_image_global_hash) ||
      all_zero(request.expected_source_state_hash) ||
      all_zero(request.expected_target_layout_hash) ||
      !dependencies.verify_image || !dependencies.reidentify_target ||
      !dependencies.set_target_offline ||
      !dependencies.open_offline_target ||
      !dependencies.collect_mbr_signatures ||
      !dependencies.make_connection_token ||
      (!request.individual_partition.has_value() &&
       !dependencies.execute_engine) ||
      (request.individual_partition.has_value() &&
       !dependencies.execute_individual_partition_engine)) {
    return failure<TsumugiPhysicalRestoreReport>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"Tsumugi物理復元開始条件",
        L"管理者権限、大文字OK確認、または必須安全依存が不足しています");
  }

  auto verified = dependencies.verify_image(request.image, request.callbacks);
  if (!verified) {
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        verified.error());
  }
  if (verified.value().container.global_hash !=
          request.expected_image_global_hash ||
      verified.value().manifest.source_state_hash !=
          request.expected_source_state_hash) {
    return failure<TsumugiPhysicalRestoreReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi復元イメージ最終照合",
        L"最終確認した完全検証済みイメージと実行時の内容が一致しません");
  }
  if (verified.value().manifest.mode != TsumugiManifestMode::exact &&
      verified.value().manifest.mode != TsumugiManifestMode::rescue) {
    return failure<TsumugiPhysicalRestoreReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi縮小／個別復元",
        L"縮小配置または個別復元adapterの接続が完了するまで物理書込みを開始しません");
  }

  auto observed = dependencies.reidentify_target(
      request.expected_target, request.confirmation);
  if (!observed) {
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        observed.error());
  }
  const auto target_class = classify_tsumugi_physical_restore_target(
      observed.value().target);
  const auto class_status = validate_tsumugi_physical_restore_target(
      observed.value().target,
      target_class,
      request.target_is_active_rescue_media);
  if (!class_status) {
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        class_status.error());
  }
  if (request.individual_partition.has_value()) {
    const auto individual_status =
        validate_tsumugi_physical_individual_partition_selection_v1(
            verified.value().manifest,
            observed.value().target,
            request.individual_partition.value());
    if (!individual_status) {
      return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
          individual_status.error());
    }
  }
  auto observed_layout =
      hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().target);
  if (!observed_layout ||
      observed_layout.value() != request.expected_target_layout_hash) {
    return failure<TsumugiPhysicalRestoreReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi復元先レイアウト再確認",
        L"最終確認画面の後にパーティション形式または配置が変化しました");
  }
  auto model_hash = hash_tsumugi_source_model_v1(
      observed.value().target.model);
  auto serial_hash = hash_tsumugi_source_serial_v1(
      observed.value().target.serial_suffix,
      observed.value().target.device_instance_id);
  if (!model_hash || !serial_hash) {
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        !model_hash ? model_hash.error() : serial_hash.error());
  }
  if (model_hash.value() ==
          verified.value().manifest.source_model_hash &&
      serial_hash.value() ==
          verified.value().manifest.source_serial_hash) {
    return failure<TsumugiPhysicalRestoreReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"Tsumugi復元元ディスク保護",
        L"イメージを作成した元ディスクと同じ対象には復元できません");
  }
  if ((!request.individual_partition.has_value() &&
       observed.value().target.size_bytes <
           verified.value().manifest.source_disk_size) ||
      observed.value().target.logical_sector_size !=
          verified.value().manifest.logical_sector_size) {
    return failure<TsumugiPhysicalRestoreReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"Tsumugi物理復元対象寸法",
        request.individual_partition.has_value()
            ? L"個別復元には同じ論理セクターと必要容量を満たす対象が必要です"
            : L"通常／救出復元には元以上の容量と同じ論理セクターが必要です");
  }

  Sha256Digest connection_token{};
  if (target_class.usb_attached) {
    auto generated = dependencies.make_connection_token();
    if (!generated) {
      return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
          generated.error());
    }
    connection_token = generated.take_value();
  }
  std::vector<std::uint32_t> signatures;
  if (!request.individual_partition.has_value() &&
      verified.value().manifest.partition_style ==
      TsumugiManifestPartitionStyle::mbr) {
    auto collected = dependencies.collect_mbr_signatures(
        request.expected_target);
    if (!collected) {
      return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
          collected.error());
    }
    signatures = collected.take_value();
  }

  auto offline = dependencies.set_target_offline(
      request.expected_target, request.confirmation, true);
  if (!offline) {
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        offline.error());
  }
  auto handle = dependencies.open_offline_target(
      request.expected_target, request.confirmation);
  if (!handle) {
    const auto protected_offline = dependencies.set_target_offline(
        request.expected_target, request.confirmation, true);
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        append_offline_failure(handle.error(), protected_offline));
  }
  const auto opened_identity = clonecore::validate_stable_identity(
      observed.value().target_identity,
      handle.value().observed.target_identity,
      L"Tsumugiロック済み復元先");
  const auto opened_class = classify_tsumugi_physical_restore_target(
      handle.value().observed.target);
  const auto opened_class_status = validate_tsumugi_physical_restore_target(
      handle.value().observed.target,
      opened_class,
      request.target_is_active_rescue_media);
  auto opened_layout = hash_tsumugi_physical_restore_target_layout_v1(
      handle.value().observed.target);
  const auto opened_individual_status = request.individual_partition
      ? validate_tsumugi_physical_individual_partition_selection_v1(
            verified.value().manifest,
            handle.value().observed.target,
            request.individual_partition.value())
      : clonecore::success_status();
  if (!opened_identity || !opened_class_status || !opened_layout ||
      !opened_individual_status ||
      opened_layout.value() != request.expected_target_layout_hash ||
      opened_class != target_class ||
      !handle.value().observed.target.offline.value_or(false) ||
      !handle.value().target ||
      handle.value().target->size_bytes() !=
          handle.value().observed.target_identity.size_bytes ||
      handle.value().target->logical_sector_size() !=
          handle.value().observed.target_identity.logical_sector_size) {
    const auto primary = opened_identity
        ? !opened_class_status
              ? opened_class_status.error()
              : !opened_individual_status
                    ? opened_individual_status.error()
              : restore_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"Tsumugiロック済み復元先寸法",
                    L"同じ対象ハンドルの属性、分類、容量、またはセクターが計画と一致しません")
        : opened_identity.error();
    handle.value().target.reset();
    const auto protected_offline = dependencies.set_target_offline(
        request.expected_target, request.confirmation, true);
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        append_offline_failure(primary, protected_offline));
  }

  auto restore_identity = make_restore_identity(
      handle.value().observed,
      opened_class,
      request.target_is_active_rescue_media,
      connection_token);
  if (!restore_identity) {
    handle.value().target.reset();
    const auto protected_offline = dependencies.set_target_offline(
        request.expected_target, request.confirmation, true);
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        append_offline_failure(
            restore_identity.error(), protected_offline));
  }

  const auto locked_expected_identity =
      handle.value().observed.target_identity;
  bool locked_target_revalidated = false;
  const TsumugiPhysicalRestoreLockedTargetRevalidator
      revalidate_locked_target = [&]()
          -> clonecore::Result<TsumugiRestoreDiskIdentity> {
    auto current = dependencies.reidentify_target(
        request.expected_target, request.confirmation);
    if (!current) {
      return clonecore::Result<TsumugiRestoreDiskIdentity>::failure(
          current.error());
    }
    const auto identity = clonecore::validate_stable_identity(
        locked_expected_identity,
        current.value().target_identity,
        L"Tsumugi書込み直前復元先");
    const auto current_class = classify_tsumugi_physical_restore_target(
        current.value().target);
    const auto current_class_status =
        validate_tsumugi_physical_restore_target(
            current.value().target,
            current_class,
            request.target_is_active_rescue_media);
    auto current_layout =
        hash_tsumugi_physical_restore_target_layout_v1(
            current.value().target);
    const auto current_individual_status = request.individual_partition
        ? validate_tsumugi_physical_individual_partition_selection_v1(
              verified.value().manifest,
              current.value().target,
              request.individual_partition.value())
        : clonecore::success_status();
    if (!identity || !current_class_status || !current_layout ||
        !current_individual_status ||
        current_layout.value() != request.expected_target_layout_hash ||
        current_class != opened_class ||
        !current.value().target.offline.value_or(false)) {
      return clonecore::Result<TsumugiRestoreDiskIdentity>::failure(
          !identity
              ? identity.error()
              : !current_class_status
                    ? current_class_status.error()
                    : !current_individual_status
                          ? current_individual_status.error()
                    : restore_error(
                          clonecore::ErrorCode::identity_mismatch,
                          ERROR_DEVICE_REINITIALIZATION_NEEDED,
                          L"Tsumugi書込み直前復元先再照合",
                          L"完全イメージ再検証中に対象の識別、分類、offline状態、またはレイアウトが変化しました"));
    }
    auto current_restore_identity = make_restore_identity(
        current.value(),
        current_class,
        request.target_is_active_rescue_media,
        connection_token);
    if (!current_restore_identity ||
        !same_restore_identity(
            restore_identity.value(), current_restore_identity.value())) {
      return clonecore::Result<TsumugiRestoreDiskIdentity>::failure(
          current_restore_identity
              ? restore_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"Tsumugi書込み直前復元先Session",
                    L"対象またはUSB接続Sessionが実行計画と一致しません")
              : current_restore_identity.error());
    }
    locked_target_revalidated = true;
    return current_restore_identity;
  };

  auto executed = [&]() {
    try {
      if (request.individual_partition.has_value()) {
        return dependencies.execute_individual_partition_engine(
            request.image,
            verified.value(),
            restore_identity.value(),
            request.individual_partition.value(),
            handle.take_value(),
            revalidate_locked_target,
            request.callbacks);
      }
      return dependencies.execute_engine(
          request.image,
          verified.value(),
          restore_identity.value(),
          handle.take_value(),
          signatures,
          revalidate_locked_target,
          request.callbacks);
    } catch (...) {
      return failure<TsumugiRestoreReport>(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"Tsumugi物理復元Engine",
          L"復元Engineが例外で停止したため対象を未完成のままoffline保持します");
    }
  }();
  const auto protected_offline = dependencies.set_target_offline(
      request.expected_target, request.confirmation, true);
  if (!executed) {
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        append_offline_failure(executed.error(), protected_offline));
  }
  if (!protected_offline) {
    return clonecore::Result<TsumugiPhysicalRestoreReport>::failure(
        protected_offline.error());
  }
  if (!executed.value().callbacks_started_after_complete_verification ||
      !executed.value().image_matched_prepared_plan ||
      !locked_target_revalidated ||
      !executed.value().target_reidentified_before_write ||
      !executed.value().all_writes_read_back_verified ||
      !executed.value().final_layout_committed) {
    return failure<TsumugiPhysicalRestoreReport>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Tsumugi物理復元完了証跡",
        L"完全検証、再識別、読戻し、または最終レイアウト確定の証跡が不足しています");
  }
  return clonecore::Result<TsumugiPhysicalRestoreReport>::success({
      .restore = executed.take_value(),
      .initial_image_verification_completed = true,
      .target_reidentified_before_offline = true,
      .target_handle_reidentified = true,
      .target_left_offline = true,
      .boot_repair_offer_required =
          request.individual_partition.has_value()
              ? selected_partition_contains_windows(
                    verified.value().manifest,
                    request.individual_partition.value())
              : contains_windows(verified.value().manifest),
      .partial_loss = verified.value().partial_loss,
  });
}

clonecore::Result<TsumugiRestoreReport>
execute_tsumugi_physical_whole_disk_restore_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    const std::span<const std::uint32_t> disallowed_mbr_signatures,
    const TsumugiRestoreHost host,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (!target || !revalidate_locked_target ||
      (host != TsumugiRestoreHost::windows &&
       host != TsumugiRestoreHost::winpe)) {
    return failure<TsumugiRestoreReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi物理復元Engine開始条件",
        L"ロック済み対象ハンドルまたは実行環境が不正です");
  }
  auto plan = prepare_tsumugi_restore_plan_v1(
      TsumugiRestorePlanRequest{
          .image = image_request,
          .host = host,
          .target = TsumugiWholeDiskRestoreTarget{
              .disk = target_identity,
          },
      },
      callbacks);
  if (!plan) {
    return clonecore::Result<TsumugiRestoreReport>::failure(plan.error());
  }
  if (plan.value().image().container.global_hash !=
          initially_verified.container.global_hash ||
      plan.value().image().manifest.source_state_hash !=
          initially_verified.manifest.source_state_hash) {
    return failure<TsumugiRestoreReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi復元イメージ再検証",
        L"対象をoffline化する前に検証したイメージと再検証結果が一致しません");
  }

  LockedPhysicalRestoreSession session(
      std::move(target),
      target_identity,
      std::vector<std::uint32_t>(
          disallowed_mbr_signatures.begin(),
          disallowed_mbr_signatures.end()),
      std::nullopt,
      host,
      revalidate_locked_target,
      callbacks);
  TsumugiBlockRestoreTransaction transaction(session);
  return execute_tsumugi_restore_plan_v1(
      plan.value(), image_request.password, transaction, callbacks);
}

clonecore::Result<TsumugiRestoreReport>
execute_tsumugi_physical_individual_partition_restore_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    const TsumugiRestoreHost host,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (!target || !revalidate_locked_target ||
      selection.source_table_index == 0U ||
      (host != TsumugiRestoreHost::windows &&
       host != TsumugiRestoreHost::winpe)) {
    return failure<TsumugiRestoreReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi個別パーティション復元Engine開始条件",
        L"ロック済み対象、復元元／復元先選択、または実行環境が不正です");
  }

  const auto service_target = std::visit(
      [&](const auto& selected) -> TsumugiIndividualPartitionTarget {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<
                          Selected,
                          TsumugiPhysicalExistingPartitionRestoreSelection>) {
          return TsumugiExistingPartitionRestoreTarget{
              .disk = target_identity,
              .target_table_index = selected.target_table_index,
              .target_partition_number = selected.target_partition_number,
              .target_offset = selected.target_offset,
              .target_size = selected.target_size,
          };
        } else {
          return TsumugiUnallocatedRestoreTarget{
              .disk = target_identity,
              .target_offset = selected.target_offset,
              .target_size = selected.target_size,
          };
        }
      },
      selection.target);

  auto plan = prepare_tsumugi_restore_plan_v1(
      TsumugiRestorePlanRequest{
          .image = image_request,
          .host = host,
          .target = TsumugiIndividualPartitionRestoreTarget{
              .source_table_index = selection.source_table_index,
              .target = service_target,
          },
      },
      callbacks);
  if (!plan) {
    return clonecore::Result<TsumugiRestoreReport>::failure(plan.error());
  }
  if (plan.value().image().container.global_hash !=
          initially_verified.container.global_hash ||
      plan.value().image().manifest.source_state_hash !=
          initially_verified.manifest.source_state_hash) {
    return failure<TsumugiRestoreReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Tsumugi個別復元イメージ再検証",
        L"対象をoffline化する前に検証したイメージと再検証結果が一致しません");
  }

  LockedPhysicalRestoreSession session(
      std::move(target),
      target_identity,
      {},
      selection,
      host,
      revalidate_locked_target,
      callbacks);
  TsumugiBlockRestoreTransaction transaction(session);
  return execute_tsumugi_restore_plan_v1(
      plan.value(), image_request.password, transaction, callbacks);
}

clonecore::Result<TsumugiRestoreReport>
execute_tsumugi_physical_existing_partition_restore_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    const TsumugiRestoreHost host,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (!std::holds_alternative<
          TsumugiPhysicalExistingPartitionRestoreSelection>(
          selection.target)) {
    return failure<TsumugiRestoreReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi既存パーティション復元Engine開始条件",
        L"互換入口には既存パーティション選択が必要です");
  }
  return execute_tsumugi_physical_individual_partition_restore_engine_v1(
      image_request,
      initially_verified,
      target_identity,
      selection,
      std::move(target),
      host,
      revalidate_locked_target,
      callbacks);
}

clonecore::Result<std::vector<std::uint32_t>>
collect_tsumugi_mbr_signatures_with_windows_apis(
    const clonecore::StableDiskIdentity& selected_target) {
  const auto selected = clonecore::validate_stable_identity(
      selected_target, selected_target, L"Tsumugi選択復元先");
  if (!selected) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        selected.error());
  }
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  auto report = inventory->enumerate();
  if (!report) {
    return clonecore::Result<std::vector<std::uint32_t>>::failure(
        report.error());
  }
  if (!report.value().issues.empty()) {
    return failure<std::vector<std::uint32_t>>(
        clonecore::ErrorCode::query_failed,
        ERROR_INVALID_DATA,
        L"Tsumugi MBR署名全ディスク列挙",
        L"未解決の列挙診断があるため署名衝突を確認できません");
  }
  std::vector<std::uint32_t> signatures;
  for (const auto& disk : report.value().disks) {
    if (diskmodel::normalize_disk_partition_style(
            disk.partition_style, disk.partitions.size()) !=
        diskmodel::PartitionStyle::mbr) {
      continue;
    }
    auto identity = diskmodel::make_stable_disk_identity(
        disk, disk.is_system_disk);
    if (!identity) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          identity.error());
    }
    auto handle =
        diskmodel::open_verified_read_only_physical_disk_with_windows_apis(
            identity.value());
    if (!handle) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          handle.error());
    }
    auto mbr = clonecore::parse_mbr(*handle.value().reader);
    if (!mbr) {
      return clonecore::Result<std::vector<std::uint32_t>>::failure(
          mbr.error());
    }
    signatures.push_back(mbr.value().disk_signature);
  }
  std::sort(signatures.begin(), signatures.end());
  signatures.erase(
      std::unique(signatures.begin(), signatures.end()), signatures.end());
  return clonecore::Result<std::vector<std::uint32_t>>::success(
      std::move(signatures));
}

clonecore::Result<Sha256Digest>
make_tsumugi_connection_instance_hash_with_windows_apis() {
  Sha256Digest result{};
  const NTSTATUS status = BCryptGenRandom(
      nullptr,
      reinterpret_cast<PUCHAR>(result.data()),
      static_cast<ULONG>(result.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status != 0 || all_zero(result)) {
    return failure<Sha256Digest>(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(status),
        L"Tsumugi USB接続Session識別",
        L"現在の接続だけに有効な識別値を生成できません");
  }
  return clonecore::Result<Sha256Digest>::success(result);
}

}  // namespace ytec::imageformat

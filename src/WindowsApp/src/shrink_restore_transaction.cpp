#include "ytec/windowsapp/shrink_restore_transaction.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

clonecore::Error transaction_error(
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
  return clonecore::Result<T>::failure(transaction_error(
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
  return clonecore::Status::failure(transaction_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

template <typename Operation>
clonecore::Status invoke_platform_status(
    Operation&& operation,
    std::wstring operation_name) noexcept {
  try {
    return operation();
  } catch (...) {
    return status_failure(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        std::move(operation_name),
        L"破壊境界が例外を送出したため、コピー先を未完了状態へ中止します");
  }
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

bool selected(
    const imageformat::TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
}

bool archive_file_system(
    const imageformat::TsumugiManifestFileSystem file_system) noexcept {
  return file_system == imageformat::TsumugiManifestFileSystem::ntfs ||
      file_system == imageformat::TsumugiManifestFileSystem::exfat ||
      file_system == imageformat::TsumugiManifestFileSystem::fat32;
}

bool same_target_identity(
    const imageformat::TsumugiRestoreDiskIdentity& expected,
    const imageformat::TsumugiRestoreDiskIdentity& current) noexcept {
  return expected.stable_identity_hash == current.stable_identity_hash &&
      expected.disk_size == current.disk_size &&
      expected.logical_sector_size == current.logical_sector_size &&
      expected.is_running_windows_system_disk ==
          current.is_running_windows_system_disk &&
      expected.is_usb_attached == current.is_usb_attached &&
      expected.is_usb_memory == current.is_usb_memory &&
      expected.is_active_rescue_media == current.is_active_rescue_media &&
      expected.is_dynamic_disk == current.is_dynamic_disk &&
      expected.is_storage_spaces == current.is_storage_spaces &&
      expected.is_windows_software_raid ==
          current.is_windows_software_raid &&
      expected.has_unresolved_hardware_raid ==
          current.has_unresolved_hardware_raid &&
      expected.connection_instance_hash == current.connection_instance_hash;
}

bool same_migration_partition(
    const migrationcore::ShrinkPlannedPartition& left,
    const migrationcore::ShrinkPlannedPartition& right) noexcept {
  return left.target_number == right.target_number &&
      left.source_table_index == right.source_table_index &&
      left.role == right.role && left.file_system == right.file_system &&
      left.action == right.action &&
      left.offset_bytes == right.offset_bytes &&
      left.size_bytes == right.size_bytes &&
      left.source_size_bytes == right.source_size_bytes &&
      left.source_used_bytes == right.source_used_bytes &&
      left.label == right.label && left.active == right.active;
}

bool same_migration_plan(
    const migrationcore::ShrinkMigrationPlan& left,
    const migrationcore::ShrinkMigrationPlan& right) noexcept {
  return left.target_style == right.target_style &&
      left.alignment_bytes == right.alignment_bytes &&
      left.minimum_target_size_bytes == right.minimum_target_size_bytes &&
      left.target_size_bytes == right.target_size_bytes &&
      left.unallocated_tail_bytes == right.unallocated_tail_bytes &&
      left.source_remains_unchanged == right.source_remains_unchanged &&
      left.boot_finalization_required == right.boot_finalization_required &&
      left.notes == right.notes &&
      left.target_partitions.size() == right.target_partitions.size() &&
      std::equal(
          left.target_partitions.begin(),
          left.target_partitions.end(),
          right.target_partitions.begin(),
          same_migration_partition);
}

bool same_layout_write(
    const imageformat::TsumugiRestoreLayoutWrite& left,
    const imageformat::TsumugiRestoreLayoutWrite& right) noexcept {
  return left.kind == right.kind && left.offset == right.offset &&
      left.bytes == right.bytes;
}

bool same_gpt_partition(
    const clonecore::GptPartition& left,
    const clonecore::GptPartition& right) noexcept {
  return left.entry_index == right.entry_index &&
      left.type_guid == right.type_guid &&
      left.unique_guid == right.unique_guid &&
      left.first_lba == right.first_lba && left.last_lba == right.last_lba &&
      left.attributes == right.attributes && left.name == right.name;
}

bool same_gpt_disk(
    const clonecore::GptDisk& left,
    const clonecore::GptDisk& right) noexcept {
  return left.logical_sector_size == right.logical_sector_size &&
      left.sector_count == right.sector_count &&
      left.disk_guid == right.disk_guid &&
      left.first_usable_lba == right.first_usable_lba &&
      left.last_usable_lba == right.last_usable_lba &&
      left.partition_entry_count == right.partition_entry_count &&
      left.partition_entry_size == right.partition_entry_size &&
      left.partitions.size() == right.partitions.size() &&
      std::equal(
          left.partitions.begin(),
          left.partitions.end(),
          right.partitions.begin(),
          same_gpt_partition);
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
    const clonecore::MbrDisk& right) noexcept {
  return left.logical_sector_size == right.logical_sector_size &&
      left.sector_count == right.sector_count &&
      left.disk_signature == right.disk_signature &&
      left.bootstrap == right.bootstrap &&
      left.partitions.size() == right.partitions.size() &&
      std::equal(
          left.partitions.begin(),
          left.partitions.end(),
          right.partitions.begin(),
          same_mbr_partition);
}

bool same_reviewed_layout(
    const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& left,
    const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& right)
    noexcept {
  const auto& left_metadata = left.metadata;
  const auto& right_metadata = right.metadata;
  if (!same_migration_plan(left.migration, right.migration) ||
      left_metadata.style != right_metadata.style ||
      left_metadata.target_size_bytes != right_metadata.target_size_bytes ||
      left_metadata.logical_sector_size !=
          right_metadata.logical_sector_size ||
      left_metadata.invalidation_ranges.size() !=
          right_metadata.invalidation_ranges.size() ||
      left_metadata.staged_writes.size() !=
          right_metadata.staged_writes.size() ||
      left_metadata.commit_writes.size() !=
          right_metadata.commit_writes.size() ||
      left_metadata.target_layout.index() !=
          right_metadata.target_layout.index() ||
      !std::equal(
          left_metadata.invalidation_ranges.begin(),
          left_metadata.invalidation_ranges.end(),
          right_metadata.invalidation_ranges.begin(),
          [](const auto& first, const auto& second) {
            return first.offset == second.offset &&
                first.length == second.length;
          }) ||
      !std::equal(
          left_metadata.staged_writes.begin(),
          left_metadata.staged_writes.end(),
          right_metadata.staged_writes.begin(),
          same_layout_write) ||
      !std::equal(
          left_metadata.commit_writes.begin(),
          left_metadata.commit_writes.end(),
          right_metadata.commit_writes.begin(),
          same_layout_write)) {
    return false;
  }
  if (const auto* left_gpt =
          std::get_if<clonecore::GptDisk>(&left_metadata.target_layout);
      left_gpt != nullptr) {
    const auto* right_gpt =
        std::get_if<clonecore::GptDisk>(&right_metadata.target_layout);
    return right_gpt != nullptr && same_gpt_disk(*left_gpt, *right_gpt);
  }
  const auto* left_mbr =
      std::get_if<clonecore::MbrDisk>(&left_metadata.target_layout);
  const auto* right_mbr =
      std::get_if<clonecore::MbrDisk>(&right_metadata.target_layout);
  return left_mbr != nullptr && right_mbr != nullptr &&
      same_mbr_disk(*left_mbr, *right_mbr);
}

}  // namespace

class WindowsTsumugiShrinkRestoreTransaction::Impl final {
 public:
  enum class State : std::uint8_t {
    ready,
    begun,
    committed,
    aborted,
  };

  struct ExpectedPartition final {
    imageformat::TsumugiManifestPartition manifest;
    imageformat::TsumugiRestorePartitionPlacement placement;
    std::uint64_t delivered{};
    bool completed{};
  };

  Impl(
      WindowsShrinkWorkPaths paths,
      imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 layout,
      std::unique_ptr<IWindowsTsumugiShrinkRestorePlatform> owned_platform)
      : work_paths(std::move(paths)),
        reviewed_layout(std::move(layout)),
        platform(std::move(owned_platform)) {}

  WindowsShrinkWorkPaths work_paths;
  imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 reviewed_layout;
  std::unique_ptr<IWindowsTsumugiShrinkRestorePlatform> platform;
  State state{State::ready};
  imageformat::TsumugiRestoreDiskIdentity target_identity{};
  std::map<std::uint32_t, ExpectedPartition> expected;
  std::optional<imageformat::TsumugiShrinkArchiveTarget> active_archive;
};

WindowsTsumugiShrinkRestoreTransaction::
WindowsTsumugiShrinkRestoreTransaction(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

WindowsTsumugiShrinkRestoreTransaction::
~WindowsTsumugiShrinkRestoreTransaction() {
  abort();
}

clonecore::Result<imageformat::TsumugiRestoreDiskIdentity>
WindowsTsumugiShrinkRestoreTransaction::begin(
    const imageformat::TsumugiVerifiedImage& image,
    const imageformat::TsumugiRestoreTarget& target,
    const imageformat::TsumugiRestoreHost host) {
  if (!impl_ || !impl_->platform || impl_->state != Impl::State::ready ||
      (host != imageformat::TsumugiRestoreHost::windows &&
       host != imageformat::TsumugiRestoreHost::winpe) ||
      image.manifest.mode != imageformat::TsumugiManifestMode::shrink) {
    return failure<imageformat::TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows縮小復元開始",
        L"未使用transaction、Windows host、縮小画像が必要です");
  }
  const auto* whole =
      std::get_if<imageformat::TsumugiWholeDiskRestoreTarget>(&target);
  if (whole == nullptr) {
    return failure<imageformat::TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Windows縮小復元対象",
        L"この最小sliceはディスク全体の縮小復元だけを対象にします");
  }
  if (!whole->reviewed_shrink_layout.has_value() ||
      !same_reviewed_layout(
          whole->reviewed_shrink_layout.value(),
          impl_->reviewed_layout)) {
    return failure<imageformat::TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小レビュー済み最終レイアウト拘束",
        L"完全検証時に封印した最終レイアウトと実行platformのレイアウトが一致しません");
  }
  const auto derived_bindings =
      imageformat::make_tsumugi_shrink_payload_bindings_v1(
          image.manifest, impl_->reviewed_layout);
  if (!derived_bindings) {
    return clonecore::Result<
        imageformat::TsumugiRestoreDiskIdentity>::failure(
        derived_bindings.error());
  }
  std::map<std::uint32_t, imageformat::TsumugiShrinkPayloadBindingV1>
      payload_bindings;
  for (const auto& binding : derived_bindings.value()) {
    if (!payload_bindings.emplace(
            binding.source_table_index, binding).second) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Windows縮小payload binding",
          L"レビュー済みpayload bindingが重複しています");
    }
  }

  std::map<std::uint32_t, imageformat::TsumugiRestorePartitionPlacement>
      placements;
  for (const auto& placement : whole->shrink_placements) {
    if (!placements.emplace(placement.source_table_index, placement).second) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::invalid_data,
          ERROR_DUP_NAME,
          L"Windows縮小復元配置",
          L"同じコピー元パーティションの配置が重複しています");
    }
  }
  const auto& reviewed = impl_->reviewed_layout;
  const bool reviewed_style_matches =
      reviewed.migration.target_style ==
              migrationcore::MigrationPartitionStyle::gpt
          ? reviewed.metadata.style == imageformat::PartitionTableStyle::gpt &&
              std::holds_alternative<clonecore::GptDisk>(
                  reviewed.metadata.target_layout)
          : reviewed.metadata.style == imageformat::PartitionTableStyle::mbr &&
              std::holds_alternative<clonecore::MbrDisk>(
                  reviewed.metadata.target_layout);
  if (!reviewed.migration.source_remains_unchanged ||
      reviewed.migration.target_size_bytes != whole->disk.disk_size ||
      reviewed.metadata.target_size_bytes != whole->disk.disk_size ||
      reviewed.metadata.logical_sector_size !=
          whole->disk.logical_sector_size ||
      !reviewed_style_matches) {
    return failure<imageformat::TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小レビュー済み最終レイアウト",
        L"OK確認済み最終レイアウトの形式、容量、セクター、またはコピー元不変条件が対象と一致しません");
  }
  std::map<std::uint32_t, imageformat::TsumugiRestorePartitionPlacement>
      reviewed_placements;
  for (const auto& partition : reviewed.migration.target_partitions) {
    if (!partition.source_table_index.has_value()) {
      continue;
    }
    if (!reviewed_placements
             .emplace(
                 partition.source_table_index.value(),
                 imageformat::TsumugiRestorePartitionPlacement{
                     .source_table_index =
                         partition.source_table_index.value(),
                     .target_offset = partition.offset_bytes,
                     .target_size = partition.size_bytes,
                 })
             .second) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Windows縮小レビュー済み配置",
          L"最終レイアウト内のコピー元対応が重複しています");
    }
  }
  if (placements.size() != reviewed_placements.size() ||
      !std::equal(
          placements.begin(),
          placements.end(),
          reviewed_placements.begin(),
          [](const auto& left, const auto& right) {
            return left.first == right.first &&
                left.second.source_table_index ==
                    right.second.source_table_index &&
                left.second.target_offset == right.second.target_offset &&
                left.second.target_size == right.second.target_size;
          })) {
    return failure<imageformat::TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小レビュー済み配置",
        L"任意placementは受理しません。OK確認済み最終レイアウトから生成した配置だけを実行できます");
  }
  std::map<std::uint32_t, Impl::ExpectedPartition> expected;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> target_ranges;
  for (const auto& partition : image.manifest.partitions) {
    if (!selected(partition)) {
      continue;
    }
    const auto binding = payload_bindings.find(
        partition.source_table_index);
    if (binding == payload_bindings.end()) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"Windows縮小payload binding",
          L"選択済みpayloadにレビュー済みbindingがありません");
    }
    const bool restore_payload = binding->second.disposition ==
        imageformat::TsumugiShrinkPayloadDispositionV1::
            restore_to_reviewed_partition;
    const bool recreate_empty = binding->second.disposition ==
        imageformat::TsumugiShrinkPayloadDispositionV1::
            recreate_empty_file_system;
    if (!restore_payload && !recreate_empty) {
      continue;
    }
    const auto placement = placements.find(partition.source_table_index);
    const bool wim = partition.payload_encoding == imageformat::
        TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
    const bool raw = partition.payload_encoding ==
        imageformat::TsumugiManifestPayloadEncoding::exact_raw;
    const bool valid_wim = wim && archive_file_system(partition.file_system) &&
        partition.payload_format_version ==
            imageformat::kTsumugiWimPayloadFormatVersion &&
        partition.cluster_size != 0U &&
        (recreate_empty ? partition.used_bytes == 0U
                        : partition.used_bytes != 0U);
    const bool valid_raw = restore_payload && raw &&
        (partition.file_system ==
             imageformat::TsumugiManifestFileSystem::unknown ||
         partition.file_system ==
             imageformat::TsumugiManifestFileSystem::none) &&
        whole->disk.logical_sector_size ==
            image.manifest.logical_sector_size &&
        partition.payload_format_version == 0U &&
        partition.cluster_size == 0U &&
        partition.payload_logical_length == partition.source_size;
    std::uint64_t placement_end{};
    if (placement == placements.end() ||
        whole->disk.logical_sector_size == 0U ||
        placement->second.target_offset %
                whole->disk.logical_sector_size !=
            0U ||
        placement->second.target_size %
                whole->disk.logical_sector_size !=
            0U ||
        placement->second.target_size < partition.minimum_target_bytes ||
        !checked_add(
            placement->second.target_offset,
            placement->second.target_size,
            placement_end) ||
        placement_end > whole->disk.disk_size ||
        binding->second.target_offset !=
            placement->second.target_offset ||
        binding->second.target_size != placement->second.target_size ||
        (!valid_wim && !valid_raw) ||
        !expected.emplace(
             partition.source_table_index,
             Impl::ExpectedPartition{
                 .manifest = partition,
                 .placement = placement->second,
             })
             .second) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"Windows縮小復元payload契約",
          L"WIM、未知形式exact RAW、配置、または一意性が製品契約と一致しません");
    }
    target_ranges.emplace_back(
        placement->second.target_offset, placement_end);
  }
  if (expected.empty() || expected.size() != placements.size()) {
    return failure<imageformat::TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"Windows縮小復元配置対応",
        L"選択済みpayloadとコピー先配置が一対一ではありません");
  }
  std::sort(target_ranges.begin(), target_ranges.end());
  for (std::size_t index = 1U; index < target_ranges.size(); ++index) {
    if (target_ranges[index].first < target_ranges[index - 1U].second) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_INVALID_DATA,
          L"Windows縮小復元配置重複",
          L"コピー先パーティション配置が重複しています");
    }
  }

  impl_->state = Impl::State::begun;
  auto begun = [&]() noexcept
      -> clonecore::Result<imageformat::TsumugiRestoreDiskIdentity> {
    try {
      return impl_->platform->begin_offline_incomplete(
          image, *whole, impl_->reviewed_layout, impl_->work_paths);
    } catch (...) {
      return failure<imageformat::TsumugiRestoreDiskIdentity>(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"Windows縮小復元platform開始例外",
          L"破壊境界が例外を送出したため未完了状態へ中止します");
    }
  }();
  if (!begun) {
    const auto error = begun.error();
    abort();
    return clonecore::Result<
        imageformat::TsumugiRestoreDiskIdentity>::failure(error);
  }
  if (!same_target_identity(whole->disk, begun.value())) {
    abort();
    return failure<imageformat::TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"Windows縮小復元先再識別",
        L"offline未完了化した対象が検討済み安定識別と一致しません");
  }
  impl_->target_identity = begun.value();
  impl_->expected = std::move(expected);
  return begun;
}

clonecore::Status
WindowsTsumugiShrinkRestoreTransaction::write_exact_raw_and_verify(
    const imageformat::TsumugiRestoreWrite& write,
    const std::span<const std::byte> plaintext) {
  if (!impl_ || impl_->state != Impl::State::begun ||
      impl_->active_archive.has_value()) {
    auto status = status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Windows縮小exact RAW状態",
        L"開始済みでWIM処理中ではないtransactionが必要です");
    if (impl_ && impl_->state == Impl::State::begun) {
      abort();
    }
    return status;
  }
  const auto found = impl_->expected.find(write.source_table_index);
  if (found == impl_->expected.end()) {
    auto status = status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"Windows縮小exact RAW対応",
        L"認証済みマニフェストに対応するRAW payloadがありません");
    abort();
    return status;
  }
  auto& expected = found->second;
  const auto& manifest = expected.manifest;
  const bool plaintext_shape_valid = write.zero_fill
      ? plaintext.empty()
      : plaintext.size() == write.length;
  std::uint64_t expected_source{};
  std::uint64_t expected_target{};
  std::uint64_t next{};
  if (manifest.payload_encoding !=
          imageformat::TsumugiManifestPayloadEncoding::exact_raw ||
      expected.completed || write.length == 0U ||
      write.unreadable_zero_fill || !plaintext_shape_valid ||
      write.stable_target_identity_hash !=
          impl_->target_identity.stable_identity_hash ||
      write.source_partition_number != manifest.source_partition_number ||
      write.source_payload_offset %
              impl_->target_identity.logical_sector_size !=
          0U ||
      write.target_offset % impl_->target_identity.logical_sector_size != 0U ||
      write.length % impl_->target_identity.logical_sector_size != 0U ||
      !checked_add(
          manifest.payload_logical_offset,
          expected.delivered,
          expected_source) ||
      !checked_add(
          expected.placement.target_offset,
          expected.delivered,
          expected_target) ||
      write.source_payload_offset != expected_source ||
      write.target_offset != expected_target ||
      !checked_add(expected.delivered, write.length, next) ||
      next > manifest.payload_logical_length) {
    auto status = status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小exact RAW範囲",
        L"RAW payloadの識別、順序、長さ、またはコピー先位置が一致しません");
    abort();
    return status;
  }
  const auto written = invoke_platform_status(
      [&] {
        return impl_->platform->write_exact_raw_and_verify(
            write, plaintext);
      },
      L"Windows縮小exact RAW書込み例外");
  if (!written) {
    const auto error = written.error();
    abort();
    return clonecore::Status::failure(error);
  }
  expected.delivered = next;
  expected.completed = next == manifest.payload_logical_length;
  return clonecore::success_status();
}

clonecore::Status WindowsTsumugiShrinkRestoreTransaction::
recreate_empty_file_system_and_verify(
    const imageformat::TsumugiShrinkArchiveTarget& target) {
  if (!impl_ || impl_->state != Impl::State::begun ||
      impl_->active_archive.has_value()) {
    auto status = status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Windows縮小空ファイルシステム状態",
        L"開始済みでWIM処理中ではないtransactionが必要です");
    if (impl_ && impl_->state == Impl::State::begun) {
      abort();
    }
    return status;
  }
  const auto found = impl_->expected.find(target.source_table_index);
  if (found == impl_->expected.end()) {
    auto status = status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"Windows縮小空ファイルシステム対応",
        L"レビュー済み計画に対応する空ファイルシステム対象がありません");
    abort();
    return status;
  }
  auto& expected = found->second;
  const auto& manifest = expected.manifest;
  if (manifest.payload_encoding != imageformat::
          TsumugiManifestPayloadEncoding::microsoft_wim_single_image ||
      manifest.used_bytes != 0U ||
      expected.completed || target.stable_target_identity_hash !=
          impl_->target_identity.stable_identity_hash ||
      target.source_partition_number != manifest.source_partition_number ||
      target.file_system != manifest.file_system ||
      target.payload_format_version != manifest.payload_format_version ||
      target.cluster_size != manifest.cluster_size ||
      target.target_offset != expected.placement.target_offset ||
      target.target_size != expected.placement.target_size ||
      target.archive_length != manifest.payload_logical_length) {
    auto status = status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小空ファイルシステム照合",
        L"空payload、コピー先FS、配置、または認証済み属性が一致しません");
    abort();
    return status;
  }
  auto status = invoke_platform_status(
      [&] { return impl_->platform->create_target_file_system(target); },
      L"Windows縮小空ファイルシステム作成例外");
  if (!status) {
    const auto error = status.error();
    abort();
    return clonecore::Status::failure(error);
  }
  status = invoke_platform_status(
      [&] {
        return impl_->platform->verify_applied_file_system_readback(target);
      },
      L"Windows縮小空ファイルシステム読戻し例外");
  if (!status) {
    const auto error = status.error();
    abort();
    return clonecore::Status::failure(error);
  }
  expected.completed = true;
  return clonecore::success_status();
}

clonecore::Status WindowsTsumugiShrinkRestoreTransaction::begin_wim_archive(
    const imageformat::TsumugiShrinkArchiveTarget& target) {
  if (!impl_ || impl_->state != Impl::State::begun ||
      impl_->active_archive.has_value()) {
    auto status = status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Windows縮小WIM開始状態",
        L"同時に開けるWIM stagingは1件だけです");
    if (impl_ && impl_->state == Impl::State::begun) {
      abort();
    }
    return status;
  }
  const auto found = impl_->expected.find(target.source_table_index);
  if (found == impl_->expected.end()) {
    auto status = status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"Windows縮小WIM対応",
        L"認証済みマニフェストに対応するWIM payloadがありません");
    abort();
    return status;
  }
  const auto& expected = found->second;
  const auto& manifest = expected.manifest;
  if (manifest.payload_encoding != imageformat::
          TsumugiManifestPayloadEncoding::microsoft_wim_single_image ||
      expected.completed || target.stable_target_identity_hash !=
          impl_->target_identity.stable_identity_hash ||
      target.source_partition_number != manifest.source_partition_number ||
      target.file_system != manifest.file_system ||
      target.payload_format_version != manifest.payload_format_version ||
      target.cluster_size != manifest.cluster_size ||
      target.target_offset != expected.placement.target_offset ||
      target.target_size != expected.placement.target_size ||
      target.archive_length != manifest.payload_logical_length) {
    auto status = status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小WIM開始照合",
        L"WIM、コピー先FS、配置、または認証済み長さが一致しません");
    abort();
    return status;
  }
  auto status = invoke_platform_status(
      [&] { return impl_->platform->create_target_file_system(target); },
      L"Windows縮小コピー先FS作成例外");
  if (!status) {
    const auto error = status.error();
    abort();
    return clonecore::Status::failure(error);
  }
  status = invoke_platform_status(
      [&] {
        return impl_->platform->begin_staged_wim(
            target, impl_->work_paths.scratch_directory);
      },
      L"Windows縮小WIM staging開始例外");
  if (!status) {
    const auto error = status.error();
    abort();
    return clonecore::Status::failure(error);
  }
  impl_->active_archive = target;
  return clonecore::success_status();
}

clonecore::Status WindowsTsumugiShrinkRestoreTransaction::append_wim_archive(
    const imageformat::TsumugiShrinkArchiveChunk& chunk,
    const std::span<const std::byte> plaintext) {
  if (!impl_ || impl_->state != Impl::State::begun ||
      !impl_->active_archive || chunk.length == 0U ||
      (chunk.zero_fill ? !plaintext.empty()
                       : plaintext.size() != chunk.length) ||
      chunk.source_table_index != impl_->active_archive->source_table_index) {
    auto status = status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Windows縮小WIM追記状態",
        L"有効なWIM staging、非ゼロchunk、実payload bytesが必要です");
    if (impl_ && impl_->state == Impl::State::begun) {
      abort();
    }
    return status;
  }
  auto& expected = impl_->expected.at(chunk.source_table_index);
  std::uint64_t expected_source{};
  std::uint64_t next{};
  if (chunk.archive_offset != expected.delivered ||
      !checked_add(
          expected.manifest.payload_logical_offset,
          expected.delivered,
          expected_source) ||
      chunk.source_payload_offset != expected_source ||
      !checked_add(expected.delivered, chunk.length, next) ||
      next > expected.manifest.payload_logical_length) {
    auto status = status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Windows縮小WIM追記範囲",
        L"WIM bytesが認証済み順序または長さと一致しません");
    abort();
    return status;
  }
  const auto appended = invoke_platform_status(
      [&] {
        return impl_->platform->append_staged_wim(chunk, plaintext);
      },
      L"Windows縮小WIM staging追記例外");
  if (!appended) {
    const auto error = appended.error();
    abort();
    return clonecore::Status::failure(error);
  }
  expected.delivered = next;
  return clonecore::success_status();
}

clonecore::Status WindowsTsumugiShrinkRestoreTransaction::
complete_wim_archive_and_verify(const std::uint32_t source_table_index) {
  if (!impl_ || impl_->state != Impl::State::begun ||
      !impl_->active_archive ||
      impl_->active_archive->source_table_index != source_table_index) {
    auto status = status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_STATE,
        L"Windows縮小WIM完了状態",
        L"完了対象のWIM stagingが開かれていません");
    if (impl_ && impl_->state == Impl::State::begun) {
      abort();
    }
    return status;
  }
  auto& expected = impl_->expected.at(source_table_index);
  if (expected.delivered != expected.manifest.payload_logical_length) {
    auto status = status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_HANDLE_EOF,
        L"Windows縮小WIM完全性",
        L"認証済みWIMの全bytesがstagingへ届いていません");
    abort();
    return status;
  }
  auto status = invoke_platform_status(
      [&] {
        return impl_->platform->verify_staged_single_image_wim(
            source_table_index);
      },
      L"Windows縮小WIM検証例外");
  if (!status) {
    const auto error = status.error();
    abort();
    return clonecore::Status::failure(error);
  }
  status = invoke_platform_status(
      [&] { return impl_->platform->apply_staged_wim(source_table_index); },
      L"Windows縮小WIM適用例外");
  if (!status) {
    const auto error = status.error();
    abort();
    return clonecore::Status::failure(error);
  }
  status = invoke_platform_status(
      [&] {
        return impl_->platform->verify_applied_file_system_readback(
            *impl_->active_archive);
      },
      L"Windows縮小適用済みFS読戻し例外");
  if (!status) {
    const auto error = status.error();
    abort();
    return clonecore::Status::failure(error);
  }
  expected.completed = true;
  impl_->active_archive.reset();
  return clonecore::success_status();
}

clonecore::Status WindowsTsumugiShrinkRestoreTransaction::commit() {
  if (!impl_ || impl_->state != Impl::State::begun ||
      impl_->active_archive.has_value() ||
      std::any_of(
          impl_->expected.begin(),
          impl_->expected.end(),
          [](const auto& entry) { return !entry.second.completed; })) {
    auto status = status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        L"Windows縮小最終commit",
        L"全WIM適用読戻しと全exact RAW読戻しが完了するまで最終レイアウトを公開できません");
    if (impl_ && impl_->state == Impl::State::begun) {
      abort();
    }
    return status;
  }
  const auto committed = invoke_platform_status(
      [&] { return impl_->platform->commit_final_layout_last(); },
      L"Windows縮小最終レイアウトcommit例外");
  if (!committed) {
    const auto error = committed.error();
    abort();
    return clonecore::Status::failure(error);
  }
  impl_->state = Impl::State::committed;
  return clonecore::success_status();
}

void WindowsTsumugiShrinkRestoreTransaction::abort() noexcept {
  if (!impl_ || !impl_->platform ||
      impl_->state == Impl::State::ready ||
      impl_->state == Impl::State::committed ||
      impl_->state == Impl::State::aborted) {
    return;
  }
  impl_->platform->abort_keep_offline_incomplete();
  impl_->state = Impl::State::aborted;
  impl_->active_archive.reset();
}

clonecore::Result<std::unique_ptr<
    WindowsTsumugiShrinkRestoreTransaction>>
make_windows_tsumugi_shrink_restore_transaction(
    const clonecore::StableDiskIdentity& protected_source,
    const WindowsShrinkWorkPaths& work_paths,
    const WindowsShrinkWorkPlacementObservation& observed_work,
    const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1&
        reviewed_layout,
    std::unique_ptr<IWindowsTsumugiShrinkRestorePlatform> platform) {
  if (!platform) {
    return failure<std::unique_ptr<
        WindowsTsumugiShrinkRestoreTransaction>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_HANDLE,
        L"Windows縮小復元platform",
        L"破壊境界を所有するWindows platform Adapterがありません");
  }
  const auto placement = validate_windows_shrink_work_placement_observation(
      protected_source, work_paths, observed_work);
  if (!placement) {
    return clonecore::Result<std::unique_ptr<
        WindowsTsumugiShrinkRestoreTransaction>>::failure(
        placement.error());
  }
  auto impl = std::make_unique<
      WindowsTsumugiShrinkRestoreTransaction::Impl>(
      WindowsShrinkWorkPaths{
          .scratch_directory = observed_work.scratch.canonical_path,
          .checkpoint_path = observed_work.checkpoint.canonical_path,
          .log_path = observed_work.log.canonical_path,
          .log_is_ram_only = work_paths.log_is_ram_only,
      },
      reviewed_layout,
      std::move(platform));
  return clonecore::Result<std::unique_ptr<
      WindowsTsumugiShrinkRestoreTransaction>>::success(
      std::unique_ptr<WindowsTsumugiShrinkRestoreTransaction>(
          new WindowsTsumugiShrinkRestoreTransaction(std::move(impl))));
}

}  // namespace ytec::windowsapp

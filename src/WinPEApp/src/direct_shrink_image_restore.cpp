#include "ytec/winpeapp/direct_shrink_image_restore.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/windowsapp/shrink_restore_transaction.h"
#include "ytec/windowsapp/windows_shrink_restore_platform.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
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

clonecore::Status status_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(restore_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool all_zero(const imageformat::Sha256Digest& digest) noexcept {
  return std::all_of(digest.begin(), digest.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

bool selected(
    const imageformat::TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
}

bool same_path(
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

bool stable_identity_matches(
    const clonecore::StableDiskIdentity& expected,
    const clonecore::StableDiskIdentity& observed,
    const std::wstring_view role) {
  return clonecore::validate_stable_identity(expected, observed, role)
      .has_value();
}

clonecore::Status reject_same_disk(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right,
    std::wstring operation,
    std::wstring message) {
  const auto left_self = clonecore::validate_stable_identity(
      left, left, L"縮小復元ディスク識別");
  const auto right_self = clonecore::validate_stable_identity(
      right, right, L"縮小復元ディスク識別");
  if (!left_self || !right_self) {
    return clonecore::Status::failure(
        !left_self ? left_self.error() : right_self.error());
  }
  if (stable_identity_matches(left, right, L"縮小復元ディスク分離")) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        std::move(operation),
        std::move(message));
  }
  return clonecore::success_status();
}

clonecore::Status compare_work_observation(
    const windowsapp::WindowsShrinkWorkPaths& requested,
    const windowsapp::WindowsShrinkWorkPlacementObservation& expected,
    const windowsapp::WindowsShrinkWorkPlacementObservation& observed) {
  const auto compare_one = [](
      const windowsapp::WindowsShrinkWorkPathObservation& left,
      const windowsapp::WindowsShrinkWorkPathObservation& right,
      const std::wstring_view role) -> clonecore::Status {
    if (!same_path(left.canonical_path, right.canonical_path) ||
        left.local_volume != right.local_volume ||
        left.parent_is_reparse != right.parent_is_reparse) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          std::wstring(role) + L"再識別",
          L"最終確認後に作業場所の正規pathまたは属性が変化しました");
    }
    return clonecore::validate_stable_identity(
        left.backing_disk, right.backing_disk, role);
  };

  auto status = compare_one(expected.scratch, observed.scratch, L"scratch");
  if (!status) {
    return status;
  }
  status = compare_one(
      expected.checkpoint, observed.checkpoint, L"checkpoint");
  if (!status) {
    return status;
  }
  if (requested.log_is_ram_only) {
    if (!observed.log.canonical_path.empty()) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"RAM log再識別",
          L"RAM限定logにfilesystem上の実体が現れました");
    }
    return clonecore::success_status();
  }
  return compare_one(expected.log, observed.log, L"log");
}

clonecore::Status validate_active_rescue_work_paths(
    const ActiveRescueMediaStorageObservation& active,
    const windowsapp::WindowsShrinkWorkPaths& paths) {
  if ((active.drive_type != DRIVE_FIXED &&
       active.drive_type != DRIVE_REMOVABLE) ||
      !active.physical_identity.has_value() ||
      !active.marker_identity_from_open_handle ||
      paths.log_is_ram_only) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE縮小復元第三作業媒体",
        L"物理識別済みレスキュー媒体のdata領域にscratch、checkpoint、logの全てが必要です");
  }

  const std::filesystem::path marker(active.marker_path);
  if (!marker.is_absolute() || marker.root_name().wstring().size() != 2U ||
      !same_path(marker.filename().wstring(), L"rescue-media-id.txt") ||
      !same_path(marker.parent_path().filename().wstring(),
                 L"YtecDiskClone") ||
      same_path(marker.root_path().wstring(), L"X:\\")) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"PE縮小復元起動媒体data拘束",
        L"X:、ISO/CD、または正規レスキューdata領域以外は作業場所にできません");
  }

  const auto data = marker.parent_path() / L"data";
  const auto expected_checkpoint =
      data / L"shrink-restore-incomplete.checkpoint";
  const auto expected_log = data / L"shrink-restore.log";
  if (!same_path(paths.scratch_directory, data.wstring()) ||
      !same_path(paths.checkpoint_path, expected_checkpoint.wstring()) ||
      !same_path(paths.log_path, expected_log.wstring())) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE縮小復元起動媒体data拘束",
        L"作業場所が現在起動中のレスキュー媒体data領域と一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_work_relationships(
    const DirectShrinkImageRestoreRequest& request,
    const windowsapp::WindowsShrinkWorkPlacementObservation& observed,
    const clonecore::StableDiskIdentity& active_rescue_disk,
    const clonecore::StableDiskIdentity& target) {
  auto status = windowsapp::
      validate_windows_shrink_work_placement_observation(
          request.image_backing_disk, request.work.paths, observed);
  if (!status) {
    return status;
  }
  status = compare_work_observation(
      request.work.paths, request.work.observation, observed);
  if (!status) {
    return status;
  }
  for (const auto* item : {
           &observed.scratch, &observed.checkpoint, &observed.log}) {
    status = clonecore::validate_stable_identity(
        active_rescue_disk, item->backing_disk, L"PE縮小復元作業媒体");
    if (!status) {
      return status;
    }
    status = reject_same_disk(
        item->backing_disk,
        target,
        L"PE縮小復元作業媒体と復元先の分離",
        L"scratch／checkpointを復元先ディスクへ置くことはできません");
    if (!status) {
      return status;
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_reviewed_target(
    const diskmodel::DiskInfo& target,
    const clonecore::StableDiskIdentity& expected_target,
    const std::optional<clonecore::StableDiskIdentity>&
        reviewed_original_source_target,
    const imageformat::Sha256Digest& expected_layout_hash,
    const imageformat::TsumugiManifest& manifest) {
  auto identity = diskmodel::make_stable_disk_identity(
      target, target.is_system_disk);
  auto layout = imageformat::hash_tsumugi_physical_restore_target_layout_v1(
      target);
  if (!identity || !layout) {
    return clonecore::Status::failure(
        !identity ? identity.error() : layout.error());
  }
  auto status = clonecore::validate_stable_identity(
      expected_target, identity.value(), L"PE縮小復元先");
  if (!status) {
    return status;
  }
  if (layout.value() != expected_layout_hash) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE縮小復元先レイアウト",
        L"最終確認した復元先のパーティション配置と一致しません");
  }
  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(target);
  status = imageformat::validate_tsumugi_physical_restore_target(
      target, target_class, false);
  if (!status) {
    return status;
  }
  if (target.logical_sector_size != 512U ||
      manifest.logical_sector_size != 512U) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE縮小復元論理セクター",
        L"この製品経路はコピー元・復元先とも512-byte logical sectorだけに対応します");
  }
  auto model = imageformat::hash_tsumugi_source_model_v1(target.model);
  auto serial = imageformat::hash_tsumugi_source_serial_v1(
      target.serial_suffix, target.device_instance_id);
  if (!model || !serial) {
    return clonecore::Status::failure(
        !model ? model.error() : serial.error());
  }
  const bool is_original_source =
      model.value() == manifest.source_model_hash &&
      serial.value() == manifest.source_serial_hash;
  if (is_original_source) {
    if (!reviewed_original_source_target.has_value()) {
      return status_failure(
          clonecore::ErrorCode::confirmation_required,
          ERROR_CANCELLED,
          L"PE縮小復元元ディスク再利用確認",
          L"元物理ディスクへ戻す選択を安定識別付きでレビューし直してください");
    }
    status = clonecore::validate_stable_identity(
        expected_target,
        *reviewed_original_source_target,
        L"PE縮小復元レビュー済み元ディスク");
    if (status) {
      status = clonecore::validate_stable_identity(
          identity.value(),
          *reviewed_original_source_target,
          L"PE縮小復元元ディスク最終再識別");
    }
    if (!status) {
      return status;
    }
  } else if (reviewed_original_source_target.has_value()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE縮小復元元ディスクレビュー",
        L"元ディスクとして確認した安定識別と認証済みsource hashが一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_reviewed_layout(
    const imageformat::TsumugiManifest& manifest,
    const diskmodel::DiskInfo& target,
    const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& layout) {
  if (layout.metadata.target_size_bytes != target.size_bytes ||
      layout.metadata.logical_sector_size != 512U ||
      layout.migration.target_size_bytes != target.size_bytes ||
      !layout.migration.source_remains_unchanged ||
      layout.migration.target_partitions.empty() ||
      layout.metadata.invalidation_ranges.empty() ||
      layout.metadata.commit_writes.empty() ||
      (layout.metadata.style == imageformat::PartitionTableStyle::gpt &&
       layout.metadata.staged_writes.empty())) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE縮小復元レビュー済み配置",
        L"レビュー済み最終配置が現在の復元先寸法と一致しません");
  }
  auto bindings = imageformat::make_tsumugi_shrink_payload_bindings_v1(
      manifest, layout);
  return bindings
      ? clonecore::success_status()
      : clonecore::Status::failure(bindings.error());
}

clonecore::Status validate_product_payload_contract(
    const imageformat::TsumugiManifest& manifest,
    const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& layout,
    const bool partial_loss) {
  if (manifest.mode != imageformat::TsumugiManifestMode::shrink ||
      manifest.logical_sector_size != 512U || partial_loss) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE縮小復元製品対応範囲",
        L"完全な512-byte logical sectorの縮小イメージだけを復元できます");
  }
  auto bindings = imageformat::make_tsumugi_shrink_payload_bindings_v1(
      manifest, layout);
  if (!bindings) {
    return clonecore::Status::failure(bindings.error());
  }

  std::size_t selected_count{};
  std::size_t ntfs_wim_count{};
  for (const auto& partition : manifest.partitions) {
    if (!selected(partition)) {
      continue;
    }
    ++selected_count;
    const auto binding = std::find_if(
        bindings.value().begin(),
        bindings.value().end(),
        [&](const auto& item) {
          return item.source_table_index == partition.source_table_index;
        });
    if (binding == bindings.value().end()) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"PE縮小復元payload binding",
          L"選択済みpayloadをレビュー済み最終区画へ一意に対応付けできません");
    }

    using Disposition = imageformat::TsumugiShrinkPayloadDispositionV1;
    using Encoding = imageformat::TsumugiManifestPayloadEncoding;
    using FileSystem = imageformat::TsumugiManifestFileSystem;
    if (binding->disposition == Disposition::regenerate_efi_system ||
        binding->disposition == Disposition::regenerate_microsoft_reserved) {
      continue;
    }
    if (binding->disposition ==
        Disposition::replace_bios_system_with_uefi) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"PE縮小復元形式変換",
          L"この経路はパーティション形式を保持し、MBR→GPT変換は別導線で実行します");
    }

    const bool ntfs_wim =
        partition.file_system == FileSystem::ntfs &&
        partition.payload_encoding ==
            Encoding::microsoft_wim_single_image &&
        partition.payload_format_version ==
            imageformat::kTsumugiWimPayloadFormatVersion &&
        partition.cluster_size != 0U;
    const bool static_raw =
        (partition.file_system == FileSystem::unknown ||
         partition.file_system == FileSystem::none) &&
        partition.payload_encoding == Encoding::exact_raw &&
        manifest.logical_sector_size == layout.metadata.logical_sector_size &&
        ((manifest.partition_style ==
              imageformat::TsumugiManifestPartitionStyle::gpt &&
          layout.metadata.style == imageformat::PartitionTableStyle::gpt) ||
         (manifest.partition_style ==
              imageformat::TsumugiManifestPartitionStyle::mbr &&
          layout.metadata.style == imageformat::PartitionTableStyle::mbr));
    if (binding->disposition == Disposition::recreate_empty_file_system) {
      if (!ntfs_wim || partition.used_bytes != 0U) {
        return status_failure(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"PE縮小復元空filesystem",
            L"空filesystem再作成はused=0のNTFS WIMだけに限定します");
      }
      ++ntfs_wim_count;
      continue;
    }
    if (binding->disposition != Disposition::restore_to_reviewed_partition ||
        (!ntfs_wim && !static_raw)) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"PE縮小復元payload形式",
          L"NTFS single-image WIMまたは同一形式・同一512セクターの静的exact RAW以外は開始しません");
    }
    if (ntfs_wim) {
      ++ntfs_wim_count;
    }
  }
  if (selected_count == 0U || ntfs_wim_count == 0U ||
      bindings.value().size() != selected_count) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE縮小復元選択payload",
        L"縮小復元には一つ以上の選択済みNTFS WIM payloadが必要です");
  }
  return clonecore::success_status();
}

clonecore::Result<clonecore::StableDiskIdentity> identity_for_local_path(
    const std::wstring& path) {
  auto before = diskmodel::query_single_disk_number_for_local_path(path);
  if (!before) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        before.error());
  }
  auto provider = diskmodel::make_windows_disk_inventory_provider();
  if (!provider) {
    return failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"PE縮小復元イメージ保存元再識別",
        L"ディスク一覧プロバイダーを作成できませんでした");
  }
  auto inventory = provider->enumerate();
  if (!inventory) {
    return clonecore::Result<clonecore::StableDiskIdentity>::failure(
        inventory.error());
  }
  auto after = diskmodel::query_single_disk_number_for_local_path(path);
  if (!after || before.value() != after.value()) {
    return failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE縮小復元イメージ保存元再識別",
        L"ディスク列挙の前後でイメージ保存元の物理ディスクが変化しました");
  }
  const auto count = static_cast<std::size_t>(std::count_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [&](const auto& disk) {
        return disk.disk_number == before.value();
      }));
  if (count != 1U) {
    return failure<clonecore::StableDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        count == 0U ? ERROR_NOT_FOUND : ERROR_DUP_NAME,
        L"PE縮小復元イメージ保存元再識別",
        L"イメージ保存元を現在の物理ディスク一覧へ一意に対応付けできません");
  }
  const auto found = std::find_if(
      inventory.value().disks.begin(),
      inventory.value().disks.end(),
      [&](const auto& disk) {
        return disk.disk_number == before.value();
      });
  return diskmodel::make_stable_disk_identity(*found, found->is_system_disk);
}

clonecore::Result<DirectShrinkImageRestoreReport>
execute_reviewed_restore_with_windows_apis(
    const DirectShrinkImageRestoreRequest& request,
    const diskmodel::ReidentifiedPhysicalTarget& initially_observed_target,
    const clonecore::StableDiskIdentity& initially_observed_image,
    const windowsapp::WindowsShrinkWorkPlacementObservation&
        initially_observed_work) {
  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(
          initially_observed_target.target);
  auto identity_hash =
      imageformat::hash_tsumugi_physical_restore_target_identity_v1(
          initially_observed_target.target_identity);
  if (!identity_hash) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        identity_hash.error());
  }
  imageformat::Sha256Digest connection_token{};
  if (target_class.usb_attached) {
    auto token = imageformat::
        make_tsumugi_connection_instance_hash_with_windows_apis();
    if (!token) {
      return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
          token.error());
    }
    connection_token = token.take_value();
  }
  imageformat::TsumugiWholeDiskRestoreTarget target{
      .disk = {
          .stable_identity_hash = identity_hash.take_value(),
          .disk_size = initially_observed_target.target.size_bytes,
          .logical_sector_size =
              initially_observed_target.target.logical_sector_size,
          .is_running_windows_system_disk =
              initially_observed_target.target.is_system_disk,
          .is_usb_attached = target_class.usb_attached,
          .is_usb_memory = target_class.usb_memory,
          .is_active_rescue_media = false,
          .is_dynamic_disk = target_class.dynamic_disk,
          .is_storage_spaces = target_class.storage_spaces,
          .is_windows_software_raid = target_class.software_raid,
          .has_unresolved_hardware_raid =
              target_class.unresolved_hardware_raid,
          .connection_instance_hash = connection_token,
      },
      .reviewed_shrink_layout = request.reviewed_layout,
  };
  imageformat::TsumugiRestorePlanRequest plan_request{
      .image = request.image,
      .host = imageformat::TsumugiRestoreHost::winpe,
      .target = std::move(target),
  };
  auto prepared = imageformat::prepare_tsumugi_restore_plan_v1(
      plan_request, request.callbacks);
  if (!prepared) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        prepared.error());
  }
  if (prepared.value().image().container.global_hash !=
          request.expected_image_global_hash ||
      prepared.value().image().manifest.source_state_hash !=
          request.expected_source_state_hash ||
      prepared.value().image().manifest.mode !=
          imageformat::TsumugiManifestMode::shrink ||
      prepared.value().image().partial_loss) {
    return failure<DirectShrinkImageRestoreReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"PE縮小復元イメージ完全再検証",
        L"レビュー後にイメージ内容、source state、または完全性が変化しました");
  }

  auto current_image = identify_direct_shrink_image_backing_with_windows_apis(
      request.image.image_path);
  if (!current_image ||
      !stable_identity_matches(
          initially_observed_image,
          current_image.value(),
          L"PE縮小復元イメージ保存元")) {
    return current_image
        ? failure<DirectShrinkImageRestoreReport>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"PE縮小復元イメージ保存元再確認",
              L"完全再検証後にイメージ保存元ディスクが変化しました")
        : clonecore::Result<DirectShrinkImageRestoreReport>::failure(
              current_image.error());
  }
  auto active = query_active_rescue_media_storage_with_windows_apis();
  if (!active || !active.value().physical_identity.has_value() ||
      !active.value().marker_identity_from_open_handle ||
      !stable_identity_matches(
          request.work.active_rescue_disk,
          *active.value().physical_identity,
          L"PE縮小復元起動媒体")) {
    return active
        ? failure<DirectShrinkImageRestoreReport>(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_DEVICE_NOT_CONNECTED,
              L"PE縮小復元起動媒体再確認",
              L"完全再検証後に起動媒体を同じ物理ディスクへ拘束できません")
        : clonecore::Result<DirectShrinkImageRestoreReport>::failure(
              active.error());
  }
  auto active_work_status = validate_active_rescue_work_paths(
      active.value(), request.work.paths);
  if (!active_work_status) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        active_work_status.error());
  }
  auto current_work = windowsapp::
      observe_windows_shrink_work_placement_with_windows_apis(
          request.work.paths);
  if (!current_work) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        current_work.error());
  }
  auto work_status = validate_work_relationships(
      request,
      current_work.value(),
      *active.value().physical_identity,
      initially_observed_target.target_identity);
  if (!work_status ||
      !compare_work_observation(
          request.work.paths,
          initially_observed_work,
          current_work.value())) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        !work_status
            ? work_status.error()
            : compare_work_observation(
                  request.work.paths,
                  initially_observed_work,
                  current_work.value()).error());
  }

  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  if (!inventory) {
    return failure<DirectShrinkImageRestoreReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"PE縮小復元先最終再識別",
        L"ディスク一覧プロバイダーを作成できませんでした");
  }
  auto final_target = diskmodel::reidentify_physical_target(
      request.expected_target, request.confirmation, *inventory);
  if (!final_target) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        final_target.error());
  }
  auto target_status = validate_reviewed_target(
      final_target.value().target,
      request.expected_target,
      request.reviewed_original_source_target,
      request.expected_target_layout_hash,
      request.reviewed_manifest);
  if (!target_status) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        target_status.error());
  }

  auto platform = windowsapp::make_windows_tsumugi_shrink_restore_platform({
      .expected_target = request.expected_target,
      .confirmation = request.confirmation,
      .expected_target_layout_hash = request.expected_target_layout_hash,
      .target_is_active_rescue_media = false,
      .callbacks = request.callbacks,
  });
  if (!platform) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        platform.error());
  }
  auto transaction =
      windowsapp::make_windows_tsumugi_shrink_restore_transaction(
          current_image.value(),
          request.work.paths,
          current_work.value(),
          request.reviewed_layout,
          platform.take_value());
  if (!transaction) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        transaction.error());
  }
  auto restored = imageformat::execute_tsumugi_shrink_restore_plan_v1(
      prepared.value(),
      request.image.password,
      *transaction.value(),
      request.callbacks);
  if (!restored) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        restored.error());
  }
  return clonecore::Result<DirectShrinkImageRestoreReport>::success({
      .restore = restored.take_value(),
      .image_completely_reverified = true,
      .image_backing_reidentified_before_write = true,
      .active_rescue_media_checked = true,
      .target_reidentified_before_plan = true,
      .work_placement_reidentified_before_write = true,
      .target_left_offline = true,
      .direct_execution_only = true,
      .boot_repair_offer_required =
          request.reviewed_layout.migration.boot_finalization_required,
  });
}

clonecore::Status validate_complete_report(
    const DirectShrinkImageRestoreReport& report,
    const bool expected_boot_repair_offer) {
  if (!report.image_completely_reverified ||
      !report.image_backing_reidentified_before_write ||
      !report.active_rescue_media_checked ||
      !report.target_reidentified_before_plan ||
      !report.work_placement_reidentified_before_write ||
      !report.target_left_offline || !report.direct_execution_only ||
      !report.restore.callbacks_started_after_complete_verification ||
      !report.restore.image_matched_prepared_plan ||
      !report.restore.target_reidentified_before_write ||
      !report.restore.all_payloads_verified_by_adapter ||
      !report.restore.final_layout_committed ||
      report.boot_repair_offer_required != expected_boot_repair_offer) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE縮小復元完了証跡",
        L"完全再検証、安定再識別、全書込み読戻し、起動修復判定、または最終commitの証跡が不足しています");
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<clonecore::StableDiskIdentity>
identify_direct_shrink_image_backing_with_windows_apis(
    const std::wstring& image_path) {
  return identity_for_local_path(image_path);
}

clonecore::Result<DirectShrinkImageRestoreWorkReview>
review_direct_shrink_active_rescue_work_with_windows_apis(
    const clonecore::StableDiskIdentity& image_backing_disk,
    const clonecore::StableDiskIdentity& expected_target) {
  auto active = query_active_rescue_media_storage_with_windows_apis();
  if (!active) {
    return clonecore::Result<DirectShrinkImageRestoreWorkReview>::failure(
        active.error());
  }
  if (!active.value().physical_identity.has_value() ||
      !active.value().marker_identity_from_open_handle) {
    return failure<DirectShrinkImageRestoreWorkReview>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE縮小復元第三作業媒体",
        L"ISO/CDまたは物理識別できない起動媒体には縮小復元の作業領域を置けません");
  }
  auto status = reject_same_disk(
      image_backing_disk,
      *active.value().physical_identity,
      L"PE縮小復元イメージと作業媒体の分離",
      L"イメージ保存元と作業媒体には別の物理ディスクが必要です");
  if (status) {
    status = reject_same_disk(
        expected_target,
        *active.value().physical_identity,
        L"PE縮小復元先と作業媒体の分離",
        L"起動中レスキュー媒体は復元先にできません");
  }
  if (status) {
    status = reject_same_disk(
        image_backing_disk,
        expected_target,
        L"PE縮小復元イメージと復元先の分離",
        L"イメージを保持するディスクは復元先にできません");
  }
  if (!status) {
    return clonecore::Result<DirectShrinkImageRestoreWorkReview>::failure(
        status.error());
  }

  const std::filesystem::path marker(active.value().marker_path);
  const std::filesystem::path data = marker.parent_path() / L"data";
  windowsapp::WindowsShrinkWorkPaths paths{
      .scratch_directory = data.wstring(),
      .checkpoint_path =
          (data / L"shrink-restore-incomplete.checkpoint").wstring(),
      .log_path = (data / L"shrink-restore.log").wstring(),
      .log_is_ram_only = false,
  };
  status = validate_active_rescue_work_paths(active.value(), paths);
  if (!status) {
    return clonecore::Result<DirectShrinkImageRestoreWorkReview>::failure(
        status.error());
  }
  auto observed = windowsapp::
      observe_windows_shrink_work_placement_with_windows_apis(paths);
  if (!observed) {
    return clonecore::Result<DirectShrinkImageRestoreWorkReview>::failure(
        observed.error());
  }
  status = windowsapp::validate_windows_shrink_work_placement_observation(
      image_backing_disk, paths, observed.value());
  if (status) {
    for (const auto* item : {
             &observed.value().scratch,
             &observed.value().checkpoint,
             &observed.value().log}) {
      status = clonecore::validate_stable_identity(
          *active.value().physical_identity,
          item->backing_disk,
          L"PE縮小復元作業媒体");
      if (!status) {
        break;
      }
    }
  }
  auto active_again = query_active_rescue_media_storage_with_windows_apis();
  if (status &&
      (!active_again ||
       !active_again.value().physical_identity.has_value() ||
       !active_again.value().marker_identity_from_open_handle ||
       active_again.value().drive_type != active.value().drive_type ||
       !same_path(
           active_again.value().marker_path,
           active.value().marker_path) ||
       !stable_identity_matches(
           *active.value().physical_identity,
           *active_again.value().physical_identity,
           L"PE縮小復元作業媒体再確認"))) {
    if (!active_again) {
      return clonecore::Result<DirectShrinkImageRestoreWorkReview>::failure(
          active_again.error());
    }
    status = status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE縮小復元作業媒体再確認",
        L"作業場所の確認中に起動媒体の物理識別が変化しました");
  }
  if (status) {
    status = validate_active_rescue_work_paths(active_again.value(), paths);
  }
  if (!status) {
    return clonecore::Result<DirectShrinkImageRestoreWorkReview>::failure(
        status.error());
  }
  return clonecore::Result<DirectShrinkImageRestoreWorkReview>::success({
      .paths = std::move(paths),
      .observation = observed.take_value(),
      .active_rescue_disk = *active_again.value().physical_identity,
  });
}

clonecore::Result<imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
make_direct_shrink_image_restore_layout_with_windows_apis(
    const imageformat::TsumugiManifest& manifest,
    const diskmodel::DiskInfo& target,
    const clonecore::StableDiskIdentity& expected_target,
    const std::optional<clonecore::StableDiskIdentity>&
        reviewed_original_source_target) {
  if (manifest.mode != imageformat::TsumugiManifestMode::shrink ||
      manifest.logical_sector_size != 512U ||
      target.logical_sector_size != 512U) {
    return failure<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE縮小復元配置作成",
        L"完全な512-byte logical sectorの縮小イメージと復元先が必要です");
  }
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(target);
  if (!target_layout) {
    return clonecore::Result<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        target_layout.error());
  }
  auto status = validate_reviewed_target(
      target,
      expected_target,
      reviewed_original_source_target,
      target_layout.value(),
      manifest);
  if (!status) {
    return clonecore::Result<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        status.error());
  }
  std::vector<std::uint32_t> disallowed_signatures;
  if (manifest.partition_style ==
      imageformat::TsumugiManifestPartitionStyle::mbr) {
    auto signatures =
        imageformat::collect_tsumugi_mbr_signatures_with_windows_apis(
            expected_target);
    if (!signatures) {
      return clonecore::Result<
          imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
          signatures.error());
    }
    disallowed_signatures = signatures.take_value();
  }
  auto guid_generator = clonecore::make_windows_guid_generator();
  auto signature_generator = clonecore::make_windows_mbr_signature_generator();
  if (!guid_generator || !signature_generator) {
    return failure<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"PE縮小復元識別子生成",
        L"最終レイアウト用識別子生成器を作成できません");
  }
  auto layout =
      imageformat::make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
          manifest,
          target.size_bytes,
          target.logical_sector_size,
          manifest.partition_style,
          true,
          *guid_generator,
          *signature_generator,
          disallowed_signatures);
  if (!layout) {
    return layout;
  }
  status = validate_product_payload_contract(manifest, layout.value(), false);
  return status
      ? std::move(layout)
      : clonecore::Result<
            imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
            status.error());
}

clonecore::Result<DirectShrinkImageRestoreReport>
execute_direct_shrink_image_restore(
    const DirectShrinkImageRestoreRequest& request,
    const DirectShrinkImageRestoreDependencies& dependencies) {
  if (!request.administrator ||
      !request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token != L"OK") {
    return failure<DirectShrinkImageRestoreReport>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"PE縮小Tsumugi直接復元確認",
        L"管理者権限、消去内容の確認、または大文字OKが不足しています");
  }
  if (!dependencies.query_active_storage ||
      !dependencies.identify_image_backing ||
      !dependencies.reidentify_target ||
      !dependencies.observe_work_placement ||
      !dependencies.execute_reviewed_restore) {
    return failure<DirectShrinkImageRestoreReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_FUNCTION,
        L"PE縮小Tsumugi直接復元依存",
        L"読取り専用再識別または専用復元Adapterが不足しています");
  }
  if (request.image.image_path.empty() ||
      (request.image.storage_file_system !=
           imageformat::TsumugiImageStorageFileSystem::ntfs &&
       request.image.storage_file_system !=
           imageformat::TsumugiImageStorageFileSystem::exfat) ||
      all_zero(request.expected_image_global_hash) ||
      all_zero(request.expected_source_state_hash) ||
      request.reviewed_manifest.source_state_hash !=
          request.expected_source_state_hash) {
    return failure<DirectShrinkImageRestoreReport>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE縮小Tsumugi直接復元レビュー",
        L"完全検証済みイメージのpath、filesystem、またはhash証跡が不足しています");
  }
  auto status = validate_product_payload_contract(
      request.reviewed_manifest,
      request.reviewed_layout,
      request.reviewed_image_partial_loss);
  if (status) {
    status = validate_reviewed_layout(
        request.reviewed_manifest,
        request.reviewed_target,
        request.reviewed_layout);
  }
  if (status) {
    status = validate_reviewed_target(
        request.reviewed_target,
        request.expected_target,
        request.reviewed_original_source_target,
        request.expected_target_layout_hash,
        request.reviewed_manifest);
  }
  if (!status) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        status.error());
  }

  auto active = dependencies.query_active_storage();
  if (!active || !active.value().physical_identity.has_value() ||
      !active.value().marker_identity_from_open_handle) {
    return active
        ? failure<DirectShrinkImageRestoreReport>(
              clonecore::ErrorCode::unsupported_layout,
              ERROR_NOT_SUPPORTED,
              L"PE縮小復元起動媒体",
              L"物理識別済みレスキュー媒体のdata領域が必要です")
        : clonecore::Result<DirectShrinkImageRestoreReport>::failure(
              active.error());
  }
  status = validate_active_rescue_work_paths(
      active.value(), request.work.paths);
  if (status) {
    status = clonecore::validate_stable_identity(
      request.work.active_rescue_disk,
      *active.value().physical_identity,
      L"PE縮小復元起動媒体");
  }
  if (status) {
    status = reject_same_disk(
        request.image_backing_disk,
        request.expected_target,
        L"PE縮小復元イメージと復元先の分離",
        L"イメージを保持するディスクは復元先にできません");
  }
  if (status) {
    status = reject_same_disk(
        request.image_backing_disk,
        *active.value().physical_identity,
        L"PE縮小復元イメージと作業媒体の分離",
        L"イメージ保存元と作業媒体には別の物理ディスクが必要です");
  }
  if (status) {
    status = reject_same_disk(
        request.expected_target,
        *active.value().physical_identity,
        L"PE縮小復元先と作業媒体の分離",
        L"起動中レスキュー媒体は復元先にできません");
  }
  if (!status) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        status.error());
  }

  auto current_image = dependencies.identify_image_backing(
      request.image.image_path);
  if (!current_image) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        current_image.error());
  }
  status = clonecore::validate_stable_identity(
      request.image_backing_disk,
      current_image.value(),
      L"PE縮小復元イメージ保存元");
  if (!status) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        status.error());
  }
  auto current_work = dependencies.observe_work_placement(request.work.paths);
  if (!current_work) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        current_work.error());
  }
  status = validate_work_relationships(
      request,
      current_work.value(),
      *active.value().physical_identity,
      request.expected_target);
  if (!status) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        status.error());
  }
  auto target = dependencies.reidentify_target(
      request.expected_target, request.confirmation);
  if (!target) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        target.error());
  }
  status = validate_reviewed_target(
      target.value().target,
      request.expected_target,
      request.reviewed_original_source_target,
      request.expected_target_layout_hash,
      request.reviewed_manifest);
  if (!status) {
    return clonecore::Result<DirectShrinkImageRestoreReport>::failure(
        status.error());
  }

  auto restored = dependencies.execute_reviewed_restore(
      request,
      target.value(),
      current_image.value(),
      current_work.value());
  if (!restored) {
    return restored;
  }
  status = validate_complete_report(
      restored.value(),
      request.reviewed_layout.migration.boot_finalization_required);
  return status
      ? restored
      : clonecore::Result<DirectShrinkImageRestoreReport>::failure(
            status.error());
}

DirectShrinkImageRestoreDependencies
make_direct_shrink_image_restore_windows_dependencies() {
  return {
      .query_active_storage =
          query_active_rescue_media_storage_with_windows_apis,
      .identify_image_backing =
          identify_direct_shrink_image_backing_with_windows_apis,
      .reidentify_target =
          [](const clonecore::StableDiskIdentity& target,
             const clonecore::TargetConfirmation& confirmation) {
            auto inventory = diskmodel::make_windows_disk_inventory_provider();
            if (!inventory) {
              return failure<diskmodel::ReidentifiedPhysicalTarget>(
                  clonecore::ErrorCode::internal_error,
                  ERROR_NOT_ENOUGH_MEMORY,
                  L"PE縮小復元先再識別",
                  L"ディスク一覧プロバイダーを作成できませんでした");
            }
            return diskmodel::reidentify_physical_target(
                target, confirmation, *inventory);
          },
      .observe_work_placement =
          windowsapp::observe_windows_shrink_work_placement_with_windows_apis,
      .execute_reviewed_restore = execute_reviewed_restore_with_windows_apis,
  };
}

clonecore::Result<DirectShrinkImageRestoreReport>
execute_direct_shrink_image_restore_with_windows_apis(
    const DirectShrinkImageRestoreRequest& request) {
  return execute_direct_shrink_image_restore(
      request, make_direct_shrink_image_restore_windows_dependencies());
}

}  // namespace ytec::winpeapp

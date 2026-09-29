#include "ytec/windowsapp/online_shrink_image_restore.h"

#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"
#include "ytec/windowsapp/online_shrink_image_product.h"
#include "ytec/windowsapp/shrink_restore_transaction.h"
#include "ytec/windowsapp/windows_data_rescue_clone.h"
#include "ytec/windowsapp/windows_shrink_restore_platform.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ytec::windowsapp {
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

bool all_zero(const imageformat::Sha256Digest& digest) noexcept {
  return std::all_of(digest.begin(), digest.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

bool manifest_contains_windows(
    const imageformat::TsumugiManifest& manifest) noexcept {
  return (static_cast<std::uint32_t>(manifest.flags) &
          static_cast<std::uint32_t>(
              imageformat::TsumugiManifestFlags::source_contains_windows)) !=
         0U;
}

void append_u8(std::vector<std::byte>& output, const std::uint8_t value) {
  output.push_back(static_cast<std::byte>(value));
}

void append_u32(
    std::vector<std::byte>& output,
    const std::uint32_t value) {
  const auto previous = output.size();
  output.resize(previous + sizeof(value));
  std::memcpy(output.data() + previous, &value, sizeof(value));
}

void append_u64(
    std::vector<std::byte>& output,
    const std::uint64_t value) {
  const auto previous = output.size();
  output.resize(previous + sizeof(value));
  std::memcpy(output.data() + previous, &value, sizeof(value));
}

void append_bytes(
    std::vector<std::byte>& output,
    const std::span<const std::byte> bytes) {
  append_u64(output, static_cast<std::uint64_t>(bytes.size()));
  output.insert(output.end(), bytes.begin(), bytes.end());
}

void append_wstring(
    std::vector<std::byte>& output,
    const std::wstring_view value) {
  append_u64(output, static_cast<std::uint64_t>(value.size()));
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  output.insert(output.end(), begin, begin + value.size() * sizeof(wchar_t));
}

clonecore::Result<std::uint64_t> planned_payload_bytes(
    const imageformat::TsumugiManifest& manifest) {
  std::uint64_t total{};
  for (const auto& partition : manifest.partitions) {
    const auto selected =
        (static_cast<std::uint32_t>(partition.flags) &
         static_cast<std::uint32_t>(
             imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
    if (!selected) {
      continue;
    }
    if (partition.payload_logical_length == 0U ||
        total > (std::numeric_limits<std::uint64_t>::max)() -
                    partition.payload_logical_length) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Windows縮小復元操作量",
          L"選択済みpayloadの長さが0または合計がオーバーフローします");
    }
    total += partition.payload_logical_length;
  }
  if (total == 0U) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows縮小復元操作量",
        L"縮小復元対象として選択されたpayloadがありません");
  }
  return clonecore::Result<std::uint64_t>::success(total);
}

clonecore::Status reject_same_disk(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right,
    std::wstring operation,
    std::wstring message) {
  if (clonecore::validate_stable_identity(left, right, L"物理ディスク")) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        std::move(operation),
        std::move(message)));
  }
  return clonecore::success_status();
}

clonecore::Status validate_work_against_target(
    const WindowsShrinkWorkPaths& paths,
    const WindowsShrinkWorkPlacementObservation& observed,
    const clonecore::StableDiskIdentity& target) {
  const auto scratch = reject_same_disk(
      observed.scratch.backing_disk,
      target,
      L"Windows縮小復元scratch分離",
      L"scratchを復元先ディスクへ置くことはできません");
  if (!scratch) {
    return scratch;
  }
  const auto checkpoint = reject_same_disk(
      observed.checkpoint.backing_disk,
      target,
      L"Windows縮小復元checkpoint分離",
      L"checkpointを復元先ディスクへ置くことはできません");
  if (!checkpoint) {
    return checkpoint;
  }
  if (!paths.log_is_ram_only) {
    const auto log = reject_same_disk(
        observed.log.backing_disk,
        target,
        L"Windows縮小復元log分離",
        L"logを復元先ディスクへ置くことはできません");
    if (!log) {
      return log;
    }
  }
  return clonecore::success_status();
}

clonecore::Status compare_work_observation(
    const WindowsShrinkWorkPaths& requested,
    const WindowsShrinkWorkPlacementObservation& expected,
    const WindowsShrinkWorkPlacementObservation& observed) {
  const auto compare_one = [](
      const WindowsShrinkWorkPathObservation& left,
      const WindowsShrinkWorkPathObservation& right,
      const std::wstring_view role) -> clonecore::Status {
    if (left.canonical_path != right.canonical_path ||
        left.local_volume != right.local_volume ||
        left.parent_is_reparse != right.parent_is_reparse) {
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          std::wstring(role) + L"再識別",
          L"最終確認後に作業場所の正規pathまたは属性が変化しました"));
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
      return clonecore::Status::failure(restore_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"RAM log再識別",
          L"RAM限定logにfilesystem上の実体が現れました"));
    }
    return clonecore::success_status();
  }
  return compare_one(expected.log, observed.log, L"log");
}

clonecore::Status validate_reviewed_target(
    const diskmodel::DiskInfo& target,
    const clonecore::StableDiskIdentity& expected_target,
    const imageformat::Sha256Digest& expected_layout_hash,
    const imageformat::TsumugiManifest& manifest,
    const bool protected_rescue_media) {
  auto identity = diskmodel::make_stable_disk_identity(
      target, target.is_system_disk);
  auto layout = imageformat::hash_tsumugi_physical_restore_target_layout_v1(
      target);
  if (!identity || !layout) {
    return clonecore::Status::failure(
        !identity ? identity.error() : layout.error());
  }
  auto status = clonecore::validate_stable_identity(
      expected_target, identity.value(), L"縮小復元先");
  if (!status) {
    return status;
  }
  if (layout.value() != expected_layout_hash) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows縮小復元先レイアウト",
        L"最終確認した復元先のパーティション配置と一致しません"));
  }
  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(target);
  status = imageformat::validate_tsumugi_physical_restore_target(
      target, target_class, protected_rescue_media);
  if (!status) {
    return status;
  }
  if (target.is_system_disk) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"Windows縮小復元先システム保護",
        L"稼働中Windows自身のシステムディスクは復元先にできません"));
  }
  auto model = imageformat::hash_tsumugi_source_model_v1(target.model);
  auto serial = imageformat::hash_tsumugi_source_serial_v1(
      target.serial_suffix, target.device_instance_id);
  if (!model || !serial) {
    return clonecore::Status::failure(
        !model ? model.error() : serial.error());
  }
  if (model.value() == manifest.source_model_hash &&
      serial.value() == manifest.source_serial_hash) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"Windows縮小復元元ディスク保護",
        L"イメージを作成した元ディスクと同じ対象には復元できません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_reviewed_layout(
    const imageformat::TsumugiManifest& manifest,
    const diskmodel::DiskInfo& target,
    const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1& layout) {
  if (layout.metadata.target_size_bytes != target.size_bytes ||
      layout.metadata.logical_sector_size != target.logical_sector_size ||
      layout.migration.target_size_bytes != target.size_bytes ||
      layout.migration.target_partitions.empty() ||
      layout.metadata.invalidation_ranges.empty() ||
      layout.metadata.commit_writes.empty() ||
      (layout.metadata.style == imageformat::PartitionTableStyle::gpt &&
       layout.metadata.staged_writes.empty())) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows縮小復元レビュー済み配置",
        L"レビュー済み最終配置が現在の復元先寸法と一致しません"));
  }
  const auto metadata_is_gpt =
      layout.metadata.style == imageformat::PartitionTableStyle::gpt;
  const auto migration_is_gpt =
      layout.migration.target_style ==
      migrationcore::MigrationPartitionStyle::gpt;
  if (metadata_is_gpt != migration_is_gpt) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows縮小復元形式拘束",
        L"最終metadataと縮小移行計画のパーティション形式が一致しません"));
  }
  auto bindings = imageformat::make_tsumugi_shrink_payload_bindings_v1(
      manifest, layout);
  if (!bindings) {
    return clonecore::Status::failure(bindings.error());
  }
  return clonecore::success_status();
}

clonecore::Status append_identity_hash(
    std::vector<std::byte>& bytes,
    const clonecore::StableDiskIdentity& identity) {
  const auto digest =
      imageformat::hash_tsumugi_physical_restore_target_identity_v1(identity);
  if (!digest) {
    return clonecore::Status::failure(digest.error());
  }
  append_bytes(bytes, digest.value());
  return clonecore::success_status();
}

clonecore::Result<operationcore::Sha256Digest> immutable_payload_hash(
    const WindowsOnlineShrinkRestoreRequest& request,
    const std::uint64_t expected_work_bytes) {
  constexpr std::string_view domain =
      "YTEC-WINDOWS-TSUMUGI-SHRINK-RESTORE-PLAN-V1";
  std::vector<std::byte> bytes;
  bytes.reserve(1024U);
  append_bytes(
      bytes,
      std::span<const std::byte>(
          reinterpret_cast<const std::byte*>(domain.data()), domain.size()));
  append_bytes(bytes, request.reviewed_image.global_hash);
  append_bytes(bytes, request.reviewed_image.manifest.source_state_hash);
  append_bytes(bytes, request.expected_target_layout_hash);
  append_u64(bytes, request.reviewed_image.image_length);
  append_u64(bytes, expected_work_bytes);
  append_u32(
      bytes,
      static_cast<std::uint32_t>(request.reviewed_image.storage_file_system));
  append_u32(
      bytes,
      static_cast<std::uint32_t>(
          request.reviewed_layout.metadata.style));
  append_u64(bytes, request.reviewed_layout.metadata.target_size_bytes);
  append_u32(bytes, request.reviewed_layout.metadata.logical_sector_size);
  append_u64(bytes, request.reviewed_layout.migration.minimum_target_size_bytes);
  append_u64(bytes, request.reviewed_layout.migration.unallocated_tail_bytes);
  append_u8(
      bytes,
      request.reviewed_layout.migration.boot_finalization_required ? 1U : 0U);

  for (const auto& partition :
       request.reviewed_layout.migration.target_partitions) {
    append_u32(bytes, partition.target_number);
    append_u8(bytes, partition.source_table_index.has_value() ? 1U : 0U);
    append_u32(bytes, partition.source_table_index.value_or(0U));
    append_u32(bytes, static_cast<std::uint32_t>(partition.role));
    append_u32(bytes, static_cast<std::uint32_t>(partition.file_system));
    append_u32(bytes, static_cast<std::uint32_t>(partition.action));
    append_u64(bytes, partition.offset_bytes);
    append_u64(bytes, partition.size_bytes);
    append_u64(bytes, partition.source_size_bytes);
    append_u64(bytes, partition.source_used_bytes);
    append_wstring(bytes, partition.label);
    append_u8(bytes, partition.active ? 1U : 0U);
  }
  for (const auto& write : request.reviewed_layout.metadata.staged_writes) {
    append_u32(bytes, static_cast<std::uint32_t>(write.kind));
    append_u64(bytes, write.offset);
    append_bytes(bytes, write.bytes);
  }
  for (const auto& write : request.reviewed_layout.metadata.commit_writes) {
    append_u32(bytes, static_cast<std::uint32_t>(write.kind));
    append_u64(bytes, write.offset);
    append_bytes(bytes, write.bytes);
  }

  const auto append_work = [&](const WindowsShrinkWorkPathObservation& item) {
    clonecore::Status status = clonecore::success_status();
    append_wstring(bytes, item.canonical_path);
    append_u8(bytes, item.local_volume ? 1U : 0U);
    append_u8(bytes, item.parent_is_reparse ? 1U : 0U);
    status = append_identity_hash(bytes, item.backing_disk);
    return status;
  };
  auto identity_status = append_work(request.observed_work.scratch);
  if (identity_status) {
    identity_status = append_work(request.observed_work.checkpoint);
  }
  append_u8(bytes, request.work_paths.log_is_ram_only ? 1U : 0U);
  if (identity_status && !request.work_paths.log_is_ram_only) {
    identity_status = append_work(request.observed_work.log);
  }
  if (identity_status) {
    identity_status = append_identity_hash(bytes, request.image_backing_disk);
  }
  if (identity_status) {
    identity_status = append_identity_hash(bytes, request.expected_target);
  }
  if (!identity_status) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        identity_status.error());
  }
  return imageformat::sha256(bytes);
}

clonecore::Result<std::uint64_t> report_processed_bytes(
    const imageformat::TsumugiShrinkRestoreReport& report) {
  std::uint64_t total{};
  for (const auto value : {
           report.archive_logical_bytes,
           report.exact_raw_logical_bytes,
           report.intentionally_omitted_logical_bytes}) {
    if (total > (std::numeric_limits<std::uint64_t>::max)() - value) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Windows縮小復元実績量",
          L"復元実績量がオーバーフローしました");
    }
    total += value;
  }
  return clonecore::Result<std::uint64_t>::success(total);
}

clonecore::Result<operationcore::ReidentifiedOperation>
reidentify_for_operation(
    const operationcore::OperationPlan& plan,
    const WindowsOnlineShrinkRestoreRequest& request,
    const WindowsOnlineShrinkRestoreDependencies& dependencies) {
  if (!plan.target || !dependencies.reidentify_target ||
      !dependencies.is_protected_rescue_media ||
      !dependencies.observe_work_placement) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windows縮小復元Operation再識別",
        L"復元先または必須の読取り専用再識別依存がありません");
  }
  auto observed = dependencies.reidentify_target(
      *plan.target, request.confirmation);
  if (!observed) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        observed.error());
  }
  auto protected_target = dependencies.is_protected_rescue_media(
      observed.value().target_identity);
  if (!protected_target) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        protected_target.error());
  }
  auto target_status = validate_reviewed_target(
      observed.value().target,
      request.expected_target,
      request.expected_target_layout_hash,
      request.reviewed_image.manifest,
      protected_target.value());
  if (!target_status) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        target_status.error());
  }
  auto work = dependencies.observe_work_placement(request.work_paths);
  if (!work) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        work.error());
  }
  auto work_status = validate_windows_shrink_work_placement_observation(
      request.image_backing_disk, request.work_paths, work.value());
  if (work_status) {
    work_status = compare_work_observation(
        request.work_paths, request.observed_work, work.value());
  }
  if (work_status) {
    work_status = validate_work_against_target(
        request.work_paths, work.value(), observed.value().target_identity);
  }
  if (!work_status) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        work_status.error());
  }
  return clonecore::Result<operationcore::ReidentifiedOperation>::success({
      .target = observed.value().target_identity,
  });
}

clonecore::Result<WindowsOnlineShrinkRestoreExecutionReport>
execute_reviewed_restore_with_windows_apis(
    const WindowsOnlineShrinkRestoreRequest& request) {
  auto inventory = diskmodel::make_windows_disk_inventory_provider();
  auto observed = diskmodel::reidentify_physical_target(
      request.expected_target, request.confirmation, *inventory);
  if (!observed) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(observed.error());
  }
  auto protected_target =
      query_windows_data_rescue_protected_target_with_windows_apis(
          observed.value().target_identity);
  if (!protected_target) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(
        protected_target.error());
  }
  auto target_status = validate_reviewed_target(
      observed.value().target,
      request.expected_target,
      request.expected_target_layout_hash,
      request.reviewed_image.manifest,
      protected_target.value());
  if (!target_status) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(
        target_status.error());
  }

  auto current_work =
      observe_windows_shrink_work_placement_with_windows_apis(
          request.work_paths);
  if (!current_work) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(
        current_work.error());
  }
  auto work_status = validate_windows_shrink_work_placement_observation(
      request.image_backing_disk,
      request.work_paths,
      current_work.value());
  if (work_status) {
    work_status = compare_work_observation(
        request.work_paths, request.observed_work, current_work.value());
  }
  if (work_status) {
    work_status = validate_work_against_target(
        request.work_paths,
        current_work.value(),
        observed.value().target_identity);
  }
  if (!work_status) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(
        work_status.error());
  }

  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(
          observed.value().target);
  auto identity_hash =
      imageformat::hash_tsumugi_physical_restore_target_identity_v1(
          observed.value().target_identity);
  if (!identity_hash) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(
        identity_hash.error());
  }
  imageformat::Sha256Digest connection_token{};
  if (target_class.usb_attached) {
    auto token = imageformat::
        make_tsumugi_connection_instance_hash_with_windows_apis();
    if (!token) {
      return clonecore::Result<
          WindowsOnlineShrinkRestoreExecutionReport>::failure(token.error());
    }
    connection_token = token.take_value();
  }

  imageformat::TsumugiWholeDiskRestoreTarget target{
      .disk = {
          .stable_identity_hash = identity_hash.take_value(),
          .disk_size = observed.value().target.size_bytes,
          .logical_sector_size =
              observed.value().target.logical_sector_size,
          .is_running_windows_system_disk =
              observed.value().target.is_system_disk,
          .is_usb_attached = target_class.usb_attached,
          .is_usb_memory = target_class.usb_memory,
          .is_active_rescue_media = protected_target.value(),
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
      .host = imageformat::TsumugiRestoreHost::windows,
      .target = std::move(target),
  };
  auto prepared = imageformat::prepare_tsumugi_restore_plan_v1(
      plan_request, request.callbacks);
  if (!prepared) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(prepared.error());
  }
  if (prepared.value().image().container.global_hash !=
          request.reviewed_image.global_hash ||
      prepared.value().image().manifest.source_state_hash !=
          request.reviewed_image.manifest.source_state_hash ||
      prepared.value().image().manifest.mode !=
          imageformat::TsumugiManifestMode::shrink) {
    return failure<WindowsOnlineShrinkRestoreExecutionReport>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Windows縮小復元イメージ完全再検証",
        L"レビュー後にイメージ内容またはsource stateが変化しました");
  }

  auto platform = make_windows_tsumugi_shrink_restore_platform({
      .expected_target = request.expected_target,
      .confirmation = request.confirmation,
      .expected_target_layout_hash = request.expected_target_layout_hash,
      .target_is_active_rescue_media = protected_target.value(),
      .callbacks = request.callbacks,
  });
  if (!platform) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(platform.error());
  }
  auto transaction = make_windows_tsumugi_shrink_restore_transaction(
      request.image_backing_disk,
      request.work_paths,
      current_work.value(),
      request.reviewed_layout,
      platform.take_value());
  if (!transaction) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(
        transaction.error());
  }
  const bool boot_offer = prepared.value().requires_boot_repair_offer();
  auto restored = imageformat::execute_tsumugi_shrink_restore_plan_v1(
      prepared.value(),
      request.image.password,
      *transaction.value(),
      request.callbacks);
  if (!restored) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreExecutionReport>::failure(restored.error());
  }
  return clonecore::Result<
      WindowsOnlineShrinkRestoreExecutionReport>::success({
      .restore = restored.take_value(),
      .image_completely_reverified = true,
      .target_reidentified_before_plan = true,
      .work_placement_reidentified_before_write = true,
      .target_left_offline = true,
      .boot_repair_offer_required = boot_offer,
  });
}

}  // namespace

clonecore::Result<
    imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
make_windows_online_shrink_restore_layout_with_windows_apis(
    const TsumugiRestoreImagePreflightReport& image,
    const diskmodel::DiskInfo& target,
    const clonecore::StableDiskIdentity& expected_target) {
  if (!image.complete_container_verified ||
      !image.metadata_verified || !image.restore_layout_verified ||
      image.manifest.mode != imageformat::TsumugiManifestMode::shrink) {
    return failure<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows縮小復元配置作成",
        L"完全検証済み縮小イメージが必要です");
  }
  auto expected = diskmodel::make_stable_disk_identity(
      target, target.is_system_disk);
  if (!expected) {
    return clonecore::Result<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        expected.error());
  }
  auto identity = clonecore::validate_stable_identity(
      expected_target, expected.value(), L"縮小復元配置対象");
  if (!identity || target.is_system_disk) {
    return failure<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>(
        !identity ? identity.error().code
                  : clonecore::ErrorCode::unsupported_layout,
        !identity ? identity.error().native_code : ERROR_ACCESS_DENIED,
        L"Windows縮小復元配置対象",
        !identity ? identity.error().message
                  : L"稼働中Windows自身のシステムディスクは対象にできません");
  }
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(target);
  if (!target_layout) {
    return clonecore::Result<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        target_layout.error());
  }
  auto target_status = validate_reviewed_target(
      target,
      expected_target,
      target_layout.value(),
      image.manifest,
      false);
  if (!target_status) {
    return clonecore::Result<
        imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1>::failure(
        target_status.error());
  }
  std::vector<std::uint32_t> disallowed_signatures;
  if (image.manifest.partition_style ==
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
        L"Windows縮小復元識別子生成",
        L"最終レイアウト用識別子生成器を作成できません");
  }
  return imageformat::make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
      image.manifest,
      target.size_bytes,
      target.logical_sector_size,
      image.manifest.partition_style,
      true,
      *guid_generator,
      *signature_generator,
      disallowed_signatures);
}

clonecore::Result<operationcore::OperationPlan>
make_windows_online_shrink_restore_operation_plan(
    const WindowsOnlineShrinkRestoreRequest& request) {
  if (!request.administrator ||
      !request.confirmation.first_step_acknowledged ||
      !request.reviewed_image.complete_container_verified ||
      !request.reviewed_image.metadata_verified ||
      !request.reviewed_image.restore_layout_verified ||
      request.reviewed_image.canonical_path.empty() ||
      request.image.image_path != request.reviewed_image.canonical_path ||
      request.image.storage_file_system !=
          request.reviewed_image.storage_file_system ||
      (request.reviewed_image.storage_file_system !=
           imageformat::TsumugiImageStorageFileSystem::ntfs &&
       request.reviewed_image.storage_file_system !=
           imageformat::TsumugiImageStorageFileSystem::exfat) ||
      request.reviewed_image.manifest.mode !=
          imageformat::TsumugiManifestMode::shrink ||
      all_zero(request.reviewed_image.global_hash) ||
      all_zero(request.reviewed_image.manifest.source_state_hash)) {
    return failure<operationcore::OperationPlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows縮小復元Operationレビュー",
        L"管理者権限または完全検証済み縮小イメージの証跡が不足しています");
  }
  auto target = diskmodel::make_stable_disk_identity(
      request.reviewed_target, request.reviewed_target.is_system_disk);
  auto target_layout =
      imageformat::hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_target);
  if (!target || !target_layout) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        !target ? target.error() : target_layout.error());
  }
  auto target_status = validate_reviewed_target(
      request.reviewed_target,
      request.expected_target,
      request.expected_target_layout_hash,
      request.reviewed_image.manifest,
      false);
  if (!target_status) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        target_status.error());
  }
  auto layout_status = validate_reviewed_layout(
      request.reviewed_image.manifest,
      request.reviewed_target,
      request.reviewed_layout);
  if (!layout_status) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        layout_status.error());
  }
  auto work_status = validate_windows_shrink_work_placement_observation(
      request.image_backing_disk,
      request.work_paths,
      request.observed_work);
  if (work_status) {
    work_status = validate_work_against_target(
        request.work_paths, request.observed_work, request.expected_target);
  }
  if (!work_status) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        work_status.error());
  }
  const auto image_target = reject_same_disk(
      request.image_backing_disk,
      request.expected_target,
      L"Windows縮小復元イメージ分離",
      L"復元イメージを保存している物理ディスク自身には復元できません");
  if (!image_target) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        image_target.error());
  }

  auto work = planned_payload_bytes(request.reviewed_image.manifest);
  if (!work) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        work.error());
  }
  auto payload_hash = immutable_payload_hash(request, work.value());
  if (!payload_hash) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        payload_hash.error());
  }
  operationcore::OperationPlan plan{
      .schema_version = operationcore::kOperationPlanSchemaVersion,
      .operation_id = request.operation_id,
      .kind = operationcore::OperationKind::image_restore,
      .environment = operationcore::OperationEnvironment::windows,
      .target = target.take_value(),
      .expected_work_bytes = work.value(),
      .immutable_payload_hash = payload_hash.take_value(),
  };
  const auto valid = operationcore::validate_operation_plan(plan);
  if (!valid) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        valid.error());
  }
  return clonecore::Result<operationcore::OperationPlan>::success(
      std::move(plan));
}

clonecore::Result<WindowsOnlineShrinkRestoreOperationReport>
execute_windows_online_shrink_restore_operation(
    const WindowsOnlineShrinkRestoreRequest& request,
    const WindowsOnlineShrinkRestoreDependencies& dependencies) {
  if (!dependencies.execute_reviewed_restore) {
    return failure<WindowsOnlineShrinkRestoreOperationReport>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windows縮小復元Operation依存",
        L"縮小復元executorがありません");
  }
  auto plan = make_windows_online_shrink_restore_operation_plan(request);
  if (!plan) {
    return clonecore::Result<
        WindowsOnlineShrinkRestoreOperationReport>::failure(plan.error());
  }
  std::optional<WindowsOnlineShrinkRestoreExecutionReport> restore_report;
  operationcore::OperationCallbacks callbacks{
      .reidentify =
          [&](const operationcore::OperationPlan& current) {
            return reidentify_for_operation(current, request, dependencies);
          },
      .execute =
          [&](const operationcore::OperationPlan& current,
              const clonecore::DiskOperationCallbacks&) {
            auto restored = dependencies.execute_reviewed_restore(request);
            if (!restored) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  restored.error());
            }
            restore_report = restored.take_value();
            auto processed = report_processed_bytes(restore_report->restore);
            if (!processed) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  processed.error());
            }
            return clonecore::Result<
                operationcore::ExecutionEvidence>::success({
                .processed_work_bytes = processed.value(),
                .output_hash = current.immutable_payload_hash,
            });
          },
      .verify =
          [&](const operationcore::OperationPlan& current,
              const operationcore::ExecutionEvidence& execution,
              const clonecore::DiskOperationCallbacks&) {
            auto processed = restore_report
                ? report_processed_bytes(restore_report->restore)
                : failure<std::uint64_t>(
                      clonecore::ErrorCode::verification_failed,
                      ERROR_CRC,
                      L"Windows縮小復元Operation最終検証",
                      L"復元実績がありません");
            if (!restore_report || !processed ||
                !restore_report->image_completely_reverified ||
                !restore_report->target_reidentified_before_plan ||
                !restore_report->work_placement_reidentified_before_write ||
                !restore_report->target_left_offline ||
                !restore_report->restore.
                    callbacks_started_after_complete_verification ||
                !restore_report->restore.image_matched_prepared_plan ||
                !restore_report->restore.target_reidentified_before_write ||
                !restore_report->restore.all_payloads_verified_by_adapter ||
                !restore_report->restore.final_layout_committed ||
                processed.value() != current.expected_work_bytes ||
                execution.processed_work_bytes != current.expected_work_bytes ||
                execution.output_hash != current.immutable_payload_hash) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windows縮小復元Operation最終検証",
                  L"完全再検証、再識別、全payload読戻し、offline保持、または最終layout確定の証跡が不足しています");
            }
            return clonecore::Result<
                operationcore::VerificationEvidence>::success({
                .verified_work_bytes = current.expected_work_bytes,
                .output_hash = current.immutable_payload_hash,
            });
          },
      .disk_operation = request.callbacks,
  };
  auto lifecycle = operationcore::run_operation(
      plan.value(), request.confirmation.typed_token, callbacks);
  return clonecore::Result<
      WindowsOnlineShrinkRestoreOperationReport>::success({
      .plan = plan.take_value(),
      .lifecycle = std::move(lifecycle),
      .restore = std::move(restore_report),
  });
}

WindowsOnlineShrinkRestoreDependencies
make_windows_online_shrink_restore_dependencies() {
  return WindowsOnlineShrinkRestoreDependencies{
      .reidentify_target =
          [](const clonecore::StableDiskIdentity& target,
             const clonecore::TargetConfirmation& confirmation) {
            auto inventory =
                diskmodel::make_windows_disk_inventory_provider();
            return diskmodel::reidentify_physical_target(
                target, confirmation, *inventory);
          },
      .is_protected_rescue_media =
          query_windows_data_rescue_protected_target_with_windows_apis,
      .observe_work_placement =
          observe_windows_shrink_work_placement_with_windows_apis,
      .execute_reviewed_restore =
          execute_reviewed_restore_with_windows_apis,
  };
}

clonecore::Result<WindowsOnlineShrinkRestoreOperationReport>
execute_windows_online_shrink_restore_operation_with_windows_apis(
    const WindowsOnlineShrinkRestoreRequest& request) {
  return execute_windows_online_shrink_restore_operation(
      request, make_windows_online_shrink_restore_dependencies());
}

}  // namespace ytec::windowsapp

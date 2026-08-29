#include "ytec/windowsapp/online_shrink_image.h"

#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace ytec::windowsapp {
namespace {

clonecore::Error shrink_error(
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
  return clonecore::Result<T>::failure(shrink_error(
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
  return clonecore::Status::failure(shrink_error(
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

bool align_up(
    const std::uint64_t value,
    const std::uint64_t alignment,
    std::uint64_t& result) noexcept {
  if (alignment == 0U ||
      value > (std::numeric_limits<std::uint64_t>::max)() -
          (alignment - 1U)) {
    return false;
  }
  result = ((value + alignment - 1U) / alignment) * alignment;
  return true;
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

bool selected(
    const imageformat::TsumugiManifestPartition& partition) noexcept {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
}

bool all_zero(const imageformat::Sha256Digest& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool is_ntfs(
    const imageformat::TsumugiManifestFileSystem file_system) noexcept {
  return file_system == imageformat::TsumugiManifestFileSystem::ntfs;
}

bool is_unknown(
    const imageformat::TsumugiManifestFileSystem file_system) noexcept {
  return file_system == imageformat::TsumugiManifestFileSystem::unknown;
}

bool is_static_exact_raw(
    const imageformat::TsumugiManifestFileSystem file_system) noexcept {
  return is_unknown(file_system) ||
      file_system == imageformat::TsumugiManifestFileSystem::none;
}

template <typename Enum>
bool has_flag(const Enum value, const Enum flag) noexcept {
  using Integer = std::underlying_type_t<Enum>;
  return (static_cast<Integer>(value) & static_cast<Integer>(flag)) != 0;
}

clonecore::Status same_work_observation(
    const WindowsShrinkWorkPlacementObservation& initial,
    const WindowsShrinkWorkPlacementObservation& current,
    const bool log_is_ram_only) {
  const auto same = [](const WindowsShrinkWorkPathObservation& left,
                       const WindowsShrinkWorkPathObservation& right,
                       const std::wstring_view role) {
    if (!equal_path(left.canonical_path, right.canonical_path)) {
      return status_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          std::wstring(role),
          L"VSS開始前と完成名確定前で作業パスが変化しました");
    }
    return clonecore::validate_stable_identity(
        left.backing_disk, right.backing_disk, role);
  };
  auto status = same(initial.scratch, current.scratch, L"縮小移行scratch再確認");
  if (!status) {
    return status;
  }
  status = same(
      initial.checkpoint, current.checkpoint, L"縮小移行checkpoint再確認");
  if (!status) {
    return status;
  }
  return log_is_ram_only
      ? clonecore::success_status()
      : same(initial.log, current.log, L"縮小移行log再確認");
}

const vssrequester::SnapshotMapping* matching_mapping(
    const vssrequester::SnapshotCopyContext& context,
    const WindowsShrinkCapturedPayload& payload) noexcept {
  const vssrequester::SnapshotMapping* match = nullptr;
  for (const auto& mapping : context.mappings) {
    if (!equal_path(
            mapping.original_volume_guid_path,
            payload.original_volume_guid_path)) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &mapping;
  }
  if (match == nullptr || payload.snapshot_id.empty() ||
      payload.snapshot_device_path.empty() ||
      !equal_path(match->snapshot_id, payload.snapshot_id) ||
      !equal_path(match->snapshot_device_path, payload.snapshot_device_path)) {
    return nullptr;
  }
  return match;
}

struct PreparedPayloads final {
  imageformat::TsumugiImageCreateRequest image;
  std::uint32_t wim_count{};
  std::uint32_t raw_count{};
};

clonecore::Result<PreparedPayloads> prepare_payloads(
    const WindowsOnlineShrinkImageCreateRequest& request,
    const vssrequester::SnapshotCopyContext& context,
    IWindowsShrinkCapturedSession& session) {
  auto image = request.image_template;
  const auto source_identity = clonecore::validate_stable_identity(
      request.source_disk,
      session.observed_source_disk(),
      L"縮小移行captureコピー元再識別");
  if (!source_identity) {
    return clonecore::Result<PreparedPayloads>::failure(
        source_identity.error());
  }
  if (session.size_bytes() != image.manifest.source_disk_size ||
      session.logical_sector_size() != image.manifest.logical_sector_size ||
      session.source_model_hash() != image.manifest.source_model_hash ||
      session.source_serial_hash() != image.manifest.source_serial_hash ||
      all_zero(session.source_state_hash())) {
    return failure<PreparedPayloads>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"縮小移行capture session",
        L"VSS capture sessionとコピー元マニフェストの安定識別が一致しません");
  }
  image.manifest.source_state_hash = session.source_state_hash();

  std::map<std::uint32_t, const WindowsShrinkCapturedPayload*> payloads;
  for (const auto& payload : session.payloads()) {
    std::uint64_t source_end{};
    if (payload.source_table_index == 0U || payload.length == 0U ||
        !checked_add(payload.session_source_offset, payload.length, source_end) ||
        source_end > session.size_bytes() ||
        !payloads.emplace(payload.source_table_index, &payload).second) {
      return failure<PreparedPayloads>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"縮小移行capture payload",
          L"capture結果の識別、範囲、または一意性が不正です");
    }
  }

  std::uint64_t logical_cursor{};
  std::size_t selected_count{};
  PreparedPayloads result{};
  for (auto& partition : image.manifest.partitions) {
    if (!selected(partition)) {
      continue;
    }
    ++selected_count;
    const auto found = payloads.find(partition.source_table_index);
    if (found == payloads.end()) {
      return failure<PreparedPayloads>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"縮小移行payload対応",
          L"選択済みパーティションのcapture結果がありません");
    }
    const auto& payload = *found->second;
    if (payload.kind == WindowsShrinkCapturedPayloadKind::vss_snapshot_wim) {
      if (!is_ntfs(partition.file_system) ||
          payload.original_source_offset != 0U ||
          matching_mapping(context, payload) == nullptr) {
        return failure<PreparedPayloads>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"縮小移行VSS WIM境界",
            L"Windows上ではNTFSだけを現在のSnapshot Setから単一WIMへcaptureできます");
      }
      partition.payload_encoding = imageformat::
          TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
      partition.payload_format_version =
          imageformat::kTsumugiWimPayloadFormatVersion;
      ++result.wim_count;
    } else if (payload.kind ==
               WindowsShrinkCapturedPayloadKind::locked_read_only_exact_raw) {
      if (!is_static_exact_raw(partition.file_system) ||
          payload.length != partition.source_size ||
          payload.original_source_offset != partition.source_offset ||
          payload.session_source_offset %
                  image.manifest.logical_sector_size !=
              0U ||
          !payload.original_volume_guid_path.empty() ||
          !payload.snapshot_id.empty() ||
          !payload.snapshot_device_path.empty()) {
        return failure<PreparedPayloads>(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"縮小移行exact RAW境界",
            L"未知／filesystemなし領域は同一状態へ固定した元サイズexact RAWだけを許可します");
      }
      partition.payload_encoding =
          imageformat::TsumugiManifestPayloadEncoding::exact_raw;
      partition.payload_format_version = 0U;
      partition.cluster_size = 0U;
      ++result.raw_count;
    } else {
      return failure<PreparedPayloads>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"縮小移行payload種別",
          L"未知のcapture payload種別を拒否しました");
    }

    if (payload.kind ==
        WindowsShrinkCapturedPayloadKind::locked_read_only_exact_raw) {
      std::uint64_t aligned{};
      if (!align_up(
              logical_cursor,
              image.manifest.logical_sector_size,
              aligned)) {
        return failure<PreparedPayloads>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"縮小移行exact RAW整列",
            L"RAW payloadの論理セクター整列に失敗しました");
      }
      logical_cursor = aligned;
    }

    std::uint64_t logical_end{};
    if (!checked_add(logical_cursor, payload.length, logical_end) ||
        logical_end > image.manifest.source_disk_size) {
      return failure<PreparedPayloads>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_DISK_FULL,
          L"縮小移行payload namespace",
          L"単一コンテナ内のpayload範囲がコピー元ディスク寸法を超えます");
    }
    partition.payload_logical_offset = logical_cursor;
    partition.payload_logical_length = payload.length;

    std::uint64_t completed{};
    while (completed < payload.length) {
      const std::uint64_t amount = (std::min<std::uint64_t>)(
          payload.length - completed, image.chunk_size);
      std::uint64_t chunk_logical{};
      std::uint64_t chunk_source{};
      if (!checked_add(logical_cursor, completed, chunk_logical) ||
          !checked_add(
              payload.session_source_offset, completed, chunk_source)) {
        return failure<PreparedPayloads>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"縮小移行payload chunk",
            L"payloadチャンク位置が64bit上限を超えます");
      }
      image.chunks.push_back(imageformat::TsumugiStreamBuildChunk{
          .logical_offset = chunk_logical,
          .logical_length = amount,
          .source_offset = chunk_source,
          .flags = imageformat::TsumugiChunkFlags::none,
          .source = &session,
      });
      completed += amount;
    }
    logical_cursor = logical_end;
  }
  if (selected_count == 0U || payloads.size() != selected_count ||
      result.wim_count == 0U) {
    return failure<PreparedPayloads>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小移行payload集合",
        L"このWindows sliceには1件以上のVSS WIMが必要で、余分なcapture結果は許可しません");
  }
  image.source_session = &session;
  result.image = std::move(image);
  return clonecore::Result<PreparedPayloads>::success(std::move(result));
}

clonecore::Error with_abort_failure(
    clonecore::Error primary,
    imageformat::TsumugiStagedImageV1& staged) {
  const auto aborted = staged.abort_incomplete();
  if (!aborted) {
    primary.message += L"。検証済み.partialの破棄にも失敗しました: " +
        aborted.error().message;
  }
  return primary;
}

clonecore::Status validate_request(
    const WindowsOnlineShrinkImageCreateRequest& request,
    const WindowsOnlineShrinkImageCreateDependencies& dependencies) {
  const auto source = clonecore::validate_stable_identity(
      request.source_disk, request.source_disk, L"縮小移行コピー元");
  if (!source) {
    return source;
  }
  const auto& image = request.image_template;
  const auto model_hash = imageformat::hash_tsumugi_source_model_v1(
      request.source_disk.model);
  if (!model_hash) {
    return clonecore::Status::failure(model_hash.error());
  }
  const auto serial_hash = imageformat::hash_tsumugi_source_serial_v1(
      request.source_disk.serial_suffix,
      request.source_disk.device_instance_id);
  if (!serial_hash) {
    return clonecore::Status::failure(serial_hash.error());
  }
  if (!request.workflow.administrator || request.workflow.volumes.empty() ||
      image.manifest.mode != imageformat::TsumugiManifestMode::shrink ||
      image.manifest.source_disk_size != request.source_disk.size_bytes ||
      image.manifest.logical_sector_size !=
          request.source_disk.logical_sector_size ||
      image.manifest.source_model_hash != model_hash.value() ||
      image.manifest.source_serial_hash != serial_hash.value() ||
      (image.chunk_size != imageformat::kImageChunkSize16MiB &&
       image.chunk_size != imageformat::kImageChunkSize32MiB) ||
      image.final_path.empty() || image.source_session != nullptr ||
      !image.chunks.empty() || !dependencies.observe_work_placement ||
      !dependencies.capture_snapshot_payloads || !dependencies.prepare_image ||
      !dependencies.backend_factory || !dependencies.revalidate_destination) {
    return status_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"オンライン縮小Tsumugi要求",
        L"管理者、縮小マニフェスト、コピー元寸法、または必須Adapterが不足しています");
  }
  if (has_flag(
          image.manifest.flags,
          imageformat::TsumugiManifestFlags::
              bitlocker_source_was_unlocked)) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"オンライン縮小BitLocker境界",
        L"現在のWindows縮小capture sliceではBitLocker元ディスクを扱えません");
  }
  for (const auto& volume : request.workflow.volumes) {
    if (volume.volume_guid_path.empty() ||
        _wcsicmp(volume.file_system.c_str(), L"NTFS") != 0) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"オンライン縮小VSS Volume",
          L"Windows上の縮小captureはVSS対応NTFS Volumeだけを対象にします");
    }
  }
  std::size_t selected_count{};
  std::size_t selected_ntfs_count{};
  for (const auto& partition : image.manifest.partitions) {
    if (has_flag(
            partition.flags,
            imageformat::TsumugiManifestPartitionFlags::
                bitlocker_was_unlocked)) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"オンライン縮小BitLocker partition境界",
          L"BitLocker解除済み印を持つパーティションをWindows上でcaptureできません");
    }
    if (!selected(partition)) {
      continue;
    }
    ++selected_count;
    if (is_ntfs(partition.file_system)) {
      ++selected_ntfs_count;
    }
    if (partition.payload_logical_offset != 0U ||
        partition.payload_logical_length != 0U ||
        partition.payload_format_version != 0U ||
        partition.minimum_target_bytes == 0U ||
        partition.planned_target_bytes < partition.minimum_target_bytes ||
        (!is_ntfs(partition.file_system) &&
         !is_static_exact_raw(partition.file_system))) {
      return status_failure(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"オンライン縮小partition template",
          L"このsliceは未確定payloadを持つNTFSまたは静的exact RAWだけを受け付けます");
    }
  }
  if (selected_count == 0U || selected_ntfs_count == 0U ||
      selected_ntfs_count != request.workflow.volumes.size()) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"オンライン縮小VSS対象件数",
        L"選択NTFSと同一Snapshot setへ追加するVolume件数が一致しません");
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<WindowsOnlineShrinkImageCreateReport>
execute_windows_online_shrink_image_create(
    const WindowsOnlineShrinkImageCreateRequest& request,
    const WindowsOnlineShrinkImageCreateDependencies& dependencies) {
  const auto valid = validate_request(request, dependencies);
  if (!valid) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        valid.error());
  }
  auto initial_work = dependencies.observe_work_placement(request.work_paths);
  if (!initial_work) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        initial_work.error());
  }
  auto status = validate_windows_shrink_work_placement_observation(
      request.source_disk, request.work_paths, initial_work.value());
  if (!status) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        status.error());
  }
  status = dependencies.revalidate_destination(nullptr);
  if (!status) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        status.error());
  }
  const WindowsShrinkWorkPaths canonical_work_paths{
      .scratch_directory = initial_work.value().scratch.canonical_path,
      .checkpoint_path = initial_work.value().checkpoint.canonical_path,
      .log_path = initial_work.value().log.canonical_path,
      .log_is_ram_only = request.work_paths.log_is_ram_only,
  };

  std::optional<imageformat::TsumugiStagedImageV1> staged;
  std::uint32_t wim_count{};
  std::uint32_t raw_count{};
  bool staging_discarded{};
  std::wstring captured_snapshot_set;
  auto backend = dependencies.backend_factory(
      [&](const vssrequester::SnapshotCopyContext& context) {
        if (staged.has_value() || context.snapshot_set_id.empty()) {
          return status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_STATE,
              L"オンライン縮小Snapshot callback",
              L"一つの操作で有効なSnapshot captureは1回だけです");
        }
        std::unique_ptr<vssrequester::VssDiffAreaOperationMonitor>
            diff_area_monitor;
        clonecore::DiskOperationCallbacks active_callbacks =
            request.callbacks;
        if (dependencies.make_diff_area_monitor) {
          if (clonecore::disk_operation_cancellation_requested(
                  request.callbacks)) {
            return status_failure(
                clonecore::ErrorCode::cancelled,
                ERROR_CANCELLED,
                L"オンライン縮小VSS差分領域初回poll",
                L"初回output変更前に取消要求を確認しました");
          }
          auto made_monitor =
              dependencies.make_diff_area_monitor(context);
          if (!made_monitor || !made_monitor.value()) {
            return clonecore::Status::failure(
                made_monitor
                    ? shrink_error(
                          clonecore::ErrorCode::internal_error,
                          ERROR_INVALID_HANDLE,
                          L"オンライン縮小VSS差分領域monitor",
                          L"製品monitor factoryが空のmonitorを返しました")
                    : made_monitor.error());
          }
          diff_area_monitor = made_monitor.take_value();
          const auto initial = diff_area_monitor->initial_poll();
          if (!initial) {
            return initial;
          }
          active_callbacks =
              diff_area_monitor->callbacks(std::move(active_callbacks));
        }
        auto captured = dependencies.capture_snapshot_payloads(
            context,
            request.image_template.manifest.partitions,
            canonical_work_paths,
            active_callbacks);
        if (!captured || !captured.value()) {
          return clonecore::Status::failure(
              captured ? shrink_error(
                             clonecore::ErrorCode::internal_error,
                             ERROR_INVALID_HANDLE,
                             L"オンライン縮小capture Adapter",
                             L"capture Adapterが空のsessionを返しました")
                       : captured.error());
        }
        auto session = captured.take_value();
        auto payloads = prepare_payloads(request, context, *session);
        if (!payloads) {
          const auto cleanup = session->discard_owned_staging();
          auto error = payloads.error();
          if (!cleanup) {
            error.message += L"。所有WIM stagingの破棄にも失敗しました: " +
                cleanup.error().message;
          }
          return clonecore::Status::failure(std::move(error));
        }
        auto prepared = dependencies.prepare_image(
            payloads.value().image, active_callbacks);
        const auto cleanup = session->discard_owned_staging();
        if (!prepared || !cleanup) {
          auto error = prepared ? cleanup.error() : prepared.error();
          if (!cleanup && !prepared) {
            error.message += L"。所有WIM stagingの破棄にも失敗しました: " +
                cleanup.error().message;
          }
          if (prepared) {
            error = with_abort_failure(std::move(error), prepared.value());
          }
          return clonecore::Status::failure(std::move(error));
        }
        if (!prepared.value().pending() ||
            !imageformat::selected_tsumugi_creation_verification_passed(
                prepared.value().report()) ||
            prepared.value().report().stream.committed) {
          auto error = shrink_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"オンライン縮小Tsumugi作成時検証",
              L"完成名未確定の選択済み検証を通過した.partialが得られませんでした");
          error = with_abort_failure(std::move(error), prepared.value());
          return clonecore::Status::failure(std::move(error));
        }
        if (diff_area_monitor) {
          auto monitored = diff_area_monitor->completion_poll();
          if (monitored) {
            monitored = vssrequester::
                validate_completed_vss_diff_area_operation_evidence(
                    diff_area_monitor->evidence());
          }
          if (!monitored) {
            return clonecore::Status::failure(with_abort_failure(
                monitored.error(), prepared.value()));
          }
        }
        captured_snapshot_set = context.snapshot_set_id;
        wim_count = payloads.value().wim_count;
        raw_count = payloads.value().raw_count;
        staging_discarded = true;
        staged.emplace(prepared.take_value());
        return clonecore::success_status();
      });
  if (!backend || !backend.value()) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        backend ? shrink_error(
                      clonecore::ErrorCode::internal_error,
                      ERROR_INVALID_HANDLE,
                      L"オンライン縮小VSS Backend",
                      L"VSS Backend Factoryが空のBackendを返しました")
                : backend.error());
  }

  auto workflow = vssrequester::execute_backup_workflow(
      request.workflow, *backend.value());
  if (!workflow) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        staged ? with_abort_failure(workflow.error(), *staged)
               : workflow.error());
  }
  if (!staged || !staging_discarded ||
      captured_snapshot_set != workflow.value().snapshot_set_id ||
      !workflow.value().snapshot_data_copied ||
      !workflow.value().backup_completed ||
      !workflow.value().snapshots_deleted) {
    auto error = shrink_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_STATE,
        L"オンライン縮小VSS完了照合",
        L"検証済み画像、WIM staging破棄、BackupComplete、またはSnapshot削除が揃っていません");
    if (staged) {
      error = with_abort_failure(std::move(error), *staged);
    }
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        std::move(error));
  }

  auto current_work = dependencies.observe_work_placement(
      canonical_work_paths);
  if (!current_work) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        with_abort_failure(current_work.error(), *staged));
  }
  status = validate_windows_shrink_work_placement_observation(
      request.source_disk, canonical_work_paths, current_work.value());
  if (status) {
    status = same_work_observation(
        initial_work.value(),
        current_work.value(),
        request.work_paths.log_is_ram_only);
  }
  if (!status) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        with_abort_failure(status.error(), *staged));
  }
  status = dependencies.revalidate_destination(&staged->report());
  if (!status) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        with_abort_failure(status.error(), *staged));
  }
  auto committed = staged->commit_verified();
  if (!committed) {
    return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::failure(
        with_abort_failure(committed.error(), *staged));
  }
  return clonecore::Result<WindowsOnlineShrinkImageCreateReport>::success({
      .workflow = workflow.take_value(),
      .image = committed.take_value(),
      .wim_payload_count = wim_count,
      .exact_raw_payload_count = raw_count,
      .every_wim_bound_to_active_snapshot_set = true,
      .work_placement_revalidated_before_commit = true,
      .staging_discarded_before_commit = true,
      .final_file_committed_after_vss = true,
  });
}

}  // namespace ytec::windowsapp

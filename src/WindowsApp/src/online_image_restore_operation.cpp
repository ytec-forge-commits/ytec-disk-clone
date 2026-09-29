#include "ytec/windowsapp/online_image_restore_operation.h"

#include "ytec/imageformat/sha256.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::windowsapp {
namespace {

clonecore::Error operation_error(
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
  return clonecore::Result<T>::failure(operation_error(
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

clonecore::Result<std::uint64_t> planned_payload_bytes(
    const imageformat::TsumugiManifest& manifest,
    const std::optional<std::uint32_t> source_table_index) {
  std::uint64_t total{};
  for (const auto& partition : manifest.partitions) {
    const auto selected =
        (static_cast<std::uint32_t>(partition.flags) &
         static_cast<std::uint32_t>(
             imageformat::TsumugiManifestPartitionFlags::selected)) != 0U;
    if (!selected ||
        (source_table_index.has_value() &&
         partition.source_table_index != source_table_index.value())) {
      continue;
    }
    if (partition.payload_logical_length == 0U ||
        total > (std::numeric_limits<std::uint64_t>::max)() -
                    partition.payload_logical_length) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi復元操作量",
          L"選択済みpayloadの長さが0または合計がオーバーフローします");
    }
    total += partition.payload_logical_length;
  }
  if (total == 0U) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi復元操作量",
        L"復元対象として選択されたpayloadがありません");
  }
  return clonecore::Result<std::uint64_t>::success(total);
}

clonecore::Result<operationcore::Sha256Digest> immutable_payload_hash(
    const OnlineImageRestoreOperationRequest& request,
    const std::uint64_t expected_work_bytes) {
  constexpr std::string_view domain =
      "YTEC-ONLINE-TSUMUGI-WHOLE-DISK-RESTORE-PLAN-V1";
  std::vector<std::byte> bytes;
  bytes.reserve(160U);
  append_u32(bytes, static_cast<std::uint32_t>(domain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  bytes.insert(
      bytes.end(),
      request.reviewed_image.global_hash.begin(),
      request.reviewed_image.global_hash.end());
  bytes.insert(
      bytes.end(),
      request.reviewed_image.manifest.source_state_hash.begin(),
      request.reviewed_image.manifest.source_state_hash.end());
  bytes.insert(
      bytes.end(),
      request.restore.expected_target_layout_hash.begin(),
      request.restore.expected_target_layout_hash.end());
  append_u64(bytes, request.reviewed_image.image_length);
  append_u64(bytes, expected_work_bytes);
  append_u32(
      bytes,
      static_cast<std::uint32_t>(request.reviewed_image.manifest.mode));
  append_u32(
      bytes,
      static_cast<std::uint32_t>(
          request.reviewed_image.manifest.partition_style));
  append_u32(
      bytes,
      static_cast<std::uint32_t>(
          request.restore.image.storage_file_system));
  append_u32(
      bytes,
      request.restore.individual_partition.has_value() ? 1U : 0U);
  if (request.restore.individual_partition.has_value()) {
    const auto& individual = request.restore.individual_partition.value();
    append_u32(bytes, individual.source_table_index);
    const auto* existing = std::get_if<imageformat::
        TsumugiPhysicalExistingPartitionRestoreSelection>(
        &individual.target);
    if (existing != nullptr) {
      append_u32(bytes, 0U);
      append_u32(bytes, existing->target_table_index);
      append_u32(bytes, existing->target_partition_number);
      append_u64(bytes, existing->target_offset);
      append_u64(bytes, existing->target_size);
    } else {
      const auto& unallocated = std::get<imageformat::
          TsumugiPhysicalUnallocatedRestoreSelection>(individual.target);
      append_u32(bytes, 1U);
      append_u64(bytes, unallocated.target_offset);
      append_u64(bytes, unallocated.target_size);
    }
  }
  return imageformat::sha256(bytes);
}

clonecore::Result<operationcore::ReidentifiedOperation>
reidentify_for_operation(
    const operationcore::OperationPlan& plan,
    const OnlineImageRestoreOperationRequest& request,
    const OnlineImageRestoreDependencies& dependencies) {
  if (!plan.target || !dependencies.reidentify_target) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Tsumugi復元Operation再識別",
        L"復元先または再識別依存がありません");
  }
  auto observed = dependencies.reidentify_target(
      *plan.target, request.restore.confirmation);
  if (!observed) {
    return clonecore::Result<
        operationcore::ReidentifiedOperation>::failure(observed.error());
  }
  auto layout = hash_online_image_restore_target_layout(
      observed.value().target);
  if (!layout || layout.value() !=
          request.restore.expected_target_layout_hash) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi復元Operationレイアウト再識別",
        L"確認後に復元先のパーティション形式または配置が変化しました");
  }
  return clonecore::Result<
      operationcore::ReidentifiedOperation>::success({
      .target = observed.value().target_identity,
  });
}

}  // namespace

clonecore::Result<operationcore::OperationId>
make_online_image_restore_operation_id_with_windows_apis() {
  GUID guid{};
  const HRESULT status = CoCreateGuid(&guid);
  if (FAILED(status)) {
    return failure<operationcore::OperationId>(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(status),
        L"Tsumugi復元操作ID",
        L"単回操作IDを生成できません");
  }
  operationcore::OperationId result{};
  static_assert(sizeof(guid) == result.size());
  std::memcpy(result.data(), &guid, result.size());
  return clonecore::Result<operationcore::OperationId>::success(result);
}

clonecore::Result<operationcore::OperationPlan>
make_online_image_restore_operation_plan(
    const OnlineImageRestoreOperationRequest& request) {
  if (!request.reviewed_image.complete_container_verified ||
      !request.reviewed_image.metadata_verified ||
      !request.reviewed_image.restore_layout_verified ||
      request.reviewed_image.canonical_path.empty() ||
      request.restore.image.image_path !=
          request.reviewed_image.canonical_path ||
      request.restore.image.storage_file_system !=
          request.reviewed_image.storage_file_system ||
      (request.reviewed_image.storage_file_system !=
           imageformat::TsumugiImageStorageFileSystem::ntfs &&
       request.reviewed_image.storage_file_system !=
           imageformat::TsumugiImageStorageFileSystem::exfat) ||
      all_zero(request.reviewed_image.global_hash) ||
      all_zero(request.reviewed_image.manifest.source_state_hash) ||
      request.restore.expected_image_global_hash !=
          request.reviewed_image.global_hash ||
      request.restore.expected_source_state_hash !=
          request.reviewed_image.manifest.source_state_hash ||
      request.reviewed_image.manifest.mode ==
          imageformat::TsumugiManifestMode::shrink) {
    return failure<operationcore::OperationPlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi復元Operationレビュー",
        L"完全検証済み通常／救出イメージと実行要求が一致しません");
  }

  auto target_identity = diskmodel::make_stable_disk_identity(
      request.reviewed_target,
      request.reviewed_target.is_system_disk);
  auto target_layout = hash_online_image_restore_target_layout(
      request.reviewed_target);
  if (!target_identity || !target_layout) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        !target_identity ? target_identity.error() : target_layout.error());
  }
  const auto identity_match = clonecore::validate_stable_identity(
      request.restore.expected_target,
      target_identity.value(),
      L"復元先レビュー");
  if (!identity_match ||
      target_layout.value() != request.restore.expected_target_layout_hash) {
    return failure<operationcore::OperationPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi復元Operation対象照合",
        L"画面で確認した復元先と実行要求の識別または配置が一致しません");
  }

  if (request.restore.individual_partition.has_value()) {
    const auto individual_status = imageformat::
        validate_tsumugi_physical_individual_partition_selection_v1(
            request.reviewed_image.manifest,
            request.reviewed_target,
            request.restore.individual_partition.value());
    if (!individual_status) {
      return failure<operationcore::OperationPlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"Tsumugi個別復元Operationレビュー",
          individual_status.error().message);
    }
  }

  auto work = planned_payload_bytes(
      request.reviewed_image.manifest,
      request.restore.individual_partition
          ? std::optional<std::uint32_t>(
                request.restore.individual_partition->source_table_index)
          : std::nullopt);
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
      .target = target_identity.take_value(),
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

clonecore::Result<OnlineImageRestoreOperationReport>
execute_online_image_restore_operation(
    const OnlineImageRestoreOperationRequest& request,
    const OnlineImageRestoreDependencies& dependencies) {
  auto plan = make_online_image_restore_operation_plan(request);
  if (!plan) {
    return clonecore::Result<OnlineImageRestoreOperationReport>::failure(
        plan.error());
  }

  std::optional<OnlineImageRestoreReport> restore_report;
  operationcore::OperationCallbacks callbacks{
      .reidentify =
          [&](const operationcore::OperationPlan& current) {
            return reidentify_for_operation(
                current, request, dependencies);
          },
      .execute =
          [&](const operationcore::OperationPlan& current,
              const clonecore::DiskOperationCallbacks&) {
            auto restored = execute_online_image_restore(
                request.restore, dependencies);
            if (!restored) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  restored.error());
            }
            restore_report = restored.take_value();
            return clonecore::Result<
                operationcore::ExecutionEvidence>::success({
                .processed_work_bytes =
                    restore_report->restore.written_logical_bytes,
                .output_hash = current.immutable_payload_hash,
            });
          },
      .verify =
          [&](const operationcore::OperationPlan& current,
              const operationcore::ExecutionEvidence& execution,
              const clonecore::DiskOperationCallbacks&) {
            if (!restore_report ||
                !restore_report->restore.
                    callbacks_started_after_complete_verification ||
                !restore_report->restore.image_matched_prepared_plan ||
                !restore_report->restore.target_reidentified_before_write ||
                !restore_report->restore.all_writes_read_back_verified ||
                !restore_report->restore.final_layout_committed ||
                restore_report->restore.written_logical_bytes !=
                    current.expected_work_bytes ||
                execution.output_hash != current.immutable_payload_hash) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Tsumugi復元Operation最終検証",
                  L"完全検証、再識別、全書込み読戻し、または最終レイアウト確定の証跡が不足しています");
            }
            return clonecore::Result<
                operationcore::VerificationEvidence>::success({
                .verified_work_bytes = current.expected_work_bytes,
                .output_hash = current.immutable_payload_hash,
            });
          },
      .disk_operation = request.restore.callbacks,
  };
  auto lifecycle = operationcore::run_operation(
      plan.value(),
      request.restore.confirmation.typed_token,
      callbacks);
  return clonecore::Result<OnlineImageRestoreOperationReport>::success({
      .plan = plan.take_value(),
      .lifecycle = std::move(lifecycle),
      .restore = std::move(restore_report),
  });
}

clonecore::Result<OnlineImageRestoreOperationReport>
execute_online_image_restore_operation_with_windows_apis(
    const OnlineImageRestoreOperationRequest& request) {
  return execute_online_image_restore_operation(
      request, make_online_image_restore_windows_dependencies());
}

}  // namespace ytec::windowsapp

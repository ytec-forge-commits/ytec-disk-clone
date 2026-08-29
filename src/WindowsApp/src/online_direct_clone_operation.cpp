#include "ytec/windowsapp/online_direct_clone_operation.h"

#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

clonecore::Result<operationcore::Sha256Digest> immutable_payload_hash(
    const OnlineDirectCloneOperationRequest& request) {
  constexpr std::string_view domain =
      "YTEC-WINDOWS-DIRECT-CLONE-PLAN-V1";
  std::vector<std::byte> bytes;
  bytes.reserve(128U);
  append_u32(bytes, static_cast<std::uint32_t>(domain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  append_array(bytes, request.clone.expected_source_layout_hash);
  append_array(bytes, request.clone.expected_target_layout_hash);
  append_u64(bytes, request.clone.expected_source.size_bytes);
  append_u64(bytes, request.clone.expected_target.size_bytes);
  append_u64(bytes, request.clone.maximum_chunk_bytes);
  append_u8(
      bytes,
      static_cast<std::uint8_t>(
          request.reviewed_source.partition_style));
  append_u8(bytes, request.clone.expected_source.is_system_disk ? 1U : 0U);
  // v1 is deliberately exact-only. Shrink/conversion will receive separate
  // immutable mode fields before those engines are exposed by this bridge.
  append_u8(bytes, 0U);
  return imageformat::sha256(bytes);
}

clonecore::Result<operationcore::Sha256Digest> clone_evidence_hash(
    const operationcore::OperationPlan& plan,
    const OnlineDirectCloneReport& report) {
  constexpr std::string_view domain =
      "YTEC-WINDOWS-DIRECT-CLONE-EVIDENCE-V2";
  std::vector<std::byte> bytes;
  bytes.reserve(128U);
  append_u32(bytes, static_cast<std::uint32_t>(domain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
  append_array(bytes, plan.immutable_payload_hash);
  append_u8(bytes, static_cast<std::uint8_t>(report.partition_style));
  append_u64(bytes, report.copied_data_bytes);
  append_u32(bytes, report.copied_partition_count);
  append_u32(bytes, report.recreated_partition_count);
  append_array(bytes, report.verified_write_digest);
  append_u8(bytes, report.read_back_verified ? 1U : 0U);
  append_u8(bytes, report.partition_table_committed ? 1U : 0U);
  append_u8(bytes, report.snapshot_backup_completed ? 1U : 0U);
  append_u8(bytes, report.snapshots_deleted ? 1U : 0U);
  append_u8(bytes, report.used_vss_snapshot ? 1U : 0U);
  append_u32(bytes, report.locked_volume_count);
  append_u8(bytes, report.source_consistency_verified ? 1U : 0U);
  append_u8(bytes, report.target_left_offline ? 1U : 0U);
  append_u8(bytes, report.boot_finalization_required ? 1U : 0U);
  append_u8(bytes, report.boot_finalization_completed ? 1U : 0U);
  return imageformat::sha256(bytes);
}

clonecore::Status validate_clone_report(
    const operationcore::OperationPlan& plan,
    const OnlineDirectCloneOperationRequest& request,
    const OnlineDirectCloneReport& report) {
  const auto expected_style =
      request.reviewed_source.partition_style ==
              diskmodel::PartitionStyle::gpt
          ? OnlineDirectClonePartitionStyle::gpt
          : OnlineDirectClonePartitionStyle::mbr;
  const std::uint64_t accounted_partitions =
      static_cast<std::uint64_t>(report.copied_partition_count) +
      static_cast<std::uint64_t>(report.recreated_partition_count);
  if (report.partition_style != expected_style ||
      report.copied_data_bytes == 0U ||
      report.copied_data_bytes > plan.expected_work_bytes ||
      accounted_partitions != request.reviewed_source.partitions.size() ||
      all_zero(report.verified_write_digest) ||
      !report.read_back_verified || !report.partition_table_committed ||
      !report.source_consistency_verified ||
      (report.used_vss_snapshot &&
       (!report.snapshot_backup_completed || !report.snapshots_deleted)) ||
      (!report.used_vss_snapshot &&
       (report.snapshot_backup_completed || report.snapshots_deleted ||
        report.locked_volume_count == 0U)) ||
      !report.target_left_offline ||
      report.boot_finalization_required !=
          request.clone.expected_source.is_system_disk ||
      (report.boot_finalization_required &&
       !report.boot_finalization_completed) ||
      plan.expected_work_bytes != request.clone.expected_source.size_bytes) {
    return clonecore::Status::failure(operation_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Windows直接クローンOperation最終検証",
        L"VSS/Volume lock整合性、全書込み読戻し、最終レイアウト確定、オフライン保持、または起動情報再構築の証跡が不足しています"));
  }
  return clonecore::success_status();
}

clonecore::Result<operationcore::ReidentifiedOperation>
reidentify_for_operation(
    const operationcore::OperationPlan& plan,
    const OnlineDirectCloneOperationRequest& request,
    const OnlineDirectCloneDependencies& dependencies) {
  if (!plan.source || !plan.target ||
      !dependencies.reidentify_clone_selection) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"Windows直接クローンOperation再識別",
        L"コピー元、コピー先、または再識別依存がありません");
  }
  auto observed = dependencies.reidentify_clone_selection(
      *plan.source, *plan.target);
  if (!observed) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        observed.error());
  }
  auto source_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().source);
  auto target_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          observed.value().target);
  if (!source_layout || !target_layout) {
    return clonecore::Result<operationcore::ReidentifiedOperation>::failure(
        !source_layout ? source_layout.error() : target_layout.error());
  }
  if (source_layout.value() != request.clone.expected_source_layout_hash ||
      target_layout.value() != request.clone.expected_target_layout_hash) {
    return failure<operationcore::ReidentifiedOperation>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows直接クローンOperationレイアウト再識別",
        L"最終確認後にコピー元またはコピー先のパーティション形式・配置が変化しました");
  }
  return clonecore::Result<operationcore::ReidentifiedOperation>::success({
      .source = observed.value().source_identity,
      .target = observed.value().target_identity,
  });
}

}  // namespace

clonecore::Result<operationcore::OperationId>
make_online_direct_clone_operation_id_with_windows_apis() {
  GUID guid{};
  const HRESULT status = CoCreateGuid(&guid);
  if (FAILED(status)) {
    return failure<operationcore::OperationId>(
        clonecore::ErrorCode::io_failed,
        static_cast<DWORD>(status),
        L"Windows直接クローン操作ID",
        L"単回操作IDを生成できません");
  }
  operationcore::OperationId result{};
  static_assert(sizeof(guid) == result.size());
  std::memcpy(result.data(), &guid, result.size());
  return clonecore::Result<operationcore::OperationId>::success(result);
}

clonecore::Result<operationcore::OperationPlan>
make_online_direct_clone_operation_plan(
    const OnlineDirectCloneOperationRequest& request) {
  if (!request.clone.administrator ||
      request.clone.maximum_chunk_bytes == 0U ||
      request.clone.maximum_chunk_bytes > 16U * 1024U * 1024U ||
      request.clone.expected_source.size_bytes == 0U ||
      all_zero(request.clone.expected_source_layout_hash) ||
      all_zero(request.clone.expected_target_layout_hash) ||
      !request.reviewed_source.offline.has_value() ||
      !request.reviewed_source.removable.has_value() ||
      !request.reviewed_target.offline.has_value() ||
      !request.reviewed_target.read_only.has_value() ||
      !request.reviewed_target.removable.has_value() ||
      request.reviewed_source.offline.value_or(true) ||
      request.reviewed_source.removable.value_or(true) ||
      request.reviewed_target.is_system_disk ||
      request.reviewed_target.read_only.value_or(true) ||
      request.reviewed_target.removable.value_or(true) ||
      diskmodel::disk_health_operation_advice(
          request.reviewed_target.health, false) ==
          diskmodel::DiskHealthOperationAdvice::block_target ||
      request.reviewed_target.size_bytes <
          request.reviewed_source.size_bytes ||
      request.reviewed_target.logical_sector_size !=
          request.reviewed_source.logical_sector_size ||
      (request.reviewed_source.partition_style !=
           diskmodel::PartitionStyle::gpt &&
       request.reviewed_source.partition_style !=
           diskmodel::PartitionStyle::mbr)) {
    return failure<operationcore::OperationPlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Windows直接クローンOperationレビュー",
        L"管理者状態、ディスク属性、容量、セクター、コピー元形式、処理量、チャンク寸法、またはレビューHashが不正です");
  }

  auto reviewed_source_identity = diskmodel::make_stable_disk_identity(
      request.reviewed_source,
      request.reviewed_source.is_system_disk);
  auto reviewed_target_identity = diskmodel::make_stable_disk_identity(
      request.reviewed_target, false);
  auto reviewed_source_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_source);
  auto reviewed_target_layout = imageformat::
      hash_tsumugi_physical_restore_target_layout_v1(
          request.reviewed_target);
  if (!reviewed_source_identity || !reviewed_target_identity ||
      !reviewed_source_layout || !reviewed_target_layout) {
    if (!reviewed_source_identity) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          reviewed_source_identity.error());
    }
    if (!reviewed_target_identity) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          reviewed_target_identity.error());
    }
    return clonecore::Result<operationcore::OperationPlan>::failure(
        !reviewed_source_layout
            ? reviewed_source_layout.error()
            : reviewed_target_layout.error());
  }

  const auto source_identity = clonecore::validate_stable_identity(
      request.clone.expected_source,
      reviewed_source_identity.value(),
      L"コピー元レビュー");
  const auto target_identity = clonecore::validate_stable_identity(
      request.clone.expected_target,
      reviewed_target_identity.value(),
      L"コピー先レビュー");
  if (!source_identity || !target_identity ||
      reviewed_source_layout.value() !=
          request.clone.expected_source_layout_hash ||
      reviewed_target_layout.value() !=
          request.clone.expected_target_layout_hash) {
    if (!source_identity) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          source_identity.error());
    }
    if (!target_identity) {
      return clonecore::Result<operationcore::OperationPlan>::failure(
          target_identity.error());
    }
    return failure<operationcore::OperationPlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Windows直接クローンOperation対象照合",
        L"画面で確認したコピー元・コピー先と実行要求のレイアウトが一致しません");
  }

  auto payload_hash = immutable_payload_hash(request);
  if (!payload_hash) {
    return clonecore::Result<operationcore::OperationPlan>::failure(
        payload_hash.error());
  }
  operationcore::OperationPlan plan{
      .schema_version = operationcore::kOperationPlanSchemaVersion,
      .operation_id = request.operation_id,
      .kind = operationcore::OperationKind::clone,
      .environment = operationcore::OperationEnvironment::windows,
      .source = request.clone.expected_source,
      .target = request.clone.expected_target,
      // Logical source coverage is stable even though used-range cloning may
      // transfer fewer physical bytes. The concrete report retains the actual
      // copied_data_bytes for UI and diagnostics.
      .expected_work_bytes = request.clone.expected_source.size_bytes,
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

clonecore::Result<OnlineDirectCloneOperationReport>
execute_online_direct_clone_operation(
    const OnlineDirectCloneOperationRequest& request,
    const OnlineDirectCloneDependencies& dependencies) {
  auto plan = make_online_direct_clone_operation_plan(request);
  if (!plan) {
    return clonecore::Result<OnlineDirectCloneOperationReport>::failure(
        plan.error());
  }

  std::optional<OnlineDirectCloneReport> clone_report;
  operationcore::OperationCallbacks callbacks{
      .reidentify =
          [&](const operationcore::OperationPlan& current) {
            return reidentify_for_operation(
                current, request, dependencies);
          },
      .execute =
          [&](const operationcore::OperationPlan& current,
              const clonecore::DiskOperationCallbacks&) {
            auto cloned = execute_online_direct_clone(
                request.clone, dependencies);
            if (!cloned) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  cloned.error());
            }
            clone_report = cloned.take_value();
            const auto valid_report = validate_clone_report(
                current, request, *clone_report);
            if (!valid_report) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  valid_report.error());
            }
            auto evidence = clone_evidence_hash(current, *clone_report);
            if (!evidence) {
              return clonecore::Result<
                  operationcore::ExecutionEvidence>::failure(
                  evidence.error());
            }
            return clonecore::Result<
                operationcore::ExecutionEvidence>::success({
                .processed_work_bytes = clone_report->copied_data_bytes,
                .output_hash = evidence.take_value(),
            });
          },
      .verify =
          [&](const operationcore::OperationPlan& current,
              const operationcore::ExecutionEvidence& execution,
              const clonecore::DiskOperationCallbacks&) {
            if (!clone_report) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windows直接クローンOperation最終証跡",
                  L"クローン実行結果がありません");
            }
            const auto valid_report = validate_clone_report(
                current, request, *clone_report);
            if (!valid_report) {
              return clonecore::Result<
                  operationcore::VerificationEvidence>::failure(
                  valid_report.error());
            }
            auto evidence = clone_evidence_hash(current, *clone_report);
            if (!evidence) {
              return clonecore::Result<
                  operationcore::VerificationEvidence>::failure(
                  evidence.error());
            }
            if (execution.output_hash != evidence.value()) {
              return failure<operationcore::VerificationEvidence>(
                  clonecore::ErrorCode::verification_failed,
                  ERROR_CRC,
                  L"Windows直接クローンOperation証跡照合",
                  L"実行時と最終検証時の証跡Hashが一致しません");
            }
            return clonecore::Result<
                operationcore::VerificationEvidence>::success({
                .verified_work_bytes = clone_report->copied_data_bytes,
                .output_hash = evidence.take_value(),
            });
          },
      .disk_operation = request.clone.callbacks,
  };
  auto lifecycle = operationcore::run_operation(
      plan.value(), request.clone.confirmation.typed_token, callbacks);
  return clonecore::Result<OnlineDirectCloneOperationReport>::success({
      .plan = plan.take_value(),
      .lifecycle = std::move(lifecycle),
      .clone = std::move(clone_report),
  });
}

clonecore::Result<OnlineDirectCloneOperationReport>
execute_online_direct_clone_operation_with_windows_apis(
    const OnlineDirectCloneOperationRequest& request) {
  return execute_online_direct_clone_operation(
      request, make_online_direct_clone_windows_dependencies());
}

}  // namespace ytec::windowsapp

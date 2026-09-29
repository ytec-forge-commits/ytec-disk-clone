#include "ytec/operationcore/operation.h"

#include "sha256_internal.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ytec::operationcore {
namespace {

constexpr std::size_t kMaximumModelCharacters = 256U;
constexpr std::size_t kMaximumSerialCharacters = 128U;
constexpr std::size_t kMaximumDeviceIdCharacters = 1024U;

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

bool id_is_zero(const OperationId& id) noexcept {
  unsigned int combined = 0U;
  for (const std::byte value : id) {
    combined |= std::to_integer<unsigned int>(value);
  }
  return combined == 0U;
}

bool known_kind(const OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::clone:
    case OperationKind::image_create:
    case OperationKind::image_restore:
    case OperationKind::boot_repair:
    case OperationKind::rescue_clone:
    case OperationKind::rescue_image:
      return true;
  }
  return false;
}

bool known_environment(const OperationEnvironment environment) noexcept {
  switch (environment) {
    case OperationEnvironment::windows:
    case OperationEnvironment::winpe:
      return true;
  }
  return false;
}

bool allows_bounded_sparse_work(const OperationKind kind) noexcept {
  return kind == OperationKind::clone ||
         kind == OperationKind::rescue_clone;
}

bool has_embedded_null(const std::wstring& value) {
  return value.find(L'\0') != std::wstring::npos;
}

bool has_invalid_serial_character(const std::string& value) {
  return std::any_of(
      value.begin(), value.end(), [](const unsigned char character) {
        return character < 0x20U || character == 0x7FU;
      });
}

clonecore::Status validate_identity_shape(
    const clonecore::StableDiskIdentity& identity,
    const std::wstring_view role) {
  if (identity.model.size() > kMaximumModelCharacters ||
      identity.serial_suffix.size() > kMaximumSerialCharacters ||
      identity.device_instance_id.size() > kMaximumDeviceIdCharacters ||
      has_embedded_null(identity.model) ||
      has_embedded_null(identity.device_instance_id) ||
      identity.serial_suffix.find('\0') != std::string::npos ||
      has_invalid_serial_character(identity.serial_suffix)) {
    return clonecore::Status::failure(operation_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(role) + L"の識別情報",
        L"識別文字列が安全上限を超えるか制御文字を含んでいます"));
  }
  return clonecore::validate_stable_identity(identity, identity, role);
}

bool same_device(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  if (!left.device_instance_id.empty() &&
      left.device_instance_id == right.device_instance_id) {
    return true;
  }
  return !left.serial_suffix.empty() &&
         left.serial_suffix == right.serial_suffix &&
         left.model == right.model;
}

clonecore::Status validate_identity_roles(
    const OperationKind kind,
    const std::optional<clonecore::StableDiskIdentity>& source,
    const std::optional<clonecore::StableDiskIdentity>& target) {
  bool needs_source = false;
  bool needs_target = false;
  switch (kind) {
    case OperationKind::clone:
    case OperationKind::rescue_clone:
      needs_source = true;
      needs_target = true;
      break;
    case OperationKind::image_create:
    case OperationKind::rescue_image:
      needs_source = true;
      break;
    case OperationKind::image_restore:
    case OperationKind::boot_repair:
      needs_target = true;
      break;
    default:
      return clonecore::Status::failure(operation_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"操作種別",
          L"未対応の操作種別です"));
  }

  if (source.has_value() != needs_source || target.has_value() != needs_target) {
    return clonecore::Status::failure(operation_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"操作対象",
        L"操作種別に必要なコピー元またはコピー先の識別情報が一致しません"));
  }
  if (source) {
    const auto status = validate_identity_shape(*source, L"コピー元");
    if (!status) {
      return status;
    }
  }
  if (target) {
    const auto status = validate_identity_shape(*target, L"コピー先");
    if (!status) {
      return status;
    }
  }
  if (source && target && same_device(*source, *target)) {
    return clonecore::Status::failure(operation_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"コピー元とコピー先の分離",
        L"コピー元とコピー先が同一ディスクを示しています"));
  }
  return clonecore::success_status();
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

void append_wstring(std::vector<std::byte>& bytes, const std::wstring& value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t character : value) {
    const auto code_unit = static_cast<std::uint16_t>(character);
    bytes.push_back(static_cast<std::byte>(code_unit & 0xFFU));
    bytes.push_back(static_cast<std::byte>((code_unit >> 8U) & 0xFFU));
  }
}

void append_string(std::vector<std::byte>& bytes, const std::string& value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const unsigned char character : value) {
    bytes.push_back(static_cast<std::byte>(character));
  }
}

void append_identity(
    std::vector<std::byte>& bytes,
    const clonecore::StableDiskIdentity& identity) {
  append_u32(bytes, identity.disk_number);
  append_u64(bytes, identity.size_bytes);
  append_u32(bytes, identity.logical_sector_size);
  append_u8(bytes, identity.is_system_disk ? 1U : 0U);
  append_wstring(bytes, identity.model);
  append_string(bytes, identity.serial_suffix);
  append_wstring(bytes, identity.device_instance_id);
}

OperationOutcome outcome_for_error(const clonecore::Error& error) noexcept {
  return error.code == clonecore::ErrorCode::cancelled
             ? OperationOutcome::cancelled
             : OperationOutcome::failed;
}

OperationResult failed_result(
    const OperationPlan& plan,
    const Sha256Digest& plan_hash,
    const OperationPhase phase,
    clonecore::Error error,
    const std::uint64_t processed = 0U,
    const std::uint64_t verified = 0U) {
  const OperationOutcome outcome = outcome_for_error(error);
  return OperationResult{
      .operation_id = plan.operation_id,
      .plan_hash = plan_hash,
      .phase = phase,
      .outcome = outcome,
      .processed_work_bytes = processed,
      .verified_work_bytes = verified,
      .error = std::move(error),
  };
}

clonecore::Error callback_error(const std::wstring_view operation) {
  return operation_error(
      clonecore::ErrorCode::internal_error,
      ERROR_UNHANDLED_EXCEPTION,
      std::wstring(operation),
      L"Callbackが例外を送出したため安全側に停止しました");
}

clonecore::Error cancelled_error() {
  return operation_error(
      clonecore::ErrorCode::cancelled,
      ERROR_CANCELLED,
      L"操作の取消",
      L"取消要求を確認したため書込み開始前または安全な境界で停止しました");
}

void report_progress(
    const OperationCallbacks& callbacks,
    const clonecore::DiskOperationStage stage,
    const OperationPlan& plan,
    const std::uint64_t processed,
    const std::uint64_t verified,
    const bool cancellation_allowed) noexcept {
  const bool actual_total_known =
      allows_bounded_sparse_work(plan.kind) && processed != 0U &&
      (stage == clonecore::DiskOperationStage::verifying_final ||
       stage == clonecore::DiskOperationStage::completed);
  const std::uint64_t total = actual_total_known
      ? processed
      : plan.expected_work_bytes;
  clonecore::report_disk_operation_progress(
      callbacks.disk_operation,
      clonecore::DiskOperationProgress{
          .stage = stage,
          .partition_index = std::nullopt,
          .total_read_bytes = total,
          .total_write_bytes = total,
          .total_verify_bytes = total,
          .read_bytes = processed,
          .written_bytes = processed,
          .verified_bytes = verified,
          .cancellation_allowed = cancellation_allowed,
      });
}

}  // namespace

clonecore::Status validate_operation_plan(const OperationPlan& plan) {
  if (plan.schema_version != kOperationPlanSchemaVersion ||
      !known_kind(plan.kind) || !known_environment(plan.environment) ||
      id_is_zero(plan.operation_id) || plan.expected_work_bytes == 0U ||
      detail::digest_is_zero(plan.immutable_payload_hash)) {
    return clonecore::Status::failure(operation_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"操作計画",
        L"版、操作ID、処理量、操作種別、環境、または不変条件Hashが不正です"));
  }
  return validate_identity_roles(plan.kind, plan.source, plan.target);
}

clonecore::Result<Sha256Digest> hash_operation_plan(
    const OperationPlan& plan) {
  const auto valid = validate_operation_plan(plan);
  if (!valid) {
    return clonecore::Result<Sha256Digest>::failure(valid.error());
  }

  std::vector<std::byte> bytes;
  bytes.reserve(256U);
  constexpr std::array<std::byte, 8> kMagic{
      std::byte{'Y'}, std::byte{'T'}, std::byte{'E'}, std::byte{'C'},
      std::byte{'O'}, std::byte{'P'}, std::byte{'L'}, std::byte{'1'}};
  append_array(bytes, kMagic);
  append_u32(bytes, plan.schema_version);
  append_u8(bytes, static_cast<std::uint8_t>(plan.kind));
  append_u8(bytes, static_cast<std::uint8_t>(plan.environment));
  append_u8(
      bytes,
      static_cast<std::uint8_t>(
          (plan.source ? 0x01U : 0U) | (plan.target ? 0x02U : 0U)));
  append_u8(bytes, 0U);
  append_u64(bytes, plan.expected_work_bytes);
  append_array(bytes, plan.operation_id);
  append_array(bytes, plan.immutable_payload_hash);
  if (plan.source) {
    append_identity(bytes, *plan.source);
  }
  if (plan.target) {
    append_identity(bytes, *plan.target);
  }
  return detail::sha256(bytes);
}

bool operation_requires_confirmation(const OperationKind kind) noexcept {
  switch (kind) {
    case OperationKind::clone:
    case OperationKind::image_restore:
    case OperationKind::boot_repair:
    case OperationKind::rescue_clone:
      return true;
    case OperationKind::image_create:
    case OperationKind::rescue_image:
      return false;
  }
  return true;
}

clonecore::Status validate_operation_confirmation(
    const OperationPlan& plan,
    const std::wstring_view typed_token) {
  const auto valid = validate_operation_plan(plan);
  if (!valid) {
    return valid;
  }
  if (operation_requires_confirmation(plan.kind) &&
      typed_token != kDestructiveConfirmationToken) {
    return clonecore::Status::failure(operation_error(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"消去対象の最終確認",
        L"確認語は大文字のOKと完全一致する必要があります"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_reidentified_operation(
    const OperationPlan& plan,
    const ReidentifiedOperation& observed) {
  const auto valid = validate_operation_plan(plan);
  if (!valid) {
    return valid;
  }
  if (plan.source.has_value() != observed.source.has_value() ||
      plan.target.has_value() != observed.target.has_value()) {
    return clonecore::Status::failure(operation_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"操作対象の再識別",
        L"選択時と実行直前でディスク対象数が一致しません"));
  }
  if (plan.source) {
    const auto status = clonecore::validate_stable_identity(
        *plan.source, *observed.source, L"コピー元");
    if (!status) {
      return status;
    }
    if (plan.source->is_system_disk != observed.source->is_system_disk) {
      return clonecore::Status::failure(operation_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"コピー元の再識別",
          L"実行中システムディスク判定が選択時から変化しました"));
    }
  }
  if (plan.target) {
    const auto status = clonecore::validate_stable_identity(
        *plan.target, *observed.target, L"コピー先");
    if (!status) {
      return status;
    }
    if (plan.target->is_system_disk != observed.target->is_system_disk) {
      return clonecore::Status::failure(operation_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_NOT_CONNECTED,
          L"コピー先の再識別",
          L"実行中システムディスク判定が選択時から変化しました"));
    }
  }
  if (observed.source && observed.target &&
      same_device(*observed.source, *observed.target)) {
    return clonecore::Status::failure(operation_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"操作対象の再識別",
        L"再識別結果が同一ディスクを指しています"));
  }
  return clonecore::success_status();
}

OperationResult run_operation(
    const OperationPlan& plan,
    const std::wstring_view typed_confirmation,
    const OperationCallbacks& callbacks) {
  Sha256Digest plan_digest{};
  const auto hashed_plan = hash_operation_plan(plan);
  if (!hashed_plan) {
    return failed_result(
        plan, plan_digest, OperationPhase::planning, hashed_plan.error());
  }
  plan_digest = hashed_plan.value();
  if (!callbacks.reidentify || !callbacks.execute || !callbacks.verify) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::planning,
        operation_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"操作Callback",
            L"再識別、実行、検証の全Callbackが必要です"));
  }

  report_progress(
      callbacks,
      clonecore::DiskOperationStage::planning,
      plan,
      0U,
      0U,
      true);
  if (clonecore::disk_operation_cancellation_requested(
          callbacks.disk_operation)) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::reidentifying,
        cancelled_error());
  }

  report_progress(
      callbacks,
      clonecore::DiskOperationStage::verifying_source,
      plan,
      0U,
      0U,
      true);
  clonecore::Result<ReidentifiedOperation> reidentified = [&]() {
    try {
      return callbacks.reidentify(plan);
    } catch (...) {
      return clonecore::Result<ReidentifiedOperation>::failure(
          callback_error(L"操作対象の再識別"));
    }
  }();
  if (!reidentified) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::reidentifying,
        reidentified.error());
  }
  const auto identities =
      validate_reidentified_operation(plan, reidentified.value());
  if (!identities) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::reidentifying,
        identities.error());
  }

  const auto confirmation =
      validate_operation_confirmation(plan, typed_confirmation);
  if (!confirmation) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::awaiting_confirmation,
        confirmation.error());
  }
  if (clonecore::disk_operation_cancellation_requested(
          callbacks.disk_operation)) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::awaiting_confirmation,
        cancelled_error());
  }

  report_progress(
      callbacks,
      clonecore::DiskOperationStage::copying_data,
      plan,
      0U,
      0U,
      true);
  clonecore::Result<ExecutionEvidence> executed = [&]() {
    try {
      return callbacks.execute(plan, callbacks.disk_operation);
    } catch (...) {
      return clonecore::Result<ExecutionEvidence>::failure(
          callback_error(L"操作の実行"));
    }
  }();
  if (!executed) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::executing,
        executed.error());
  }
  const bool execution_amount_valid =
      allows_bounded_sparse_work(plan.kind)
          ? executed.value().processed_work_bytes != 0U &&
                executed.value().processed_work_bytes <=
                    plan.expected_work_bytes
          : executed.value().processed_work_bytes ==
                plan.expected_work_bytes;
  if (!execution_amount_valid ||
      detail::digest_is_zero(executed.value().output_hash)) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::executing,
        operation_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_WRITE_FAULT,
            L"実行証跡",
            L"処理量または出力Hashが操作計画と一致しません"),
        executed.value().processed_work_bytes);
  }
  // A successful execute result means the write boundary has completed.
  // From this point onward a late cancellation must not skip mandatory
  // read-back verification; the concrete engine handles cancellation only at
  // its own safe execution boundaries.
  report_progress(
      callbacks,
      clonecore::DiskOperationStage::verifying_final,
      plan,
      executed.value().processed_work_bytes,
      0U,
      false);
  clonecore::Result<VerificationEvidence> verified = [&]() {
    try {
      return callbacks.verify(
          plan, executed.value(), callbacks.disk_operation);
    } catch (...) {
      return clonecore::Result<VerificationEvidence>::failure(
          callback_error(L"操作結果の検証"));
    }
  }();
  if (!verified) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::verifying,
        verified.error(),
        executed.value().processed_work_bytes);
  }
  const std::uint64_t required_verified_bytes =
      allows_bounded_sparse_work(plan.kind)
          ? executed.value().processed_work_bytes
          : plan.expected_work_bytes;
  if (verified.value().verified_work_bytes != required_verified_bytes ||
      !detail::digest_equal(
          executed.value().output_hash, verified.value().output_hash)) {
    return failed_result(
        plan,
        plan_digest,
        OperationPhase::verifying,
        operation_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"操作結果の読戻し検証",
            L"検証量または読戻しHashが実行証跡と一致しません"),
        executed.value().processed_work_bytes,
        verified.value().verified_work_bytes);
  }

  report_progress(
      callbacks,
      clonecore::DiskOperationStage::completed,
      plan,
      executed.value().processed_work_bytes,
      verified.value().verified_work_bytes,
      false);
  return OperationResult{
      .operation_id = plan.operation_id,
      .plan_hash = plan_digest,
      .phase = OperationPhase::completed,
      .outcome = OperationOutcome::completed,
      .processed_work_bytes = executed.value().processed_work_bytes,
      .verified_work_bytes = verified.value().verified_work_bytes,
      .error = std::nullopt,
  };
}

}  // namespace ytec::operationcore

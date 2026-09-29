#include "ytec/clonecore/disk_identity.h"

#include <Windows.h>

namespace ytec::clonecore {
namespace {

Status failure(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Status::failure(Error{
      .code = code,
      .native_code = native_code,
      .operation = std::move(operation),
      .message = std::move(message),
  });
}

bool has_secondary_identifier(const StableDiskIdentity& identity) {
  return !identity.serial_suffix.empty() || !identity.device_instance_id.empty();
}

bool same_device(const StableDiskIdentity& left, const StableDiskIdentity& right) {
  if (!left.serial_suffix.empty() && !right.serial_suffix.empty() &&
      left.serial_suffix == right.serial_suffix && left.model == right.model) {
    return true;
  }
  return !left.device_instance_id.empty() &&
         left.device_instance_id == right.device_instance_id;
}

}  // namespace

std::wstring make_target_confirmation_token(
    const StableDiskIdentity&) {
  return L"OK";
}

Status validate_stable_identity(
    const StableDiskIdentity& expected,
    const StableDiskIdentity& observed,
    const std::wstring_view role) {
  if (expected.model.empty() || expected.size_bytes == 0 ||
      expected.logical_sector_size == 0 ||
      !has_secondary_identifier(expected)) {
    return failure(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        std::wstring(role) + L"の安定識別",
        L"モデル、容量、セクターサイズ、シリアル末尾またはデバイス識別子が必要です");
  }

  const bool matches =
      expected.model == observed.model &&
      expected.size_bytes == observed.size_bytes &&
      expected.logical_sector_size == observed.logical_sector_size &&
      expected.serial_suffix == observed.serial_suffix &&
      expected.device_instance_id == observed.device_instance_id;
  if (!matches) {
    return failure(
        ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        std::wstring(role) + L"の再識別",
        L"選択時と書込み直前の安定識別情報が一致しません");
  }
  return success_status();
}

Status validate_clone_selection(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& observed_source,
    const StableDiskIdentity& expected_target,
    const StableDiskIdentity& observed_target,
    const bool require_target_same_or_larger) {
  const Status source_status =
      validate_stable_identity(expected_source, observed_source, L"コピー元");
  if (!source_status) {
    return source_status;
  }
  const Status target_status =
      validate_stable_identity(expected_target, observed_target, L"コピー先");
  if (!target_status) {
    return target_status;
  }
  if (same_device(observed_source, observed_target)) {
    return failure(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"コピー元とコピー先の分離",
        L"コピー元とコピー先が同一ディスクを示しています");
  }
  if (observed_target.is_system_disk) {
    return failure(
        ErrorCode::unsupported_layout,
        ERROR_ACCESS_DENIED,
        L"コピー先のシステムディスク保護",
        L"実行中システムのディスクはコピー先にできません");
  }
  if (require_target_same_or_larger &&
      observed_target.size_bytes < observed_source.size_bytes) {
    return failure(
        ErrorCode::unsupported_layout,
        ERROR_DISK_FULL,
        L"コピー先容量の確認",
        L"コピー先ディスクがコピー元より小さいため開始できません");
  }
  if (observed_target.logical_sector_size !=
      observed_source.logical_sector_size) {
    return failure(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"論理セクターサイズの確認",
        L"コピー元とコピー先の論理セクターサイズが一致しません");
  }
  return success_status();
}

Status validate_clone_identities(
    const StableDiskIdentity& expected_source,
    const StableDiskIdentity& observed_source,
    const StableDiskIdentity& expected_target,
    const StableDiskIdentity& observed_target,
    const TargetConfirmation& confirmation,
    const bool require_target_same_or_larger) {
  const Status selection_status = validate_clone_selection(
      expected_source,
      observed_source,
      expected_target,
      observed_target,
      require_target_same_or_larger);
  if (!selection_status) {
    return selection_status;
  }
  if (!confirmation.first_step_acknowledged ||
      confirmation.typed_token !=
          make_target_confirmation_token(observed_target)) {
    return failure(
        ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"コピー先消去の二段階確認",
        L"コピー先の確認操作または入力確認文字列が一致しません");
  }
  return success_status();
}

}  // namespace ytec::clonecore

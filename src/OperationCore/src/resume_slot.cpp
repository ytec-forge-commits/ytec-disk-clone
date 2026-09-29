#include "ytec/operationcore/resume_slot.h"

#include "sha256_internal.h"

#include <Windows.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace ytec::operationcore {
namespace {

clonecore::Error resume_error(
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

clonecore::Status resume_failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Status::failure(resume_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool operation_id_equal(
    const OperationId& left,
    const OperationId& right) noexcept {
  unsigned int difference = 0U;
  for (std::size_t index = 0U; index < left.size(); ++index) {
    difference |= std::to_integer<unsigned int>(left[index] ^ right[index]);
  }
  return difference == 0U;
}

bool identities_equal(
    const ResumeIdentityBinding& left,
    const ResumeIdentityBinding& right) noexcept {
  return detail::digest_equal(
             left.source_identity_hash, right.source_identity_hash) &&
         detail::digest_equal(
             left.target_identity_hash, right.target_identity_hash) &&
         detail::digest_equal(
             left.output_identity_hash, right.output_identity_hash);
}

bool partial_equal(
    const ResumeOwnedPartialBinding& left,
    const ResumeOwnedPartialBinding& right) noexcept {
  return operation_id_equal(left.operation_id, right.operation_id) &&
         identities_equal(left.identities, right.identities) &&
         detail::digest_equal(
             left.file_object_identity_hash,
             right.file_object_identity_hash);
}

bool optional_partial_equal(
    const std::optional<ResumeOwnedPartialBinding>& left,
    const std::optional<ResumeOwnedPartialBinding>& right) noexcept {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  return !left || partial_equal(*left, *right);
}

bool owned_object_role_known(const ResumeOwnedObjectRole role) noexcept {
  switch (role) {
    case ResumeOwnedObjectRole::image_partial:
    case ResumeOwnedObjectRole::image_resume_journal:
    case ResumeOwnedObjectRole::rescue_stage:
      return true;
  }
  return false;
}

bool owned_object_equal(
    const ResumeOwnedObjectBinding& left,
    const ResumeOwnedObjectBinding& right) noexcept {
  return left.role == right.role &&
         operation_id_equal(left.operation_id, right.operation_id) &&
         identities_equal(left.identities, right.identities) &&
         detail::digest_equal(
             left.file_object_identity_hash,
             right.file_object_identity_hash);
}

bool owned_objects_equal(
    const std::vector<ResumeOwnedObjectBinding>& left,
    const std::vector<ResumeOwnedObjectBinding>& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.size(); ++index) {
    if (!owned_object_equal(left[index], right[index])) {
      return false;
    }
  }
  return true;
}

bool owned_object_review_bindings_equal(
    const std::vector<ResumeOwnedObjectReviewBinding>& left,
    const std::vector<ResumeOwnedObjectReviewBinding>& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.size(); ++index) {
    if (left[index].role != right[index].role ||
        !detail::digest_equal(
            left[index].file_object_identity_hash,
            right[index].file_object_identity_hash)) {
      return false;
    }
  }
  return true;
}

bool binding_equal(
    const ResumeSlotBinding& left,
    const ResumeSlotBinding& right) noexcept {
  if (left.capability != right.capability ||
      !operation_id_equal(left.operation_id, right.operation_id) ||
      !identities_equal(left.identities, right.identities) ||
      !detail::digest_equal(
          left.checkpoint_record_hash, right.checkpoint_record_hash) ||
      left.partial_file_object_identity_hash.has_value() !=
          right.partial_file_object_identity_hash.has_value() ||
      !owned_object_review_bindings_equal(
          left.owned_object_file_bindings,
          right.owned_object_file_bindings)) {
    return false;
  }
  return !left.partial_file_object_identity_hash ||
         detail::digest_equal(
             *left.partial_file_object_identity_hash,
             *right.partial_file_object_identity_hash);
}

bool records_equal(
    const ResumeSlotRecord& left,
    const ResumeSlotRecord& right) noexcept {
  return left.capability == right.capability &&
         operation_id_equal(
             left.checkpoint.checkpoint.operation_id,
             right.checkpoint.checkpoint.operation_id) &&
         identities_equal(left.identities, right.identities) &&
         detail::digest_equal(
             left.checkpoint.record_hash,
             right.checkpoint.record_hash) &&
         optional_partial_equal(left.owned_partial, right.owned_partial) &&
         owned_objects_equal(left.owned_objects, right.owned_objects);
}

bool is_fixed_checkpoint_path(const std::wstring& path) {
  if (path.empty() || path.ends_with(L"\\") || path.ends_with(L"/")) {
    return false;
  }
  const std::size_t separator = path.find_last_of(L"\\/");
  const std::wstring_view file_name = separator == std::wstring::npos
      ? std::wstring_view(path)
      : std::wstring_view(path).substr(separator + 1U);
  return file_name == kResumeSlotFileName;
}

clonecore::Status validate_existing_file_proof(
    const ResumeFileStorageProof& proof,
    const std::wstring_view role) {
  if (!proof.exists) {
    return clonecore::success_status();
  }
  if (!proof.is_regular_file || !proof.is_reparse_free ||
      proof.hard_link_count != 1U) {
    return resume_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        std::wstring(role) + L"の配置証明",
        L"通常ファイル、reparseなし、hard link数1を証明できません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_storage_shape(
    const ResumeSlotStorageProof& storage) {
  if (!is_fixed_checkpoint_path(storage.checkpoint_path)) {
    return resume_failure(
        clonecore::ErrorCode::invalid_argument,
        ERROR_BAD_PATHNAME,
        L"単一Resume Slotパス",
        L"固定名active.checkpoint以外のslotは使用できません");
  }
  if (!storage.paths_are_canonical_local ||
      !storage.parent_chain_reparse_free ||
      !storage.placement_separated_from_source ||
      !storage.checkpoint_and_partial_paths_distinct) {
    return resume_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_REPARSE_TAG_INVALID,
        L"単一Resume Slot配置",
        L"canonical local path、親reparse排除、コピー元との配置分離、またはpartialとの分離を証明できません");
  }
  const auto checkpoint = validate_existing_file_proof(
      storage.checkpoint_file, L"Resume Slot checkpoint");
  if (!checkpoint) {
    return checkpoint;
  }
  const auto partial = validate_existing_file_proof(
      storage.owned_partial_file, L"Resume Slot owned partial");
  if (!partial) {
    return partial;
  }
  if (storage.owned_object_files.size() > kMaximumResumeOwnedObjects) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot owned object配置証明",
        L"owned object数が安全上限を超えています");
  }
  for (const auto& object : storage.owned_object_files) {
    const auto valid = validate_existing_file_proof(
        object, L"Resume Slot owned object");
    if (!valid) {
      return valid;
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_observation_relationship(
    const ResumeSlotObservation& observation,
    const bool allow_unattached_partial) {
  if (observation.storage.checkpoint_file.exists !=
      observation.slot.has_value()) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_INVALID,
        L"単一Resume Slot checkpoint観測",
        L"ファイル存在状態と検証済みcheckpoint recordが一致しません");
  }
  if (observation.storage.owned_partial_file.exists !=
      observation.observed_owned_partial.has_value()) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_INVALID,
        L"単一Resume Slot partial観測",
        L"partial存在状態と検証済み所有情報が一致しません");
  }
  if (observation.storage.owned_object_files.size() !=
      observation.observed_owned_objects.size()) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_FILE_INVALID,
        L"Resume Slot owned object観測",
        L"owned objectの配置証明と所有情報の件数が一致しません");
  }
  for (std::size_t index = 0U;
       index < observation.storage.owned_object_files.size(); ++index) {
    if (!observation.storage.owned_object_files[index].exists) {
      return resume_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_FILE_INVALID,
          L"Resume Slot owned object観測",
          L"観測されたowned objectの存在証明が不正です");
    }
  }
  if (!observation.slot) {
    if ((observation.observed_owned_partial ||
         !observation.observed_owned_objects.empty()) &&
        !allow_unattached_partial) {
      return resume_failure(
          clonecore::ErrorCode::access_denied,
          ERROR_FILE_EXISTS,
          L"単一Resume Slot孤立partial",
          L"checkpointに結合されていないpartialは自動取得または削除しません");
    }
    return clonecore::success_status();
  }

  const auto record = validate_resume_slot_record(*observation.slot);
  if (!record) {
    return record;
  }
  if (!optional_partial_equal(
          observation.slot->owned_partial,
          observation.observed_owned_partial)) {
    return resume_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"単一Resume Slot partial所有照合",
        L"checkpointが宣言したpartialと開いたファイルの所有情報が一致しません");
  }
  if (!owned_objects_equal(
          observation.slot->owned_objects,
          observation.observed_owned_objects)) {
    return resume_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Resume Slot owned object所有照合",
        L"checkpointが宣言したowned objectと開いたファイルの所有情報が一致しません");
  }
  return clonecore::success_status();
}

clonecore::Result<ResumeSlotObservation> observation_failure(
    const clonecore::Error& error) {
  return clonecore::Result<ResumeSlotObservation>::failure(error);
}

clonecore::Error callback_exception(const std::wstring_view operation) {
  return resume_error(
      clonecore::ErrorCode::internal_error,
      ERROR_UNHANDLED_EXCEPTION,
      std::wstring(operation),
      L"Resume Slot platform seamが例外を送出したため安全側に停止しました");
}

}  // namespace

clonecore::Result<PersistentPeExactImageCreateObservation>
IResumeSlotPlatform::inspect_persistent_pe_exact_image_create() {
  return clonecore::Result<
      PersistentPeExactImageCreateObservation>::failure(resume_error(
      clonecore::ErrorCode::unsupported_platform,
      ERROR_NOT_SUPPORTED,
      L"Resume Slot WinPE image-create inspect",
      L"このplatformはWinPE通常イメージ作成の永続回復を実装していません"));
}

clonecore::Result<PersistentPeExactImageCreateCommitReport>
IResumeSlotPlatform::commit_persistent_pe_exact_image_create(
    const PersistentPeExactImageCreateCommitRequest&) {
  return clonecore::Result<
      PersistentPeExactImageCreateCommitReport>::failure(resume_error(
      clonecore::ErrorCode::unsupported_platform,
      ERROR_NOT_SUPPORTED,
      L"Resume Slot WinPE image-create commit",
      L"このplatformはWinPE通常イメージ作成の永続確定を実装していません"));
}

ResumeLifetime resume_lifetime(const ResumeCapability capability) noexcept {
  switch (capability) {
    case ResumeCapability::persistent_exact_restore:
    case ResumeCapability::persistent_rescue_restore:
    case ResumeCapability::persistent_pe_exact_image_create:
      return ResumeLifetime::persistent;
    case ResumeCapability::same_process_only_vss_image_create:
    case ResumeCapability::same_process_only_vss_clone:
    case ResumeCapability::same_process_only_pe_image_create:
    case ResumeCapability::same_process_only_pe_clone:
      return ResumeLifetime::same_process_only;
    case ResumeCapability::unsupported_shrink_migration:
    case ResumeCapability::unsupported_raw_rescue:
      return ResumeLifetime::unsupported;
  }
  return ResumeLifetime::unsupported;
}

clonecore::Status validate_resume_capability(
    const ResumeCapability capability,
    const OperationKind kind,
    const OperationEnvironment environment) {
  bool shape_matches = false;
  switch (capability) {
    case ResumeCapability::persistent_exact_restore:
    case ResumeCapability::persistent_rescue_restore:
      shape_matches = kind == OperationKind::image_restore;
      break;
    case ResumeCapability::persistent_pe_exact_image_create:
      shape_matches = kind == OperationKind::image_create &&
                      environment == OperationEnvironment::winpe;
      break;
    case ResumeCapability::same_process_only_vss_image_create:
      shape_matches = kind == OperationKind::image_create &&
                      environment == OperationEnvironment::windows;
      break;
    case ResumeCapability::same_process_only_vss_clone:
      shape_matches = kind == OperationKind::clone &&
                      environment == OperationEnvironment::windows;
      break;
    case ResumeCapability::same_process_only_pe_image_create:
      shape_matches = kind == OperationKind::image_create &&
                      environment == OperationEnvironment::winpe;
      break;
    case ResumeCapability::same_process_only_pe_clone:
      shape_matches = kind == OperationKind::clone &&
                      environment == OperationEnvironment::winpe;
      break;
    case ResumeCapability::unsupported_shrink_migration:
      shape_matches = kind == OperationKind::clone ||
                      kind == OperationKind::image_create ||
                      kind == OperationKind::image_restore;
      break;
    case ResumeCapability::unsupported_raw_rescue:
      shape_matches = kind == OperationKind::rescue_clone ||
                      kind == OperationKind::rescue_image;
      break;
  }
  if (!shape_matches) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume capability",
        L"操作種別または実行環境とResume capabilityが一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_resume_slot_record(
    const ResumeSlotRecord& record) {
  if (resume_lifetime(record.capability) != ResumeLifetime::persistent) {
    return resume_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"永続Resume Slot",
        L"永続再開対応のrestoreまたはWinPE exact image-createだけを保存できます");
  }
  const auto capability = validate_resume_capability(
      record.capability,
      record.checkpoint.checkpoint.kind,
      record.checkpoint.checkpoint.environment);
  if (!capability) {
    return capability;
  }
  if (detail::digest_is_zero(record.identities.source_identity_hash) ||
      detail::digest_is_zero(record.identities.target_identity_hash) ||
      detail::digest_is_zero(record.identities.output_identity_hash) ||
      !detail::digest_equal(
          record.identities.output_identity_hash,
          record.checkpoint.checkpoint.output_identity_hash)) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot識別Hash",
        L"source、target、output Hashが欠落するかcheckpointと一致しません");
  }

  const auto serialized = serialize_checkpoint(record.checkpoint.checkpoint);
  if (!serialized) {
    return clonecore::Status::failure(serialized.error());
  }
  const auto reparsed = parse_checkpoint(serialized.value());
  if (!reparsed ||
      !detail::digest_equal(
          reparsed.value().record_hash,
          record.checkpoint.record_hash)) {
    return resume_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Resume Slot checkpoint record Hash",
        L"checkpoint内容とrecord Hashが一致しません");
  }

  if (record.owned_partial) {
    if (!operation_id_equal(
            record.checkpoint.checkpoint.operation_id,
            record.owned_partial->operation_id) ||
        !identities_equal(
            record.identities, record.owned_partial->identities) ||
        detail::digest_is_zero(
            record.owned_partial->file_object_identity_hash)) {
      return resume_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot owned partial",
          L"operation ID、identity Hash、またはfile-object Hashが一致しません");
    }
  }
  if (record.owned_partial && !record.owned_objects.empty()) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot owned object形式",
        L"旧partial束縛とschema v3の複数object束縛は併用できません");
  }
  if (record.owned_objects.size() > kMaximumResumeOwnedObjects) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Resume Slot owned object件数",
        L"owned object数が安全上限を超えています");
  }
  for (std::size_t index = 0U; index < record.owned_objects.size(); ++index) {
    const auto& object = record.owned_objects[index];
    if (!owned_object_role_known(object.role) ||
        !operation_id_equal(
            record.checkpoint.checkpoint.operation_id,
            object.operation_id) ||
        !identities_equal(record.identities, object.identities) ||
        detail::digest_is_zero(object.file_object_identity_hash)) {
      return resume_failure(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"Resume Slot owned object",
          L"role、operation ID、identity Hash、またはfile-object Hashが不正です");
    }
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (record.owned_objects[prior].role == object.role) {
        return resume_failure(
            clonecore::ErrorCode::invalid_data,
            ERROR_DUP_NAME,
            L"Resume Slot owned object role",
            L"同じowned object roleを複数束縛できません");
      }
    }
    if (index != 0U &&
        static_cast<std::uint8_t>(record.owned_objects[index - 1U].role) >=
            static_cast<std::uint8_t>(object.role)) {
      return resume_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Resume Slot owned object role順",
          L"owned objectはroleの昇順でcanonicalに保存する必要があります");
    }
  }
  if (record.capability ==
      ResumeCapability::persistent_pe_exact_image_create) {
    bool has_partial = false;
    bool has_journal = false;
    for (const auto& object : record.owned_objects) {
      has_partial = has_partial ||
          object.role == ResumeOwnedObjectRole::image_partial;
      has_journal = has_journal ||
          object.role == ResumeOwnedObjectRole::image_resume_journal;
    }
    if (record.checkpoint.checkpoint.schema_version !=
            kCheckpointSchemaVersionV3 ||
        record.owned_objects.size() != 2U || !has_partial || !has_journal ||
        record.owned_partial) {
      return resume_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"WinPE image-create Resume Slot",
          L"schema v3とimage partial/journalの2オブジェク完全束縛が必要です");
    }
  } else if (!record.owned_objects.empty()) {
    return resume_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_NOT_SUPPORTED,
        L"Resume Slot owned object capability",
        L"複数owned object束縛はWinPE exact image-create専用です");
  }
  return clonecore::success_status();
}

clonecore::Result<ResumeSlotBinding> make_resume_slot_binding(
    const ResumeSlotRecord& record) {
  const auto valid = validate_resume_slot_record(record);
  if (!valid) {
    return clonecore::Result<ResumeSlotBinding>::failure(valid.error());
  }
  return clonecore::Result<ResumeSlotBinding>::success(ResumeSlotBinding{
      .capability = record.capability,
      .operation_id = record.checkpoint.checkpoint.operation_id,
      .identities = record.identities,
      .checkpoint_record_hash = record.checkpoint.record_hash,
      .partial_file_object_identity_hash = record.owned_partial
          ? std::optional<Sha256Digest>(
                record.owned_partial->file_object_identity_hash)
          : std::nullopt,
      .owned_object_file_bindings = [&record]() {
        std::vector<ResumeOwnedObjectReviewBinding> bindings;
        bindings.reserve(record.owned_objects.size());
        for (const auto& object : record.owned_objects) {
          bindings.push_back(ResumeOwnedObjectReviewBinding{
              .role = object.role,
              .file_object_identity_hash =
                  object.file_object_identity_hash,
          });
        }
        return bindings;
      }(),
  });
}

SingleResumeSlot::SingleResumeSlot(IResumeSlotPlatform& platform) noexcept
    : platform_(&platform) {}

clonecore::Result<ResumeSlotObservation> SingleResumeSlot::observe() {
  clonecore::Result<ResumeSlotObservation> observed = [&]() {
    try {
      return platform_->observe_fixed_slot();
    } catch (...) {
      return observation_failure(callback_exception(L"Resume Slot観測"));
    }
  }();
  if (!observed) {
    return observed;
  }
  const auto storage = validate_storage_shape(observed.value().storage);
  if (!storage) {
    return observation_failure(storage.error());
  }
  if (bound_checkpoint_path_ &&
      *bound_checkpoint_path_ != observed.value().storage.checkpoint_path) {
    return observation_failure(resume_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_BAD_PATHNAME,
        L"単一Resume Slot固定パス",
        L"同一Controller内で別pathのslotへ切り替えることはできません"));
  }
  if (!bound_checkpoint_path_) {
    bound_checkpoint_path_ = observed.value().storage.checkpoint_path;
  }
  return observed;
}

clonecore::Result<std::optional<ResumeSlotRecord>>
SingleResumeSlot::inspect() {
  auto observed = observe();
  if (!observed) {
    return clonecore::Result<std::optional<ResumeSlotRecord>>::failure(
        observed.error());
  }
  const auto relationship =
      validate_observation_relationship(observed.value(), false);
  if (!relationship) {
    return clonecore::Result<std::optional<ResumeSlotRecord>>::failure(
        relationship.error());
  }
  return clonecore::Result<std::optional<ResumeSlotRecord>>::success(
      std::move(observed.value().slot));
}

clonecore::Status SingleResumeSlot::guard_new_operation_start(
    const OperationPlan& plan) {
  const auto valid_plan = validate_operation_plan(plan);
  if (!valid_plan) {
    return valid_plan;
  }

  auto active = inspect();
  if (!active) {
    return clonecore::Status::failure(active.error());
  }
  if (active.value()) {
    return resume_failure(
        clonecore::ErrorCode::access_denied,
        ERROR_BUSY,
        L"新規操作開始の単一Resume Slot gate",
        L"前回中断した処理が残っているため新規OperationPlanを開始できません。"
        L"同じslotを完全拘束したresumeまたはowned discardだけを選択してください");
  }
  return clonecore::success_status();
}

clonecore::Result<ResumeSlotRecord> SingleResumeSlot::open_bound(
    const ResumeSlotBinding& expected) {
  auto current = inspect();
  if (!current) {
    return clonecore::Result<ResumeSlotRecord>::failure(current.error());
  }
  if (!current.value()) {
    return clonecore::Result<ResumeSlotRecord>::failure(resume_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_NOT_FOUND,
        L"Resume Slot拘束",
        L"再開対象のcheckpointが存在しません"));
  }
  const auto actual = make_resume_slot_binding(*current.value());
  if (!actual || !binding_equal(actual.value(), expected)) {
    return clonecore::Result<ResumeSlotRecord>::failure(resume_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Resume Slot拘束",
        L"capability、operation ID、source/target/output、record、またはpartial Hashが一致しません"));
  }
  return clonecore::Result<ResumeSlotRecord>::success(
      std::move(*current.value()));
}

clonecore::Status SingleResumeSlot::create(
    const ResumeSlotRecord& record) {
  const auto valid = validate_resume_slot_record(record);
  if (!valid) {
    return valid;
  }
  auto before = observe();
  if (!before) {
    return clonecore::Status::failure(before.error());
  }
  const auto relationship =
      validate_observation_relationship(before.value(), true);
  if (!relationship) {
    return relationship;
  }
  if (before.value().slot) {
    return resume_failure(
        clonecore::ErrorCode::access_denied,
        ERROR_FILE_EXISTS,
        L"Resume Slot新規作成",
        L"既存slotは上書きせず、同時に複数の中断処理を保持しません");
  }
  if (!optional_partial_equal(
          record.owned_partial,
          before.value().observed_owned_partial)) {
    return resume_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Resume Slot新規partial拘束",
        L"宣言したowned partialをfile-object identityで確認できません");
  }
  if (!owned_objects_equal(
          record.owned_objects,
          before.value().observed_owned_objects)) {
    return resume_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"Resume Slot新規owned object拘束",
        L"宣言したowned objectをroleとfile-object identityで確認できません");
  }

  clonecore::Status created = [&]() {
    try {
      return platform_->create_fixed_slot(record);
    } catch (...) {
      return clonecore::Status::failure(
          callback_exception(L"Resume Slot新規作成"));
    }
  }();
  if (!created) {
    return created;
  }
  auto after = inspect();
  if (!after || !after.value() || !records_equal(*after.value(), record)) {
    return resume_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Resume Slot新規作成読戻し",
        L"確定後のslotを同じ固定pathから完全一致で確認できません");
  }
  return clonecore::success_status();
}

clonecore::Status SingleResumeSlot::replace(
    const ResumeSlotBinding& expected,
    const ParsedCheckpoint& next_checkpoint) {
  auto current = open_bound(expected);
  if (!current) {
    return clonecore::Status::failure(current.error());
  }
  const auto transition = validate_checkpoint_transition(
      current.value().checkpoint.checkpoint,
      next_checkpoint.checkpoint);
  if (!transition) {
    return transition;
  }
  ResumeSlotRecord next = current.value();
  next.checkpoint = next_checkpoint;
  const auto next_valid = validate_resume_slot_record(next);
  if (!next_valid) {
    return next_valid;
  }

  clonecore::Status replaced = [&]() {
    try {
      return platform_->replace_fixed_slot(
          expected.checkpoint_record_hash, next);
    } catch (...) {
      return clonecore::Status::failure(
          callback_exception(L"Resume Slot置換"));
    }
  }();
  if (!replaced) {
    return replaced;
  }
  auto after = inspect();
  if (!after || !after.value() || !records_equal(*after.value(), next)) {
    return resume_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Resume Slot置換読戻し",
        L"置換後のslotを同じ固定pathから完全一致で確認できません");
  }
  return clonecore::success_status();
}

clonecore::Status SingleResumeSlot::discard(
    const ResumeSlotBinding& expected) {
  auto current = open_bound(expected);
  if (!current) {
    return clonecore::Status::failure(current.error());
  }

  clonecore::Status discarded = [&]() {
    try {
      return platform_->discard_fixed_slot_and_owned_partial(expected);
    } catch (...) {
      return clonecore::Status::failure(
          callback_exception(L"Resume Slot破棄"));
    }
  }();
  if (!discarded) {
    return discarded;
  }
  auto after = observe();
  if (!after) {
    return clonecore::Status::failure(after.error());
  }
  const auto relationship =
      validate_observation_relationship(after.value(), false);
  if (!relationship || after.value().slot ||
      after.value().observed_owned_partial ||
      !after.value().observed_owned_objects.empty()) {
    return resume_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_FILE_INVALID,
        L"Resume Slot破棄読戻し",
        L"破棄後もcheckpointまたは宣言済みowned partialが残っています");
  }
  return clonecore::success_status();
}

}  // namespace ytec::operationcore

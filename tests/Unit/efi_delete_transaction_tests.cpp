#include "ytec/bootrepair/efi_delete_transaction.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::bootrepair::EfiDeleteCandidateObservation;
using ytec::bootrepair::EfiDeleteEntryKind;
using ytec::bootrepair::EfiDeleteMutationExtent;
using ytec::bootrepair::EfiDeletePlatformFailureKind;
using ytec::bootrepair::EfiDeletePlatformStepResult;
using ytec::bootrepair::EfiDeleteReviewObservation;
using ytec::bootrepair::EfiDeleteTransactionOutcome;
using ytec::clonecore::Error;
using ytec::clonecore::ErrorCode;
using ytec::clonecore::Result;

int failures = 0;

void check(const bool condition, const char* const message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

Error test_error(
    const wchar_t* const operation,
    const ErrorCode code = ErrorCode::io_failed) {
  return Error{
      .code = code,
      .native_code = ERROR_GEN_FAILURE,
      .operation = operation,
      .message = L"synthetic failure",
  };
}

ytec::bootrepair::EfiDeleteFileId file_id(const unsigned char seed) {
  ytec::bootrepair::EfiDeleteFileId value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>(seed + index);
  }
  return value;
}

ytec::bootrepair::EfiDeleteSha256 digest(const unsigned char seed) {
  ytec::bootrepair::EfiDeleteSha256 value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>(seed + index);
  }
  return value;
}

ytec::bootrepair::EfiDeleteTreeEntryObservation directory(
    std::wstring path,
    const unsigned char id_seed) {
  return ytec::bootrepair::EfiDeleteTreeEntryObservation{
      .kind = EfiDeleteEntryKind::directory,
      .relative_path = std::move(path),
      .volume_serial_number = 0x11223344ULL,
      .file_id = file_id(id_seed),
      .file_id_valid = true,
      .size_bytes = 0U,
      .creation_time = 100U + id_seed,
      .last_access_time = 150U + id_seed,
      .last_write_time = 200U + id_seed,
      .change_time = 300U + id_seed,
      .hard_link_count = 1U,
  };
}

ytec::bootrepair::EfiDeleteTreeEntryObservation file(
    std::wstring path,
    const unsigned char id_seed,
    const std::uint64_t size = 4'096U) {
  return ytec::bootrepair::EfiDeleteTreeEntryObservation{
      .kind = EfiDeleteEntryKind::regular_file,
      .relative_path = std::move(path),
      .volume_serial_number = 0x11223344ULL,
      .file_id = file_id(id_seed),
      .file_id_valid = true,
      .size_bytes = size,
      .creation_time = 400U + id_seed,
      .last_access_time = 450U + id_seed,
      .last_write_time = 500U + id_seed,
      .change_time = 600U + id_seed,
      .hard_link_count = 1U,
      .file_sha256 = digest(id_seed),
      .file_sha256_valid = true,
  };
}

EfiDeleteReviewObservation valid_observation() {
  EfiDeleteCandidateObservation vendor_a{
      .relative_name = L"VendorA",
      .entries = {
          directory(L"VendorA", 1U),
          directory(L"VendorA\\Tools", 2U),
          file(L"VendorA\\Tools\\loader.efi", 3U),
      },
  };
  EfiDeleteCandidateObservation vendor_b{
      .relative_name = L"VendorB",
      .entries = {
          directory(L"VendorB", 11U),
          file(L"VendorB\\boot.efi", 12U, 8'192U),
      },
  };
  return EfiDeleteReviewObservation{
      .disk = ytec::clonecore::StableDiskIdentity{
          .disk_number = 7U,
          .model = L"Synthetic GPT disk",
          .size_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL,
          .logical_sector_size = 512U,
          .serial_suffix = "A1B2C3D4",
          .device_instance_id = L"SYNTHETIC\\DISK7",
          .is_system_disk = false,
      },
      .esp = ytec::bootrepair::EfiDeleteEspIdentity{
          .partition_number = 1U,
          .offset_bytes = 2'048ULL * 512ULL,
          .length_bytes = 260ULL * 1024ULL * 1024ULL,
          .partition_identifier =
              L"{11111111-2222-3333-4444-555555555555}",
          .partition_type_identifier =
              L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
          .partition_attributes = 0x8000000000000001ULL,
          .volume_guid_root =
              L"\\\\?\\Volume{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}\\",
          .filesystem_name = L"FAT32",
          .volume_serial_number = 0x11223344ULL,
      },
      .ownership = ytec::bootrepair::EfiBootOwnershipEvidence{
          .state = ytec::bootrepair::EfiBootOwnershipState::
              non_microsoft_or_untrusted_present,
          .efi_directory_present = true,
          .microsoft_namespace_present = true,
          .boot_namespace_present = true,
          .fallback_loader_present = true,
          .fallback_loader_microsoft_signed = true,
          .microsoft_signed_efi_loader_count = 3U,
          .non_microsoft_or_untrusted_entry_count = 2U,
          .top_level_non_microsoft_namespace_count = 2U,
          .boot_namespace_nonstandard_entry_count = 0U,
          .non_microsoft_or_untrusted_efi_loader_count = 0U,
      },
      .bounded_top_level_enumeration_complete = true,
      .candidates = {std::move(vendor_a), std::move(vendor_b)},
  };
}

class MockPlatform final
    : public ytec::bootrepair::IEfiDeleteTransactionPlatform {
 public:
  explicit MockPlatform(EfiDeleteReviewObservation observation)
      : fresh(std::move(observation)) {
    quarantine_identity.volume_serial_number =
        fresh.esp.volume_serial_number;
    quarantine_identity.file_id = file_id(240U);
  }

  Result<EfiDeleteReviewObservation> inspect_candidates_read_only(
      const ytec::clonecore::StableDiskIdentity&,
      const ytec::bootrepair::EfiDeleteEspIdentity&) override {
    events.push_back(L"inspect");
    if (inspection_failure.has_value()) {
      return Result<EfiDeleteReviewObservation>::failure(
          *inspection_failure);
    }
    return Result<EfiDeleteReviewObservation>::success(fresh);
  }

  ytec::bootrepair::EfiDeleteQuarantineCreateResult
  create_owned_quarantine_no_replace(
      const ytec::bootrepair::ReviewedEfiDeletePlan&) override {
    events.push_back(L"create-quarantine");
    if (create_failure.has_value()) {
      return {*create_failure, {}};
    }
    return {
        EfiDeletePlatformStepResult::completed(),
        quarantine_identity,
    };
  }

  EfiDeletePlatformStepResult move_candidate_to_quarantine_handle_bound(
      const ytec::bootrepair::ReviewedEfiDeletePlan&,
      const std::size_t candidate_index,
      const ytec::bootrepair::EfiDeleteCandidateManifest& candidate,
      const ytec::bootrepair::EfiDeleteObjectIdentity&) override {
    events.push_back(L"move:" + candidate.relative_name);
    if (fail_move_index.has_value() &&
        candidate_index == *fail_move_index) {
      return move_failure.value();
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult
  rollback_candidate_from_quarantine_handle_bound(
      const ytec::bootrepair::ReviewedEfiDeletePlan&,
      const std::size_t candidate_index,
      const ytec::bootrepair::EfiDeleteCandidateManifest& candidate,
      const ytec::bootrepair::EfiDeleteObjectIdentity&) override {
    events.push_back(L"rollback:" + candidate.relative_name);
    if (fail_rollback_index.has_value() &&
        candidate_index == *fail_rollback_index) {
      return rollback_failure.value();
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult rebuild_microsoft_bcd_and_verify_readback(
      const ytec::bootrepair::ReviewedEfiDeletePlan&) override {
    events.push_back(L"bcd-readback");
    if (bcd_failure.has_value()) {
      return *bcd_failure;
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult
  rollback_microsoft_bcd_rebuild_if_identity_matches(
      const ytec::bootrepair::ReviewedEfiDeletePlan&) override {
    events.push_back(L"bcd-rollback");
    if (bcd_rollback_failure.has_value()) {
      return *bcd_rollback_failure;
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult commit_microsoft_bcd_rebuild(
      const ytec::bootrepair::ReviewedEfiDeletePlan&) override {
    events.push_back(L"bcd-commit");
    if (bcd_commit_failure.has_value()) {
      return *bcd_commit_failure;
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult
  delete_quarantined_candidate_tree_handle_bound(
      const ytec::bootrepair::ReviewedEfiDeletePlan&,
      const std::size_t candidate_index,
      const ytec::bootrepair::EfiDeleteCandidateManifest& candidate,
      const ytec::bootrepair::EfiDeleteObjectIdentity&) override {
    events.push_back(L"delete:" + candidate.relative_name);
    if (fail_delete_index.has_value() &&
        candidate_index == *fail_delete_index) {
      return delete_failure.value();
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeletePlatformStepResult
  remove_owned_quarantine_if_empty_handle_bound(
      const ytec::bootrepair::ReviewedEfiDeletePlan&,
      const ytec::bootrepair::EfiDeleteObjectIdentity&) override {
    events.push_back(L"remove-quarantine");
    if (cleanup_failure.has_value()) {
      return *cleanup_failure;
    }
    return EfiDeletePlatformStepResult::completed();
  }

  EfiDeleteReviewObservation fresh;
  ytec::bootrepair::EfiDeleteObjectIdentity quarantine_identity;
  std::optional<Error> inspection_failure;
  std::optional<EfiDeletePlatformStepResult> create_failure;
  std::optional<std::size_t> fail_move_index;
  std::optional<EfiDeletePlatformStepResult> move_failure;
  std::optional<std::size_t> fail_rollback_index;
  std::optional<EfiDeletePlatformStepResult> rollback_failure;
  std::optional<EfiDeletePlatformStepResult> bcd_failure;
  std::optional<EfiDeletePlatformStepResult> bcd_rollback_failure;
  std::optional<EfiDeletePlatformStepResult> bcd_commit_failure;
  std::optional<std::size_t> fail_delete_index;
  std::optional<EfiDeletePlatformStepResult> delete_failure;
  std::optional<EfiDeletePlatformStepResult> cleanup_failure;
  std::vector<std::wstring> events;
};

ytec::bootrepair::EfiDeleteConfirmation confirmed() {
  return ytec::bootrepair::EfiDeleteConfirmation{
      .destructive_warning_acknowledged = true,
      .typed_token = L"OK",
  };
}

EfiDeletePlatformStepResult failed_step(
    const EfiDeletePlatformFailureKind kind,
    const EfiDeleteMutationExtent extent,
    const wchar_t* const operation) {
  return EfiDeletePlatformStepResult::failed(
      kind, extent, test_error(operation));
}

void test_manifest_is_canonical_and_complete() {
  const auto original = valid_observation();
  auto reordered = original;
  std::reverse(reordered.candidates.begin(), reordered.candidates.end());
  for (auto& candidate : reordered.candidates) {
    std::reverse(candidate.entries.begin(), candidate.entries.end());
  }
  const auto first = ytec::bootrepair::review_efi_delete_candidates(original);
  const auto second =
      ytec::bootrepair::review_efi_delete_candidates(reordered);
  check(first.has_value() && second.has_value(),
        "valid bounded EFI trees should review");
  if (!first || !second) {
    return;
  }
  MockPlatform read_only_inspector(original);
  const auto inspected =
      ytec::bootrepair::review_efi_delete_candidates_read_only(
          original.disk, original.esp, read_only_inspector);
  check(inspected.has_value() &&
            read_only_inspector.events ==
                std::vector<std::wstring>{L"inspect"},
        "review entry point must use exactly one read-only inspection");
  check(ytec::bootrepair::equivalent_efi_delete_manifest(
            first.value(), second.value()),
        "enumeration order must not change the immutable manifest");
  check(first.value().candidates().front().relative_name == L"VendorA",
        "candidate order must be canonical");
  check(first.value().candidates().front().entries.front().relative_path ==
            L"VendorA",
        "candidate root must be present in the canonical tree");
  check(ytec::bootrepair::efi_delete_source_relative_path(
            first.value().candidates().front()) == L"EFI\\VendorA",
        "source path must be constrained below EFI");
  const auto slot =
      ytec::bootrepair::efi_delete_quarantine_slot_relative_path(0U);
  check(slot.has_value() &&
            slot.value() ==
                L"YTEC-EFI-REMOVE-QUARANTINE-V1\\C0000",
        "quarantine slot must use the fixed ESP-root namespace");

  auto changed = original;
  changed.candidates[0].entries[2].last_write_time += 1U;
  const auto changed_plan =
      ytec::bootrepair::review_efi_delete_candidates(changed);
  check(changed_plan.has_value() &&
            !ytec::bootrepair::equivalent_efi_delete_manifest(
                first.value(), changed_plan.value()),
        "entry times must be bound into the immutable manifest");

  auto changed_identity = original;
  changed_identity.candidates[0].entries[2].file_id[0] ^=
      std::byte{0x01};
  const auto identity_plan =
      ytec::bootrepair::review_efi_delete_candidates(changed_identity);
  check(identity_plan.has_value() &&
            identity_plan.value().manifest_sha256() !=
                first.value().manifest_sha256(),
        "FileId must be bound into the immutable manifest hash");

  auto changed_size = original;
  ++changed_size.candidates[0].entries[2].size_bytes;
  const auto size_plan =
      ytec::bootrepair::review_efi_delete_candidates(changed_size);
  check(size_plan.has_value() &&
            size_plan.value().manifest_sha256() !=
                first.value().manifest_sha256(),
        "file size must be bound into the immutable manifest hash");

  auto changed_path = original;
  changed_path.candidates[1].entries[1].relative_path =
      L"VendorB\\renamed.efi";
  const auto path_plan =
      ytec::bootrepair::review_efi_delete_candidates(changed_path);
  check(path_plan.has_value() &&
            path_plan.value().manifest_sha256() !=
                first.value().manifest_sha256(),
        "relative path must be bound into the immutable manifest hash");

  auto changed_type = original;
  auto& changed_entry = changed_type.candidates[1].entries[1];
  changed_entry.kind = EfiDeleteEntryKind::directory;
  changed_entry.size_bytes = 0U;
  changed_entry.file_sha256 = {};
  changed_entry.file_sha256_valid = false;
  const auto type_plan =
      ytec::bootrepair::review_efi_delete_candidates(changed_type);
  check(type_plan.has_value() &&
            type_plan.value().manifest_sha256() !=
                first.value().manifest_sha256(),
        "entry type must be bound into the immutable manifest hash");
}

void test_review_rejects_unsafe_names_and_object_types() {
  auto wrong_partition_type = valid_observation();
  wrong_partition_type.esp.partition_type_identifier =
      L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
  check(!ytec::bootrepair::review_efi_delete_candidates(
            wrong_partition_type),
        "a non-ESP GPT partition type must fail closed");

  auto wrong_filesystem = valid_observation();
  wrong_filesystem.esp.filesystem_name = L"NTFS";
  check(!ytec::bootrepair::review_efi_delete_candidates(wrong_filesystem),
        "a non-FAT32 filesystem must fail closed");

  auto incomplete = valid_observation();
  incomplete.bounded_top_level_enumeration_complete = false;
  check(!ytec::bootrepair::review_efi_delete_candidates(incomplete),
        "incomplete EFI top-level enumeration must fail closed");

  auto omitted_namespace = valid_observation();
  omitted_namespace.ownership.top_level_non_microsoft_namespace_count = 3U;
  omitted_namespace.ownership.non_microsoft_or_untrusted_entry_count = 3U;
  check(!ytec::bootrepair::review_efi_delete_candidates(omitted_namespace),
        "ownership count must prove that no candidate was omitted");

  auto microsoft = valid_observation();
  microsoft.candidates[0].relative_name = L"mIcRoSoFt";
  microsoft.candidates[0].entries[0].relative_path = L"mIcRoSoFt";
  check(!ytec::bootrepair::review_efi_delete_candidates(microsoft),
        "EFI Microsoft must never be a candidate");

  auto dotdot = valid_observation();
  dotdot.candidates[0].entries[2].relative_path =
      L"VendorA\\..\\escape.efi";
  check(!ytec::bootrepair::review_efi_delete_candidates(dotdot),
        "dot-dot path escape must fail closed");

  auto reserved = valid_observation();
  reserved.candidates[0].relative_name = L"CON";
  reserved.candidates[0].entries[0].relative_path = L"CON";
  check(!ytec::bootrepair::review_efi_delete_candidates(reserved),
        "reserved DOS names must fail closed");

  auto reparse = valid_observation();
  reparse.candidates[0].entries[1].kind = EfiDeleteEntryKind::reparse;
  check(!ytec::bootrepair::review_efi_delete_candidates(reparse),
        "reparse entries must never become actionable");

  auto hardlink = valid_observation();
  hardlink.candidates[0].entries[2].hard_link_count = 2U;
  check(!ytec::bootrepair::review_efi_delete_candidates(hardlink),
        "hard-linked files must fail closed");

  auto missing_hash = valid_observation();
  missing_hash.candidates[0].entries[2].file_sha256_valid = false;
  check(!ytec::bootrepair::review_efi_delete_candidates(missing_hash),
        "every regular file must have a SHA-256");

  auto duplicate_id = valid_observation();
  duplicate_id.candidates[1].entries[1].file_id =
      duplicate_id.candidates[0].entries[2].file_id;
  check(!ytec::bootrepair::review_efi_delete_candidates(duplicate_id),
        "duplicate FileIds must fail even if link count claims one");
}

void test_confirmation_and_fresh_identity_stop_before_writes() {
  const auto observation = valid_observation();
  const auto reviewed =
      ytec::bootrepair::review_efi_delete_candidates(observation);
  check(reviewed.has_value(), "fixture should review");
  if (!reviewed) {
    return;
  }

  MockPlatform lowercase(observation);
  auto confirmation = confirmed();
  confirmation.typed_token = L"ok";
  const auto rejected = ytec::bootrepair::execute_efi_delete_transaction(
      reviewed.value(), confirmation, lowercase);
  check(rejected.outcome == EfiDeleteTransactionOutcome::stopped_before_mutation,
        "lowercase confirmation must stop before mutation");
  check(lowercase.events.empty(),
        "confirmation rejection must make zero platform calls");

  auto changed_identity = observation;
  changed_identity.disk.serial_suffix = "DIFFERENT";
  MockPlatform drift(std::move(changed_identity));
  const auto identity_failure =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), drift);
  check(identity_failure.outcome ==
            EfiDeleteTransactionOutcome::stopped_before_mutation &&
            identity_failure.platform_failure ==
                EfiDeletePlatformFailureKind::tamper_detected,
        "stable disk drift must stop before quarantine creation");
  check(drift.events == std::vector<std::wstring>{L"inspect"},
        "identity drift must perform only fresh read-only inspection");

  auto changed_esp = observation;
  changed_esp.esp.partition_attributes ^= 0x1ULL;
  MockPlatform esp_drift(std::move(changed_esp));
  const auto esp_identity_failure =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), esp_drift);
  check(esp_identity_failure.outcome ==
            EfiDeleteTransactionOutcome::stopped_before_mutation &&
            esp_identity_failure.platform_failure ==
                EfiDeletePlatformFailureKind::tamper_detected,
        "GPT ESP attribute drift must stop before quarantine creation");
  check(esp_drift.events == std::vector<std::wstring>{L"inspect"},
        "ESP identity drift must remain read-only");

  auto changed_tree = observation;
  changed_tree.candidates[0].entries[2].file_sha256[0] ^= std::byte{0x01};
  MockPlatform tamper(std::move(changed_tree));
  const auto manifest_failure =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), tamper);
  check(manifest_failure.outcome ==
            EfiDeleteTransactionOutcome::stopped_before_mutation &&
            manifest_failure.platform_failure ==
                EfiDeletePlatformFailureKind::tamper_detected,
        "tree hash drift must stop before quarantine creation");
  check(tamper.events == std::vector<std::wstring>{L"inspect"},
        "tree tamper must remain read-only");

  auto changed_ownership = observation;
  ++changed_ownership.ownership.microsoft_signed_efi_loader_count;
  MockPlatform ownership_tamper(std::move(changed_ownership));
  const auto ownership_failure =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), ownership_tamper);
  check(ownership_failure.outcome ==
            EfiDeleteTransactionOutcome::stopped_before_mutation &&
            ownership_failure.platform_failure ==
                EfiDeletePlatformFailureKind::tamper_detected,
        "protected EFI ownership drift must stop before quarantine creation");
}

void test_foreign_quarantine_and_move_race_are_distinct() {
  const auto observation = valid_observation();
  const auto reviewed =
      ytec::bootrepair::review_efi_delete_candidates(observation);
  check(reviewed.has_value(), "fixture should review");
  if (!reviewed) {
    return;
  }

  MockPlatform foreign(observation);
  foreign.create_failure = failed_step(
      EfiDeletePlatformFailureKind::foreign_object,
      EfiDeleteMutationExtent::none,
      L"foreign quarantine");
  const auto foreign_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), foreign);
  check(foreign_report.outcome ==
            EfiDeleteTransactionOutcome::stopped_before_mutation &&
            foreign_report.platform_failure ==
                EfiDeletePlatformFailureKind::foreign_object,
        "pre-existing quarantine must be reported as foreign, not deleted");

  MockPlatform race(observation);
  race.fail_move_index = 1U;
  race.move_failure = failed_step(
      EfiDeletePlatformFailureKind::race_detected,
      EfiDeleteMutationExtent::none,
      L"rename race");
  const auto race_report = ytec::bootrepair::execute_efi_delete_transaction(
      reviewed.value(), confirmed(), race);
  check(race_report.outcome == EfiDeleteTransactionOutcome::rolled_back &&
            race_report.platform_failure ==
                EfiDeletePlatformFailureKind::race_detected &&
            race_report.rolled_back_candidates == 1U,
        "move race with no current mutation must roll prior moves back exactly");
  check(std::find(
            race.events.begin(), race.events.end(), L"bcd-readback") ==
            race.events.end(),
        "BCDBoot must not run after a quarantine move race");

  MockPlatform invalid_quarantine_identity(observation);
  invalid_quarantine_identity.quarantine_identity.volume_serial_number ^= 1U;
  const auto invalid_identity_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), invalid_quarantine_identity);
  check(invalid_identity_report.outcome ==
            EfiDeleteTransactionOutcome::partial_rollback &&
            invalid_identity_report.failure_stage ==
                ytec::bootrepair::EfiDeleteFailureStage::platform_contract &&
            invalid_identity_report.platform_failure ==
                EfiDeletePlatformFailureKind::platform_contract_violation,
        "foreign-volume quarantine identity must be a contract violation");

  MockPlatform invalid_step(observation);
  invalid_step.create_failure = EfiDeletePlatformStepResult{
      .succeeded = true,
      .failure_kind = EfiDeletePlatformFailureKind::race_detected,
      .mutation_extent = EfiDeleteMutationExtent::complete,
      .error = test_error(L"contradictory create result"),
  };
  const auto invalid_step_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), invalid_step);
  check(invalid_step_report.outcome ==
            EfiDeleteTransactionOutcome::partial_rollback &&
            invalid_step_report.failure_stage ==
                ytec::bootrepair::EfiDeleteFailureStage::platform_contract &&
            invalid_step_report.platform_failure ==
                EfiDeletePlatformFailureKind::platform_contract_violation,
        "contradictory mutation evidence must fail the platform contract");
}

void test_partial_rollback_is_not_reported_as_recovered() {
  const auto observation = valid_observation();
  const auto reviewed =
      ytec::bootrepair::review_efi_delete_candidates(observation);
  check(reviewed.has_value(), "fixture should review");
  if (!reviewed) {
    return;
  }
  MockPlatform platform(observation);
  platform.fail_move_index = 1U;
  platform.move_failure = failed_step(
      EfiDeletePlatformFailureKind::race_detected,
      EfiDeleteMutationExtent::none,
      L"move race");
  platform.fail_rollback_index = 0U;
  platform.rollback_failure = failed_step(
      EfiDeletePlatformFailureKind::tamper_detected,
      EfiDeleteMutationExtent::partial_or_unknown,
      L"rollback tamper");
  const auto report = ytec::bootrepair::execute_efi_delete_transaction(
      reviewed.value(), confirmed(), platform);
  check(report.outcome == EfiDeleteTransactionOutcome::partial_rollback &&
            report.rollback_error.has_value() &&
            report.rollback_platform_failure ==
                EfiDeletePlatformFailureKind::tamper_detected,
        "rollback tamper must be an explicit partial rollback");

  MockPlatform uncertain_move(observation);
  uncertain_move.fail_move_index = 0U;
  uncertain_move.move_failure = failed_step(
      EfiDeletePlatformFailureKind::io_failure,
      EfiDeleteMutationExtent::partial_or_unknown,
      L"move completion unknown");
  const auto uncertain_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), uncertain_move);
  check(uncertain_report.outcome ==
            EfiDeleteTransactionOutcome::partial_rollback &&
            uncertain_report.rolled_back_candidates == 0U,
        "unknown current move must never be reported as recovered");

  MockPlatform cleanup_failure(observation);
  cleanup_failure.fail_move_index = 1U;
  cleanup_failure.move_failure = failed_step(
      EfiDeletePlatformFailureKind::race_detected,
      EfiDeleteMutationExtent::none,
      L"second move race");
  cleanup_failure.cleanup_failure = failed_step(
      EfiDeletePlatformFailureKind::verification_failure,
      EfiDeleteMutationExtent::none,
      L"rollback quarantine cleanup");
  const auto cleanup_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), cleanup_failure);
  check(cleanup_report.outcome ==
            EfiDeleteTransactionOutcome::partial_rollback &&
            cleanup_report.rolled_back_candidates == 1U &&
            cleanup_report.rollback_platform_failure ==
                EfiDeletePlatformFailureKind::verification_failure,
        "rollback cleanup failure must remain a partial rollback");
}

void test_bcd_failure_rolls_back_before_any_delete() {
  const auto observation = valid_observation();
  const auto reviewed =
      ytec::bootrepair::review_efi_delete_candidates(observation);
  check(reviewed.has_value(), "fixture should review");
  if (!reviewed) {
    return;
  }
  MockPlatform platform(observation);
  platform.bcd_failure = failed_step(
      EfiDeletePlatformFailureKind::verification_failure,
      EfiDeleteMutationExtent::none,
      L"BCD readback");
  const auto report = ytec::bootrepair::execute_efi_delete_transaction(
      reviewed.value(), confirmed(), platform);
  check(report.outcome == EfiDeleteTransactionOutcome::rolled_back &&
            report.microsoft_bcd_failure_rollback_verified &&
            report.rolled_back_candidates == 2U &&
            report.deleted_candidates == 0U,
        "fully rolled-back BCD failure must restore every candidate");
  check(platform.events.size() >= 7U &&
            platform.events[5] == L"rollback:VendorB" &&
            platform.events[6] == L"rollback:VendorA",
        "candidate rollback must run in reverse move order");
  check(std::none_of(
            platform.events.begin(),
            platform.events.end(),
            [](const std::wstring& event) {
              return event.starts_with(L"delete:");
            }),
        "no candidate deletion is permitted before BCD success/readback");

  MockPlatform uncertain_bcd(observation);
  uncertain_bcd.bcd_failure = failed_step(
      EfiDeletePlatformFailureKind::verification_failure,
      EfiDeleteMutationExtent::partial_or_unknown,
      L"BCD rollback unknown");
  const auto uncertain = ytec::bootrepair::execute_efi_delete_transaction(
      reviewed.value(), confirmed(), uncertain_bcd);
  check(uncertain.outcome == EfiDeleteTransactionOutcome::partial_rollback &&
            !uncertain.microsoft_bcd_failure_rollback_verified,
        "unknown Microsoft/BCD rollback state must remain partial");
}

void test_success_orders_quarantine_bcd_delete_and_cleanup() {
  const auto observation = valid_observation();
  const auto reviewed =
      ytec::bootrepair::review_efi_delete_candidates(observation);
  check(reviewed.has_value(), "fixture should review");
  if (!reviewed) {
    return;
  }
  MockPlatform platform(observation);
  const auto report = ytec::bootrepair::execute_efi_delete_transaction(
      reviewed.value(), confirmed(), platform);
  const std::vector<std::wstring> expected{
      L"inspect",
      L"create-quarantine",
      L"move:VendorA",
      L"move:VendorB",
      L"bcd-readback",
      L"delete:VendorA",
      L"delete:VendorB",
      L"bcd-commit",
      L"remove-quarantine",
  };
  check(report.outcome == EfiDeleteTransactionOutcome::committed &&
            report.all_candidates_were_quarantined_before_bcd &&
            report.microsoft_bcd_rebuild_readback_verified &&
            report.microsoft_bcd_rollback_boundary_committed &&
            report.deleted_candidates == 2U,
        "successful transaction must report full verified commit");
  check(platform.events == expected,
        "all candidates must move before BCD and delete only after readback");
}

void test_final_delete_and_cleanup_failures_remain_explicit() {
  const auto observation = valid_observation();
  const auto reviewed =
      ytec::bootrepair::review_efi_delete_candidates(observation);
  check(reviewed.has_value(), "fixture should review");
  if (!reviewed) {
    return;
  }

  MockPlatform no_delete(observation);
  no_delete.fail_delete_index = 0U;
  no_delete.delete_failure = failed_step(
      EfiDeletePlatformFailureKind::race_detected,
      EfiDeleteMutationExtent::none,
      L"delete race before mutation");
  const auto incomplete = ytec::bootrepair::execute_efi_delete_transaction(
      reviewed.value(), confirmed(), no_delete);
  check(incomplete.outcome == EfiDeleteTransactionOutcome::rolled_back &&
            incomplete.deleted_candidates == 0U &&
            incomplete.rolled_back_candidates == 2U &&
            incomplete.microsoft_bcd_rolled_back_after_delete_stop,
        "pre-delete race must restore candidates and the retained BCD");
  check(!no_delete.events.empty() &&
            no_delete.events.back() == L"bcd-rollback",
        "BCD rollback must follow exact candidate rollback and cleanup");

  MockPlatform rollback_cleanup_failure(observation);
  rollback_cleanup_failure.fail_delete_index = 0U;
  rollback_cleanup_failure.delete_failure = failed_step(
      EfiDeletePlatformFailureKind::race_detected,
      EfiDeleteMutationExtent::none,
      L"delete race before mutation");
  rollback_cleanup_failure.cleanup_failure = failed_step(
      EfiDeletePlatformFailureKind::verification_failure,
      EfiDeleteMutationExtent::none,
      L"rollback quarantine cleanup");
  const auto rollback_cleanup_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), rollback_cleanup_failure);
  check(rollback_cleanup_report.outcome ==
            EfiDeleteTransactionOutcome::partial_rollback &&
            rollback_cleanup_report.rolled_back_candidates == 2U &&
            rollback_cleanup_report.microsoft_bcd_rolled_back_after_delete_stop &&
            !rollback_cleanup_failure.events.empty() &&
            rollback_cleanup_failure.events.back() == L"bcd-rollback",
        "BCD rollback must continue after only quarantine cleanup fails");

  MockPlatform bcd_rollback_failure(observation);
  bcd_rollback_failure.fail_delete_index = 0U;
  bcd_rollback_failure.delete_failure = failed_step(
      EfiDeletePlatformFailureKind::race_detected,
      EfiDeleteMutationExtent::none,
      L"delete race before mutation");
  bcd_rollback_failure.bcd_rollback_failure = failed_step(
      EfiDeletePlatformFailureKind::tamper_detected,
      EfiDeleteMutationExtent::partial_or_unknown,
      L"BCD rollback identity");
  const auto bcd_rollback_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), bcd_rollback_failure);
  check(bcd_rollback_report.outcome ==
            EfiDeleteTransactionOutcome::partial_rollback &&
            bcd_rollback_report.rollback_platform_failure ==
                EfiDeletePlatformFailureKind::tamper_detected &&
            !bcd_rollback_report.microsoft_bcd_rolled_back_after_delete_stop,
        "failed exact BCD rollback must never claim full recovery");

  MockPlatform partial(observation);
  partial.fail_delete_index = 1U;
  partial.delete_failure = failed_step(
      EfiDeletePlatformFailureKind::io_failure,
      EfiDeleteMutationExtent::none,
      L"second delete race");
  const auto partial_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), partial);
  check(partial_report.outcome ==
            EfiDeleteTransactionOutcome::partial_delete &&
            partial_report.deleted_candidates == 1U &&
            partial_report.rolled_back_candidates == 1U,
        "a prior deletion must stay partial while intact trees roll back");
  check(std::find(
            partial.events.begin(),
            partial.events.end(),
            L"rollback:VendorB") != partial.events.end(),
        "later intact tree must be restored after an earlier commit");

  MockPlatform unknown_current(observation);
  unknown_current.fail_delete_index = 0U;
  unknown_current.delete_failure = failed_step(
      EfiDeletePlatformFailureKind::io_failure,
      EfiDeleteMutationExtent::partial_or_unknown,
      L"recursive delete partial");
  const auto unknown_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), unknown_current);
  check(unknown_report.outcome ==
            EfiDeleteTransactionOutcome::partial_delete &&
            unknown_report.rolled_back_candidates == 1U,
        "unknown current deletion must still restore untouched later trees");
  check(std::find(
            unknown_current.events.begin(),
            unknown_current.events.end(),
            L"rollback:VendorB") != unknown_current.events.end(),
        "unknown current tree must not prevent later exact rollback");

  MockPlatform cleanup(observation);
  cleanup.cleanup_failure = failed_step(
      EfiDeletePlatformFailureKind::verification_failure,
      EfiDeleteMutationExtent::none,
      L"empty quarantine cleanup");
  const auto cleanup_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), cleanup);
  check(cleanup_report.outcome ==
            EfiDeleteTransactionOutcome::
                committed_quarantine_cleanup_incomplete &&
            cleanup_report.deleted_candidates == 2U &&
            cleanup_report.microsoft_bcd_rollback_boundary_committed,
        "empty quarantine cleanup failure must remain distinct from data deletion");

  MockPlatform bcd_commit(observation);
  bcd_commit.bcd_commit_failure = failed_step(
      EfiDeletePlatformFailureKind::verification_failure,
      EfiDeleteMutationExtent::none,
      L"BCD backup cleanup");
  const auto bcd_commit_report =
      ytec::bootrepair::execute_efi_delete_transaction(
          reviewed.value(), confirmed(), bcd_commit);
  check(bcd_commit_report.outcome ==
            EfiDeleteTransactionOutcome::
                committed_bcd_cleanup_incomplete &&
            bcd_commit_report.deleted_candidates == 2U &&
            !bcd_commit_report.microsoft_bcd_rollback_boundary_committed,
        "BCD retained-backup cleanup failure must remain explicit");
}

}  // namespace

int main() {
  test_manifest_is_canonical_and_complete();
  test_review_rejects_unsafe_names_and_object_types();
  test_confirmation_and_fresh_identity_stop_before_writes();
  test_foreign_quarantine_and_move_race_are_distinct();
  test_partial_rollback_is_not_reported_as_recovered();
  test_bcd_failure_rolls_back_before_any_delete();
  test_success_orders_quarantine_bcd_delete_and_cleanup();
  test_final_delete_and_cleanup_failures_remain_explicit();
  if (failures != 0) {
    std::cerr << failures << " EFI delete transaction test(s) failed\n";
    return 1;
  }
  std::cout << "PASS: bounded EFI delete manifest/mock transaction tests\n";
  return 0;
}

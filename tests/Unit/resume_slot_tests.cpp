#include "ytec/operationcore/resume_slot.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::operationcore::OperationId make_operation_id(
    const unsigned char seed = 1U) {
  ytec::operationcore::OperationId id{};
  for (std::size_t index = 0U; index < id.size(); ++index) {
    id[index] = static_cast<std::byte>(seed + index);
  }
  return id;
}

ytec::operationcore::Sha256Digest make_digest(const unsigned char seed) {
  ytec::operationcore::Sha256Digest digest{};
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    digest[index] = static_cast<std::byte>(seed + index);
  }
  return digest;
}

ytec::clonecore::StableDiskIdentity make_target() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 9U,
      .model = L"Synthetic restore target",
      .size_bytes = 256ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .serial_suffix = "TARGET09",
      .device_instance_id = L"SYNTHETIC\\RESTORE-TARGET",
      .is_system_disk = false,
  };
}

ytec::operationcore::OperationPlan make_restore_plan() {
  return ytec::operationcore::OperationPlan{
      .schema_version = ytec::operationcore::kOperationPlanSchemaVersion,
      .operation_id = make_operation_id(),
      .kind = ytec::operationcore::OperationKind::image_restore,
      .environment = ytec::operationcore::OperationEnvironment::winpe,
      .source = std::nullopt,
      .target = make_target(),
      .expected_work_bytes = 8192U,
      .immutable_payload_hash = make_digest(0x20U),
  };
}

ytec::operationcore::OperationPlan make_new_clone_plan() {
  auto source = make_target();
  source.disk_number = 4U;
  source.model = L"Synthetic clone source";
  source.serial_suffix = "SOURCE04";
  source.device_instance_id = L"SYNTHETIC\\CLONE-SOURCE";

  auto target = make_target();
  target.disk_number = 5U;
  target.model = L"Synthetic clone target";
  target.serial_suffix = "TARGET05";
  target.device_instance_id = L"SYNTHETIC\\CLONE-TARGET";

  return ytec::operationcore::OperationPlan{
      .schema_version = ytec::operationcore::kOperationPlanSchemaVersion,
      .operation_id = make_operation_id(0x70U),
      .kind = ytec::operationcore::OperationKind::clone,
      .environment = ytec::operationcore::OperationEnvironment::windows,
      .source = std::move(source),
      .target = std::move(target),
      .expected_work_bytes = 16384U,
      .immutable_payload_hash = make_digest(0x80U),
  };
}

ytec::operationcore::OperationPlan make_image_create_plan() {
  auto source = make_target();
  source.disk_number = 3U;
  source.model = L"Synthetic image source";
  source.serial_suffix = "IMAGE03";
  source.device_instance_id = L"SYNTHETIC\\IMAGE-SOURCE";
  return ytec::operationcore::OperationPlan{
      .schema_version = ytec::operationcore::kOperationPlanSchemaVersion,
      .operation_id = make_operation_id(0x41U),
      .kind = ytec::operationcore::OperationKind::image_create,
      .environment = ytec::operationcore::OperationEnvironment::winpe,
      .source = std::move(source),
      .target = std::nullopt,
      .expected_work_bytes = 8192U,
      .immutable_payload_hash = make_digest(0x42U),
  };
}

ytec::operationcore::ParsedCheckpoint parse_checkpoint_or_throw(
    const ytec::operationcore::InterruptionCheckpoint& checkpoint) {
  const auto bytes = ytec::operationcore::serialize_checkpoint(checkpoint);
  if (!bytes) {
    throw TestFailure{"Synthetic checkpoint must serialize"};
  }
  auto parsed = ytec::operationcore::parse_checkpoint(bytes.value());
  if (!parsed) {
    throw TestFailure{"Synthetic checkpoint must parse"};
  }
  return parsed.take_value();
}

ytec::operationcore::ResumeSlotRecord make_record(
    const bool with_partial = true,
    const ytec::operationcore::ResumeCapability capability =
        ytec::operationcore::ResumeCapability::persistent_exact_restore) {
  const auto plan = make_restore_plan();
  const auto plan_hash = ytec::operationcore::hash_operation_plan(plan);
  if (!plan_hash) {
    throw TestFailure{"Synthetic restore plan must hash"};
  }
  const ytec::operationcore::InterruptionCheckpoint checkpoint{
      .schema_version = ytec::operationcore::kCheckpointSchemaVersion,
      .operation_id = plan.operation_id,
      .kind = plan.kind,
      .environment = plan.environment,
      .phase = ytec::operationcore::CheckpointPhase::executing,
      .revision = 1U,
      .expected_work_bytes = plan.expected_work_bytes,
      .verified_work_bytes = 1024U,
      .verified_chunk_count = 1U,
      .plan_hash = plan_hash.value(),
      .output_identity_hash = make_digest(0x50U),
      .source = plan.source,
      .target = plan.target,
      .continuity_token = L"OFFLINE-RESTORE-EPOCH-0001",
  };
  const ytec::operationcore::ResumeIdentityBinding identities{
      .source_identity_hash = make_digest(0x30U),
      .target_identity_hash = make_digest(0x40U),
      .output_identity_hash = checkpoint.output_identity_hash,
  };
  ytec::operationcore::ResumeSlotRecord record{
      .capability = capability,
      .checkpoint = parse_checkpoint_or_throw(checkpoint),
      .identities = identities,
      .owned_partial = std::nullopt,
  };
  if (with_partial) {
    record.owned_partial = ytec::operationcore::ResumeOwnedPartialBinding{
        .operation_id = checkpoint.operation_id,
        .identities = identities,
        .file_object_identity_hash = make_digest(0x60U),
    };
  }
  return record;
}

ytec::operationcore::ResumeSlotRecord make_v3_image_create_record() {
  const auto plan = make_image_create_plan();
  const auto plan_hash = ytec::operationcore::hash_operation_plan(plan);
  if (!plan_hash) {
    throw TestFailure{"Synthetic image-create plan must hash"};
  }
  const ytec::operationcore::InterruptionCheckpoint checkpoint{
      .schema_version = ytec::operationcore::kCheckpointSchemaVersionV3,
      .operation_id = plan.operation_id,
      .kind = plan.kind,
      .environment = plan.environment,
      .phase = ytec::operationcore::CheckpointPhase::preparing,
      .revision = 1U,
      .expected_work_bytes = plan.expected_work_bytes,
      .verified_work_bytes = 0U,
      .verified_chunk_count = 0U,
      .plan_hash = plan_hash.value(),
      .output_identity_hash = make_digest(0x43U),
      .source = plan.source,
      .target = std::nullopt,
      .continuity_token = L"PE-SOURCE-STATE-SYNTHETIC-0001",
      .preparation_evidence = std::nullopt,
      .output_progress_evidence =
          ytec::operationcore::CheckpointOutputProgressEvidence{
              .verified_prefix_hash = make_digest(0x44U),
              .primary_output_length = 0U,
              .journal_length = 0U,
              .auxiliary_output_length = 0U,
          },
  };
  const ytec::operationcore::ResumeIdentityBinding identities{
      .source_identity_hash = make_digest(0x45U),
      .target_identity_hash = make_digest(0x46U),
      .output_identity_hash = checkpoint.output_identity_hash,
  };
  return ytec::operationcore::ResumeSlotRecord{
      .capability =
          ytec::operationcore::ResumeCapability::
              persistent_pe_exact_image_create,
      .checkpoint = parse_checkpoint_or_throw(checkpoint),
      .identities = identities,
      .owned_partial = std::nullopt,
      .owned_objects = {
          {
              .role = ytec::operationcore::ResumeOwnedObjectRole::
                  image_partial,
              .operation_id = checkpoint.operation_id,
              .identities = identities,
              .file_object_identity_hash = make_digest(0x47U),
          },
          {
              .role = ytec::operationcore::ResumeOwnedObjectRole::
                  image_resume_journal,
              .operation_id = checkpoint.operation_id,
              .identities = identities,
              .file_object_identity_hash = make_digest(0x48U),
          },
      },
  };
}

ytec::clonecore::Error synthetic_error(
    const ytec::clonecore::ErrorCode code,
    std::wstring operation) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::move(operation),
      .message = L"synthetic failure",
  };
}

class SyntheticResumePlatform final
    : public ytec::operationcore::IResumeSlotPlatform {
 public:
  std::wstring path{L"C:\\synthetic\\data\\active.checkpoint"};
  bool paths_canonical{true};
  bool parent_reparse_free{true};
  bool placement_separated{true};
  bool paths_distinct{true};
  bool checkpoint_regular{true};
  bool checkpoint_reparse_free{true};
  std::uint32_t checkpoint_links{1U};
  bool partial_regular{true};
  bool partial_reparse_free{true};
  std::uint32_t partial_links{1U};
  bool fail_observe{};
  std::optional<ytec::operationcore::ResumeSlotRecord> slot;
  std::optional<ytec::operationcore::ResumeOwnedPartialBinding> partial;
  std::vector<ytec::operationcore::ResumeOwnedObjectBinding> objects;
  int observe_calls{};
  int create_calls{};
  int replace_calls{};
  int discard_calls{};

  [[nodiscard]] ytec::clonecore::Result<
      ytec::operationcore::ResumeSlotObservation>
  observe_fixed_slot() override {
    ++observe_calls;
    if (fail_observe) {
      return ytec::clonecore::Result<
          ytec::operationcore::ResumeSlotObservation>::failure(
          synthetic_error(
              ytec::clonecore::ErrorCode::invalid_data,
              L"synthetic corrupt slot"));
    }
    std::vector<ytec::operationcore::ResumeFileStorageProof> object_files;
    object_files.reserve(objects.size());
    for (std::size_t index = 0U; index < objects.size(); ++index) {
      object_files.push_back({
          .exists = true,
          .is_regular_file = true,
          .is_reparse_free = true,
          .hard_link_count = 1U,
      });
    }
    return ytec::clonecore::Result<
        ytec::operationcore::ResumeSlotObservation>::success({
        .storage = {
            .checkpoint_path = path,
            .paths_are_canonical_local = paths_canonical,
            .parent_chain_reparse_free = parent_reparse_free,
            .placement_separated_from_source = placement_separated,
            .checkpoint_and_partial_paths_distinct = paths_distinct,
            .checkpoint_file = {
                .exists = slot.has_value(),
                .is_regular_file = checkpoint_regular,
                .is_reparse_free = checkpoint_reparse_free,
                .hard_link_count = checkpoint_links,
            },
            .owned_partial_file = {
                .exists = partial.has_value(),
                .is_regular_file = partial_regular,
                .is_reparse_free = partial_reparse_free,
                .hard_link_count = partial_links,
            },
            .owned_object_files = std::move(object_files),
        },
        .slot = slot,
        .observed_owned_partial = partial,
        .observed_owned_objects = objects,
    });
  }

  [[nodiscard]] ytec::clonecore::Status create_fixed_slot(
      const ytec::operationcore::ResumeSlotRecord& record) override {
    ++create_calls;
    if (slot) {
      return ytec::clonecore::Status::failure(synthetic_error(
          ytec::clonecore::ErrorCode::access_denied,
          L"synthetic create"));
    }
    slot = record;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status replace_fixed_slot(
      const ytec::operationcore::Sha256Digest& expected_hash,
      const ytec::operationcore::ResumeSlotRecord& next) override {
    ++replace_calls;
    if (!slot || slot->checkpoint.record_hash != expected_hash) {
      return ytec::clonecore::Status::failure(synthetic_error(
          ytec::clonecore::ErrorCode::identity_mismatch,
          L"synthetic replace"));
    }
    slot = next;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status
  discard_fixed_slot_and_owned_partial(
      const ytec::operationcore::ResumeSlotBinding& binding) override {
    ++discard_calls;
    if (!slot) {
      return ytec::clonecore::Status::failure(synthetic_error(
          ytec::clonecore::ErrorCode::identity_mismatch,
          L"synthetic discard"));
    }
    const auto actual =
        ytec::operationcore::make_resume_slot_binding(*slot);
    if (!actual || actual.value().capability != binding.capability ||
        actual.value().operation_id != binding.operation_id ||
        actual.value().identities.source_identity_hash !=
            binding.identities.source_identity_hash ||
        actual.value().identities.target_identity_hash !=
            binding.identities.target_identity_hash ||
        actual.value().identities.output_identity_hash !=
            binding.identities.output_identity_hash ||
        actual.value().checkpoint_record_hash !=
            binding.checkpoint_record_hash ||
        actual.value().partial_file_object_identity_hash !=
            binding.partial_file_object_identity_hash ||
        actual.value().owned_object_file_bindings !=
            binding.owned_object_file_bindings ||
        binding.partial_file_object_identity_hash.has_value() !=
            partial.has_value() ||
        binding.owned_object_file_bindings.size() != objects.size() ||
        (partial && partial->file_object_identity_hash !=
                        *binding.partial_file_object_identity_hash)) {
      return ytec::clonecore::Status::failure(synthetic_error(
          ytec::clonecore::ErrorCode::identity_mismatch,
          L"synthetic discard binding"));
    }
    for (std::size_t index = 0U; index < objects.size(); ++index) {
      if (objects[index].role !=
              binding.owned_object_file_bindings[index].role ||
          objects[index].file_object_identity_hash !=
              binding.owned_object_file_bindings[index]
                  .file_object_identity_hash) {
        return ytec::clonecore::Status::failure(synthetic_error(
            ytec::clonecore::ErrorCode::identity_mismatch,
            L"synthetic discard object binding"));
      }
    }
    slot.reset();
    partial.reset();
    objects.clear();
    return ytec::clonecore::success_status();
  }
};

void test_capability_matrix_and_persistent_gate() {
  using ytec::operationcore::OperationEnvironment;
  using ytec::operationcore::OperationKind;
  using ytec::operationcore::ResumeCapability;
  using ytec::operationcore::ResumeLifetime;

  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::persistent_exact_restore) == 0U);
  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::persistent_rescue_restore) == 1U);
  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::same_process_only_vss_image_create) == 2U);
  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::same_process_only_vss_clone) == 3U);
  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::same_process_only_pe_image_create) == 4U);
  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::same_process_only_pe_clone) == 5U);
  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::unsupported_shrink_migration) == 6U);
  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::unsupported_raw_rescue) == 7U);
  static_assert(
      static_cast<std::uint8_t>(
          ResumeCapability::persistent_pe_exact_image_create) == 8U);
  check(
      ytec::operationcore::resume_lifetime(
          static_cast<ResumeCapability>(2U)) ==
              ResumeLifetime::same_process_only &&
          ytec::operationcore::resume_lifetime(
              static_cast<ResumeCapability>(4U)) ==
              ResumeLifetime::same_process_only &&
          ytec::operationcore::resume_lifetime(
              static_cast<ResumeCapability>(6U)) ==
              ResumeLifetime::unsupported &&
          ytec::operationcore::resume_lifetime(
              static_cast<ResumeCapability>(7U)) ==
              ResumeLifetime::unsupported,
      "Legacy envelope values 2..7 must retain their rejection lifetime");

  check(
      ytec::operationcore::resume_lifetime(
          ResumeCapability::persistent_exact_restore) ==
          ResumeLifetime::persistent &&
          ytec::operationcore::resume_lifetime(
              ResumeCapability::persistent_rescue_restore) ==
              ResumeLifetime::persistent &&
          ytec::operationcore::resume_lifetime(
              ResumeCapability::persistent_pe_exact_image_create) ==
              ResumeLifetime::persistent,
      "Restore and PE exact image-create should be persistent-capable");
  check(
      ytec::operationcore::resume_lifetime(
          ResumeCapability::same_process_only_vss_clone) ==
              ResumeLifetime::same_process_only &&
          ytec::operationcore::resume_lifetime(
              ResumeCapability::same_process_only_pe_image_create) ==
              ResumeLifetime::same_process_only,
      "VSS and PE create/clone should be process-local only");
  check(
      ytec::operationcore::resume_lifetime(
          ResumeCapability::unsupported_shrink_migration) ==
              ResumeLifetime::unsupported &&
          ytec::operationcore::resume_lifetime(
              ResumeCapability::unsupported_raw_rescue) ==
              ResumeLifetime::unsupported,
      "Shrink and raw rescue should refuse resume");
  check(
      ytec::operationcore::validate_resume_capability(
          ResumeCapability::same_process_only_vss_clone,
          OperationKind::clone,
          OperationEnvironment::windows)
          .has_value() &&
          !ytec::operationcore::validate_resume_capability(
              ResumeCapability::same_process_only_vss_clone,
              OperationKind::clone,
              OperationEnvironment::winpe),
      "Capability should bind the operation environment");

  SyntheticResumePlatform platform;
  ytec::operationcore::SingleResumeSlot slot(platform);
  auto process_only = make_record(
      false, ResumeCapability::same_process_only_vss_clone);
  process_only.checkpoint.checkpoint.kind = OperationKind::clone;
  process_only.checkpoint.checkpoint.environment =
      OperationEnvironment::windows;
  check(
      !slot.create(process_only) && platform.create_calls == 0,
      "A same-process capability must never persist a slot");
}

void test_create_bind_replace_and_pair_discard() {
  auto record = make_record(true);
  SyntheticResumePlatform platform;
  platform.partial = record.owned_partial;
  ytec::operationcore::SingleResumeSlot slot(platform);
  check(slot.create(record).has_value() && platform.create_calls == 1,
        "A matching owned partial should permit one slot creation");

  const auto binding = ytec::operationcore::make_resume_slot_binding(record);
  check(binding.has_value() && slot.open_bound(binding.value()).has_value(),
        "Every identity and record hash should open the slot");

  auto wrong = binding.value();
  wrong.operation_id[0] ^= std::byte{0x01};
  check(!slot.open_bound(wrong), "Operation id changes must refuse resume");
  wrong = binding.value();
  wrong.identities.source_identity_hash[0] ^= std::byte{0x01};
  check(!slot.open_bound(wrong), "Source identity changes must refuse resume");
  wrong = binding.value();
  wrong.identities.target_identity_hash[0] ^= std::byte{0x01};
  check(!slot.open_bound(wrong), "Target identity changes must refuse resume");
  wrong = binding.value();
  wrong.identities.output_identity_hash[0] ^= std::byte{0x01};
  check(!slot.open_bound(wrong), "Output identity changes must refuse resume");
  wrong = binding.value();
  wrong.checkpoint_record_hash[0] ^= std::byte{0x01};
  check(!slot.open_bound(wrong), "Record hash changes must refuse resume");
  wrong = binding.value();
  (*wrong.partial_file_object_identity_hash)[0] ^= std::byte{0x01};
  check(!slot.open_bound(wrong),
        "Partial file-object identity changes must refuse resume");

  auto next_checkpoint = record.checkpoint.checkpoint;
  next_checkpoint.revision = 2U;
  next_checkpoint.verified_work_bytes = 2048U;
  next_checkpoint.verified_chunk_count = 2U;
  const auto next_parsed = parse_checkpoint_or_throw(next_checkpoint);
  check(slot.replace(binding.value(), next_parsed).has_value() &&
            platform.replace_calls == 1,
        "A monotonic checkpoint should replace the fixed slot");
  check(!slot.discard(binding.value()) && platform.discard_calls == 0,
        "A stale record hash must not reach platform discard");

  record.checkpoint = next_parsed;
  const auto next_binding =
      ytec::operationcore::make_resume_slot_binding(record);
  check(next_binding.has_value() && slot.discard(next_binding.value()).has_value() &&
            platform.discard_calls == 1 && !platform.slot &&
            !platform.partial,
        "The matched checkpoint and owned partial should discard together");
}

void test_whole_disk_restore_without_partial_discards_checkpoint_only() {
  const auto record = make_record(false);
  SyntheticResumePlatform platform;
  ytec::operationcore::SingleResumeSlot slot(platform);
  check(slot.create(record).has_value(),
        "Exact whole-disk restore should not require a partial file");
  const auto binding = ytec::operationcore::make_resume_slot_binding(record);
  check(binding.has_value() &&
            !binding.value().partial_file_object_identity_hash &&
            slot.discard(binding.value()).has_value() && !platform.slot,
        "A fully bound whole-disk restore checkpoint may be discarded alone");
}

void test_unknown_corrupt_or_partial_mismatch_is_never_deleted() {
  const auto record = make_record(true);
  const auto binding = ytec::operationcore::make_resume_slot_binding(record);
  check(binding.has_value(), "Synthetic record should produce a binding");

  SyntheticResumePlatform corrupt;
  corrupt.slot = record;
  corrupt.partial = record.owned_partial;
  corrupt.fail_observe = true;
  ytec::operationcore::SingleResumeSlot corrupt_slot(corrupt);
  check(!corrupt_slot.discard(binding.value()) &&
            corrupt.discard_calls == 0 && corrupt.slot && corrupt.partial,
        "An unreadable slot must remain untouched");

  SyntheticResumePlatform bad_record;
  bad_record.slot = record;
  bad_record.slot->checkpoint.record_hash[0] ^= std::byte{0x01};
  bad_record.partial = record.owned_partial;
  ytec::operationcore::SingleResumeSlot bad_record_slot(bad_record);
  check(!bad_record_slot.discard(binding.value()) &&
            bad_record.discard_calls == 0 && bad_record.slot &&
            bad_record.partial,
        "A corrupt checkpoint record hash must remain untouched");

  SyntheticResumePlatform bad_partial;
  bad_partial.slot = record;
  bad_partial.partial = record.owned_partial;
  bad_partial.partial->file_object_identity_hash[0] ^= std::byte{0x01};
  ytec::operationcore::SingleResumeSlot bad_partial_slot(bad_partial);
  check(!bad_partial_slot.discard(binding.value()) &&
            bad_partial.discard_calls == 0 && bad_partial.slot &&
            bad_partial.partial,
        "A changed owned partial must block every deletion");
}

void test_storage_proofs_and_fixed_path_fail_closed() {
  const auto record = make_record(false);
  const auto binding = ytec::operationcore::make_resume_slot_binding(record);
  check(binding.has_value(), "Synthetic record should produce a binding");

  const auto rejected = [&](const std::function<void(SyntheticResumePlatform&)>&
                                mutate,
                            const std::string& message) {
    SyntheticResumePlatform platform;
    platform.slot = record;
    mutate(platform);
    ytec::operationcore::SingleResumeSlot slot(platform);
    check(!slot.discard(binding.value()) && platform.discard_calls == 0 &&
              platform.slot,
          message);
  };
  rejected(
      [](SyntheticResumePlatform& value) {
        value.path = L"C:\\synthetic\\data\\active.resume";
      },
      "A .resume path must fail the .checkpoint contract");
  rejected(
      [](SyntheticResumePlatform& value) {
        value.path = L"C:\\synthetic\\data\\active.cache";
      },
      "A .cache path must fail the .checkpoint contract");
  rejected(
      [](SyntheticResumePlatform& value) {
        value.path = L"C:\\synthetic\\data\\second.checkpoint";
      },
      "A second checkpoint filename must not become another slot");
  rejected(
      [](SyntheticResumePlatform& value) {
        value.paths_canonical = false;
      },
      "An unproven canonical local path must block discard");
  rejected(
      [](SyntheticResumePlatform& value) {
        value.parent_reparse_free = false;
      },
      "An unproven parent chain must block discard");
  rejected(
      [](SyntheticResumePlatform& value) {
        value.placement_separated = false;
      },
      "Unproven source placement separation must block discard");
  rejected(
      [](SyntheticResumePlatform& value) {
        value.checkpoint_reparse_free = false;
      },
      "A checkpoint reparse point must block discard");
  rejected(
      [](SyntheticResumePlatform& value) {
        value.checkpoint_links = 2U;
      },
      "A hard-linked checkpoint must block discard");

  SyntheticResumePlatform drifting;
  ytec::operationcore::SingleResumeSlot one_path(drifting);
  check(one_path.inspect().has_value(),
        "The first safe fixed-path observation should bind the controller");
  drifting.path = L"D:\\other\\data\\active.checkpoint";
  check(!one_path.inspect(),
        "A controller must never switch to another path with the same name");
}

void test_orphan_partial_can_only_be_claimed_by_exact_create() {
  auto record = make_record(true);
  SyntheticResumePlatform platform;
  platform.partial = record.owned_partial;
  ytec::operationcore::SingleResumeSlot slot(platform);
  check(!slot.inspect(),
        "An orphan partial should not appear as an empty reusable slot");

  auto mismatched = record;
  mismatched.owned_partial->file_object_identity_hash[0] ^=
      std::byte{0x01};
  check(!slot.create(mismatched) && platform.create_calls == 0 &&
            platform.partial,
        "A mismatched orphan partial must remain untouched");
  check(slot.create(record).has_value() && platform.create_calls == 1,
        "Only the exact operation-owned partial may be attached to a slot");
}

void test_global_start_gate_requires_bound_resume_or_discard() {
  const auto active_record = make_record(true);
  const auto active_binding =
      ytec::operationcore::make_resume_slot_binding(active_record);
  check(active_binding.has_value(),
        "Synthetic active record should produce a binding");

  SyntheticResumePlatform empty_platform;
  ytec::operationcore::SingleResumeSlot empty_slot(empty_platform);
  const auto new_clone = make_new_clone_plan();
  check(empty_slot.guard_new_operation_start(new_clone).has_value(),
        "A valid new plan should pass only when the fixed slot is empty");

  SyntheticResumePlatform occupied_platform;
  occupied_platform.slot = active_record;
  occupied_platform.partial = active_record.owned_partial;
  ytec::operationcore::SingleResumeSlot occupied_slot(occupied_platform);
  check(!occupied_slot.guard_new_operation_start(new_clone) &&
            occupied_platform.create_calls == 0 &&
            occupied_platform.replace_calls == 0 &&
            occupied_platform.discard_calls == 0 &&
            occupied_platform.slot && occupied_platform.partial,
        "An active slot must block every different new plan without mutation");
  check(!occupied_slot.guard_new_operation_start(make_restore_plan()) &&
            occupied_platform.slot && occupied_platform.partial,
        "Even the same operation shape must use bound resume, not new start");
  check(occupied_slot.open_bound(active_binding.value()).has_value(),
        "The exact active binding should remain available for resume");
  check(occupied_slot.discard(active_binding.value()).has_value() &&
            !occupied_platform.slot && !occupied_platform.partial,
        "The exact bound discard should remove only the owned pair");
  check(occupied_slot.guard_new_operation_start(new_clone).has_value(),
        "A new plan may start only after the owned active slot is gone");
}

void test_global_start_gate_fails_closed_on_unknown_state() {
  const auto new_clone = make_new_clone_plan();

  SyntheticResumePlatform corrupt_platform;
  corrupt_platform.slot = make_record(true);
  corrupt_platform.partial = corrupt_platform.slot->owned_partial;
  corrupt_platform.fail_observe = true;
  ytec::operationcore::SingleResumeSlot corrupt_slot(corrupt_platform);
  check(!corrupt_slot.guard_new_operation_start(new_clone) &&
            corrupt_platform.create_calls == 0 &&
            corrupt_platform.replace_calls == 0 &&
            corrupt_platform.discard_calls == 0 &&
            corrupt_platform.slot && corrupt_platform.partial,
        "An unreadable or corrupt slot must block new work without deletion");

  SyntheticResumePlatform orphan_platform;
  const auto orphan_record = make_record(true);
  orphan_platform.partial = orphan_record.owned_partial;
  ytec::operationcore::SingleResumeSlot orphan_slot(orphan_platform);
  check(!orphan_slot.guard_new_operation_start(new_clone) &&
            !orphan_platform.slot && orphan_platform.partial &&
            orphan_platform.create_calls == 0 &&
            orphan_platform.replace_calls == 0 &&
            orphan_platform.discard_calls == 0,
        "An orphan owned-looking partial must block new work and remain untouched");

  SyntheticResumePlatform invalid_plan_platform;
  ytec::operationcore::SingleResumeSlot invalid_plan_slot(
      invalid_plan_platform);
  auto invalid_plan = new_clone;
  invalid_plan.operation_id = {};
  check(!invalid_plan_slot.guard_new_operation_start(invalid_plan) &&
            invalid_plan_platform.observe_calls == 0,
        "An invalid OperationPlan must never reach slot admission");
}

void test_v3_image_create_requires_exact_two_object_binding() {
  auto record = make_v3_image_create_record();
  SyntheticResumePlatform platform;
  platform.objects = record.owned_objects;
  ytec::operationcore::SingleResumeSlot slot(platform);
  check(slot.create(record).has_value() && platform.create_calls == 1,
        "A v3 image slot should claim exactly its partial and journal");

  const auto binding = ytec::operationcore::make_resume_slot_binding(record);
  check(binding.has_value() &&
            binding.value().owned_object_file_bindings.size() == 2U &&
            slot.open_bound(binding.value()).has_value(),
        "The role-tagged file-object identities should bind resume");

  auto relinked = binding.value();
  relinked.owned_object_file_bindings[1].file_object_identity_hash[0] ^=
      std::byte{0x01};
  check(!slot.open_bound(relinked),
        "A relinked journal identity must fail closed");

  auto next = record.checkpoint.checkpoint;
  next.revision = 2U;
  next.phase = ytec::operationcore::CheckpointPhase::prepared;
  next.output_progress_evidence->verified_prefix_hash = make_digest(0x49U);
  next.output_progress_evidence->journal_length = 256U;
  const auto parsed = parse_checkpoint_or_throw(next);
  check(slot.replace(binding.value(), parsed).has_value(),
        "A durable v3 journal header should permit one exact slot replace");

  record.checkpoint = parsed;
  const auto replaced_binding =
      ytec::operationcore::make_resume_slot_binding(record);
  check(replaced_binding.has_value() &&
            slot.discard(replaced_binding.value()).has_value() &&
            !platform.slot && platform.objects.empty(),
        "Discard should remove the exact checkpoint/partial/journal set");

  auto missing = make_v3_image_create_record();
  missing.owned_objects.pop_back();
  check(!ytec::operationcore::validate_resume_slot_record(missing),
        "A missing journal must not enable persistent image-create resume");

  auto reversed = make_v3_image_create_record();
  std::swap(reversed.owned_objects[0], reversed.owned_objects[1]);
  check(!ytec::operationcore::validate_resume_slot_record(reversed),
        "Owned objects must use one canonical role order");

  auto duplicate_role = make_v3_image_create_record();
  duplicate_role.owned_objects[1].role =
      ytec::operationcore::ResumeOwnedObjectRole::image_partial;
  check(!ytec::operationcore::validate_resume_slot_record(duplicate_role),
        "A duplicated owned-object role must fail closed");

  auto legacy_schema = make_v3_image_create_record();
  legacy_schema.checkpoint.checkpoint.schema_version =
      ytec::operationcore::kCheckpointSchemaVersionV2;
  legacy_schema.checkpoint.checkpoint.phase =
      ytec::operationcore::CheckpointPhase::executing;
  legacy_schema.checkpoint.checkpoint.output_progress_evidence.reset();
  legacy_schema.checkpoint =
      parse_checkpoint_or_throw(legacy_schema.checkpoint.checkpoint);
  check(!ytec::operationcore::validate_resume_slot_record(legacy_schema),
        "A legacy checkpoint must not be auto-migrated into image resume");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"capability_matrix_and_persistent_gate",
       test_capability_matrix_and_persistent_gate},
      {"create_bind_replace_and_pair_discard",
       test_create_bind_replace_and_pair_discard},
      {"whole_disk_restore_without_partial_discards_checkpoint_only",
       test_whole_disk_restore_without_partial_discards_checkpoint_only},
      {"unknown_corrupt_or_partial_mismatch_is_never_deleted",
       test_unknown_corrupt_or_partial_mismatch_is_never_deleted},
      {"storage_proofs_and_fixed_path_fail_closed",
       test_storage_proofs_and_fixed_path_fail_closed},
      {"orphan_partial_can_only_be_claimed_by_exact_create",
       test_orphan_partial_can_only_be_claimed_by_exact_create},
      {"global_start_gate_requires_bound_resume_or_discard",
       test_global_start_gate_requires_bound_resume_or_discard},
      {"global_start_gate_fails_closed_on_unknown_state",
       test_global_start_gate_fails_closed_on_unknown_state},
      {"v3_image_create_requires_exact_two_object_binding",
       test_v3_image_create_requires_exact_two_object_binding},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

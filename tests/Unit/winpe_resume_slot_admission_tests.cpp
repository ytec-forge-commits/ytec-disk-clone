#include "ytec/winpeapp/resume_slot_admission.h"
#include "ytec/winpeapp/direct_image_restore_resume.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
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

ytec::clonecore::StableDiskIdentity make_disk(
    const std::uint32_t number,
    const std::wstring& role) {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = number,
      .model = L"Synthetic " + role,
      .size_bytes = 256ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .serial_suffix = number == 4U ? "SOURCE04" : "TARGET09",
      .device_instance_id = L"SYNTHETIC\\" + role,
      .is_system_disk = false,
  };
}

ytec::operationcore::ParsedCheckpoint parse_checkpoint_or_throw(
    const ytec::operationcore::InterruptionCheckpoint& checkpoint) {
  const auto serialized = ytec::operationcore::serialize_checkpoint(checkpoint);
  if (!serialized) {
    throw std::runtime_error("Synthetic checkpoint must serialize");
  }
  auto parsed = ytec::operationcore::parse_checkpoint(serialized.value());
  if (!parsed) {
    throw std::runtime_error("Synthetic checkpoint must parse");
  }
  return parsed.take_value();
}

ytec::operationcore::ResumeSlotRecord make_record() {
  const std::array<std::wstring_view, 1U> fields{L"exact restore"};
  auto plan = ytec::winpeapp::make_winpe_resume_slot_admission_plan(
      make_operation_id(),
      ytec::operationcore::OperationKind::image_restore,
      std::nullopt,
      make_disk(9U, L"RESTORE-TARGET"),
      8192U,
      fields);
  if (!plan) {
    throw std::runtime_error("Synthetic restore plan must build");
  }
  const auto plan_hash =
      ytec::operationcore::hash_operation_plan(plan.value());
  if (!plan_hash) {
    throw std::runtime_error("Synthetic restore plan must hash");
  }
  const ytec::operationcore::InterruptionCheckpoint checkpoint{
      .schema_version = ytec::operationcore::kCheckpointSchemaVersion,
      .operation_id = plan.value().operation_id,
      .kind = plan.value().kind,
      .environment = plan.value().environment,
      .phase = ytec::operationcore::CheckpointPhase::executing,
      .revision = 1U,
      .expected_work_bytes = plan.value().expected_work_bytes,
      .verified_work_bytes = 1024U,
      .verified_chunk_count = 1U,
      .plan_hash = plan_hash.value(),
      .output_identity_hash = make_digest(0x50U),
      .source = plan.value().source,
      .target = plan.value().target,
      .continuity_token = L"OFFLINE-RESTORE-EPOCH-0001",
  };
  return ytec::operationcore::ResumeSlotRecord{
      .capability =
          ytec::operationcore::ResumeCapability::persistent_exact_restore,
      .checkpoint = parse_checkpoint_or_throw(checkpoint),
      .identities = {
          .source_identity_hash = make_digest(0x30U),
          .target_identity_hash = make_digest(0x40U),
          .output_identity_hash = checkpoint.output_identity_hash,
      },
  };
}

ytec::clonecore::Error synthetic_error() {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::invalid_data,
      .native_code = ERROR_INVALID_DATA,
      .operation = L"synthetic slot observation",
      .message = L"synthetic failure",
  };
}

class MockResumeSlotPlatform final
    : public ytec::operationcore::IResumeSlotPlatform {
 public:
  bool fail_observe{};
  std::optional<ytec::operationcore::ResumeSlotRecord> slot;
  std::optional<ytec::operationcore::ResumeOwnedPartialBinding> orphan_partial;
  std::uint32_t observe_calls{};
  std::uint32_t create_calls{};
  std::uint32_t replace_calls{};
  std::uint32_t discard_calls{};

  ytec::clonecore::Result<ytec::operationcore::ResumeSlotObservation>
  observe_fixed_slot() override {
    ++observe_calls;
    if (fail_observe) {
      return ytec::clonecore::Result<
          ytec::operationcore::ResumeSlotObservation>::failure(
          synthetic_error());
    }
    return ytec::clonecore::Result<
        ytec::operationcore::ResumeSlotObservation>::success({
        .storage = {
            .checkpoint_path = L"C:\\synthetic\\data\\active.checkpoint",
            .paths_are_canonical_local = true,
            .parent_chain_reparse_free = true,
            .placement_separated_from_source = true,
            .checkpoint_and_partial_paths_distinct = true,
            .checkpoint_file = {
                .exists = slot.has_value(),
                .is_regular_file = true,
                .is_reparse_free = true,
                .hard_link_count = slot.has_value() ? 1U : 0U,
            },
            .owned_partial_file = {
                .exists = orphan_partial.has_value(),
                .is_regular_file = true,
                .is_reparse_free = true,
                .hard_link_count = orphan_partial.has_value() ? 1U : 0U,
            },
        },
        .slot = slot,
        .observed_owned_partial = orphan_partial,
    });
  }

  ytec::clonecore::Status create_fixed_slot(
      const ytec::operationcore::ResumeSlotRecord&) override {
    ++create_calls;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status replace_fixed_slot(
      const ytec::operationcore::Sha256Digest&,
      const ytec::operationcore::ResumeSlotRecord&) override {
    ++replace_calls;
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status discard_fixed_slot_and_owned_partial(
      const ytec::operationcore::ResumeSlotBinding&) override {
    ++discard_calls;
    return ytec::clonecore::success_status();
  }
};

ytec::operationcore::OperationPlan make_new_clone_admission() {
  const std::array<std::wstring_view, 2U> fields{
      L"direct clone", L"reviewed target layout"};
  const std::array<ytec::operationcore::Sha256Digest, 1U> digests{
      make_digest(0x80U)};
  auto plan = ytec::winpeapp::make_winpe_resume_slot_admission_plan(
      make_operation_id(0x70U),
      ytec::operationcore::OperationKind::clone,
      make_disk(4U, L"CLONE-SOURCE"),
      make_disk(9U, L"CLONE-TARGET"),
      16384U,
      fields,
      digests);
  if (!plan) {
    throw std::runtime_error("Synthetic clone admission must build");
  }
  return plan.take_value();
}

void admission_plan_binds_reviewed_output_and_rejects_missing_binding() {
  const auto operation_id = make_operation_id(0x20U);
  const std::array<std::wstring_view, 1U> first{L"C:\\first.tsumugi"};
  const std::array<std::wstring_view, 1U> second{L"D:\\second.tsumugi"};
  auto first_plan = ytec::winpeapp::make_winpe_resume_slot_admission_plan(
      operation_id,
      ytec::operationcore::OperationKind::image_create,
      make_disk(4U, L"IMAGE-SOURCE"),
      std::nullopt,
      4096U,
      first);
  auto second_plan = ytec::winpeapp::make_winpe_resume_slot_admission_plan(
      operation_id,
      ytec::operationcore::OperationKind::image_create,
      make_disk(4U, L"IMAGE-SOURCE"),
      std::nullopt,
      4096U,
      second);
  check(
      first_plan.has_value() && second_plan.has_value() &&
          first_plan.value().immutable_payload_hash !=
              second_plan.value().immutable_payload_hash,
      "Different reviewed outputs must produce different admission hashes");

  const std::span<const std::wstring_view> no_fields;
  const auto missing = ytec::winpeapp::
      make_winpe_resume_slot_admission_plan(
          operation_id,
          ytec::operationcore::OperationKind::image_create,
          make_disk(4U, L"IMAGE-SOURCE"),
          std::nullopt,
          4096U,
          no_fields);
  check(!missing, "An admission-only plan without a reviewed binding must fail");
}

void admission_canonicalization_rejects_empty_huge_and_overflow_shapes() {
  const auto operation_id = make_operation_id(0x31U);
  const auto source = make_disk(4U, L"IMAGE-SOURCE");
  const auto build = [&](const std::span<const std::wstring_view> fields) {
    return ytec::winpeapp::make_winpe_resume_slot_admission_plan(
        operation_id,
        ytec::operationcore::OperationKind::image_create,
        source,
        std::nullopt,
        4096U,
        fields);
  };

  const std::array<std::wstring_view, 1U> empty{L""};
  check(!build(empty), "An empty framed field must fail closed");

  const std::wstring huge(32U * 1024U + 1U, L'x');
  const std::array<std::wstring_view, 1U> huge_field{huge};
  check(!build(huge_field), "An over-limit UTF-16 field must fail closed");

  const std::wstring_view impossible_length(
      L"x", (std::numeric_limits<std::size_t>::max)());
  const std::array<std::wstring_view, 1U> overflow_field{impossible_length};
  check(
      !build(overflow_field),
      "An overflow-shaped view must be rejected before dereference");

  std::array<std::wstring_view, 17U> too_many{};
  too_many.fill(L"bounded");
  check(!build(too_many), "Too many framed fields must fail closed");

  const std::wstring maximum_sized_field(32U * 1024U, L'y');
  std::array<std::wstring_view, 4U> excessive_total{};
  excessive_total.fill(maximum_sized_field);
  check(
      !build(excessive_total),
      "Individually bounded fields must still obey the total byte limit");

  std::array<ytec::operationcore::Sha256Digest, 17U> too_many_digests{};
  too_many_digests.fill(make_digest(0x91U));
  const std::span<const std::wstring_view> no_fields;
  check(
      !ytec::winpeapp::make_winpe_resume_slot_admission_plan(
          operation_id,
          ytec::operationcore::OperationKind::image_create,
          source,
          std::nullopt,
          4096U,
          no_fields,
          too_many_digests),
      "Too many immutable digests must fail closed");

  const std::array<ytec::operationcore::Sha256Digest, 1U> digest_only{
      make_digest(0x90U)};
  check(
      ytec::winpeapp::make_winpe_resume_slot_admission_plan(
          operation_id,
          ytec::operationcore::OperationKind::image_create,
          source,
          std::nullopt,
          4096U,
          no_fields,
          digest_only)
          .has_value(),
      "A bounded digest-only immutable binding must remain valid");
}

void fresh_empty_slot_allows_new_operation_without_mutation() {
  MockResumeSlotPlatform platform;
  const auto status = ytec::winpeapp::guard_new_winpe_operation_start(
      make_new_clone_admission(), platform);
  check(
      status.has_value() && platform.observe_calls == 1U &&
          platform.create_calls == 0U && platform.replace_calls == 0U &&
          platform.discard_calls == 0U,
      "A fresh empty slot may admit a new writer by observation only");
}

void active_unknown_and_orphaned_slot_state_block_every_new_operation() {
  for (int mode = 0; mode < 3; ++mode) {
    MockResumeSlotPlatform platform;
    if (mode == 0) {
      platform.slot = make_record();
    } else if (mode == 1) {
      platform.fail_observe = true;
    } else {
      const auto record = make_record();
      platform.orphan_partial = ytec::operationcore::ResumeOwnedPartialBinding{
          .operation_id = record.checkpoint.checkpoint.operation_id,
          .identities = record.identities,
          .file_object_identity_hash = make_digest(0x60U),
      };
    }
    const auto status = ytec::winpeapp::guard_new_winpe_operation_start(
        make_new_clone_admission(), platform);
    check(
        !status && platform.observe_calls == 1U &&
            platform.create_calls == 0U && platform.replace_calls == 0U &&
            platform.discard_calls == 0U,
        "Active, unknown, and orphaned slot state must fail closed without mutation");
  }
}

void active_slot_also_blocks_starting_the_same_operation_as_new() {
  MockResumeSlotPlatform platform;
  platform.slot = make_record();
  const auto& checkpoint = platform.slot->checkpoint.checkpoint;
  const std::array<std::wstring_view, 1U> fields{
      L"attempted restart as a new operation"};
  auto same_operation =
      ytec::winpeapp::make_winpe_resume_slot_admission_plan(
          checkpoint.operation_id,
          checkpoint.kind,
          checkpoint.source,
          checkpoint.target,
          checkpoint.expected_work_bytes,
          fields);
  check(same_operation.has_value(), "Same-operation admission fixture must build");
  const auto status = ytec::winpeapp::guard_new_winpe_operation_start(
      same_operation.value(), platform);
  check(
      !status && platform.observe_calls == 1U && platform.create_calls == 0U &&
          platform.replace_calls == 0U && platform.discard_calls == 0U,
      "An active slot must reject even the same operation through start-new");
}

void resume_requires_the_exact_complete_binding_and_never_mutates_preflight() {
  MockResumeSlotPlatform platform;
  platform.slot = make_record();
  const auto binding =
      ytec::operationcore::make_resume_slot_binding(*platform.slot);
  check(binding.has_value(), "Synthetic active slot must produce a binding");
  const auto exact = ytec::winpeapp::guard_bound_winpe_restore_resume(
      binding.value(), platform);
  check(exact.has_value(), "The exact active binding must pass read-only preflight");

  auto mismatch = binding.value();
  mismatch.operation_id[0] ^= std::byte{0x5A};
  const auto stale = ytec::winpeapp::guard_bound_winpe_restore_resume(
      mismatch, platform);
  check(
      !stale && platform.observe_calls == 2U && platform.create_calls == 0U &&
          platform.replace_calls == 0U && platform.discard_calls == 0U,
      "A mismatched resume binding must fail without touching owned state");
}

void startup_review_rejects_every_non_restore_persistent_capability() {
  const auto record = make_record();
  const auto binding = ytec::operationcore::make_resume_slot_binding(record);
  check(binding.has_value(), "Synthetic startup slot must produce a binding");
  const ytec::winpeapp::DirectImageRestoreResumeOutcome outcome{
      .kind = ytec::winpeapp::
          DirectImageRestoreResumeOutcomeKind::decision_required,
      .existing_slot = binding.value(),
      .verified_logical_bytes =
          record.checkpoint.checkpoint.verified_work_bytes,
      .verified_chunk_count =
          record.checkpoint.checkpoint.verified_chunk_count,
      .expected_logical_bytes =
          record.checkpoint.checkpoint.expected_work_bytes,
      .checkpoint_phase = record.checkpoint.checkpoint.phase,
      .capability = ytec::operationcore::
          ResumeCapability::persistent_pe_exact_image_create,
  };
  check(
      !ytec::winpeapp::format_direct_image_restore_resume_startup_review_v1(
          outcome),
      "Startup UI must not present a persistent non-restore capability");
}

}  // namespace

int main() {
  try {
    admission_plan_binds_reviewed_output_and_rejects_missing_binding();
    admission_canonicalization_rejects_empty_huge_and_overflow_shapes();
    fresh_empty_slot_allows_new_operation_without_mutation();
    active_unknown_and_orphaned_slot_state_block_every_new_operation();
    active_slot_also_blocks_starting_the_same_operation_as_new();
    resume_requires_the_exact_complete_binding_and_never_mutates_preflight();
    startup_review_rejects_every_non_restore_persistent_capability();
    std::cout << "winpe resume slot admission tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& exception) {
    std::cerr << "winpe resume slot admission tests: FAIL: "
              << exception.what() << '\n';
    return EXIT_FAILURE;
  }
}

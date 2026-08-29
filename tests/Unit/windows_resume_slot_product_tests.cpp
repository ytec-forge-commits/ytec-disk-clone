#include "ytec/windowsapp/resume_slot_product.h"

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
#include <vector>

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
      .serial_suffix = number == 4U ? "SOURCE04" :
          number == 9U ? "TARGET09" : "OUTPUT12",
      .device_instance_id = L"SYNTHETIC\\" + role,
      .is_system_disk = false,
  };
}

ytec::operationcore::ParsedCheckpoint parse_checkpoint_or_throw(
    const ytec::operationcore::InterruptionCheckpoint& checkpoint) {
  const auto bytes = ytec::operationcore::serialize_checkpoint(checkpoint);
  if (!bytes) {
    throw std::runtime_error("Synthetic checkpoint must serialize");
  }
  auto parsed = ytec::operationcore::parse_checkpoint(bytes.value());
  if (!parsed) {
    throw std::runtime_error("Synthetic checkpoint must parse");
  }
  return parsed.take_value();
}

ytec::operationcore::ResumeSlotRecord make_record() {
  const ytec::operationcore::OperationPlan plan{
      .operation_id = make_operation_id(),
      .kind = ytec::operationcore::OperationKind::image_restore,
      .environment = ytec::operationcore::OperationEnvironment::windows,
      .source = std::nullopt,
      .target = make_disk(9U, L"RESTORE-TARGET"),
      .expected_work_bytes = 8192U,
      .immutable_payload_hash = make_digest(0x20U),
  };
  const auto plan_hash = ytec::operationcore::hash_operation_plan(plan);
  if (!plan_hash) {
    throw std::runtime_error("Synthetic restore plan must hash");
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
      .continuity_token = L"WINDOWS-RESTORE-SYNTHETIC-0001",
  };
  const ytec::operationcore::ResumeIdentityBinding identities{
      .source_identity_hash = make_digest(0x30U),
      .target_identity_hash = make_digest(0x40U),
      .output_identity_hash = checkpoint.output_identity_hash,
  };
  return ytec::operationcore::ResumeSlotRecord{
      .capability =
          ytec::operationcore::ResumeCapability::persistent_exact_restore,
      .checkpoint = parse_checkpoint_or_throw(checkpoint),
      .identities = identities,
      .owned_partial =
          ytec::operationcore::ResumeOwnedPartialBinding{
              .operation_id = checkpoint.operation_id,
              .identities = identities,
              .file_object_identity_hash = make_digest(0x60U),
          },
  };
}

ytec::operationcore::ResumeSlotRecord make_image_create_record() {
  const ytec::operationcore::OperationPlan plan{
      .operation_id = make_operation_id(0x61U),
      .kind = ytec::operationcore::OperationKind::image_create,
      .environment = ytec::operationcore::OperationEnvironment::winpe,
      .source = make_disk(4U, L"IMAGE-CREATE-SOURCE"),
      .target = std::nullopt,
      .expected_work_bytes = 8192U,
      .immutable_payload_hash = make_digest(0x62U),
  };
  const auto plan_hash = ytec::operationcore::hash_operation_plan(plan);
  if (!plan_hash) {
    throw std::runtime_error("Synthetic image-create plan must hash");
  }
  const ytec::operationcore::InterruptionCheckpoint checkpoint{
      .schema_version = ytec::operationcore::kCheckpointSchemaVersionV3,
      .operation_id = plan.operation_id,
      .kind = plan.kind,
      .environment = plan.environment,
      .phase = ytec::operationcore::CheckpointPhase::prepared,
      .revision = 1U,
      .expected_work_bytes = plan.expected_work_bytes,
      .verified_work_bytes = 1024U,
      .verified_chunk_count = 1U,
      .plan_hash = plan_hash.value(),
      .output_identity_hash = make_digest(0x50U),
      .source = plan.source,
      .target = std::nullopt,
      .continuity_token = L"PE-SOURCE-STATE-SYNTHETIC-0001",
      .preparation_evidence = std::nullopt,
      .output_progress_evidence =
          ytec::operationcore::CheckpointOutputProgressEvidence{
              .verified_prefix_hash = make_digest(0x63U),
              .primary_output_length = 4096U,
              .journal_length = 752U,
              .auxiliary_output_length = 0U,
          },
  };
  const ytec::operationcore::ResumeIdentityBinding identities{
      .source_identity_hash = make_digest(0x30U),
      .target_identity_hash = make_digest(0x40U),
      .output_identity_hash = checkpoint.output_identity_hash,
  };
  return ytec::operationcore::ResumeSlotRecord{
      .capability = ytec::operationcore::ResumeCapability::
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
              .file_object_identity_hash = make_digest(0x70U),
          },
          {
              .role = ytec::operationcore::ResumeOwnedObjectRole::
                  image_resume_journal,
              .operation_id = checkpoint.operation_id,
              .identities = identities,
              .file_object_identity_hash = make_digest(0x71U),
          },
      },
  };
}

ytec::clonecore::Error synthetic_error() {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::invalid_data,
      .native_code = ERROR_INVALID_DATA,
      .operation = L"synthetic Windows resume slot",
      .message = L"synthetic failure",
  };
}

class MockResumeSlotPlatform final
    : public ytec::operationcore::IResumeSlotPlatform {
 public:
  bool fail_observe{};
  std::optional<ytec::operationcore::ResumeSlotRecord> slot;
  std::optional<ytec::operationcore::ResumeOwnedPartialBinding>
      observed_partial;
  std::vector<ytec::operationcore::ResumeOwnedObjectBinding>
      observed_objects;
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
    const std::vector<ytec::operationcore::ResumeFileStorageProof>
        object_files(
            observed_objects.size(),
            ytec::operationcore::ResumeFileStorageProof{
                .exists = true,
                .is_regular_file = true,
                .is_reparse_free = true,
                .hard_link_count = 1U,
            });
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
                .exists = observed_partial.has_value(),
                .is_regular_file = true,
                .is_reparse_free = true,
                .hard_link_count = observed_partial.has_value() ? 1U : 0U,
            },
            .owned_object_files = object_files,
        },
        .slot = slot,
        .observed_owned_partial = observed_partial,
        .observed_owned_objects = observed_objects,
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
      const ytec::operationcore::ResumeSlotBinding& expected) override {
    ++discard_calls;
    if (!slot.has_value()) {
      return ytec::clonecore::Status::failure(synthetic_error());
    }
    const auto actual =
        ytec::operationcore::make_resume_slot_binding(slot.value());
    if (!actual || actual.value().operation_id != expected.operation_id ||
        actual.value().checkpoint_record_hash !=
            expected.checkpoint_record_hash ||
        actual.value().identities.source_identity_hash !=
            expected.identities.source_identity_hash ||
        actual.value().identities.target_identity_hash !=
            expected.identities.target_identity_hash ||
        actual.value().identities.output_identity_hash !=
            expected.identities.output_identity_hash ||
        actual.value().partial_file_object_identity_hash !=
            expected.partial_file_object_identity_hash ||
        actual.value().owned_object_file_bindings !=
            expected.owned_object_file_bindings) {
      return ytec::clonecore::Status::failure(synthetic_error());
    }
    slot.reset();
    observed_partial.reset();
    observed_objects.clear();
    return ytec::clonecore::success_status();
  }
};

ytec::operationcore::OperationPlan make_clone_admission() {
  const std::array<std::wstring_view, 1U> fields{
      L"windows-exact-clone-v1"};
  auto plan = ytec::windowsapp::make_windows_resume_slot_admission_plan({
      .operation_id = make_operation_id(0x70U),
      .kind = ytec::operationcore::OperationKind::clone,
      .source = make_disk(4U, L"CLONE-SOURCE"),
      .target = make_disk(9U, L"CLONE-TARGET"),
      .output_backing = std::nullopt,
      .expected_work_bytes = 16384U,
      .immutable_review_fields = fields,
      .immutable_review_digests = {},
  });
  if (!plan) {
    throw std::runtime_error("Synthetic clone admission must build");
  }
  return plan.take_value();
}

void admission_plan_is_bounded_and_binds_real_output() {
  const auto source = make_disk(4U, L"IMAGE-SOURCE");
  const auto first_output = make_disk(12U, L"OUTPUT-A");
  auto second_output = first_output;
  second_output.device_instance_id = L"SYNTHETIC\\OUTPUT-B";
  const std::array<std::wstring_view, 1U> fields{
      L"C:\\reviewed\\backup.tsumugi"};
  const auto build = [&](const ytec::clonecore::StableDiskIdentity& output) {
    return ytec::windowsapp::make_windows_resume_slot_admission_plan({
        .operation_id = make_operation_id(0x20U),
        .kind = ytec::operationcore::OperationKind::image_create,
        .source = source,
        .target = std::nullopt,
        .output_backing = output,
        .expected_work_bytes = 4096U,
        .immutable_review_fields = fields,
        .immutable_review_digests = {},
    });
  };
  const auto first = build(first_output);
  const auto second = build(second_output);
  check(
      first && second &&
          first.value().immutable_payload_hash !=
              second.value().immutable_payload_hash &&
          first.value().environment ==
              ytec::operationcore::OperationEnvironment::windows,
      "Admission must bind the reviewed output backing without claiming a capability");

  const std::array<std::wstring_view, 1U> empty{L""};
  check(
      !ytec::windowsapp::make_windows_resume_slot_admission_plan({
          .operation_id = make_operation_id(0x21U),
          .kind = ytec::operationcore::OperationKind::image_create,
          .source = source,
          .target = std::nullopt,
          .output_backing = std::nullopt,
          .expected_work_bytes = 4096U,
          .immutable_review_fields = empty,
          .immutable_review_digests = {},
      }),
      "An empty framed review field must fail closed");
  const std::wstring huge(32U * 1024U + 1U, L'x');
  const std::array<std::wstring_view, 1U> huge_field{huge};
  check(
      !ytec::windowsapp::make_windows_resume_slot_admission_plan({
          .operation_id = make_operation_id(0x22U),
          .kind = ytec::operationcore::OperationKind::image_create,
          .source = source,
          .target = std::nullopt,
          .output_backing = std::nullopt,
          .expected_work_bytes = 4096U,
          .immutable_review_fields = huge_field,
          .immutable_review_digests = {},
      }),
      "An over-limit UTF-16 review field must fail closed");
  const std::wstring_view impossible(
      L"x", (std::numeric_limits<std::size_t>::max)());
  const std::array<std::wstring_view, 1U> overflow_field{impossible};
  check(
      !ytec::windowsapp::make_windows_resume_slot_admission_plan({
          .operation_id = make_operation_id(0x23U),
          .kind = ytec::operationcore::OperationKind::image_create,
          .source = source,
          .target = std::nullopt,
          .output_backing = std::nullopt,
          .expected_work_bytes = 4096U,
          .immutable_review_fields = overflow_field,
          .immutable_review_digests = {},
      }),
      "An overflow-shaped view must fail before dereference");
  std::array<std::wstring_view, 17U> too_many{};
  too_many.fill(L"bounded");
  check(
      !ytec::windowsapp::make_windows_resume_slot_admission_plan({
          .operation_id = make_operation_id(0x24U),
          .kind = ytec::operationcore::OperationKind::image_create,
          .source = source,
          .target = std::nullopt,
          .output_backing = std::nullopt,
          .expected_work_bytes = 4096U,
          .immutable_review_fields = too_many,
          .immutable_review_digests = {},
      }),
      "Too many review fields must fail closed");

  const std::wstring maximum_field(32U * 1024U, L'b');
  std::array<std::wstring_view, 16U> canonical_overflow{};
  canonical_overflow.fill(maximum_field);
  check(
      !ytec::windowsapp::make_windows_resume_slot_admission_plan({
          .operation_id = make_operation_id(0x25U),
          .kind = ytec::operationcore::OperationKind::image_create,
          .source = source,
          .target = std::nullopt,
          .output_backing = std::nullopt,
          .expected_work_bytes = 4096U,
          .immutable_review_fields = canonical_overflow,
          .immutable_review_digests = {},
      }),
      "Aggregate canonical bytes must remain bounded");

  std::array<ytec::operationcore::Sha256Digest, 17U> too_many_digests{};
  check(
      !ytec::windowsapp::make_windows_resume_slot_admission_plan({
          .operation_id = make_operation_id(0x26U),
          .kind = ytec::operationcore::OperationKind::image_create,
          .source = source,
          .target = std::nullopt,
          .output_backing = std::nullopt,
          .expected_work_bytes = 4096U,
          .immutable_review_fields = {},
          .immutable_review_digests = too_many_digests,
      }),
      "Too many review digests must fail closed");

  auto over_limit_output = first_output;
  over_limit_output.device_instance_id.assign(1025U, L'd');
  check(
      !build(over_limit_output),
      "An over-limit output backing identity must fail before hashing");
  auto control_output = first_output;
  control_output.serial_suffix = std::string("OUT\x01PUT", 7U);
  check(
      !build(control_output),
      "A control-bearing output backing identity must fail before hashing");
}

void global_admission_freshly_blocks_active_unknown_corrupt_and_orphan_state() {
  {
    MockResumeSlotPlatform platform;
    const auto status = ytec::windowsapp::guard_new_windows_operation_start(
        make_clone_admission(), platform);
    check(
        status && platform.observe_calls == 1U &&
            platform.create_calls == 0U && platform.replace_calls == 0U &&
            platform.discard_calls == 0U,
        "A fresh empty slot should admit by observation only");
  }
  for (int mode = 0; mode < 4; ++mode) {
    MockResumeSlotPlatform platform;
    if (mode == 0) {
      platform.slot = make_record();
      platform.observed_partial = platform.slot->owned_partial;
    } else if (mode == 1) {
      platform.fail_observe = true;
    } else if (mode == 2) {
      const auto record = make_record();
      platform.observed_partial = record.owned_partial;
    } else {
      platform.slot = make_record();
      platform.slot->identities.output_identity_hash[0] ^=
          std::byte{0x01};
      platform.observed_partial = platform.slot->owned_partial;
    }
    const auto status = ytec::windowsapp::guard_new_windows_operation_start(
        make_clone_admission(), platform);
    check(
        !status && platform.observe_calls == 1U &&
            platform.create_calls == 0U && platform.replace_calls == 0U &&
            platform.discard_calls == 0U,
        "Active, unknown, corrupt, or orphan state must block without mutation");
  }
}

void startup_view_never_offers_fake_resume_and_discard_is_exact_bound() {
  MockResumeSlotPlatform platform;
  platform.slot = make_record();
  platform.observed_partial = platform.slot->owned_partial;
  auto view = ytec::windowsapp::inspect_windows_resume_slot(platform);
  check(
      view && view.value().active &&
          !view.value().resume_action_available &&
          view.value().binding.has_value() &&
          view.value().details.find(L"backend") != std::wstring::npos &&
          view.value().owned_discard_summary.find(L".partial") !=
              std::wstring::npos,
      "Windows startup must explain unavailable resume and enumerate owned discard");

  auto wrong = view.value().binding.value();
  wrong.checkpoint_record_hash[0] ^= std::byte{0x01};
  const auto rejected = ytec::windowsapp::discard_bound_windows_resume_slot(
      wrong, platform);
  check(
      !rejected && platform.slot.has_value() &&
          platform.observed_partial.has_value() &&
          platform.discard_calls == 0U,
      "A stale binding must leave checkpoint and owned partial untouched");

  const auto discarded =
      ytec::windowsapp::discard_bound_windows_resume_slot(
          view.value().binding.value(), platform);
  check(
      discarded && !platform.slot.has_value() &&
          !platform.observed_partial.has_value() &&
          platform.discard_calls == 1U,
      "Only the exact binding may discard checkpoint and its owned partial");

  MockResumeSlotPlatform object_platform;
  object_platform.slot = make_image_create_record();
  object_platform.observed_objects = object_platform.slot->owned_objects;
  auto object_view =
      ytec::windowsapp::inspect_windows_resume_slot(object_platform);
  check(
      object_view &&
          object_view.value().owned_discard_summary.find(L"再開journal") !=
              std::wstring::npos,
      "Startup review must enumerate every checkpoint-owned object role");
  const auto object_discarded =
      ytec::windowsapp::discard_bound_windows_resume_slot(
          object_view.value().binding.value(), object_platform);
  check(
      object_discarded && object_platform.observed_objects.empty(),
      "Exact discard must include every checkpoint-enumerated owned object");
}

}  // namespace

int main() {
  try {
    admission_plan_is_bounded_and_binds_real_output();
    global_admission_freshly_blocks_active_unknown_corrupt_and_orphan_state();
    startup_view_never_offers_fake_resume_and_discard_is_exact_bound();
    std::cout << "windows_resume_slot_product_tests: PASS\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "windows_resume_slot_product_tests: FAIL: "
              << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

#include "ytec/operationcore/checkpoint.h"
#include "ytec/operationcore/operation.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
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

ytec::operationcore::Sha256Digest make_digest(
    const unsigned char seed) {
  ytec::operationcore::Sha256Digest digest{};
  for (std::size_t index = 0U; index < digest.size(); ++index) {
    digest[index] = static_cast<std::byte>(seed + index);
  }
  return digest;
}

ytec::clonecore::StableDiskIdentity make_identity(
    const std::uint32_t disk_number,
    std::wstring model,
    std::string serial,
    std::wstring device_id,
    const bool system = false) {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = disk_number,
      .model = std::move(model),
      .size_bytes = 128ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .serial_suffix = std::move(serial),
      .device_instance_id = std::move(device_id),
      .is_system_disk = system,
  };
}

ytec::operationcore::OperationPlan make_clone_plan() {
  return ytec::operationcore::OperationPlan{
      .schema_version = ytec::operationcore::kOperationPlanSchemaVersion,
      .operation_id = make_operation_id(),
      .kind = ytec::operationcore::OperationKind::clone,
      .environment = ytec::operationcore::OperationEnvironment::windows,
      .source = make_identity(
          1U, L"Source SSD", "SRC00001", L"TEST\\SOURCE", true),
      .target = make_identity(
          2U, L"Target SSD", "DST00002", L"TEST\\TARGET"),
      .expected_work_bytes = 4096U,
      .immutable_payload_hash = make_digest(0x20U),
  };
}

ytec::operationcore::InterruptionCheckpoint make_checkpoint(
    const ytec::operationcore::OperationPlan& plan) {
  const auto plan_hash = ytec::operationcore::hash_operation_plan(plan);
  if (!plan_hash) {
    throw TestFailure{"The sample operation plan must hash"};
  }
  return ytec::operationcore::InterruptionCheckpoint{
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
      .output_identity_hash = make_digest(0x70U),
      .source = plan.source,
      .target = plan.target,
      .continuity_token = L"VSS-SNAPSHOT-{SYNTHETIC-0001}",
  };
}

ytec::operationcore::InterruptionCheckpoint make_preparation_checkpoint(
    const ytec::operationcore::OperationPlan& plan,
    const ytec::operationcore::CheckpointPhase phase =
        ytec::operationcore::CheckpointPhase::preparing) {
  auto checkpoint = make_checkpoint(plan);
  checkpoint.phase = phase;
  checkpoint.preparation_evidence =
      ytec::operationcore::CheckpointPreparationEvidence{
          .initial_layout_hash = make_digest(0x31U),
          .logical_sector_size = 512U,
          .original_sectors = {
              {
                  .offset = 0U,
                  .length = 512U,
                  .original_hash = make_digest(0x41U),
              },
              {
                  .offset = 512U,
                  .length = 512U,
                  .original_hash = make_digest(0x42U),
              },
          },
      };
  if (phase == ytec::operationcore::CheckpointPhase::preparing) {
    checkpoint.verified_work_bytes = 0U;
    checkpoint.verified_chunk_count = 0U;
  } else if (phase == ytec::operationcore::CheckpointPhase::commit_ready) {
    checkpoint.verified_work_bytes = checkpoint.expected_work_bytes;
    checkpoint.verified_chunk_count = 4U;
  }
  return checkpoint;
}

ytec::operationcore::InterruptionCheckpoint make_v3_image_checkpoint() {
  const ytec::operationcore::OperationPlan plan{
      .schema_version = ytec::operationcore::kOperationPlanSchemaVersion,
      .operation_id = make_operation_id(0x51U),
      .kind = ytec::operationcore::OperationKind::image_create,
      .environment = ytec::operationcore::OperationEnvironment::winpe,
      .source = make_identity(
          3U, L"Synthetic image source", "IMAGE-SRC", L"TEST\\IMAGE-SRC"),
      .target = std::nullopt,
      .expected_work_bytes = 4096U,
      .immutable_payload_hash = make_digest(0x52U),
  };
  const auto plan_hash = ytec::operationcore::hash_operation_plan(plan);
  if (!plan_hash) {
    throw TestFailure{"The sample image-create plan must hash"};
  }
  return ytec::operationcore::InterruptionCheckpoint{
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
      .output_identity_hash = make_digest(0x53U),
      .source = plan.source,
      .target = std::nullopt,
      .continuity_token = L"PE-SOURCE-STATE-SYNTHETIC-0001",
      .preparation_evidence = std::nullopt,
      .output_progress_evidence =
          ytec::operationcore::CheckpointOutputProgressEvidence{
              .verified_prefix_hash = make_digest(0x54U),
              .primary_output_length = 0U,
              .journal_length = 0U,
              .auxiliary_output_length = 0U,
          },
  };
}

ytec::clonecore::Error test_error(
    const ytec::clonecore::ErrorCode code,
    std::wstring operation) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::move(operation),
      .message = L"synthetic failure",
  };
}

ytec::operationcore::OperationCallbacks successful_callbacks(
    const ytec::operationcore::Sha256Digest& output_hash,
    std::vector<std::string>* order = nullptr) {
  ytec::operationcore::OperationCallbacks callbacks;
  callbacks.reidentify = [order](
                               const ytec::operationcore::OperationPlan& plan) {
    if (order != nullptr) {
      order->push_back("reidentify");
    }
    auto source = plan.source;
    auto target = plan.target;
    // Disk numbers are not stable identity. Simulate a harmless renumbering.
    if (source) {
      source->disk_number += 10U;
    }
    if (target) {
      target->disk_number += 10U;
    }
    return ytec::clonecore::Result<
        ytec::operationcore::ReidentifiedOperation>::success(
        ytec::operationcore::ReidentifiedOperation{
            .source = std::move(source),
            .target = std::move(target),
        });
  };
  callbacks.execute = [output_hash, order](
                          const ytec::operationcore::OperationPlan& plan,
                          const ytec::clonecore::DiskOperationCallbacks&) {
    if (order != nullptr) {
      order->push_back("execute");
    }
    return ytec::clonecore::Result<
        ytec::operationcore::ExecutionEvidence>::success(
        ytec::operationcore::ExecutionEvidence{
            .processed_work_bytes = plan.expected_work_bytes,
            .output_hash = output_hash,
        });
  };
  callbacks.verify = [output_hash, order](
                         const ytec::operationcore::OperationPlan& plan,
                         const ytec::operationcore::ExecutionEvidence&,
                         const ytec::clonecore::DiskOperationCallbacks&) {
    if (order != nullptr) {
      order->push_back("verify");
    }
    return ytec::clonecore::Result<
        ytec::operationcore::VerificationEvidence>::success(
        ytec::operationcore::VerificationEvidence{
            .verified_work_bytes = plan.expected_work_bytes,
            .output_hash = output_hash,
        });
  };
  return callbacks;
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    static unsigned long instance_counter = 0U;
    ++instance_counter;
    std::array<wchar_t, MAX_PATH + 1U> temporary{};
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(temporary.size()), temporary.data());
    if (length == 0U || length >= temporary.size()) {
      throw TestFailure{"A temporary directory must be available"};
    }
    path_ = std::wstring(temporary.data(), length) +
            L"ytec-operation-core-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(instance_counter);
    if (!CreateDirectoryW(path_.c_str(), nullptr)) {
      throw TestFailure{"The isolated temporary directory must be created"};
    }
  }

  ~TemporaryDirectory() {
    static_cast<void>(DeleteFileW(checkpoint_path().c_str()));
    static_cast<void>(DeleteFileW(stage_path().c_str()));
    static_cast<void>(RemoveDirectoryW(path_.c_str()));
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] std::wstring checkpoint_path() const {
    return path_ + L"\\active.checkpoint";
  }

  [[nodiscard]] std::wstring stage_path() const {
    return checkpoint_path() + L".new";
  }

 private:
  std::wstring path_;
};

void write_new_bytes(
    const std::wstring& path,
    const std::vector<std::byte>& bytes) {
  HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      0,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    throw TestFailure{"The synthetic file must be created"};
  }
  DWORD written{};
  const BOOL write_ok = WriteFile(
      file,
      bytes.data(),
      static_cast<DWORD>(bytes.size()),
      &written,
      nullptr);
  CloseHandle(file);
  if (!write_ok || static_cast<std::size_t>(written) != bytes.size()) {
    throw TestFailure{"The synthetic file must be written completely"};
  }
}

std::vector<std::byte> read_bytes(const std::wstring& path) {
  HANDLE file = CreateFileW(
      path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    throw TestFailure{"The synthetic file must remain readable"};
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      size.QuadPart > 1024 * 1024) {
    CloseHandle(file);
    throw TestFailure{"The synthetic file must have a bounded size"};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  DWORD read{};
  const BOOL read_ok = ReadFile(
      file,
      bytes.data(),
      static_cast<DWORD>(bytes.size()),
      &read,
      nullptr);
  CloseHandle(file);
  if (!read_ok || static_cast<std::size_t>(read) != bytes.size()) {
    throw TestFailure{"The synthetic file must be read completely"};
  }
  return bytes;
}

bool file_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void test_plan_validation_and_hash_binding() {
  auto plan = make_clone_plan();
  check(
      ytec::operationcore::validate_operation_plan(plan).has_value(),
      "A complete clone plan should be accepted");
  const auto first_hash = ytec::operationcore::hash_operation_plan(plan);
  check(first_hash.has_value(), "A valid operation plan should hash");

  plan.immutable_payload_hash[0] ^= std::byte{0x01};
  const auto changed_hash = ytec::operationcore::hash_operation_plan(plan);
  check(changed_hash.has_value() &&
            changed_hash.value() != first_hash.value(),
        "The immutable payload must be bound into the plan hash");

  plan = make_clone_plan();
  plan.operation_id = {};
  check(!ytec::operationcore::validate_operation_plan(plan),
        "A zero operation id must fail closed");
  plan = make_clone_plan();
  plan.target.reset();
  check(!ytec::operationcore::validate_operation_plan(plan),
        "A clone plan must contain exactly source and target identities");
  plan = make_clone_plan();
  plan.target = plan.source;
  check(!ytec::operationcore::validate_operation_plan(plan),
        "The same stable device must never be source and target");
}

void test_pipeline_requires_fresh_identity_exact_ok_and_verification() {
  const auto plan = make_clone_plan();
  const auto output_hash = make_digest(0x90U);
  std::vector<std::string> order;
  auto callbacks = successful_callbacks(output_hash, &order);
  std::vector<ytec::clonecore::DiskOperationStage> progress;
  callbacks.disk_operation.progress = [&progress](
      const ytec::clonecore::DiskOperationProgress& value) {
    progress.push_back(value.stage);
  };

  const auto lowercase =
      ytec::operationcore::run_operation(plan, L"ok", callbacks);
  check(lowercase.outcome == ytec::operationcore::OperationOutcome::failed &&
            lowercase.phase ==
                ytec::operationcore::OperationPhase::awaiting_confirmation &&
            lowercase.error &&
            lowercase.error->code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "Only the exact uppercase OK token may cross the destructive boundary");
  check(order == std::vector<std::string>{"reidentify"},
        "Confirmation must happen after re-identification and before execute");

  order.clear();
  progress.clear();
  const auto completed =
      ytec::operationcore::run_operation(plan, L"OK", callbacks);
  check(completed.outcome ==
                ytec::operationcore::OperationOutcome::completed &&
            completed.phase == ytec::operationcore::OperationPhase::completed &&
            !completed.error && completed.processed_work_bytes == 4096U &&
            completed.verified_work_bytes == 4096U,
        "A plan may complete only after the mandatory verify callback");
  check(order == std::vector<std::string>(
                     {"reidentify", "execute", "verify"}),
        "The lifecycle order must remain fixed");
  check(!progress.empty() &&
            progress.back() == ytec::clonecore::DiskOperationStage::completed,
        "Existing disk progress observers should receive completion");

  callbacks = successful_callbacks(output_hash);
  callbacks.verify = [](const ytec::operationcore::OperationPlan& plan,
                        const ytec::operationcore::ExecutionEvidence&,
                        const ytec::clonecore::DiskOperationCallbacks&) {
    return ytec::clonecore::Result<
        ytec::operationcore::VerificationEvidence>::success(
        ytec::operationcore::VerificationEvidence{
            .verified_work_bytes = plan.expected_work_bytes,
            .output_hash = make_digest(0xA0U),
        });
  };
  const auto mismatch =
      ytec::operationcore::run_operation(plan, L"OK", callbacks);
  check(mismatch.outcome == ytec::operationcore::OperationOutcome::failed &&
            mismatch.phase == ytec::operationcore::OperationPhase::verifying &&
            mismatch.error &&
            mismatch.error->code ==
                ytec::clonecore::ErrorCode::verification_failed,
        "A read-back hash mismatch must never complete");

  int cancellation_checks = 0;
  callbacks = successful_callbacks(output_hash);
  callbacks.disk_operation.cancellation_requested = [&cancellation_checks]() {
    ++cancellation_checks;
    return cancellation_checks >= 3;
  };
  const auto late_cancel =
      ytec::operationcore::run_operation(plan, L"OK", callbacks);
  check(late_cancel.outcome ==
            ytec::operationcore::OperationOutcome::completed,
        "A late cancellation must never bypass verification after execution");
}

void test_pipeline_fails_before_execution_on_identity_change() {
  const auto plan = make_clone_plan();
  bool executed = false;
  auto callbacks = successful_callbacks(make_digest(0x90U));
  callbacks.reidentify = [](const ytec::operationcore::OperationPlan& plan) {
    auto target = plan.target;
    target->serial_suffix = "CHANGED";
    return ytec::clonecore::Result<
        ytec::operationcore::ReidentifiedOperation>::success(
        ytec::operationcore::ReidentifiedOperation{
            .source = plan.source,
            .target = std::move(target),
        });
  };
  callbacks.execute = [&executed](
                          const ytec::operationcore::OperationPlan&,
                          const ytec::clonecore::DiskOperationCallbacks&) {
    executed = true;
    return ytec::clonecore::Result<
        ytec::operationcore::ExecutionEvidence>::failure(
        test_error(ytec::clonecore::ErrorCode::internal_error, L"unexpected"));
  };
  const auto result =
      ytec::operationcore::run_operation(plan, L"OK", callbacks);
  check(!executed &&
            result.phase ==
                ytec::operationcore::OperationPhase::reidentifying &&
            result.error &&
            result.error->code ==
                ytec::clonecore::ErrorCode::identity_mismatch,
        "Changed stable identity must stop before confirmation and execution");
}

void test_clone_pipeline_records_actual_bounded_sparse_bytes() {
  const auto plan = make_clone_plan();
  const auto output_hash = make_digest(0x91U);
  auto callbacks = successful_callbacks(output_hash);
  callbacks.execute = [output_hash](
                          const ytec::operationcore::OperationPlan&,
                          const ytec::clonecore::DiskOperationCallbacks&) {
    return ytec::clonecore::Result<
        ytec::operationcore::ExecutionEvidence>::success({
        .processed_work_bytes = 1024U,
        .output_hash = output_hash,
    });
  };
  callbacks.verify = [output_hash](
                         const ytec::operationcore::OperationPlan&,
                         const ytec::operationcore::ExecutionEvidence&,
                         const ytec::clonecore::DiskOperationCallbacks&) {
    return ytec::clonecore::Result<
        ytec::operationcore::VerificationEvidence>::success({
        .verified_work_bytes = 1024U,
        .output_hash = output_hash,
    });
  };
  const auto sparse =
      ytec::operationcore::run_operation(plan, L"OK", callbacks);
  check(
      sparse.outcome ==
              ytec::operationcore::OperationOutcome::completed &&
          sparse.processed_work_bytes == 1024U &&
          sparse.verified_work_bytes == 1024U,
      "Clone lifecycle must record actual sparse transfer bytes");

  callbacks.verify = [output_hash](
                         const ytec::operationcore::OperationPlan& plan,
                         const ytec::operationcore::ExecutionEvidence&,
                         const ytec::clonecore::DiskOperationCallbacks&) {
    return ytec::clonecore::Result<
        ytec::operationcore::VerificationEvidence>::success({
        .verified_work_bytes = plan.expected_work_bytes,
        .output_hash = output_hash,
    });
  };
  const auto mismatched =
      ytec::operationcore::run_operation(plan, L"OK", callbacks);
  check(
      mismatched.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          mismatched.phase ==
              ytec::operationcore::OperationPhase::verifying,
      "Sparse verification must equal actual execution bytes");

  callbacks = successful_callbacks(output_hash);
  callbacks.execute = [output_hash](
                          const ytec::operationcore::OperationPlan& plan,
                          const ytec::clonecore::DiskOperationCallbacks&) {
    return ytec::clonecore::Result<
        ytec::operationcore::ExecutionEvidence>::success({
        .processed_work_bytes = plan.expected_work_bytes + 1U,
        .output_hash = output_hash,
    });
  };
  const auto overflow =
      ytec::operationcore::run_operation(plan, L"OK", callbacks);
  check(
      overflow.outcome ==
              ytec::operationcore::OperationOutcome::failed &&
          overflow.phase ==
              ytec::operationcore::OperationPhase::executing,
      "Sparse execution may not exceed the reviewed logical upper bound");
}

void test_non_destructive_image_create_does_not_require_ok() {
  auto plan = make_clone_plan();
  plan.kind = ytec::operationcore::OperationKind::image_create;
  plan.target.reset();
  auto callbacks = successful_callbacks(make_digest(0x90U));
  const auto result =
      ytec::operationcore::run_operation(plan, L"", callbacks);
  check(result.outcome == ytec::operationcore::OperationOutcome::completed,
        "CREATE_NEW image creation should not require a destructive OK token");
}

void test_checkpoint_round_trip_rejects_version_size_and_hash_tampering() {
  const auto plan = make_clone_plan();
  const auto checkpoint = make_checkpoint(plan);
  const auto bytes = ytec::operationcore::serialize_checkpoint(checkpoint);
  check(bytes.has_value() &&
            bytes.value().size() <=
                ytec::operationcore::kMaximumCheckpointBytes &&
            bytes.value()[10] == std::byte{0x01} &&
            bytes.value()[11] == std::byte{0x00},
        "A schema-v2 checkpoint should serialize as binary minor 1 within the fixed bound");
  const auto parsed = ytec::operationcore::parse_checkpoint(bytes.value());
  check(parsed.has_value() &&
            parsed.value().checkpoint.schema_version ==
                ytec::operationcore::kCheckpointSchemaVersionV2 &&
            parsed.value().checkpoint.revision == checkpoint.revision &&
            parsed.value().checkpoint.continuity_token ==
                checkpoint.continuity_token &&
            parsed.value().checkpoint.source->serial_suffix ==
                checkpoint.source->serial_suffix,
        "All continuity and stable identity fields must round-trip");

  const auto preparation = make_preparation_checkpoint(
      plan, ytec::operationcore::CheckpointPhase::prepared);
  const auto preparation_bytes =
      ytec::operationcore::serialize_checkpoint(preparation);
  check(preparation_bytes.has_value(),
        "Schema-v2 preparation evidence must serialize");
  const auto parsed_preparation =
      ytec::operationcore::parse_checkpoint(preparation_bytes.value());
  check(
      parsed_preparation &&
          parsed_preparation.value().checkpoint.preparation_evidence ==
              preparation.preparation_evidence,
      "Schema-v2 preparation evidence must round-trip after disk identities");

  auto legacy = checkpoint;
  legacy.schema_version = ytec::operationcore::kCheckpointSchemaVersionV1;
  const auto legacy_bytes =
      ytec::operationcore::serialize_checkpoint(legacy);
  check(legacy_bytes.has_value(), "Schema-v1 checkpoint must serialize");
  const auto parsed_legacy =
      ytec::operationcore::parse_checkpoint(legacy_bytes.value());
  check(
      legacy_bytes && legacy_bytes.value()[10] == std::byte{0x00} &&
          legacy_bytes.value()[11] == std::byte{0x00} && parsed_legacy &&
          parsed_legacy.value().checkpoint.schema_version ==
              ytec::operationcore::kCheckpointSchemaVersionV1 &&
          !parsed_legacy.value().checkpoint.preparation_evidence,
      "Binary minor 0/schema v1 checkpoints must remain parse-compatible");

  auto tampered = bytes.value();
  tampered[80] ^= std::byte{0x01};
  check(!ytec::operationcore::parse_checkpoint(tampered),
        "Payload tampering must fail SHA-256 validation");

  auto wrong_version = bytes.value();
  wrong_version[8] = std::byte{0x02};
  check(!ytec::operationcore::parse_checkpoint(wrong_version),
        "Unknown checkpoint major versions must fail closed");

  auto wrong_minor = bytes.value();
  wrong_minor[10] = std::byte{0x02};
  check(!ytec::operationcore::parse_checkpoint(wrong_minor),
        "Unknown checkpoint minor versions must fail closed");

  auto wrong_size = bytes.value();
  wrong_size[12] ^= std::byte{0x01};
  check(!ytec::operationcore::parse_checkpoint(wrong_size),
        "A declared-size mismatch must fail closed");

  auto unknown_flags = bytes.value();
  unknown_flags[19] |= std::byte{0x80};
  check(!ytec::operationcore::parse_checkpoint(unknown_flags),
        "Unknown binary fields must fail closed");

  auto nonzero_reserved = bytes.value();
  nonzero_reserved[20] = std::byte{0x01};
  check(!ytec::operationcore::parse_checkpoint(nonzero_reserved),
        "Nonzero reserved fields must fail closed");

  auto unknown_trailing = bytes.value();
  unknown_trailing.insert(
      unknown_trailing.end() -
          static_cast<std::ptrdiff_t>(
              ytec::operationcore::Sha256Digest{}.size()),
      std::byte{0x01});
  const auto expanded_size =
      static_cast<std::uint32_t>(unknown_trailing.size());
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    unknown_trailing[12U + shift / 8U] = static_cast<std::byte>(
        (expanded_size >> shift) & 0xffU);
  }
  check(!ytec::operationcore::parse_checkpoint(unknown_trailing),
        "Declared but unknown trailing fields must fail closed");

  auto unknown_phase = checkpoint;
  unknown_phase.phase =
      static_cast<ytec::operationcore::CheckpointPhase>(0xffU);
  check(!ytec::operationcore::serialize_checkpoint(unknown_phase),
        "Unknown checkpoint phases must fail before serialization");

  auto unknown_schema = checkpoint;
  unknown_schema.schema_version = 3U;
  check(!ytec::operationcore::serialize_checkpoint(unknown_schema),
        "Unknown checkpoint schema versions must fail before serialization");
}

void test_checkpoint_resume_requires_same_plan_identity_state_and_output() {
  auto plan = make_clone_plan();
  const auto checkpoint = make_checkpoint(plan);
  auto observed_source = plan.source;
  auto observed_target = plan.target;
  observed_source->disk_number = 7U;
  observed_target->disk_number = 8U;
  const ytec::operationcore::ReidentifiedOperation observed{
      .source = observed_source,
      .target = observed_target,
  };
  check(
      ytec::operationcore::validate_checkpoint_for_resume(
          checkpoint,
          plan,
          observed,
          checkpoint.continuity_token,
          checkpoint.output_identity_hash)
          .has_value(),
      "Stable identity plus exact continuity and output should permit resume");
  check(
      !ytec::operationcore::validate_checkpoint_for_resume(
          checkpoint,
          plan,
          observed,
          L"DIFFERENT-SNAPSHOT",
          checkpoint.output_identity_hash),
      "A different snapshot/source-state token must refuse resume");
  check(
      !ytec::operationcore::validate_checkpoint_for_resume(
          checkpoint,
          plan,
          observed,
          checkpoint.continuity_token,
          make_digest(0x71U)),
      "A different output identity must refuse resume");

  plan.immutable_payload_hash[0] ^= std::byte{0x01};
  check(
      !ytec::operationcore::validate_checkpoint_for_resume(
          checkpoint,
          plan,
          observed,
          checkpoint.continuity_token,
          checkpoint.output_identity_hash),
      "Any immutable plan change must refuse resume");
}

void test_checkpoint_preparation_evidence_is_bounded_and_target_bound() {
  const auto plan = make_clone_plan();
  const auto valid = make_preparation_checkpoint(plan);
  check(ytec::operationcore::validate_checkpoint(valid).has_value(),
        "A sector-aligned schema-v2 preparing checkpoint should be valid");

  auto legacy_with_evidence = valid;
  legacy_with_evidence.schema_version =
      ytec::operationcore::kCheckpointSchemaVersionV1;
  check(!ytec::operationcore::validate_checkpoint(legacy_with_evidence),
        "Schema v1 must reject preparation phases and evidence");

  auto missing_evidence = valid;
  missing_evidence.preparation_evidence.reset();
  check(!ytec::operationcore::validate_checkpoint(missing_evidence),
        "Preparation phases must require preparation evidence");

  auto evidence_on_legacy_phase = valid;
  evidence_on_legacy_phase.phase =
      ytec::operationcore::CheckpointPhase::executing;
  check(!ytec::operationcore::validate_checkpoint(evidence_on_legacy_phase),
        "Legacy executing/verifying phases must reject preparation evidence");

  auto preparing_with_progress = valid;
  preparing_with_progress.verified_work_bytes = 512U;
  preparing_with_progress.verified_chunk_count = 1U;
  check(!ytec::operationcore::validate_checkpoint(preparing_with_progress),
        "Preparing must retain a zero progress cursor");

  auto missing_target = valid;
  missing_target.target.reset();
  check(!ytec::operationcore::validate_checkpoint(missing_target),
        "Preparation evidence must require a target identity");

  auto sector_mismatch = valid;
  sector_mismatch.preparation_evidence->logical_sector_size = 4096U;
  check(!ytec::operationcore::validate_checkpoint(sector_mismatch),
        "Evidence sector size must equal the target logical sector size");

  auto wrong_length = valid;
  wrong_length.preparation_evidence->original_sectors[0].length = 1024U;
  check(!ytec::operationcore::validate_checkpoint(wrong_length),
        "Every preparation entry must cover exactly one logical sector");

  auto overlap = valid;
  overlap.preparation_evidence->original_sectors[1].offset = 0U;
  check(!ytec::operationcore::validate_checkpoint(overlap),
        "Preparation sector entries must be strictly ordered and nonoverlapping");

  auto out_of_bounds = valid;
  out_of_bounds.preparation_evidence->original_sectors.back().offset =
      out_of_bounds.target->size_bytes;
  check(!ytec::operationcore::validate_checkpoint(out_of_bounds),
        "Preparation sector entries must remain within the target size");

  auto zero_hash = valid;
  zero_hash.preparation_evidence->original_sectors[0].original_hash = {};
  check(!ytec::operationcore::validate_checkpoint(zero_hash),
        "Every original sector digest must be nonzero");

  auto zero_layout_hash = valid;
  zero_layout_hash.preparation_evidence->initial_layout_hash = {};
  check(!ytec::operationcore::validate_checkpoint(zero_layout_hash),
        "The reviewed initial-layout digest must be nonzero");

  auto too_many = valid;
  too_many.preparation_evidence->original_sectors.resize(
      ytec::operationcore::kMaximumCheckpointPreparationSectors + 1U,
      valid.preparation_evidence->original_sectors.front());
  check(!ytec::operationcore::validate_checkpoint(too_many),
        "Preparation sector count must fail above the strict bound");

  auto full_capacity = valid;
  auto& sectors = full_capacity.preparation_evidence->original_sectors;
  sectors.clear();
  sectors.reserve(ytec::operationcore::kMaximumCheckpointPreparationSectors);
  constexpr std::uint64_t kSectorBytes = 512U;
  constexpr std::size_t kSectorsPerRange = 2048U;
  for (std::size_t index = 0U; index < kSectorsPerRange; ++index) {
    sectors.push_back({
        .offset = static_cast<std::uint64_t>(index) * kSectorBytes,
        .length = kSectorBytes,
        .original_hash = make_digest(0x51U),
    });
  }
  const std::uint64_t final_range =
      full_capacity.target->size_bytes -
      static_cast<std::uint64_t>(kSectorsPerRange) * kSectorBytes;
  for (std::size_t index = 0U; index < kSectorsPerRange; ++index) {
    sectors.push_back({
        .offset = final_range +
            static_cast<std::uint64_t>(index) * kSectorBytes,
        .length = kSectorBytes,
        .original_hash = make_digest(0x52U),
    });
  }
  const auto full_bytes =
      ytec::operationcore::serialize_checkpoint(full_capacity);
  static_assert(
      ytec::operationcore::kMaximumCheckpointBytes == 256U * 1024U,
      "The checkpoint envelope must remain bounded to 256 KiB");
  check(
      sectors.size() ==
          ytec::operationcore::kMaximumCheckpointPreparationSectors,
      "The maximum preparation evidence must contain exactly 4096 sectors");
  check(full_bytes.has_value(),
        "The maximum preparation evidence must serialize");
  check(
      full_bytes.value().size() <=
          ytec::operationcore::kMaximumCheckpointBytes,
      "Two 1-MiB metadata ranges must fit as 4096 sector hashes under 256 KiB");
  const auto parsed_full =
      ytec::operationcore::parse_checkpoint(full_bytes.value());
  check(
      parsed_full && parsed_full.value().checkpoint.preparation_evidence &&
          parsed_full.value()
                  .checkpoint.preparation_evidence->original_sectors.size() ==
              ytec::operationcore::kMaximumCheckpointPreparationSectors,
      "The bounded parser must accept exactly the 4096-sector maximum");
}

void test_checkpoint_transition_is_single_monotonic_revision() {
  const auto plan = make_clone_plan();
  const auto current = make_checkpoint(plan);
  auto next = current;
  next.revision = 2U;
  next.verified_work_bytes = 2048U;
  next.verified_chunk_count = 2U;
  check(
      ytec::operationcore::validate_checkpoint_transition(current, next)
          .has_value(),
      "The same operation may advance by exactly one revision");

  auto skipped = next;
  skipped.revision = 3U;
  check(!ytec::operationcore::validate_checkpoint_transition(current, skipped),
        "Skipped checkpoint revisions must not be accepted");
  auto regressed = next;
  regressed.verified_work_bytes = 512U;
  check(
      !ytec::operationcore::validate_checkpoint_transition(current, regressed),
      "Verified progress must never regress");
  auto changed_output = next;
  changed_output.output_identity_hash = make_digest(0x72U);
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          current, changed_output),
      "A checkpoint update cannot switch its output identity");

  auto verifying = current;
  verifying.revision = 2U;
  verifying.phase = ytec::operationcore::CheckpointPhase::verifying;
  check(
      ytec::operationcore::validate_checkpoint_transition(current, verifying)
          .has_value(),
      "Legacy executing may still enter verifying without changing progress");
  auto leave_verifying = verifying;
  leave_verifying.revision = 3U;
  leave_verifying.phase = ytec::operationcore::CheckpointPhase::executing;
  leave_verifying.verified_work_bytes = 2048U;
  leave_verifying.verified_chunk_count = 2U;
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          verifying, leave_verifying),
      "Legacy verifying must remain terminal except for monotonic verification progress");
}

void test_checkpoint_preparation_phase_transitions_are_strict() {
  const auto plan = make_clone_plan();
  const auto preparing = make_preparation_checkpoint(plan);

  auto prepared = preparing;
  prepared.revision = 2U;
  prepared.phase = ytec::operationcore::CheckpointPhase::prepared;
  check(
      ytec::operationcore::validate_checkpoint_transition(
          preparing, prepared)
          .has_value(),
      "Preparing may transition only to a zero-progress prepared checkpoint");

  auto prepared_with_progress = prepared;
  prepared_with_progress.revision = 3U;
  prepared_with_progress.verified_work_bytes = 1024U;
  prepared_with_progress.verified_chunk_count = 1U;
  check(
      ytec::operationcore::validate_checkpoint_transition(
          prepared, prepared_with_progress)
          .has_value(),
      "Prepared payload progress may advance monotonically");

  auto full_prepared = prepared_with_progress;
  full_prepared.revision = 4U;
  full_prepared.verified_work_bytes = full_prepared.expected_work_bytes;
  full_prepared.verified_chunk_count = 4U;
  check(
      ytec::operationcore::validate_checkpoint_transition(
          prepared_with_progress, full_prepared)
          .has_value(),
      "Prepared may durably reach full payload progress");

  auto commit_ready = full_prepared;
  commit_ready.revision = 5U;
  commit_ready.phase = ytec::operationcore::CheckpointPhase::commit_ready;
  check(
      ytec::operationcore::validate_checkpoint_transition(
          full_prepared, commit_ready)
          .has_value(),
      "Only a full prepared checkpoint may enter commit-ready without changing progress");

  auto skipped_prepared = preparing;
  skipped_prepared.revision = 2U;
  skipped_prepared.phase =
      ytec::operationcore::CheckpointPhase::commit_ready;
  skipped_prepared.verified_work_bytes = skipped_prepared.expected_work_bytes;
  skipped_prepared.verified_chunk_count = 4U;
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          preparing, skipped_prepared),
      "Preparing must not skip directly to commit-ready");

  auto progress_during_prepare_transition = prepared;
  progress_during_prepare_transition.verified_work_bytes = 512U;
  progress_during_prepare_transition.verified_chunk_count = 1U;
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          preparing, progress_during_prepare_transition),
      "Preparing-to-prepared must not combine preparation and payload progress");

  auto premature_commit = prepared_with_progress;
  premature_commit.revision = 4U;
  premature_commit.phase =
      ytec::operationcore::CheckpointPhase::commit_ready;
  check(!ytec::operationcore::validate_checkpoint(premature_commit),
        "Commit-ready must require full work and at least one verified chunk");

  auto changed_evidence = prepared_with_progress;
  changed_evidence.revision = 4U;
  changed_evidence.verified_work_bytes = 2048U;
  changed_evidence.verified_chunk_count = 2U;
  changed_evidence.preparation_evidence->initial_layout_hash[0] ^=
      std::byte{0x01};
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          prepared_with_progress, changed_evidence),
      "Preparation evidence must remain immutable across progress updates");

  auto leave_commit_ready = commit_ready;
  leave_commit_ready.revision = 6U;
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          commit_ready, leave_commit_ready),
      "Commit-ready checkpoints must not admit another transition");
}

void test_checkpoint_legacy_upgrade_to_preparation_is_one_way() {
  const auto plan = make_clone_plan();
  auto legacy_progress = make_checkpoint(plan);
  legacy_progress.schema_version =
      ytec::operationcore::kCheckpointSchemaVersionV1;

  auto upgraded_prepared = make_preparation_checkpoint(
      plan, ytec::operationcore::CheckpointPhase::prepared);
  upgraded_prepared.revision = legacy_progress.revision + 1U;
  check(
      ytec::operationcore::validate_checkpoint_transition(
          legacy_progress, upgraded_prepared)
          .has_value(),
      "Legacy executing progress may upgrade once to v2 prepared with "
      "identical progress and new evidence");

  auto legacy_zero = legacy_progress;
  legacy_zero.verified_work_bytes = 0U;
  legacy_zero.verified_chunk_count = 0U;
  auto upgraded_preparing = make_preparation_checkpoint(plan);
  upgraded_preparing.revision = legacy_zero.revision + 1U;
  check(
      ytec::operationcore::validate_checkpoint_transition(
          legacy_zero, upgraded_preparing)
          .has_value(),
      "A zero-cursor legacy executing checkpoint may upgrade once to v2 preparing");

  auto changed_progress = upgraded_prepared;
  changed_progress.verified_work_bytes = 2048U;
  changed_progress.verified_chunk_count = 2U;
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          legacy_progress, changed_progress),
      "Schema upgrade must not change the durable cursor");

  auto changed_target = upgraded_prepared;
  changed_target.target->serial_suffix = "REPLACED";
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          legacy_progress, changed_target),
      "Schema upgrade must preserve the bound operation target identity");

  auto legacy_verifying = legacy_progress;
  legacy_verifying.phase = ytec::operationcore::CheckpointPhase::verifying;
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          legacy_verifying, upgraded_prepared),
      "Only legacy executing checkpoints may enter the preparation lifecycle");

  auto downgrade = legacy_progress;
  downgrade.revision = upgraded_prepared.revision + 1U;
  check(
      !ytec::operationcore::validate_checkpoint_transition(
          upgraded_prepared, downgrade),
      "A schema-v2 preparation checkpoint must never downgrade to schema v1");
}

void test_checkpoint_store_create_replace_and_hash_bound_discard() {
  TemporaryDirectory temporary;
  const auto plan = make_clone_plan();
  const auto current = make_checkpoint(plan);
  const auto created = ytec::operationcore::create_single_checkpoint(
      temporary.checkpoint_path(), current);
  check(created.has_value(), "A new checkpoint should commit safely");
  auto observed = ytec::operationcore::read_single_checkpoint(
      temporary.checkpoint_path());
  check(observed.has_value() && observed.value().has_value(),
        "The committed single checkpoint should read back");
  const auto current_hash = observed.value()->record_hash;

  auto next = current;
  next.revision = 2U;
  next.verified_work_bytes = 2048U;
  next.verified_chunk_count = 2U;
  check(
      !ytec::operationcore::replace_single_checkpoint(
          temporary.checkpoint_path(), make_digest(0xF0U), next),
      "A stale or foreign expected hash must not replace the checkpoint");
  observed = ytec::operationcore::read_single_checkpoint(
      temporary.checkpoint_path());
  check(observed.value()->checkpoint.revision == 1U,
        "Failed replacement must preserve the current record");

  check(
      ytec::operationcore::replace_single_checkpoint(
          temporary.checkpoint_path(), current_hash, next)
          .has_value(),
      "A verified monotonic update should replace atomically");
  observed = ytec::operationcore::read_single_checkpoint(
      temporary.checkpoint_path());
  check(observed.has_value() && observed.value() &&
            observed.value()->checkpoint.revision == 2U,
        "Only the latest single checkpoint should remain");
  const auto next_hash = observed.value()->record_hash;

  check(
      !ytec::operationcore::discard_single_checkpoint(
          temporary.checkpoint_path(), current_hash),
      "A stale hash must not delete a newer checkpoint");
  check(file_exists(temporary.checkpoint_path()),
        "A refused discard must leave the exact file intact");
  check(
      ytec::operationcore::discard_single_checkpoint(
          temporary.checkpoint_path(), next_hash)
          .has_value(),
      "The exact validated open checkpoint may be discarded");
  check(!file_exists(temporary.checkpoint_path()),
        "A matched discard should remove the single checkpoint");
}

void test_unknown_existing_files_are_never_overwritten_or_deleted() {
  TemporaryDirectory final_case;
  const std::vector<std::byte> unknown{
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
  write_new_bytes(final_case.checkpoint_path(), unknown);
  const auto plan = make_clone_plan();
  const auto checkpoint = make_checkpoint(plan);
  check(
      !ytec::operationcore::create_single_checkpoint(
          final_case.checkpoint_path(), checkpoint),
      "An unknown existing final file must block CREATE_NEW");
  check(read_bytes(final_case.checkpoint_path()) == unknown,
        "An unknown final file must remain byte-for-byte untouched");
  check(
      !ytec::operationcore::discard_single_checkpoint(
          final_case.checkpoint_path(), make_digest(0x11U)),
      "An unknown file must never be deleted through discard");
  check(read_bytes(final_case.checkpoint_path()) == unknown,
        "A failed unknown-file discard must preserve every byte");

  TemporaryDirectory stage_case;
  write_new_bytes(stage_case.stage_path(), unknown);
  check(
      !ytec::operationcore::create_single_checkpoint(
          stage_case.checkpoint_path(), checkpoint),
      "A pre-existing unknown stage must block creation");
  check(!file_exists(stage_case.checkpoint_path()),
        "Blocked staging must not create a final checkpoint");
  check(read_bytes(stage_case.stage_path()) == unknown,
        "A pre-existing unknown stage must never be claimed or deleted");
}

void test_checkpoint_v3_output_progress_is_bounded_and_monotonic() {
  auto preparing = make_v3_image_checkpoint();
  const auto serialized =
      ytec::operationcore::serialize_checkpoint(preparing);
  check(serialized.has_value(), "A valid v3 image checkpoint should serialize");
  const auto parsed = ytec::operationcore::parse_checkpoint(serialized.value());
  check(parsed.has_value() &&
            parsed.value().checkpoint.schema_version ==
                ytec::operationcore::kCheckpointSchemaVersionV3 &&
            parsed.value().checkpoint.output_progress_evidence ==
                preparing.output_progress_evidence,
        "V3 output progress evidence should round-trip exactly");

  auto missing = preparing;
  missing.output_progress_evidence.reset();
  check(!ytec::operationcore::validate_checkpoint(missing),
        "V3 must fail closed without output progress evidence");

  auto wrong_kind = preparing;
  wrong_kind.kind = ytec::operationcore::OperationKind::rescue_image;
  check(!ytec::operationcore::validate_checkpoint(wrong_kind),
        "V3 must not silently enable rescue image resume");

  auto prepared = preparing;
  prepared.revision = 2U;
  prepared.phase = ytec::operationcore::CheckpointPhase::prepared;
  prepared.output_progress_evidence->verified_prefix_hash =
      make_digest(0x55U);
  prepared.output_progress_evidence->journal_length = 256U;
  check(ytec::operationcore::validate_checkpoint_transition(
            preparing, prepared)
            .has_value(),
        "A durable journal header may move v3 preparing to prepared");

  auto advanced = prepared;
  advanced.revision = 3U;
  advanced.verified_work_bytes = 2048U;
  advanced.verified_chunk_count = 1U;
  advanced.output_progress_evidence->verified_prefix_hash =
      make_digest(0x56U);
  advanced.output_progress_evidence->primary_output_length = 1024U;
  advanced.output_progress_evidence->journal_length = 512U;
  check(ytec::operationcore::validate_checkpoint_transition(
            prepared, advanced)
            .has_value(),
        "A verified chunk may advance bytes, object lengths, and prefix hash");

  auto stale_evidence = advanced;
  stale_evidence.revision = 4U;
  stale_evidence.verified_work_bytes = 3072U;
  stale_evidence.verified_chunk_count = 2U;
  check(!ytec::operationcore::validate_checkpoint_transition(
            advanced, stale_evidence),
        "Progress must not advance without new durable output evidence");

  auto shorter = advanced;
  shorter.revision = 4U;
  shorter.verified_work_bytes = 3072U;
  shorter.verified_chunk_count = 2U;
  shorter.output_progress_evidence->verified_prefix_hash =
      make_digest(0x57U);
  shorter.output_progress_evidence->primary_output_length = 512U;
  check(!ytec::operationcore::validate_checkpoint_transition(
            advanced, shorter),
        "A v3 transition must never shrink a bound object length");

  auto complete = advanced;
  complete.revision = 4U;
  complete.verified_work_bytes = complete.expected_work_bytes;
  complete.verified_chunk_count = 2U;
  complete.output_progress_evidence->verified_prefix_hash =
      make_digest(0x58U);
  complete.output_progress_evidence->primary_output_length = 2048U;
  complete.output_progress_evidence->journal_length = 768U;
  check(ytec::operationcore::validate_checkpoint_transition(
            advanced, complete)
            .has_value(),
        "The final verified chunk may complete prepared progress");

  auto commit_ready = complete;
  commit_ready.revision = 5U;
  commit_ready.phase = ytec::operationcore::CheckpointPhase::commit_ready;
  commit_ready.output_progress_evidence->verified_prefix_hash =
      make_digest(0x59U);
  commit_ready.output_progress_evidence->primary_output_length += 64U;
  check(ytec::operationcore::validate_checkpoint_transition(
            complete, commit_ready)
            .has_value(),
        "Commit-ready must preserve the fully verified output evidence");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"plan_validation_and_hash_binding",
       test_plan_validation_and_hash_binding},
      {"pipeline_requires_fresh_identity_exact_ok_and_verification",
       test_pipeline_requires_fresh_identity_exact_ok_and_verification},
      {"pipeline_fails_before_execution_on_identity_change",
       test_pipeline_fails_before_execution_on_identity_change},
      {"clone_pipeline_records_actual_bounded_sparse_bytes",
       test_clone_pipeline_records_actual_bounded_sparse_bytes},
      {"non_destructive_image_create_does_not_require_ok",
       test_non_destructive_image_create_does_not_require_ok},
      {"checkpoint_round_trip_rejects_version_size_and_hash_tampering",
       test_checkpoint_round_trip_rejects_version_size_and_hash_tampering},
      {"checkpoint_resume_requires_same_plan_identity_state_and_output",
       test_checkpoint_resume_requires_same_plan_identity_state_and_output},
      {"checkpoint_preparation_evidence_is_bounded_and_target_bound",
       test_checkpoint_preparation_evidence_is_bounded_and_target_bound},
      {"checkpoint_transition_is_single_monotonic_revision",
       test_checkpoint_transition_is_single_monotonic_revision},
      {"checkpoint_preparation_phase_transitions_are_strict",
       test_checkpoint_preparation_phase_transitions_are_strict},
      {"checkpoint_legacy_upgrade_to_preparation_is_one_way",
       test_checkpoint_legacy_upgrade_to_preparation_is_one_way},
      {"checkpoint_store_create_replace_and_hash_bound_discard",
       test_checkpoint_store_create_replace_and_hash_bound_discard},
      {"unknown_existing_files_are_never_overwritten_or_deleted",
       test_unknown_existing_files_are_never_overwritten_or_deleted},
      {"checkpoint_v3_output_progress_is_bounded_and_monotonic",
       test_checkpoint_v3_output_progress_is_bounded_and_monotonic},
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

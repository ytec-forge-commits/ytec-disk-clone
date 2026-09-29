#include "ytec/winpeapp/direct_image_restore_resume.h"
#include "ytec/winpeapp/direct_image_restore_resume_backend.h"
#include "ytec/winpeapp/direct_image_restore_resume_storage.h"

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

struct TestFailure final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

ytec::operationcore::Sha256Digest digest(const unsigned int seed) {
  ytec::operationcore::Sha256Digest value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>((seed + index) & 0xffU);
  }
  return value;
}

ytec::operationcore::OperationId operation_id(const unsigned int seed) {
  ytec::operationcore::OperationId value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = static_cast<std::byte>((seed + index) & 0xffU);
  }
  return value;
}

ytec::clonecore::StableDiskIdentity target_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 7U,
      .model = L"Synthetic PE resume target",
      .size_bytes = 64ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .serial_suffix = "RSM07",
      .device_instance_id = L"SYNTHETIC\\PE-RESUME-TARGET",
      .is_system_disk = false,
  };
}

ytec::imageformat::TsumugiVerifiedImage verified_image(
    const bool encrypted = false) {
  using namespace ytec::imageformat;
  TsumugiVerifiedImage image;
  image.manifest.mode = TsumugiManifestMode::exact;
  image.manifest.partition_style = TsumugiManifestPartitionStyle::gpt;
  image.manifest.source_disk_size = 64ULL * 1024ULL * 1024ULL;
  image.manifest.logical_sector_size = 512U;
  image.manifest.physical_sector_size = 4096U;
  image.manifest.source_state_hash = digest(0x21U);
  image.container.header.payload_kind = TsumugiPayloadKind::exact_disk;
  image.container.header.required_features = encrypted
      ? static_cast<std::uint32_t>(TsumugiRequiredFeature::encrypted)
      : 0U;
  image.container.header.source_disk_size =
      image.manifest.source_disk_size;
  image.container.header.logical_sector_size = 512U;
  image.container.header.physical_sector_size = 4096U;
  image.container.header.chunk_count = 3U;
  image.container.header.header_hash = digest(0x31U);
  for (std::size_t index = 0U;
       index < image.container.header.image_id.size();
       ++index) {
    image.container.header.image_id[index] =
        static_cast<std::byte>(0x40U + index);
  }
  if (encrypted) {
    for (std::size_t index = 0U;
         index < image.container.header.base_nonce.size();
         ++index) {
      image.container.header.base_nonce[index] =
          static_cast<std::byte>(0x50U + index);
    }
  }
  image.container.global_hash = digest(0x61U);
  image.container.header_hash_verified = true;
  image.container.metadata_authenticated = encrypted;
  image.container.all_chunks_verified = true;
  image.container.global_hash_verified = true;
  for (std::uint64_t index = 0U; index < 3U; ++index) {
    TsumugiChunkRecord record{
        .logical_offset = 1024U * 1024U + index * 4096U,
        .logical_length = 4096U,
        .stored_offset = 8192U + index * 4096U,
        .stored_length = 4096U,
        .flags = TsumugiChunkFlags::none,
        .compression = ImageCompression::none,
        .nonce_counter = encrypted ? index + 1U : 0U,
        .plaintext_hash = digest(static_cast<unsigned int>(0x70U + index)),
    };
    if (encrypted) {
      for (std::size_t tag_index = 0U;
           tag_index < record.authentication_tag.size();
           ++tag_index) {
        record.authentication_tag[tag_index] = static_cast<std::byte>(
            0x80U + index + tag_index);
      }
    }
    image.container.records.push_back(record);
  }
  return image;
}

ytec::imageformat::TsumugiVerifiedImage rescue_image() {
  auto image = verified_image();
  image.manifest.mode = ytec::imageformat::TsumugiManifestMode::rescue;
  image.container.header.payload_kind =
      ytec::imageformat::TsumugiPayloadKind::rescue_disk;
  image.container.header.required_features =
      static_cast<std::uint32_t>(
          ytec::imageformat::TsumugiRequiredFeature::unreadable_range_map) |
      static_cast<std::uint32_t>(
          ytec::imageformat::TsumugiRequiredFeature::rescue_read_evidence);
  auto& unreadable = image.container.records[1U];
  unreadable.flags =
      ytec::imageformat::TsumugiChunkFlags::unreadable_zero_filled;
  unreadable.stored_length = 0U;
  unreadable.nonce_counter = 0U;
  unreadable.rescue_read_evidence =
      ytec::imageformat::TsumugiRescueReadEvidence{
          .forward_attempts = 1U,
          .reverse_attempts = 1U,
          .sector_attempts = 1U,
          .zero_fill_read_back_verified = true,
          .forward_native_error = ERROR_CRC,
          .reverse_native_error = ERROR_CRC,
          .sector_native_error = ERROR_CRC,
      };
  image.unreadable_ranges = {{
      .offset = unreadable.logical_offset,
      .length = unreadable.logical_length,
  }};
  image.partial_loss = true;
  return image;
}

ytec::winpeapp::DirectImageRestoreRequest request_for(
    const ytec::imageformat::TsumugiVerifiedImage& image) {
  return ytec::winpeapp::DirectImageRestoreRequest{
      .image = {
          .image_path = L"E:\\synthetic\\reviewed.tsumugi",
          .storage_file_system = ytec::imageformat::
              TsumugiImageStorageFileSystem::ntfs,
      },
      .expected_image_global_hash = image.container.global_hash,
      .expected_source_state_hash = image.manifest.source_state_hash,
      .expected_target = target_identity(),
      .expected_target_layout_hash = digest(0x91U),
      .confirmation = {
          .first_step_acknowledged = true,
          .typed_token = L"OK",
      },
      .administrator = true,
  };
}

class SyntheticResumeSlotPlatform final
    : public ytec::operationcore::IResumeSlotPlatform {
 public:
  std::optional<ytec::operationcore::ResumeSlotRecord> record;
  bool placement_separated{true};
  bool fail_discard{};
  int observe_calls{};
  int create_calls{};
  int replace_calls{};
  int discard_calls{};

  [[nodiscard]] ytec::clonecore::Result<
      ytec::operationcore::ResumeSlotObservation>
  observe_fixed_slot() override {
    ++observe_calls;
    return ytec::clonecore::Result<
        ytec::operationcore::ResumeSlotObservation>::success({
        .storage = {
            .checkpoint_path =
                L"D:\\YTEC\\data\\active.checkpoint",
            .paths_are_canonical_local = true,
            .parent_chain_reparse_free = true,
            .placement_separated_from_source = placement_separated,
            .checkpoint_and_partial_paths_distinct = true,
            .checkpoint_file = {
                .exists = record.has_value(),
                .is_regular_file = record.has_value(),
                .is_reparse_free = record.has_value(),
                .hard_link_count = record ? 1U : 0U,
            },
            .owned_partial_file = {},
        },
        .slot = record,
        .observed_owned_partial = std::nullopt,
    });
  }

  [[nodiscard]] ytec::clonecore::Status create_fixed_slot(
      const ytec::operationcore::ResumeSlotRecord& next) override {
    ++create_calls;
    if (record) {
      return ytec::clonecore::Status::failure(error(L"create exists"));
    }
    record = next;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status replace_fixed_slot(
      const ytec::operationcore::Sha256Digest& expected,
      const ytec::operationcore::ResumeSlotRecord& next) override {
    ++replace_calls;
    if (!record || record->checkpoint.record_hash != expected) {
      return ytec::clonecore::Status::failure(error(L"replace stale"));
    }
    record = next;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status
  discard_fixed_slot_and_owned_partial(
      const ytec::operationcore::ResumeSlotBinding& binding) override {
    ++discard_calls;
    if (fail_discard || !record ||
        record->checkpoint.record_hash != binding.checkpoint_record_hash) {
      return ytec::clonecore::Status::failure(error(L"discard failed"));
    }
    record.reset();
    return ytec::clonecore::success_status();
  }

 private:
  static ytec::clonecore::Error error(std::wstring operation) {
    return ytec::clonecore::Error{
        .code = ytec::clonecore::ErrorCode::io_failed,
        .native_code = ERROR_GEN_FAILURE,
        .operation = std::move(operation),
        .message = L"synthetic failure",
    };
  }
};

struct Fixture final {
  ytec::imageformat::TsumugiVerifiedImage image{verified_image()};
  ytec::winpeapp::DirectImageRestoreRequest request{request_for(image)};
  SyntheticResumeSlotPlatform slot;
  ytec::winpeapp::DirectImageRestoreDependencies physical;
  unsigned int placement_collision_with{};
  bool image_target_storage_collision{};
  bool target_active_storage_collision{};
  unsigned int storage_generation{};
  bool use_incomplete_state{};
  bool bad_incomplete_plan{};
  bool change_file_identity{};
  bool exact_or_rescue_whole_disk_only{true};
  bool fail_before_first{};
  bool fail_after_two{};
  bool split_first_segment{};
  bool tamper_transfer_partial_loss{};
  bool tamper_transfer_bad_ranges{};
  bool transfer_called{};
  std::optional<ytec::winpeapp::DirectImageRestoreTargetResumeState>
      target_state_override;
  int evidence_calls{};
  int storage_proof_calls{};
  std::vector<std::uint64_t> transfer_indices;
  std::optional<ytec::winpeapp::DirectImageRestoreResumeCursor> last_cursor;

  Fixture() {
    physical.persistent_exact_resume_capable = true;
  }

  ytec::winpeapp::DirectImageRestoreResumeDependencies dependencies() {
    return {
        .slot_platform = &slot,
        .physical_dependencies = &physical,
        .prove_storage_separation =
            [&](const ytec::winpeapp::DirectImageRestoreRequest&) {
              ++storage_proof_calls;
              auto checkpoint = digest(0x01U + 0x10U * storage_generation);
              if (placement_collision_with != 0U) {
                checkpoint = digest(0x01U + placement_collision_with);
              }
              const auto image_storage = digest(0x02U);
              const auto target_storage = image_target_storage_collision
                  ? image_storage
                  : digest(0x03U);
              const auto active_storage = target_active_storage_collision
                  ? target_storage
                  : digest(0x04U);
              return ytec::clonecore::Result<
                  ytec::winpeapp::DirectImageRestoreResumeStorageProof>::
                  success({
                      .checkpoint_storage_identity_hash = checkpoint,
                      .image_storage_identity_hash = image_storage,
                      .target_storage_identity_hash = target_storage,
                      .active_rescue_storage_identity_hash = active_storage,
                      .image_file_object_identity_hash = digest(0xb1U),
                      .all_identities_from_open_handles = true,
                  });
            },
        .collect_evidence =
            [&](const ytec::winpeapp::DirectImageRestoreRequest& selected,
                const std::optional<
                    ytec::operationcore::ResumeSlotRecord>& existing) {
              ++evidence_calls;
              ytec::winpeapp::DirectImageRestoreResumeEvidence evidence{
                  .image = image,
                  .image_file_object_identity_hash =
                      digest(change_file_identity ? 0xb2U : 0xb1U),
                  .target_storage_identity_hash = digest(0x03U),
                  .observed_operation = {
                      .source = std::nullopt,
                      .target = selected.expected_target,
                  },
                  .observed_initial_target_layout_hash =
                      selected.expected_target_layout_hash,
                  .target_state = target_state_override.value_or(
                      use_incomplete_state
                          ? ytec::winpeapp::
                                DirectImageRestoreTargetResumeState::
                                    incomplete_layout_bound_to_operation
                          : ytec::winpeapp::
                                DirectImageRestoreTargetResumeState::
                                    reviewed_initial_layout),
                  .complete_image_verified_on_one_immutable_handle = true,
                  .image_file_identity_from_that_handle = true,
                  .target_state_from_locked_handle = true,
                  .active_rescue_media_excluded_by_stable_identity = true,
                  .exact_or_rescue_whole_disk_layout_only =
                      exact_or_rescue_whole_disk_only,
              };
              if (existing) {
                evidence.incomplete_layout_plan_hash =
                    existing->checkpoint.checkpoint.plan_hash;
                if (bad_incomplete_plan) {
                  evidence.incomplete_layout_plan_hash[0] ^=
                      std::byte{0x01};
                }
                if (existing->checkpoint.checkpoint.preparation_evidence) {
                  for (const auto& sector : existing->checkpoint.checkpoint
                           .preparation_evidence->original_sectors) {
                    evidence.preparation_sectors.push_back({
                        .offset = sector.offset,
                        .length = static_cast<std::uint32_t>(sector.length),
                        .original_hash = sector.original_hash,
                    });
                  }
                  evidence.observed_initial_target_layout_hash =
                      existing->checkpoint.checkpoint.preparation_evidence
                          ->initial_layout_hash;
                }
              } else {
                evidence.preparation_sectors.push_back({
                    .offset = 0U,
                    .length = 512U,
                    .original_hash = digest(0xD1U),
                });
              }
              for (std::uint64_t index = 0U;
                   index < image.container.records.size();
                   ++index) {
                const auto& record = image.container.records[
                    static_cast<std::size_t>(index)];
                if (index == 0U && split_first_segment) {
                  evidence.payload_segments.push_back({
                      .record_index = index,
                      .record_plaintext_offset = 0U,
                      .target_offset = record.logical_offset,
                      .length = record.logical_length / 2U,
                  });
                  evidence.payload_segments.push_back({
                      .record_index = index,
                      .record_plaintext_offset = record.logical_length / 2U,
                      .target_offset =
                          record.logical_offset + record.logical_length / 2U,
                      .length = record.logical_length / 2U,
                  });
                } else {
                  evidence.payload_segments.push_back({
                      .record_index = index,
                      .record_plaintext_offset = 0U,
                      .target_offset = record.logical_offset,
                      .length = record.logical_length,
                  });
                }
              }
              return ytec::clonecore::Result<
                  ytec::winpeapp::DirectImageRestoreResumeEvidence>::success(
                  std::move(evidence));
            },
        .execute_transfer =
            [&](const ytec::winpeapp::DirectImageRestoreRequest&,
                const ytec::winpeapp::DirectImageRestoreResumeEvidence&,
                const ytec::winpeapp::DirectImageRestoreResumeCursor& cursor,
                const ytec::winpeapp::DirectImageRestoreResumePhaseCommit&
                    preparation_commit,
                const ytec::winpeapp::
                    DirectImageRestoreChunkReadbackCommit& commit,
                const ytec::winpeapp::DirectImageRestoreResumePhaseCommit&
                    commit_ready_commit) {
              transfer_called = true;
              last_cursor = cursor;
              std::uint64_t completed_bytes =
                  cursor.verified_logical_bytes;
              std::uint64_t completed_chunks =
                  cursor.verified_chunk_count;
              if (fail_before_first) {
                return ytec::clonecore::Result<ytec::winpeapp::
                    DirectImageRestoreResumeTransferReport>::failure(
                    ytec::clonecore::Error{
                        .code = ytec::clonecore::ErrorCode::io_failed,
                        .native_code = ERROR_READ_FAULT,
                        .operation = L"synthetic full verify interruption",
                        .message = L"before target I/O",
                    });
              }
              if (cursor.durable_phase == ytec::imageformat::
                      TsumugiPhysicalResumeCursorV1::DurablePhase::
                          preparing) {
                const auto prepared = preparation_commit();
                if (!prepared) {
                  return ytec::clonecore::Result<ytec::winpeapp::
                      DirectImageRestoreResumeTransferReport>::failure(
                      prepared.error());
                }
              }
              for (std::uint64_t index = cursor.verified_chunk_count;
                   index < cursor.payload_segments.size();
                   ++index) {
                transfer_indices.push_back(index);
                const auto& segment = cursor.payload_segments[
                    static_cast<std::size_t>(index)];
                const auto committed = commit(index, segment);
                if (!committed) {
                  return ytec::clonecore::Result<ytec::winpeapp::
                      DirectImageRestoreResumeTransferReport>::failure(
                      committed.error());
                }
                completed_bytes += segment.length;
                completed_chunks = index + 1U;
                if (fail_after_two && completed_chunks == 2U) {
                  return ytec::clonecore::Result<ytec::winpeapp::
                      DirectImageRestoreResumeTransferReport>::failure(
                      ytec::clonecore::Error{
                          .code = ytec::clonecore::ErrorCode::io_failed,
                          .native_code = ERROR_WRITE_FAULT,
                          .operation = L"synthetic interruption",
                          .message = L"after two readbacks",
                      });
                }
              }
              if (cursor.durable_phase != ytec::imageformat::
                      TsumugiPhysicalResumeCursorV1::DurablePhase::
                          commit_ready) {
                const auto ready = commit_ready_commit();
                if (!ready) {
                  return ytec::clonecore::Result<ytec::winpeapp::
                      DirectImageRestoreResumeTransferReport>::failure(
                      ready.error());
                }
              }
              return ytec::clonecore::Result<ytec::winpeapp::
                  DirectImageRestoreResumeTransferReport>::success({
                  .resumed_verified_logical_bytes =
                      cursor.verified_logical_bytes,
                  .resumed_verified_chunk_count =
                      cursor.verified_chunk_count,
                  .final_verified_logical_bytes = completed_bytes,
                  .final_verified_chunk_count = completed_chunks,
                  .full_image_reverified_on_same_handle_before_first_write =
                      true,
                  .target_and_incomplete_layout_reidentified_before_first_write =
                      true,
                  .verified_prefix_was_not_rewritten = true,
                  .every_new_chunk_flushed_and_read_back = true,
                  .final_layout_committed = true,
                  .target_left_offline = true,
                  .rescue_mode = image.manifest.mode == ytec::imageformat::
                      TsumugiManifestMode::rescue,
                  .partial_loss = tamper_transfer_partial_loss
                      ? false
                      : image.partial_loss,
                  .unreadable_ranges = tamper_transfer_bad_ranges
                      ? std::vector<ytec::clonecore::ByteRange>{}
                      : image.unreadable_ranges,
              });
            },
    };
  }
};

ytec::winpeapp::DirectImageRestoreResumeCommand start_command() {
  return {
      .action =
          ytec::winpeapp::DirectImageRestoreResumeAction::start_new,
      .new_operation_id = operation_id(0x11U),
  };
}

ytec::winpeapp::DirectImageRestoreResumeOutcome inspect(Fixture& fixture) {
  auto result = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      {},
      fixture.dependencies());
  check(result.has_value(), "slot inspection must succeed");
  return result.take_value();
}

void no_slot_and_current_physical_factory_fail_before_transfer() {
  Fixture fixture;
  const auto initial = inspect(fixture);
  check(initial.kind ==
            ytec::winpeapp::DirectImageRestoreResumeOutcomeKind::no_slot &&
            fixture.storage_proof_calls == 0 &&
            fixture.evidence_calls == 0 && !fixture.transfer_called,
        "startup inspect must expose no slot without a selected image, four-storage proof, image verification, or transfer");

  auto current = ytec::winpeapp::make_direct_image_restore_windows_dependencies(
      [](const ytec::clonecore::StableDiskIdentity&,
         const ytec::clonecore::TargetConfirmation&) {
        return ytec::clonecore::Result<bool>::success(false);
      });
  fixture.physical = std::move(current);
  auto blocked = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      start_command(),
      fixture.dependencies());
  check(!blocked && fixture.evidence_calls == 0 && !fixture.transfer_called &&
            !fixture.slot.record,
        "the existing random-layout physical factory must reject resume before evidence, target I/O, or slot creation");
}

void production_backend_verify_failure_performs_zero_target_calls() {
  Fixture fixture;
  int verify_calls{};
  int target_calls{};
  const auto error = []() {
    return ytec::clonecore::Error{
        .code = ytec::clonecore::ErrorCode::verification_failed,
        .native_code = ERROR_CRC,
        .operation = L"synthetic production full verify",
        .message = L"failed before target access",
    };
  };
  ytec::winpeapp::DirectImageRestoreDependencies physical;
  physical.is_active_rescue_media =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        ++target_calls;
        return ytec::clonecore::Result<bool>::success(false);
      };
  physical.physical.verify_image =
      [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
          const ytec::clonecore::DiskOperationCallbacks&) {
        ++verify_calls;
        return ytec::clonecore::Result<
            ytec::imageformat::TsumugiVerifiedImage>::failure(error());
      };
  physical.physical.reidentify_target =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        ++target_calls;
        return ytec::clonecore::Result<
            ytec::diskmodel::ReidentifiedPhysicalTarget>::failure(error());
      };
  physical.physical.set_target_offline =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&,
          const bool) {
        ++target_calls;
        return ytec::clonecore::Status::failure(error());
      };
  physical.physical.open_offline_target =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        ++target_calls;
        return ytec::clonecore::Result<
            ytec::diskmodel::PhysicalTargetHandle>::failure(error());
      };
  physical.physical.collect_mbr_signatures =
      [&](const ytec::clonecore::StableDiskIdentity&) {
        ++target_calls;
        return ytec::clonecore::Result<
            std::vector<std::uint32_t>>::failure(error());
      };
  physical.physical.make_connection_token = [&]() {
    ++target_calls;
    return ytec::clonecore::Result<
        ytec::imageformat::Sha256Digest>::failure(error());
  };
  auto backend = ytec::winpeapp::
      make_direct_image_restore_resume_backend_v1(
          fixture.slot,
          std::move(physical),
          [](const ytec::winpeapp::DirectImageRestoreRequest&) {
            return ytec::clonecore::Result<ytec::winpeapp::
                DirectImageRestoreResumeStorageProof>::success({
                .checkpoint_storage_identity_hash = digest(0x01U),
                .image_storage_identity_hash = digest(0x02U),
                .target_storage_identity_hash = digest(0x03U),
                .active_rescue_storage_identity_hash = digest(0x04U),
                .image_file_object_identity_hash = digest(0xb1U),
                .all_identities_from_open_handles = true,
            });
          });
  check(backend.has_value(), "injected production backend must compose");
  auto failed = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      start_command(),
      backend.value()->dependencies());
  check(
      !failed && verify_calls == 1 && target_calls == 0 &&
          !fixture.slot.record,
      "production backend full verification failure must perform zero target calls and create no slot");
}

void production_backend_exact_loss_mismatch_performs_zero_target_calls() {
  Fixture fixture;
  auto malformed = fixture.image;
  malformed.partial_loss = true;
  malformed.unreadable_ranges = {{
      .offset = malformed.container.records.front().logical_offset,
      .length = malformed.container.records.front().logical_length,
  }};
  int verify_calls{};
  int target_calls{};
  const auto forbidden = []() {
    return ytec::clonecore::Error{
        .code = ytec::clonecore::ErrorCode::io_failed,
        .native_code = ERROR_GEN_FAILURE,
        .operation = L"unexpected target access",
        .message = L"image-only gate was bypassed",
    };
  };
  ytec::winpeapp::DirectImageRestoreDependencies physical;
  physical.is_active_rescue_media =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        ++target_calls;
        return ytec::clonecore::Result<bool>::failure(forbidden());
      };
  physical.physical.verify_image =
      [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
          const ytec::clonecore::DiskOperationCallbacks&) {
        ++verify_calls;
        return ytec::clonecore::Result<
            ytec::imageformat::TsumugiVerifiedImage>::success(malformed);
      };
  physical.physical.reidentify_target =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        ++target_calls;
        return ytec::clonecore::Result<
            ytec::diskmodel::ReidentifiedPhysicalTarget>::failure(
            forbidden());
      };
  physical.physical.set_target_offline =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&,
          const bool) {
        ++target_calls;
        return ytec::clonecore::Status::failure(forbidden());
      };
  physical.physical.open_offline_target =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        ++target_calls;
        return ytec::clonecore::Result<
            ytec::diskmodel::PhysicalTargetHandle>::failure(forbidden());
      };
  physical.physical.collect_mbr_signatures =
      [&](const ytec::clonecore::StableDiskIdentity&) {
        ++target_calls;
        return ytec::clonecore::Result<
            std::vector<std::uint32_t>>::failure(forbidden());
      };
  physical.physical.make_connection_token = [&]() {
    ++target_calls;
    return ytec::clonecore::Result<
        ytec::imageformat::Sha256Digest>::failure(forbidden());
  };
  auto backend = ytec::winpeapp::
      make_direct_image_restore_resume_backend_v1(
          fixture.slot,
          std::move(physical),
          [](const ytec::winpeapp::DirectImageRestoreRequest&) {
            return ytec::clonecore::Result<ytec::winpeapp::
                DirectImageRestoreResumeStorageProof>::success({
                .checkpoint_storage_identity_hash = digest(0x01U),
                .image_storage_identity_hash = digest(0x02U),
                .target_storage_identity_hash = digest(0x03U),
                .active_rescue_storage_identity_hash = digest(0x04U),
                .image_file_object_identity_hash = digest(0xb1U),
                .all_identities_from_open_handles = true,
            });
          });
  check(backend.has_value(), "Malformed-image backend must compose");
  auto failed = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      start_command(),
      backend.value()->dependencies());
  check(
      !failed && verify_calls == 1 && target_calls == 0 &&
          !fixture.slot.record,
      "An exact image carrying loss evidence must fail immediately after verification with zero target calls");
}

void production_backend_serialless_fixed_usb_is_reselection_required() {
  Fixture fixture;
  fixture.request.expected_target.serial_suffix.clear();
  fixture.image.container.opened_file = {
      .volume_serial = 0x11223344U,
      .file_id = {std::byte{0x41}},
      .size = 64U * 1024U,
      .last_write_time = 0x12345678U,
      .identity_from_open_handle = true,
  };
  int active_queries{};
  int reidentify_calls{};
  int destructive_calls{};
  ytec::diskmodel::DiskInfo disk{
      .disk_number = fixture.request.expected_target.disk_number,
      .device_path = L"\\\\.\\PhysicalDrive7",
      .device_interface_path = L"SYNTHETIC-USB-PATH",
      .connection_location_path = L"PCIROOT(0)#USBROOT(1)#USB(2)",
      .device_instance_id =
          fixture.request.expected_target.device_instance_id,
      .model = fixture.request.expected_target.model,
      .size_bytes = fixture.request.expected_target.size_bytes,
      .sector_count = fixture.request.expected_target.size_bytes / 512U,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"USB",
      .serial_suffix = {},
      .partition_style = ytec::diskmodel::PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  ytec::winpeapp::DirectImageRestoreDependencies physical;
  physical.is_active_rescue_media =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        ++active_queries;
        return ytec::clonecore::Result<bool>::success(false);
      };
  physical.physical.verify_image =
      [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
          const ytec::clonecore::DiskOperationCallbacks&) {
        return ytec::clonecore::Result<
            ytec::imageformat::TsumugiVerifiedImage>::success(fixture.image);
      };
  physical.physical.reidentify_target =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        ++reidentify_calls;
        return ytec::clonecore::Result<
            ytec::diskmodel::ReidentifiedPhysicalTarget>::success({
            .target = disk,
            .target_identity = fixture.request.expected_target,
        });
      };
  const auto forbidden = [&]() {
    ++destructive_calls;
    return ytec::clonecore::Error{
        .code = ytec::clonecore::ErrorCode::io_failed,
        .native_code = ERROR_GEN_FAILURE,
        .operation = L"unexpected serialless USB target I/O",
        .message = L"reselection gate was bypassed",
    };
  };
  physical.physical.set_target_offline =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&,
          const bool) {
        return ytec::clonecore::Status::failure(forbidden());
      };
  physical.physical.open_offline_target =
      [&](const ytec::clonecore::StableDiskIdentity&,
          const ytec::clonecore::TargetConfirmation&) {
        return ytec::clonecore::Result<
            ytec::diskmodel::PhysicalTargetHandle>::failure(forbidden());
      };
  physical.physical.collect_mbr_signatures =
      [&](const ytec::clonecore::StableDiskIdentity&) {
        return ytec::clonecore::Result<
            std::vector<std::uint32_t>>::failure(forbidden());
      };
  physical.physical.make_connection_token = [&]() {
    return ytec::clonecore::Result<
        ytec::imageformat::Sha256Digest>::failure(forbidden());
  };
  auto backend = ytec::winpeapp::
      make_direct_image_restore_resume_backend_v1(
          fixture.slot,
          std::move(physical),
          [](const ytec::winpeapp::DirectImageRestoreRequest&) {
            return ytec::clonecore::Result<ytec::winpeapp::
                DirectImageRestoreResumeStorageProof>::success({
                .checkpoint_storage_identity_hash = digest(0x01U),
                .image_storage_identity_hash = digest(0x02U),
                .target_storage_identity_hash = digest(0x03U),
                .active_rescue_storage_identity_hash = digest(0x04U),
                .image_file_object_identity_hash = digest(0xb1U),
                .all_identities_from_open_handles = true,
            });
          });
  check(backend.has_value(), "Serialless USB backend must compose");
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      start_command(),
      backend.value()->dependencies());
  check(
      !rejected && active_queries == 1 && reidentify_calls == 1 &&
          destructive_calls == 0 && !fixture.slot.record,
      "A serialless fixed USB target must require irreversible reselection before offline/open/write or slot creation");
}

void interrupted_transfer_resumes_only_after_authenticated_prefix() {
  Fixture fixture;
  fixture.fail_after_two = true;
  auto interrupted =
      ytec::winpeapp::control_direct_image_restore_resume_v1(
          fixture.request,
          start_command(),
          fixture.dependencies());
  check(!interrupted && fixture.slot.record &&
            fixture.slot.record->checkpoint.checkpoint.verified_chunk_count ==
                2U &&
            fixture.slot.record->checkpoint.checkpoint.verified_work_bytes ==
                8192U &&
            fixture.transfer_indices == std::vector<std::uint64_t>{0U, 1U},
        "checkpoint must advance only after each completed readback");

  fixture.fail_after_two = false;
  fixture.use_incomplete_state = true;
  fixture.transfer_called = false;
  fixture.transfer_indices.clear();
  const auto prompt = inspect(fixture);
  check(prompt.kind == ytec::winpeapp::
            DirectImageRestoreResumeOutcomeKind::decision_required &&
            prompt.existing_slot,
        "existing slot must become a pure resume/discard decision");
  const ytec::winpeapp::DirectImageRestoreResumeCommand resume{
      .action = ytec::winpeapp::
          DirectImageRestoreResumeAction::resume_existing,
      .reviewed_existing_slot = prompt.existing_slot,
  };
  auto completed = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      resume,
      fixture.dependencies());
  check(completed &&
            completed.value().kind == ytec::winpeapp::
                DirectImageRestoreResumeOutcomeKind::completed &&
            fixture.last_cursor &&
            fixture.last_cursor->verified_chunk_count == 2U &&
            fixture.last_cursor->verified_logical_bytes == 8192U &&
            fixture.transfer_indices == std::vector<std::uint64_t>{2U} &&
            !fixture.slot.record,
        "resume must skip exactly the authenticated contiguous prefix and discard only after final commit");
}

void changed_file_or_incomplete_layout_plan_fails_before_transfer() {
  Fixture fixture;
  fixture.fail_after_two = true;
  check(!ytec::winpeapp::control_direct_image_restore_resume_v1(
             fixture.request,
             start_command(),
             fixture.dependencies()) &&
            fixture.slot.record,
        "fixture must retain an interrupted slot");
  const auto prompt = inspect(fixture);
  const ytec::winpeapp::DirectImageRestoreResumeCommand resume{
      .action = ytec::winpeapp::
          DirectImageRestoreResumeAction::resume_existing,
      .reviewed_existing_slot = prompt.existing_slot,
  };

  fixture.fail_after_two = false;
  fixture.use_incomplete_state = true;
  fixture.transfer_called = false;
  fixture.change_file_identity = true;
  auto changed_file =
      ytec::winpeapp::control_direct_image_restore_resume_v1(
          fixture.request,
          resume,
          fixture.dependencies());
  check(!changed_file && !fixture.transfer_called && fixture.slot.record,
        "same path/content without the same image file-object identity must not resume");

  fixture.change_file_identity = false;
  fixture.bad_incomplete_plan = true;
  auto changed_layout =
      ytec::winpeapp::control_direct_image_restore_resume_v1(
          fixture.request,
          resume,
          fixture.dependencies());
  check(!changed_layout && !fixture.transfer_called && fixture.slot.record,
        "an incomplete target layout with a different plan hash must not resume");
}

void zero_cursor_can_retry_initial_layout_but_positive_cursor_cannot() {
  Fixture zero;
  zero.fail_before_first = true;
  auto interrupted = ytec::winpeapp::control_direct_image_restore_resume_v1(
      zero.request,
      start_command(),
      zero.dependencies());
  check(!interrupted && zero.slot.record &&
            zero.slot.record->checkpoint.checkpoint.verified_chunk_count ==
                0U,
        "full verification failure must retain a zero cursor slot");
  zero.fail_before_first = false;
  zero.transfer_called = false;
  const auto prompt = inspect(zero);
  auto retried = ytec::winpeapp::control_direct_image_restore_resume_v1(
      zero.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      zero.dependencies());
  check(retried && zero.transfer_called && !zero.slot.record,
        "zero cursor may retry from the unchanged reviewed initial layout");

  Fixture positive;
  positive.fail_after_two = true;
  check(!ytec::winpeapp::control_direct_image_restore_resume_v1(
             positive.request,
             start_command(),
             positive.dependencies()) &&
            positive.slot.record,
        "fixture must retain a positive cursor");
  positive.fail_after_two = false;
  positive.transfer_called = false;
  const auto positive_prompt = inspect(positive);
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      positive.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = positive_prompt.existing_slot,
      },
      positive.dependencies());
  check(!rejected && !positive.transfer_called && positive.slot.record,
        "a positive cursor must reject the reviewed initial layout state");
}

void authenticated_segment_boundaries_cannot_change_on_resume() {
  Fixture fixture;
  fixture.fail_after_two = true;
  check(!ytec::winpeapp::control_direct_image_restore_resume_v1(
             fixture.request,
             start_command(),
             fixture.dependencies()) &&
            fixture.slot.record,
        "fixture must retain an interrupted cursor");
  fixture.fail_after_two = false;
  fixture.use_incomplete_state = true;
  fixture.split_first_segment = true;
  fixture.transfer_called = false;
  const auto prompt = inspect(fixture);
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      fixture.dependencies());
  check(!rejected && !fixture.transfer_called && fixture.slot.record,
        "payload segment mapping boundaries must be authenticated by the durable plan hash");
}

void encrypted_nonce_prefix_is_rechecked_and_mismatch_is_rejected() {
  Fixture valid;
  valid.image = verified_image(true);
  valid.request = request_for(valid.image);
  auto completed = ytec::winpeapp::control_direct_image_restore_resume_v1(
      valid.request,
      start_command(),
      valid.dependencies());
  check(completed && !valid.slot.record,
        "canonical encrypted chunk-index nonce sequence must be accepted");

  Fixture invalid;
  invalid.image = verified_image(true);
  invalid.image.container.records[1].nonce_counter = 9U;
  invalid.request = request_for(invalid.image);
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      invalid.request,
      start_command(),
      invalid.dependencies());
  check(!rejected && !invalid.transfer_called && !invalid.slot.record,
        "encrypted nonce state inconsistent with authenticated chunk index must fail before slot creation and transfer");
}

void unsupported_layouts_and_inconsistent_rescue_are_fail_closed() {
  Fixture shrink;
  shrink.image.manifest.mode =
      ytec::imageformat::TsumugiManifestMode::shrink;
  shrink.image.container.header.payload_kind =
      ytec::imageformat::TsumugiPayloadKind::shrink_disk;
  auto shrink_result =
      ytec::winpeapp::control_direct_image_restore_resume_v1(
          shrink.request,
          start_command(),
          shrink.dependencies());
  check(!shrink_result && !shrink.transfer_called && !shrink.slot.record,
        "shrink migration must never enter the persistent whole-disk resume slice");

  Fixture inconsistent_rescue;
  inconsistent_rescue.image.manifest.mode =
      ytec::imageformat::TsumugiManifestMode::rescue;
  inconsistent_rescue.image.container.header.payload_kind =
      ytec::imageformat::TsumugiPayloadKind::rescue_disk;
  inconsistent_rescue.image.partial_loss = true;
  auto rescue_result =
      ytec::winpeapp::control_direct_image_restore_resume_v1(
          inconsistent_rescue.request,
          start_command(),
          inconsistent_rescue.dependencies());
  check(
      !rescue_result && !inconsistent_rescue.transfer_called &&
          !inconsistent_rescue.slot.record,
      "rescue partial-loss classification without authenticated unreadable records and ranges must fail closed");

  Fixture partial;
  partial.exact_or_rescue_whole_disk_only = false;
  auto partial_result =
      ytec::winpeapp::control_direct_image_restore_resume_v1(
          partial.request,
          start_command(),
          partial.dependencies());
  check(!partial_result && !partial.transfer_called && !partial.slot.record,
        "an individual or partial layout must fail before slot creation");
}

void rescue_partial_loss_resumes_without_classification_upgrade() {
  Fixture fixture;
  fixture.image = rescue_image();
  fixture.request = request_for(fixture.image);
  fixture.fail_after_two = true;
  auto interrupted = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      start_command(),
      fixture.dependencies());
  check(
      !interrupted && fixture.slot.record &&
          fixture.slot.record->checkpoint.checkpoint.verified_chunk_count ==
              2U,
      "rescue interruption must retain the durable payload-segment prefix");

  const auto prompt = inspect(fixture);
  fixture.fail_after_two = false;
  fixture.use_incomplete_state = true;
  fixture.transfer_called = false;
  fixture.transfer_indices.clear();
  auto resumed = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      fixture.dependencies());
  check(
      resumed &&
          resumed.value().kind == ytec::winpeapp::
              DirectImageRestoreResumeOutcomeKind::completed &&
          resumed.value().rescue_mode && resumed.value().partial_loss &&
          resumed.value().unreadable_ranges.size() == 1U &&
          resumed.value().unreadable_ranges[0].offset ==
              fixture.image.container.records[1U].logical_offset &&
          resumed.value().unreadable_ranges[0].length ==
              fixture.image.container.records[1U].logical_length &&
          resumed.value().transfer &&
          resumed.value().transfer->rescue_mode &&
          resumed.value().transfer->partial_loss && fixture.last_cursor &&
          fixture.last_cursor->verified_chunk_count == 2U &&
          fixture.transfer_indices == std::vector<std::uint64_t>{2U} &&
          !fixture.slot.record,
      "resumed rescue must skip only the durable prefix and remain an explicit partial-loss result");
}

void rescue_transfer_cannot_upgrade_partial_loss_classification() {
  Fixture fixture;
  fixture.image = rescue_image();
  fixture.request = request_for(fixture.image);
  fixture.tamper_transfer_partial_loss = true;
  fixture.tamper_transfer_bad_ranges = true;
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      start_command(),
      fixture.dependencies());
  check(
      !rejected && fixture.slot.record &&
          fixture.slot.record->checkpoint.checkpoint.phase ==
              ytec::operationcore::CheckpointPhase::commit_ready &&
          fixture.slot.discard_calls == 0,
      "A transfer adapter cannot report rescue bytes as a normal success or erase the durable partial-loss checkpoint");
}

void true_restart_uses_durable_target_shape_and_ignores_disk_number() {
  Fixture first_process;
  first_process.fail_after_two = true;
  check(
      !ytec::winpeapp::control_direct_image_restore_resume_v1(
          first_process.request,
          start_command(),
          first_process.dependencies()) &&
          first_process.slot.record,
      "The first process must retain a positive durable prefix");
  const auto persisted = *first_process.slot.record;

  Fixture swapped_process;
  swapped_process.slot.record = persisted;
  swapped_process.use_incomplete_state = true;
  swapped_process.request.expected_target.disk_number = 19U;
  swapped_process.request.expected_target.serial_suffix = "SWAPPED";
  swapped_process.request.expected_target.device_instance_id =
      L"SYNTHETIC\\OTHER-TARGET";
  const auto swapped_prompt = inspect(swapped_process);
  auto swapped = ytec::winpeapp::control_direct_image_restore_resume_v1(
      swapped_process.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = swapped_prompt.existing_slot,
      },
      swapped_process.dependencies());
  check(
      !swapped && !swapped_process.transfer_called &&
          swapped_process.slot.record,
      "A new process must reject a stably different target before transfer");

  Fixture restarted_process;
  restarted_process.slot.record = persisted;
  restarted_process.use_incomplete_state = true;
  restarted_process.request.expected_target.disk_number = 23U;
  restarted_process.request.expected_target_layout_hash = digest(0xeeU);
  const auto prompt = inspect(restarted_process);
  auto resumed = ytec::winpeapp::control_direct_image_restore_resume_v1(
      restarted_process.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      restarted_process.dependencies());
  check(
      resumed && restarted_process.transfer_called &&
          restarted_process.last_cursor &&
          restarted_process.last_cursor->verified_chunk_count == 2U &&
          !restarted_process.slot.record,
      "Slot bytes plus a freshly reselected stable target must resume across a real process boundary despite disk-number and current-layout drift");
}

void legacy_partial_invalidation_state_cannot_upgrade_to_v2() {
  Fixture fixture;
  fixture.fail_after_two = true;
  check(
      !ytec::winpeapp::control_direct_image_restore_resume_v1(
          fixture.request,
          start_command(),
          fixture.dependencies()) &&
          fixture.slot.record,
      "Legacy migration fixture must retain progress");
  auto legacy = fixture.slot.record->checkpoint.checkpoint;
  legacy.schema_version =
      ytec::operationcore::kCheckpointSchemaVersionV1;
  legacy.phase = ytec::operationcore::CheckpointPhase::executing;
  legacy.preparation_evidence.reset();
  auto bytes = ytec::operationcore::serialize_checkpoint(legacy);
  check(bytes.has_value(), "Legacy checkpoint must serialize");
  auto parsed = ytec::operationcore::parse_checkpoint(bytes.value());
  check(parsed.has_value(), "Legacy checkpoint must parse");
  fixture.slot.record->checkpoint = parsed.take_value();
  fixture.fail_after_two = false;
  fixture.transfer_called = false;
  fixture.target_state_override = ytec::winpeapp::
      DirectImageRestoreTargetResumeState::
          preparation_original_or_zero_bound_to_operation;
  const int replace_before = fixture.slot.replace_calls;
  const auto prompt = inspect(fixture);
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      fixture.dependencies());
  check(
      !rejected && !fixture.transfer_called && fixture.slot.record &&
          fixture.slot.replace_calls == replace_before,
      "A v1 checkpoint with partially invalidated metadata must remain untouched and never acquire post-hoc v2 evidence");
}

void rescue_bad_range_substitution_is_rejected_before_transfer() {
  Fixture fixture;
  fixture.image = rescue_image();
  fixture.request = request_for(fixture.image);
  fixture.fail_after_two = true;
  check(
      !ytec::winpeapp::control_direct_image_restore_resume_v1(
          fixture.request, start_command(), fixture.dependencies()) &&
          fixture.slot.record,
      "fixture must retain an interrupted rescue slot");
  const auto prompt = inspect(fixture);

  auto& old_missing = fixture.image.container.records[1U];
  old_missing.flags = ytec::imageformat::TsumugiChunkFlags::none;
  old_missing.stored_length = old_missing.logical_length;
  old_missing.rescue_read_evidence.reset();
  auto& substituted = fixture.image.container.records[2U];
  substituted.flags =
      ytec::imageformat::TsumugiChunkFlags::unreadable_zero_filled;
  substituted.stored_length = 0U;
  substituted.nonce_counter = 0U;
  substituted.rescue_read_evidence =
      ytec::imageformat::TsumugiRescueReadEvidence{
          .forward_attempts = 1U,
          .reverse_attempts = 1U,
          .sector_attempts = 1U,
          .zero_fill_read_back_verified = true,
          .forward_native_error = ERROR_CRC,
          .reverse_native_error = ERROR_CRC,
          .sector_native_error = ERROR_CRC,
      };
  fixture.image.unreadable_ranges = {{
      .offset = substituted.logical_offset,
      .length = substituted.logical_length,
  }};
  fixture.fail_after_two = false;
  fixture.use_incomplete_state = true;
  fixture.transfer_called = false;
  fixture.transfer_indices.clear();
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      fixture.dependencies());
  check(
      !rejected && !fixture.transfer_called && fixture.slot.record,
      "a substituted authenticated bad-range map must change the bound plan and fail before transfer");
}

void tampered_checkpoint_prefix_is_rejected_before_transfer() {
  Fixture bad_prefix;
  bad_prefix.fail_after_two = true;
  check(!ytec::winpeapp::control_direct_image_restore_resume_v1(
             bad_prefix.request,
             start_command(),
             bad_prefix.dependencies()) &&
            bad_prefix.slot.record,
        "fixture must retain an interrupted exact slot");
  auto checkpoint = bad_prefix.slot.record->checkpoint.checkpoint;
  checkpoint.verified_work_bytes += 512U;
  auto bytes = ytec::operationcore::serialize_checkpoint(checkpoint);
  check(bytes.has_value(), "tampered prefix checkpoint must serialize");
  auto parsed = ytec::operationcore::parse_checkpoint(bytes.value());
  check(parsed.has_value(), "tampered prefix checkpoint must parse");
  bad_prefix.slot.record->checkpoint = parsed.take_value();
  bad_prefix.fail_after_two = false;
  bad_prefix.use_incomplete_state = true;
  bad_prefix.transfer_called = false;
  const auto prompt = inspect(bad_prefix);
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      bad_prefix.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      bad_prefix.dependencies());
  check(!rejected && !bad_prefix.transfer_called && bad_prefix.slot.record,
        "checkpoint bytes must equal the sum of the authenticated contiguous payload-segment prefix");
}

void operation_target_and_completed_checkpoint_cleanup_are_bound() {
  Fixture fixture;
  fixture.fail_after_two = true;
  check(!ytec::winpeapp::control_direct_image_restore_resume_v1(
             fixture.request,
             start_command(),
             fixture.dependencies()) &&
            fixture.slot.record,
        "fixture must retain an interrupted exact slot");
  fixture.fail_after_two = false;
  fixture.use_incomplete_state = true;
  auto prompt = inspect(fixture);
  fixture.transfer_called = false;

  auto wrong_operation = *prompt.existing_slot;
  wrong_operation.operation_id[0] ^= std::byte{0x01};
  auto operation_rejected =
      ytec::winpeapp::control_direct_image_restore_resume_v1(
          fixture.request,
          {
              .action = ytec::winpeapp::
                  DirectImageRestoreResumeAction::resume_existing,
              .reviewed_existing_slot = wrong_operation,
          },
          fixture.dependencies());
  check(!operation_rejected && !fixture.transfer_called &&
            fixture.slot.record,
        "a different operation id must fail before transfer");

  auto changed_target_request = fixture.request;
  changed_target_request.expected_target.serial_suffix = "OTHER7";
  auto target_rejected =
      ytec::winpeapp::control_direct_image_restore_resume_v1(
          changed_target_request,
          {
              .action = ytec::winpeapp::
                  DirectImageRestoreResumeAction::resume_existing,
              .reviewed_existing_slot = prompt.existing_slot,
          },
          fixture.dependencies());
  check(!target_rejected && !fixture.transfer_called && fixture.slot.record,
        "a different stable target identity must fail before transfer");

  Fixture cleanup;
  cleanup.slot.fail_discard = true;
  auto completed = ytec::winpeapp::control_direct_image_restore_resume_v1(
      cleanup.request,
      start_command(),
      cleanup.dependencies());
  check(completed && completed.value().kind == ytec::winpeapp::
            DirectImageRestoreResumeOutcomeKind::
                completed_checkpoint_retained &&
            completed.value().checkpoint_cleanup_error.has_value() &&
            cleanup.slot.record &&
            cleanup.slot.record->checkpoint.checkpoint.verified_chunk_count ==
                3U,
        "a completed restore must be reported honestly while an exact cleanup failure leaves the bound checkpoint untouched");
}

void storage_collisions_fail_before_unsafe_evidence_or_transfer() {
  for (unsigned int collision_role = 1U;
       collision_role <= 2U;
       ++collision_role) {
    Fixture collision;
    collision.placement_collision_with = collision_role;
    auto blocked = ytec::winpeapp::control_direct_image_restore_resume_v1(
        collision.request,
        start_command(),
        collision.dependencies());
    check(!blocked && collision.evidence_calls == 0 &&
              !collision.transfer_called && !collision.slot.record,
          "checkpoint storage on image or target storage must fail before evidence, transfer, or slot creation");
  }

  {
    Fixture checkpoint_on_active_media;
    checkpoint_on_active_media.placement_collision_with = 3U;
    checkpoint_on_active_media.fail_before_first = true;
    auto active_checkpoint =
        ytec::winpeapp::control_direct_image_restore_resume_v1(
            checkpoint_on_active_media.request,
            start_command(),
            checkpoint_on_active_media.dependencies());
    check(
        !active_checkpoint &&
            checkpoint_on_active_media.evidence_calls == 1 &&
            checkpoint_on_active_media.transfer_called &&
            checkpoint_on_active_media.slot.record,
        "the EXE-adjacent persistent checkpoint may share the active rescue medium while remaining disjoint from image and target storage");
  }

  {
    Fixture image_on_target;
    image_on_target.image_target_storage_collision = true;
    auto image_target_blocked =
        ytec::winpeapp::control_direct_image_restore_resume_v1(
            image_on_target.request,
            start_command(),
            image_on_target.dependencies());
    check(
        !image_target_blocked && image_on_target.evidence_calls == 0 &&
            !image_on_target.transfer_called && !image_on_target.slot.record,
        "image backing storage must be disjoint from the restore target before evidence, transfer, or slot creation");
  }

  {
    Fixture target_is_active_media;
    target_is_active_media.target_active_storage_collision = true;
    auto target_active_blocked =
        ytec::winpeapp::control_direct_image_restore_resume_v1(
            target_is_active_media.request,
            start_command(),
            target_is_active_media.dependencies());
    check(
        !target_active_blocked &&
            target_is_active_media.evidence_calls == 0 &&
            !target_is_active_media.transfer_called &&
            !target_is_active_media.slot.record,
        "restore target storage must be disjoint from the active rescue backing before evidence, transfer, or slot creation");
  }
}

void stale_decision_storage_collision_and_explicit_discard_are_safe() {
  storage_collisions_fail_before_unsafe_evidence_or_transfer();
  Fixture fixture;
  fixture.fail_after_two = true;
  check(!ytec::winpeapp::control_direct_image_restore_resume_v1(
             fixture.request,
             start_command(),
             fixture.dependencies()) &&
            fixture.slot.record,
        "fixture must retain an interrupted slot");
  auto prompt = inspect(fixture);
  check(
      prompt.expected_logical_bytes ==
          fixture.slot.record->checkpoint.checkpoint.expected_work_bytes &&
          prompt.checkpoint_phase.has_value() &&
          prompt.capability.has_value(),
      "startup inspection must expose only bounded path-free progress and capability summary fields");
  const int proof_calls_before_discard = fixture.storage_proof_calls;
  auto stale = *prompt.existing_slot;
  stale.checkpoint_record_hash[0] ^= std::byte{0x01};
  auto refused = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::discard_existing,
          .reviewed_existing_slot = stale,
      },
      fixture.dependencies());
  check(!refused && fixture.slot.record && fixture.slot.discard_calls == 0,
        "a stale displayed binding must not delete a replacement slot");
  check(
      fixture.storage_proof_calls == proof_calls_before_discard,
      "stale discard must not require a reselected image or run four-storage proof");

  auto discarded = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::discard_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      fixture.dependencies());
  check(discarded && discarded.value().kind == ytec::winpeapp::
            DirectImageRestoreResumeOutcomeKind::discarded &&
            !fixture.slot.record && fixture.evidence_calls == 1,
        "explicit discard must use only the foundation binding and never run a new transfer");
  check(
      fixture.storage_proof_calls == proof_calls_before_discard,
      "binding-checked discard must complete without image/target reselection or four-storage proof");
}

void checkpoint_storage_identity_is_bound_across_restart() {
  Fixture fixture;
  fixture.fail_after_two = true;
  check(!ytec::winpeapp::control_direct_image_restore_resume_v1(
             fixture.request,
             start_command(),
             fixture.dependencies()) &&
            fixture.slot.record,
        "fixture must retain an interrupted slot");
  const auto prompt = inspect(fixture);
  fixture.use_incomplete_state = true;
  fixture.storage_generation = 1U;
  fixture.transfer_called = false;
  auto rejected = ytec::winpeapp::control_direct_image_restore_resume_v1(
      fixture.request,
      {
          .action = ytec::winpeapp::
              DirectImageRestoreResumeAction::resume_existing,
          .reviewed_existing_slot = prompt.existing_slot,
      },
      fixture.dependencies());
  check(
      !rejected && !fixture.transfer_called && fixture.slot.record,
      "a copied or moved checkpoint slot must fail when its opened storage identity changes");
}

void production_storage_composition_binds_slot_after_four_opened_domains() {
  using ytec::winpeapp::OpenedResumeStorageDomainV1;
  using ytec::winpeapp::ResumeStoragePathRole;
  int data_observations = 0;
  int image_observations = 0;
  auto platform = ytec::winpeapp::
      make_direct_image_restore_resume_storage_platform_v1({
          .observe_path_storage =
              [&](const std::wstring&, const ResumeStoragePathRole role) {
                if (role == ResumeStoragePathRole::checkpoint_data) {
                  ++data_observations;
                  return ytec::clonecore::Result<
                      OpenedResumeStorageDomainV1>::success({
                      .storage_identity_hash = digest(0xa0U),
                      .identity_from_open_handles = true,
                  });
                }
                ++image_observations;
                return ytec::clonecore::Result<
                    OpenedResumeStorageDomainV1>::success({
                    .storage_identity_hash = digest(0xb0U),
                    .file_object_identity_hash = digest(0xb1U),
                    .identity_from_open_handles = true,
                });
              },
          .observe_locked_target_storage =
              [](const ytec::clonecore::StableDiskIdentity&) {
                return ytec::clonecore::Result<
                    OpenedResumeStorageDomainV1>::success({
                    .storage_identity_hash = digest(0xc0U),
                    .identity_from_open_handles = true,
                });
              },
          .observe_active_rescue_storage = []() {
            return ytec::clonecore::Result<
                OpenedResumeStorageDomainV1>::success({
                .storage_identity_hash = digest(0xa0U),
                .identity_from_open_handles = true,
            });
          },
      });
  check(platform.has_value(), "storage composition must be constructible");
  auto startup = platform.value().prove_data_backing(L"C:\\data", std::nullopt);
  check(startup && startup.value().identity_from_open_handle &&
            !startup.value().separated_from_source,
        "startup slot inspection may observe data but must not invent selected-source separation");

  Fixture fixture;
  auto proof = platform.value().prove_restore_storage(fixture.request);
  check(proof && proof.value().all_identities_from_open_handles &&
            proof.value().checkpoint_storage_identity_hash == digest(0xa0U) &&
            proof.value().active_rescue_storage_identity_hash == digest(0xa0U) &&
            proof.value().image_storage_identity_hash == digest(0xb0U) &&
            proof.value().image_file_object_identity_hash == digest(0xb1U) &&
            proof.value().target_storage_identity_hash == digest(0xc0U),
        "restore proof must bind data/image file/target/active opened domains without path claims");
  auto mutation = platform.value().prove_data_backing(L"C:\\data", std::nullopt);
  check(mutation && mutation.value().separated_from_source &&
            data_observations == 3 && image_observations == 1,
        "slot mutation must reopen data after the four-domain proof and require image/target separation");
}

void startup_review_is_bounded_and_path_free() {
  Fixture fixture;
  fixture.fail_after_two = true;
  check(!ytec::winpeapp::control_direct_image_restore_resume_v1(
             fixture.request, start_command(), fixture.dependencies()) &&
            fixture.slot.record,
        "fixture must retain a resumable slot");
  const auto prompt = inspect(fixture);
  auto text = ytec::winpeapp::
      format_direct_image_restore_resume_startup_review_v1(prompt);
  check(text && text.value().find(L"読戻し確認済み") != std::wstring::npos &&
            text.value().find(L"active.checkpoint") == std::wstring::npos &&
            text.value().find(L"C:\\") == std::wstring::npos &&
            text.value().size() < 1024U,
        "startup review must expose bounded counters/classification without paths or private bindings");
}

void ram_backed_checkpoint_never_authorizes_persistent_slot_mutation() {
  using ytec::winpeapp::OpenedResumeStorageDomainV1;
  using ytec::winpeapp::ResumeStoragePathRole;
  int image_observations = 0;
  int target_observations = 0;
  int active_observations = 0;
  auto platform = ytec::winpeapp::
      make_direct_image_restore_resume_storage_platform_v1({
          .observe_path_storage =
              [&](const std::wstring&, const ResumeStoragePathRole role) {
                if (role == ResumeStoragePathRole::image) {
                  ++image_observations;
                }
                return ytec::clonecore::Result<
                    OpenedResumeStorageDomainV1>::success({
                    .storage_identity_hash = role ==
                            ResumeStoragePathRole::checkpoint_data
                        ? digest(0xa0U)
                        : digest(0xb0U),
                    .file_object_identity_hash = role ==
                            ResumeStoragePathRole::image
                        ? std::optional<ytec::operationcore::Sha256Digest>(
                              digest(0xb1U))
                        : std::nullopt,
                    .identity_from_open_handles = true,
                    .persistent_checkpoint_backing = role !=
                        ResumeStoragePathRole::checkpoint_data,
                });
              },
          .observe_locked_target_storage =
              [&](const ytec::clonecore::StableDiskIdentity&) {
                ++target_observations;
                return ytec::clonecore::Result<
                    OpenedResumeStorageDomainV1>::success({
                    .storage_identity_hash = digest(0xc0U),
                    .identity_from_open_handles = true,
                });
              },
          .observe_active_rescue_storage = [&]() {
            ++active_observations;
            return ytec::clonecore::Result<
                OpenedResumeStorageDomainV1>::success({
                .storage_identity_hash = digest(0xd0U),
                .identity_from_open_handles = true,
            });
          },
      });
  Fixture fixture;
  check(platform.has_value() &&
            !platform.value().prove_restore_storage(fixture.request) &&
            image_observations == 0 && target_observations == 0 &&
            active_observations == 0,
        "RAM-backed checkpoint data must fail before selected image, target, or active-media observation");
  auto backing = platform.value().prove_data_backing(
      L"X:\\YtecDiskClone\\data", std::nullopt);
  check(backing && !backing.value().separated_from_source,
        "X: RAM data must never be represented as a persistent checkpoint backing");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"no_slot_and_current_physical_factory_fail_before_transfer",
       no_slot_and_current_physical_factory_fail_before_transfer},
      {"production_backend_verify_failure_performs_zero_target_calls",
       production_backend_verify_failure_performs_zero_target_calls},
      {"production_backend_exact_loss_mismatch_performs_zero_target_calls",
       production_backend_exact_loss_mismatch_performs_zero_target_calls},
      {"production_backend_serialless_fixed_usb_is_reselection_required",
       production_backend_serialless_fixed_usb_is_reselection_required},
      {"interrupted_transfer_resumes_only_after_authenticated_prefix",
       interrupted_transfer_resumes_only_after_authenticated_prefix},
      {"changed_file_or_incomplete_layout_plan_fails_before_transfer",
       changed_file_or_incomplete_layout_plan_fails_before_transfer},
      {"zero_cursor_can_retry_initial_layout_but_positive_cursor_cannot",
       zero_cursor_can_retry_initial_layout_but_positive_cursor_cannot},
      {"authenticated_segment_boundaries_cannot_change_on_resume",
       authenticated_segment_boundaries_cannot_change_on_resume},
      {"encrypted_nonce_prefix_is_rechecked_and_mismatch_is_rejected",
       encrypted_nonce_prefix_is_rechecked_and_mismatch_is_rejected},
      {"unsupported_layouts_and_inconsistent_rescue_are_fail_closed",
       unsupported_layouts_and_inconsistent_rescue_are_fail_closed},
      {"rescue_partial_loss_resumes_without_classification_upgrade",
       rescue_partial_loss_resumes_without_classification_upgrade},
      {"rescue_transfer_cannot_upgrade_partial_loss_classification",
       rescue_transfer_cannot_upgrade_partial_loss_classification},
      {"true_restart_uses_durable_target_shape_and_ignores_disk_number",
       true_restart_uses_durable_target_shape_and_ignores_disk_number},
      {"legacy_partial_invalidation_state_cannot_upgrade_to_v2",
       legacy_partial_invalidation_state_cannot_upgrade_to_v2},
      {"rescue_bad_range_substitution_is_rejected_before_transfer",
       rescue_bad_range_substitution_is_rejected_before_transfer},
      {"tampered_checkpoint_prefix_is_rejected_before_transfer",
       tampered_checkpoint_prefix_is_rejected_before_transfer},
      {"operation_target_and_completed_checkpoint_cleanup_are_bound",
       operation_target_and_completed_checkpoint_cleanup_are_bound},
      {"stale_decision_storage_collision_and_explicit_discard_are_safe",
       stale_decision_storage_collision_and_explicit_discard_are_safe},
      {"checkpoint_storage_identity_is_bound_across_restart",
       checkpoint_storage_identity_is_bound_across_restart},
      {"production_storage_composition_binds_slot_after_four_opened_domains",
       production_storage_composition_binds_slot_after_four_opened_domains},
      {"startup_review_is_bounded_and_path_free",
       startup_review_is_bounded_and_path_free},
      {"ram_backed_checkpoint_never_authorizes_persistent_slot_mutation",
       ram_backed_checkpoint_never_authorizes_persistent_slot_mutation},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

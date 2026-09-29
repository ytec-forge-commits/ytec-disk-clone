#include "ytec/winpeapp/direct_image_restore_resume_backend.h"

#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_physical_restore_resume.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
namespace {

clonecore::Error backend_error(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return {
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
  return clonecore::Result<T>::failure(backend_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

void append_u32(
    std::vector<std::byte>& bytes,
    const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(
    std::vector<std::byte>& bytes,
    const std::uint64_t value) {
  for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_domain(
    std::vector<std::byte>& bytes,
    const std::string_view domain) {
  append_u32(bytes, static_cast<std::uint32_t>(domain.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(domain.data()),
      reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
}

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
}

clonecore::Result<operationcore::Sha256Digest> hash_opened_image_identity(
    const imageformat::TsumugiStreamInspection::OpenedFileObservationV1&
        observation) {
  if (!observation.identity_from_open_handle ||
      observation.volume_serial == 0U || all_zero(observation.file_id) ||
      observation.size == 0U || observation.last_write_time == 0U) {
    return failure<operationcore::Sha256Digest>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"PE Resume image file object identity",
        L"完全検証に使用したopened handleのvolume、File ID、size、または更新時刻を取得できません");
  }
  std::vector<std::byte> material;
  material.reserve(96U);
  append_domain(material, "YTEC-PE-RESUME-IMAGE-FILE-OBJECT-V1");
  append_u64(material, observation.volume_serial);
  append_array(material, observation.file_id);
  append_u64(material, observation.size);
  append_u64(material, observation.last_write_time);
  return imageformat::sha256(material);
}

bool same_restore_identity(
    const imageformat::TsumugiRestoreDiskIdentity& left,
    const imageformat::TsumugiRestoreDiskIdentity& right) noexcept {
  return left.stable_identity_hash == right.stable_identity_hash &&
      left.disk_size == right.disk_size &&
      left.logical_sector_size == right.logical_sector_size &&
      left.is_running_windows_system_disk ==
          right.is_running_windows_system_disk &&
      left.is_usb_attached == right.is_usb_attached &&
      left.is_usb_memory == right.is_usb_memory &&
      left.is_active_rescue_media == right.is_active_rescue_media &&
      left.is_dynamic_disk == right.is_dynamic_disk &&
      left.is_storage_spaces == right.is_storage_spaces &&
      left.is_windows_software_raid == right.is_windows_software_raid &&
      left.has_unresolved_hardware_raid ==
          right.has_unresolved_hardware_raid &&
      left.connection_instance_hash == right.connection_instance_hash;
}

clonecore::Result<imageformat::TsumugiRestoreDiskIdentity>
make_restore_identity(
    const diskmodel::ReidentifiedPhysicalTarget& observed,
    const imageformat::TsumugiPhysicalRestoreTargetClass& target_class,
    const imageformat::Sha256Digest& connection_token) {
  auto stable = imageformat::hash_tsumugi_physical_restore_target_identity_v1(
      observed.target_identity);
  if (!stable) {
    return clonecore::Result<
        imageformat::TsumugiRestoreDiskIdentity>::failure(stable.error());
  }
  if (target_class.usb_attached && all_zero(connection_token)) {
    return failure<imageformat::TsumugiRestoreDiskIdentity>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"PE Resume USB connection",
        L"USB targetの現在接続session tokenがありません");
  }
  return clonecore::Result<imageformat::TsumugiRestoreDiskIdentity>::success({
      .stable_identity_hash = stable.take_value(),
      .disk_size = observed.target_identity.size_bytes,
      .logical_sector_size = observed.target_identity.logical_sector_size,
      .is_running_windows_system_disk =
          observed.target_identity.is_system_disk,
      .is_usb_attached = target_class.usb_attached,
      .is_usb_memory = target_class.usb_memory,
      .is_active_rescue_media = false,
      .is_dynamic_disk = target_class.dynamic_disk,
      .is_storage_spaces = target_class.storage_spaces,
      .is_windows_software_raid = target_class.software_raid,
      .has_unresolved_hardware_raid =
          target_class.unresolved_hardware_raid,
      .connection_instance_hash = connection_token,
  });
}

clonecore::Status validate_image_mode_and_loss(
    const DirectImageRestoreRequest& request,
    const imageformat::TsumugiVerifiedImage& image);

clonecore::Status validate_source_target_and_geometry(
    const DirectImageRestoreRequest& request,
    const imageformat::TsumugiVerifiedImage& image,
    const diskmodel::ReidentifiedPhysicalTarget& target,
    const bool active_rescue_media) {
  const auto image_status = validate_image_mode_and_loss(request, image);
  if (!image_status) {
    return image_status;
  }
  const auto target_class =
      imageformat::classify_tsumugi_physical_restore_target(target.target);
  const auto class_status =
      imageformat::validate_tsumugi_physical_restore_target(
          target.target, target_class, active_rescue_media);
  if (!class_status) {
    return class_status;
  }
  if (target_class.usb_attached && target.target.serial_suffix.empty()) {
    return clonecore::Status::failure(backend_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_NOT_CONNECTED,
        L"PE Resume serialless USB target",
        L"永続Resumeは一意serialのない固定USB targetをdevice instanceだけでは拘束できないため、再選択しても開始できません"));
  }
  auto model = imageformat::hash_tsumugi_source_model_v1(
      target.target.model);
  auto serial = imageformat::hash_tsumugi_source_serial_v1(
      target.target.serial_suffix, target.target.device_instance_id);
  if (!model || !serial) {
    return clonecore::Status::failure(
        !model ? model.error() : serial.error());
  }
  if (model.value() == image.manifest.source_model_hash &&
      serial.value() == image.manifest.source_serial_hash) {
    return clonecore::Status::failure(backend_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DRIVE,
        L"PE Resume source disk protection",
        L"画像を作成したsource diskと同じtargetには復元できません"));
  }
  if (target.target_identity.logical_sector_size != 512U ||
      target.target_identity.logical_sector_size !=
          image.manifest.logical_sector_size ||
      target.target_identity.size_bytes < image.manifest.source_disk_size) {
    return clonecore::Status::failure(backend_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE Resume exact/rescue whole-disk boundary",
        L"この製品接続は認証済みexact/rescue whole-diskかつ512-byte logical sectorだけを受け付けます"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_image_mode_and_loss(
    const DirectImageRestoreRequest& request,
    const imageformat::TsumugiVerifiedImage& image) {
  const bool exact =
      image.manifest.mode == imageformat::TsumugiManifestMode::exact &&
      image.container.header.payload_kind ==
          imageformat::TsumugiPayloadKind::exact_disk;
  const bool rescue =
      image.manifest.mode == imageformat::TsumugiManifestMode::rescue &&
      image.container.header.payload_kind ==
          imageformat::TsumugiPayloadKind::rescue_disk;
  if (request.individual_partition.has_value() || (!exact && !rescue) ||
      (exact &&
       (image.partial_loss || !image.unreadable_ranges.empty())) ||
      (rescue &&
       image.partial_loss != !image.unreadable_ranges.empty())) {
    return clonecore::Status::failure(backend_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE Resume image-only whole-disk boundary",
        L"認証済みexact/rescue whole-disk mode、payload kind、およびpartial-loss分類が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Error append_offline_failure(
    clonecore::Error primary,
    const clonecore::Status& offline) {
  if (!offline) {
    primary.message.append(L"。target offline再確認にも失敗しました: ")
        .append(offline.error().operation)
        .append(L" / ")
        .append(offline.error().message);
  }
  return primary;
}

imageformat::TsumugiPhysicalResumeLayoutSeedV1 mapping_seed(
    const imageformat::TsumugiVerifiedImage& image) {
  imageformat::TsumugiPhysicalResumeLayoutSeedV1 seed{};
  seed.operation_id = image.container.header.image_id;
  seed.plan_hash = image.container.global_hash;
  if (all_zero(seed.operation_id)) {
    seed.operation_id[0] = std::byte{1};
  }
  if (all_zero(seed.plan_hash)) {
    seed.plan_hash[0] = std::byte{1};
  }
  return seed;
}

clonecore::Result<std::vector<
    imageformat::TsumugiPhysicalResumePreparationSectorV1>>
physical_preparation_from_checkpoint(
    const operationcore::CheckpointPreparationEvidence& evidence) {
  if (evidence.original_sectors.size() >
      imageformat::kTsumugiPhysicalResumeMaximumPreparationSectorsV1) {
    return failure<std::vector<
        imageformat::TsumugiPhysicalResumePreparationSectorV1>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"PE Resume checkpoint preparation",
        L"checkpoint sector証跡がphysical backend上限を超えます");
  }
  std::vector<imageformat::TsumugiPhysicalResumePreparationSectorV1>
      result;
  result.reserve(evidence.original_sectors.size());
  for (const auto& sector : evidence.original_sectors) {
    if (sector.length >
        (std::numeric_limits<std::uint32_t>::max)()) {
      return failure<std::vector<
          imageformat::TsumugiPhysicalResumePreparationSectorV1>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE Resume checkpoint preparation",
          L"checkpoint sector長がphysical backend表現上限を超えます");
    }
    result.push_back({
        .offset = sector.offset,
        .length = static_cast<std::uint32_t>(sector.length),
        .original_hash = sector.original_hash,
    });
  }
  return clonecore::Result<std::vector<
      imageformat::TsumugiPhysicalResumePreparationSectorV1>>::success(
      std::move(result));
}

clonecore::Result<bool> all_captured_sectors_are_zero(
    const std::span<const
        imageformat::TsumugiPhysicalResumePreparationSectorV1> sectors,
    const std::uint32_t logical_sector_size) {
  std::vector<std::byte> zeroes(logical_sector_size, std::byte{0});
  auto zero_hash = imageformat::sha256(zeroes);
  if (!zero_hash) {
    return clonecore::Result<bool>::failure(zero_hash.error());
  }
  return clonecore::Result<bool>::success(
      !sectors.empty() &&
      std::all_of(
          sectors.begin(),
          sectors.end(),
          [&](const auto& sector) {
            return sector.length == logical_sector_size &&
                sector.original_hash == zero_hash.value();
          }));
}

}  // namespace

class DirectImageRestoreResumeBackendV1::Impl final {
 public:
  Impl(
      operationcore::IResumeSlotPlatform& slot_platform,
      DirectImageRestoreDependencies physical_dependencies,
      DirectImageRestoreResumeStorageProbe storage_probe)
      : physical_(std::move(physical_dependencies)),
        storage_probe_(std::move(storage_probe)) {
    physical_.persistent_exact_resume_capable = true;
    dependencies_ = {
        .slot_platform = &slot_platform,
        .physical_dependencies = &physical_,
        .prove_storage_separation = storage_probe_,
        .collect_evidence =
            [this](const DirectImageRestoreRequest& request,
                   const std::optional<operationcore::ResumeSlotRecord>&
                       existing) {
              return collect_evidence(request, existing);
            },
        .execute_transfer =
            [this](const DirectImageRestoreRequest& request,
                   const DirectImageRestoreResumeEvidence& evidence,
                   const DirectImageRestoreResumeCursor& cursor,
                   const DirectImageRestoreResumePhaseCommit&
                       preparation_commit,
                   const DirectImageRestoreChunkReadbackCommit& commit,
                   const DirectImageRestoreResumePhaseCommit&
                       commit_ready_commit) {
              return execute_transfer(
                  request,
                  evidence,
                  cursor,
                  preparation_commit,
                  commit,
                  commit_ready_commit);
            },
    };
  }

  [[nodiscard]] const DirectImageRestoreResumeDependencies&
  dependencies() const noexcept {
    return dependencies_;
  }

 private:
  clonecore::Result<DirectImageRestoreResumeEvidence> collect_evidence(
      const DirectImageRestoreRequest& request,
      const std::optional<operationcore::ResumeSlotRecord>& existing) {
    auto image = physical_.physical.verify_image(
        request.image, request.callbacks);
    if (!image) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          image.error());
    }
    if (image.value().container.global_hash !=
            request.expected_image_global_hash ||
        image.value().manifest.source_state_hash !=
            request.expected_source_state_hash) {
      return failure<DirectImageRestoreResumeEvidence>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"PE Resume image final review",
          L"完全検証済みimageが最終確認したglobal/source state Hashと一致しません");
    }
    const auto image_boundary = validate_image_mode_and_loss(
        request, image.value());
    if (!image_boundary) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          image_boundary.error());
    }
    auto file_identity = hash_opened_image_identity(
        image.value().container.opened_file);
    if (!file_identity) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          file_identity.error());
    }
    auto active = physical_.is_active_rescue_media(
        request.expected_target, request.confirmation);
    if (!active) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          active.error());
    }
    if (active.value()) {
      return failure<DirectImageRestoreResumeEvidence>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_ACCESS_DENIED,
          L"PE Resume active rescue media",
          L"現在起動中のrescue mediaをtargetにできません");
    }
    auto observed = physical_.physical.reidentify_target(
        request.expected_target, request.confirmation);
    if (!observed) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          observed.error());
    }
    const auto target_status = validate_source_target_and_geometry(
        request, image.value(), observed.value(), false);
    if (!target_status) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          target_status.error());
    }
    auto initial_layout =
        imageformat::hash_tsumugi_physical_restore_target_layout_v1(
            observed.value().target);
    if (!initial_layout) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          initial_layout.error());
    }
    if (!existing && initial_layout.value() !=
            request.expected_target_layout_hash) {
      return failure<DirectImageRestoreResumeEvidence>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"PE Resume initial target layout",
          L"最終確認後にtarget layoutが変化しました");
    }
    const auto offline = physical_.physical.set_target_offline(
        request.expected_target, request.confirmation, true);
    if (!offline) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          offline.error());
    }
    auto opened = physical_.physical.open_offline_target(
        request.expected_target, request.confirmation);
    if (!opened) {
      const auto protected_offline = physical_.physical.set_target_offline(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          append_offline_failure(opened.error(), protected_offline));
    }
    const auto opened_stable = clonecore::validate_stable_identity(
        observed.value().target_identity,
        opened.value().observed.target_identity,
        L"PE Resume opened target");
    const auto opened_status = validate_source_target_and_geometry(
        request, image.value(), opened.value().observed, false);
    auto opened_layout =
        imageformat::hash_tsumugi_physical_restore_target_layout_v1(
            opened.value().observed.target);
    if (!opened_stable || !opened_status || !opened_layout ||
        !opened.value().observed.target.offline.value_or(false) ||
        !opened.value().target ||
        opened.value().target->size_bytes() !=
            opened.value().observed.target_identity.size_bytes ||
        opened.value().target->logical_sector_size() != 512U) {
      const auto primary = !opened_stable
          ? opened_stable.error()
          : !opened_status
              ? opened_status.error()
              : backend_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"PE Resume opened target geometry",
                    L"opened targetのlayout、offline、size、またはsectorが一致しません");
      opened.value().target.reset();
      const auto protected_offline = physical_.physical.set_target_offline(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          append_offline_failure(primary, protected_offline));
    }
    imageformat::Sha256Digest connection{};
    const auto target_class =
        imageformat::classify_tsumugi_physical_restore_target(
            opened.value().observed.target);
    if (target_class.usb_attached) {
      auto token = physical_.physical.make_connection_token();
      if (!token) {
        return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
            token.error());
      }
      connection = token.take_value();
    }
    auto restore_identity = make_restore_identity(
        opened.value().observed, target_class, connection);
    if (!restore_identity) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          restore_identity.error());
    }
    auto target_storage_identity =
        imageformat::hash_tsumugi_physical_restore_target_identity_v1(
            opened.value().observed.target_identity);
    if (!target_storage_identity) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          target_storage_identity.error());
    }
    auto seed = mapping_seed(image.value());
    if (existing) {
      seed.operation_id = existing->checkpoint.checkpoint.operation_id;
      seed.plan_hash = existing->checkpoint.checkpoint.plan_hash;
    }
    std::vector<std::uint32_t> signatures;
    if (image.value().manifest.partition_style ==
        imageformat::TsumugiManifestPartitionStyle::mbr) {
      auto collected = physical_.physical.collect_mbr_signatures(
          request.expected_target);
      if (!collected) {
        return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
            collected.error());
      }
      signatures = collected.take_value();
    }
    auto plan = imageformat::make_tsumugi_physical_resume_layout_plan_v1(
        image.value(), restore_identity.value(), seed, signatures);
    if (!plan) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          plan.error());
    }
    auto segments =
        imageformat::make_tsumugi_physical_resume_payload_segments_v1(
            image.value().container.records, plan.value());
    if (!segments) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          segments.error());
    }

    auto target_state =
        DirectImageRestoreTargetResumeState::reviewed_initial_layout;
    imageformat::Sha256Digest incomplete_plan_hash{};
    std::vector<imageformat::TsumugiPhysicalResumePreparationSectorV1>
        preparation_sectors;
    std::optional<imageformat::TsumugiRestoreLayoutPublicationInspectionV1>
        publication;
    imageformat::Sha256Digest durable_initial_layout =
        opened_layout.value();
    if (existing) {
      const auto& checkpoint = existing->checkpoint.checkpoint;
      incomplete_plan_hash = checkpoint.plan_hash;
      if (checkpoint.preparation_evidence) {
        durable_initial_layout =
            checkpoint.preparation_evidence->initial_layout_hash;
        auto converted = physical_preparation_from_checkpoint(
            *checkpoint.preparation_evidence);
        if (!converted) {
          return clonecore::Result<
              DirectImageRestoreResumeEvidence>::failure(
              converted.error());
        }
        preparation_sectors = converted.take_value();
      } else {
        auto captured =
            imageformat::capture_tsumugi_physical_resume_preparation_v1(
                plan.value(), *opened.value().target);
        if (!captured) {
          return clonecore::Result<
              DirectImageRestoreResumeEvidence>::failure(
              captured.error());
        }
        preparation_sectors = captured.take_value();
      }

      if (checkpoint.schema_version ==
          operationcore::kCheckpointSchemaVersionV1) {
        const bool unchanged_initial =
            checkpoint.verified_chunk_count == 0U &&
            checkpoint.verified_work_bytes == 0U &&
            opened_layout.value() == request.expected_target_layout_hash;
        if (!unchanged_initial) {
          auto wholly_zero = all_captured_sectors_are_zero(
              preparation_sectors, plan.value().logical_sector_size);
          if (!wholly_zero || !wholly_zero.value()) {
            return wholly_zero
                ? failure<DirectImageRestoreResumeEvidence>(
                      clonecore::ErrorCode::verification_failed,
                      ERROR_CRC,
                      L"PE Resume legacy partial invalidation",
                      L"v1 checkpointは完全なinitial layoutまたは全zero無効化だけを再開できます")
                : clonecore::Result<
                      DirectImageRestoreResumeEvidence>::failure(
                      wholly_zero.error());
          }
          const auto withheld = imageformat::
              verify_tsumugi_physical_resume_layout_withheld_v1(
                  plan.value(), *opened.value().target);
          if (!withheld) {
            return clonecore::Result<
                DirectImageRestoreResumeEvidence>::failure(
                withheld.error());
          }
          target_state = DirectImageRestoreTargetResumeState::
              incomplete_layout_bound_to_operation;
        }
      } else if (checkpoint.phase ==
                     operationcore::CheckpointPhase::preparing) {
        auto inspected =
            imageformat::inspect_tsumugi_physical_resume_preparation_v1(
                plan.value(), preparation_sectors, *opened.value().target);
        if (!inspected) {
          return clonecore::Result<
              DirectImageRestoreResumeEvidence>::failure(
              inspected.error());
        }
        if (inspected.value().state != imageformat::
                TsumugiPhysicalResumePreparationStateV1::all_original ||
            opened_layout.value() != durable_initial_layout) {
          target_state = DirectImageRestoreTargetResumeState::
              preparation_original_or_zero_bound_to_operation;
        }
      } else if (checkpoint.phase ==
                     operationcore::CheckpointPhase::prepared) {
        auto inspected =
            imageformat::inspect_tsumugi_physical_resume_preparation_v1(
                plan.value(), preparation_sectors, *opened.value().target);
        if (!inspected || inspected.value().state != imageformat::
                TsumugiPhysicalResumePreparationStateV1::all_zero) {
          return inspected
              ? failure<DirectImageRestoreResumeEvidence>(
                    clonecore::ErrorCode::verification_failed,
                    ERROR_CRC,
                    L"PE Resume prepared layout",
                    L"prepared checkpointの全metadata sectorがzeroではありません")
              : clonecore::Result<
                    DirectImageRestoreResumeEvidence>::failure(
                    inspected.error());
        }
        target_state = DirectImageRestoreTargetResumeState::
            incomplete_layout_bound_to_operation;
      } else if (checkpoint.phase ==
                     operationcore::CheckpointPhase::commit_ready) {
        const auto nonpublication_zero = imageformat::
            verify_tsumugi_physical_resume_nonpublication_zero_v1(
                plan.value(), preparation_sectors, *opened.value().target);
        if (!nonpublication_zero) {
          return clonecore::Result<
              DirectImageRestoreResumeEvidence>::failure(
              nonpublication_zero.error());
        }
        auto inspected = imageformat::
            inspect_tsumugi_whole_disk_restore_layout_publication_v1(
                plan.value(), *opened.value().target);
        if (!inspected) {
          return clonecore::Result<
              DirectImageRestoreResumeEvidence>::failure(
              inspected.error());
        }
        publication = inspected.take_value();
        switch (publication->state) {
          case imageformat::TsumugiRestoreLayoutPublicationStateV1::all_zero:
            target_state = DirectImageRestoreTargetResumeState::
                incomplete_layout_bound_to_operation;
            break;
          case imageformat::TsumugiRestoreLayoutPublicationStateV1::
              known_write_prefix:
            target_state = DirectImageRestoreTargetResumeState::
                commit_publication_bound_to_operation;
            break;
          case imageformat::TsumugiRestoreLayoutPublicationStateV1::all_final:
            target_state = DirectImageRestoreTargetResumeState::
                completed_layout_bound_to_operation;
            break;
        }
      } else {
        return failure<DirectImageRestoreResumeEvidence>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_STATE,
            L"PE Resume checkpoint phase",
            L"whole-disk backendへ渡せないcheckpoint phaseです");
      }
    } else {
      auto captured =
          imageformat::capture_tsumugi_physical_resume_preparation_v1(
              plan.value(), *opened.value().target);
      if (!captured) {
        return clonecore::Result<
            DirectImageRestoreResumeEvidence>::failure(captured.error());
      }
      preparation_sectors = captured.take_value();
    }
    opened.value().target.reset();
    const auto protected_offline = physical_.physical.set_target_offline(
        request.expected_target, request.confirmation, true);
    if (!protected_offline) {
      return clonecore::Result<DirectImageRestoreResumeEvidence>::failure(
          protected_offline.error());
    }
    return clonecore::Result<DirectImageRestoreResumeEvidence>::success({
        .image = image.take_value(),
        .image_file_object_identity_hash = file_identity.take_value(),
        .target_storage_identity_hash =
            target_storage_identity.take_value(),
        .observed_operation = {
            .source = std::nullopt,
            .target = opened.value().observed.target_identity,
        },
        .observed_initial_target_layout_hash = durable_initial_layout,
        .target_state = target_state,
        .incomplete_layout_plan_hash = incomplete_plan_hash,
        .preparation_sectors = std::move(preparation_sectors),
        .publication = publication,
        .payload_segments = segments.take_value(),
        .complete_image_verified_on_one_immutable_handle = true,
        .image_file_identity_from_that_handle = true,
        .target_state_from_locked_handle = true,
        .active_rescue_media_excluded_by_stable_identity = true,
        .exact_or_rescue_whole_disk_layout_only = true,
    });
  }

  clonecore::Result<DirectImageRestoreResumeTransferReport> execute_transfer(
      const DirectImageRestoreRequest& request,
      const DirectImageRestoreResumeEvidence& evidence,
      const DirectImageRestoreResumeCursor& cursor,
      const DirectImageRestoreResumePhaseCommit& preparation_commit,
      const DirectImageRestoreChunkReadbackCommit& commit,
      const DirectImageRestoreResumePhaseCommit& commit_ready_commit) {
    std::vector<clonecore::ByteRange> unreadable_ranges;
    try {
      // Reserve every potentially-throwing completion allocation before any
      // target transition. After layout commit the report path is move-only.
      unreadable_ranges = evidence.image.unreadable_ranges;
    } catch (...) {
      return failure<DirectImageRestoreResumeTransferReport>(
          clonecore::ErrorCode::internal_error,
          ERROR_OUTOFMEMORY,
          L"PE Resume completion evidence allocation",
          L"target I/O前にpartial-loss結果領域を確保できませんでした");
    }
    const bool rescue_mode = evidence.image.manifest.mode ==
        imageformat::TsumugiManifestMode::rescue;
    const bool partial_loss = evidence.image.partial_loss;
    auto active = physical_.is_active_rescue_media(
        request.expected_target, request.confirmation);
    if (!active) {
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(active.error());
    }
    if (active.value()) {
      return failure<DirectImageRestoreResumeTransferReport>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_ACCESS_DENIED,
          L"PE Resume active rescue media再確認",
          L"転送直前にtargetが起動中rescue mediaと一致しました");
    }
    auto observed = physical_.physical.reidentify_target(
        request.expected_target, request.confirmation);
    if (!observed) {
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(observed.error());
    }
    const auto target_status = validate_source_target_and_geometry(
        request, evidence.image, observed.value(), false);
    if (!target_status) {
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(
          target_status.error());
    }
    const bool reviewed_initial = evidence.target_state ==
        DirectImageRestoreTargetResumeState::reviewed_initial_layout;
    if (reviewed_initial) {
      auto layout =
          imageformat::hash_tsumugi_physical_restore_target_layout_v1(
              observed.value().target);
      if (!layout || layout.value() !=
              evidence.observed_initial_target_layout_hash) {
        return failure<DirectImageRestoreResumeTransferReport>(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DEVICE_REINITIALIZATION_NEEDED,
            L"PE Resume transfer initial layout",
            L"layout invalidation前にtarget layoutが変化しました");
      }
    }
    imageformat::Sha256Digest connection{};
    const auto target_class =
        imageformat::classify_tsumugi_physical_restore_target(
            observed.value().target);
    if (target_class.usb_attached) {
      auto token = physical_.physical.make_connection_token();
      if (!token) {
        return clonecore::Result<
            DirectImageRestoreResumeTransferReport>::failure(token.error());
      }
      connection = token.take_value();
    }
    std::vector<std::uint32_t> signatures;
    if (evidence.image.manifest.partition_style ==
        imageformat::TsumugiManifestPartitionStyle::mbr) {
      auto collected = physical_.physical.collect_mbr_signatures(
          request.expected_target);
      if (!collected) {
        return clonecore::Result<
            DirectImageRestoreResumeTransferReport>::failure(
            collected.error());
      }
      signatures = collected.take_value();
    }
    auto offline = physical_.physical.set_target_offline(
        request.expected_target, request.confirmation, true);
    if (!offline) {
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(offline.error());
    }
    auto opened = physical_.physical.open_offline_target(
        request.expected_target, request.confirmation);
    if (!opened) {
      const auto protected_offline = physical_.physical.set_target_offline(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(
          append_offline_failure(opened.error(), protected_offline));
    }
    const auto stable = clonecore::validate_stable_identity(
        observed.value().target_identity,
        opened.value().observed.target_identity,
        L"PE Resume transfer opened target");
    const auto opened_status = validate_source_target_and_geometry(
        request, evidence.image, opened.value().observed, false);
    if (!stable || !opened_status || !opened.value().target ||
        !opened.value().observed.target.offline.value_or(false) ||
        opened.value().target->size_bytes() !=
            opened.value().observed.target_identity.size_bytes ||
        opened.value().target->logical_sector_size() != 512U) {
      const auto primary = !stable
          ? stable.error()
          : !opened_status
              ? opened_status.error()
              : backend_error(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"PE Resume transfer opened target geometry",
                    L"opened targetのidentity、offline、size、またはsectorが一致しません");
      opened.value().target.reset();
      const auto protected_offline = physical_.physical.set_target_offline(
          request.expected_target, request.confirmation, true);
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(
          append_offline_failure(primary, protected_offline));
    }
    const auto opened_class =
        imageformat::classify_tsumugi_physical_restore_target(
            opened.value().observed.target);
    auto restore_identity = make_restore_identity(
        opened.value().observed, opened_class, connection);
    if (!restore_identity) {
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(
          restore_identity.error());
    }
    const auto locked_identity = opened.value().observed.target_identity;
    bool first_revalidation = true;
    const imageformat::TsumugiPhysicalRestoreLockedTargetRevalidator
        revalidate = [&]()
            -> clonecore::Result<imageformat::TsumugiRestoreDiskIdentity> {
      auto current = physical_.physical.reidentify_target(
          request.expected_target, request.confirmation);
      if (!current) {
        return clonecore::Result<
            imageformat::TsumugiRestoreDiskIdentity>::failure(
            current.error());
      }
      const auto same_stable = clonecore::validate_stable_identity(
          locked_identity,
          current.value().target_identity,
          L"PE Resume locked target revalidation");
      const auto current_status = validate_source_target_and_geometry(
          request, evidence.image, current.value(), false);
      const auto current_class =
          imageformat::classify_tsumugi_physical_restore_target(
              current.value().target);
      if (!same_stable || !current_status ||
          current_class != opened_class ||
          !current.value().target.offline.value_or(false)) {
        return clonecore::Result<
            imageformat::TsumugiRestoreDiskIdentity>::failure(
            !same_stable ? same_stable.error() : !current_status
                ? current_status.error()
                : backend_error(
                      clonecore::ErrorCode::identity_mismatch,
                      ERROR_DEVICE_REINITIALIZATION_NEEDED,
                      L"PE Resume target connection revalidation",
                      L"target分類、offline、またはconnectionが変化しました"));
      }
      if (first_revalidation && reviewed_initial) {
        auto current_layout =
            imageformat::hash_tsumugi_physical_restore_target_layout_v1(
                current.value().target);
        if (!current_layout || current_layout.value() !=
                evidence.observed_initial_target_layout_hash) {
          return clonecore::Result<
              imageformat::TsumugiRestoreDiskIdentity>::failure(
              current_layout
                  ? backend_error(
                        clonecore::ErrorCode::identity_mismatch,
                        ERROR_DEVICE_REINITIALIZATION_NEEDED,
                        L"PE Resume first-write layout revalidation",
                        L"完全image検証中にinitial target layoutが変化しました")
                  : current_layout.error());
        }
      }
      first_revalidation = false;
      auto identity = make_restore_identity(
          current.value(), current_class, connection);
      if (!identity ||
          !same_restore_identity(restore_identity.value(), identity.value())) {
        return clonecore::Result<
            imageformat::TsumugiRestoreDiskIdentity>::failure(
            identity ? backend_error(
                           clonecore::ErrorCode::identity_mismatch,
                           ERROR_DEVICE_REINITIALIZATION_NEEDED,
                           L"PE Resume target session identity",
                           L"opened targetと再識別targetのconnection-bound identityが一致しません")
                     : identity.error());
      }
      return identity;
    };

    imageformat::TsumugiPhysicalResumeCursorV1 physical_cursor{
        .layout_seed = {
            .operation_id = cursor.operation_id,
            .plan_hash = cursor.plan_hash,
        },
        .durable_phase = cursor.durable_phase,
        .preparation_sectors = cursor.preparation_sectors,
        .verified_payload_bytes = cursor.verified_logical_bytes,
        .verified_segment_count = cursor.verified_chunk_count,
        .expected_payload_bytes = cursor.expected_logical_bytes,
        .expected_segment_count = cursor.expected_chunk_count,
        .segments = cursor.payload_segments,
    };
    auto executed = [&]()
        -> clonecore::Result<
            imageformat::TsumugiPhysicalResumeEngineReportV1> {
      try {
        return imageformat::
            execute_tsumugi_physical_whole_disk_resume_engine_v1(
                request.image,
                evidence.image,
                restore_identity.value(),
                std::move(opened.value().target),
                signatures,
                physical_cursor,
                revalidate,
                preparation_commit,
                commit,
                commit_ready_commit,
                request.callbacks);
      } catch (...) {
        return failure<imageformat::TsumugiPhysicalResumeEngineReportV1>(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"PE Resume transfer exception",
            L"転送中の例外をfail-closedで停止し、checkpointを保持してtargetをoffline再確認します");
      }
    }();
    const auto protected_offline = physical_.physical.set_target_offline(
        request.expected_target, request.confirmation, true);
    if (!executed) {
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(
          append_offline_failure(executed.error(), protected_offline));
    }
    if (!protected_offline) {
      return clonecore::Result<
          DirectImageRestoreResumeTransferReport>::failure(
          protected_offline.error());
    }
    return clonecore::Result<DirectImageRestoreResumeTransferReport>::success({
        .resumed_verified_logical_bytes =
            executed.value().resumed_verified_payload_bytes,
        .resumed_verified_chunk_count =
            executed.value().resumed_verified_segment_count,
        .final_verified_logical_bytes =
            executed.value().final_verified_payload_bytes,
        .final_verified_chunk_count =
            executed.value().final_verified_segment_count,
        .full_image_reverified_on_same_handle_before_first_write =
            executed.value().
                full_image_reverified_on_same_handle_before_first_write,
        .target_and_incomplete_layout_reidentified_before_first_write =
            executed.value().
                target_and_incomplete_layout_reidentified_before_first_write,
        .verified_prefix_was_not_rewritten =
            executed.value().verified_prefix_was_not_rewritten,
        .every_new_chunk_flushed_and_read_back =
            executed.value().every_new_segment_flushed_and_read_back,
        .final_layout_committed = executed.value().final_layout_committed,
        .target_left_offline = true,
        .rescue_mode = rescue_mode,
        .partial_loss = partial_loss,
        .unreadable_ranges = std::move(unreadable_ranges),
    });
  }

  DirectImageRestoreDependencies physical_;
  DirectImageRestoreResumeStorageProbe storage_probe_;
  DirectImageRestoreResumeDependencies dependencies_;
};

DirectImageRestoreResumeBackendV1::DirectImageRestoreResumeBackendV1(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

DirectImageRestoreResumeBackendV1::~DirectImageRestoreResumeBackendV1() =
    default;

const DirectImageRestoreResumeDependencies&
DirectImageRestoreResumeBackendV1::dependencies() const noexcept {
  return impl_->dependencies();
}

clonecore::Result<std::unique_ptr<DirectImageRestoreResumeBackendV1>>
make_direct_image_restore_resume_backend_v1(
    operationcore::IResumeSlotPlatform& slot_platform,
    DirectImageRestoreDependencies physical_dependencies,
    DirectImageRestoreResumeStorageProbe prove_storage_separation) {
  const auto& physical = physical_dependencies.physical;
  if (!prove_storage_separation ||
      !physical_dependencies.is_active_rescue_media ||
      !physical.verify_image || !physical.reidentify_target ||
      !physical.set_target_offline || !physical.open_offline_target ||
      !physical.collect_mbr_signatures || !physical.make_connection_token) {
    return failure<std::unique_ptr<DirectImageRestoreResumeBackendV1>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_FUNCTION,
        L"PE Resume backend factory",
        L"slot、opened-storage proof、active-media resolver、または必須physical dependencyが不足しています");
  }
  auto impl = std::make_unique<DirectImageRestoreResumeBackendV1::Impl>(
      slot_platform,
      std::move(physical_dependencies),
      std::move(prove_storage_separation));
  return clonecore::Result<
      std::unique_ptr<DirectImageRestoreResumeBackendV1>>::success(
      std::unique_ptr<DirectImageRestoreResumeBackendV1>(
          new DirectImageRestoreResumeBackendV1(std::move(impl))));
}

clonecore::Result<std::unique_ptr<DirectImageRestoreResumeBackendV1>>
make_direct_image_restore_windows_resume_backend_v1(
    operationcore::IResumeSlotPlatform& slot_platform,
    ActiveRescueMediaTargetQuery active_rescue_media_query,
    DirectImageRestoreResumeStorageProbe prove_storage_separation) {
  return make_direct_image_restore_resume_backend_v1(
      slot_platform,
      make_direct_image_restore_windows_dependencies(
          std::move(active_rescue_media_query)),
      std::move(prove_storage_separation));
}

}  // namespace ytec::winpeapp

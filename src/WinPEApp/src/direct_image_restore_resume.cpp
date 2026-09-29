#include "ytec/winpeapp/direct_image_restore_resume.h"

#include "ytec/imageformat/sha256.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::winpeapp {
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

template <typename T>
clonecore::Result<T> failure(
    const clonecore::ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(resume_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

clonecore::Status status_failure(
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

template <std::size_t Size>
bool all_zero(const std::array<std::byte, Size>& value) noexcept {
  return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > (std::numeric_limits<std::uint64_t>::max)() - left) {
    return false;
  }
  result = left + right;
  return true;
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

template <std::size_t Size>
void append_array(
    std::vector<std::byte>& bytes,
    const std::array<std::byte, Size>& value) {
  bytes.insert(bytes.end(), value.begin(), value.end());
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

void append_wstring(
    std::vector<std::byte>& bytes,
    const std::wstring_view value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  for (const wchar_t character : value) {
    const auto unit = static_cast<std::uint16_t>(character);
    bytes.push_back(static_cast<std::byte>(unit & 0xffU));
    bytes.push_back(static_cast<std::byte>((unit >> 8U) & 0xffU));
  }
}

void append_string(
    std::vector<std::byte>& bytes,
    const std::string_view value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(
      bytes.end(),
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()));
}

clonecore::Result<operationcore::Sha256Digest> hash_bytes(
    const std::vector<std::byte>& bytes) {
  return imageformat::sha256(bytes);
}

bool same_stable_identity(
    const clonecore::StableDiskIdentity& left,
    const clonecore::StableDiskIdentity& right) noexcept {
  return left.model == right.model &&
      left.size_bytes == right.size_bytes &&
      left.logical_sector_size == right.logical_sector_size &&
      left.serial_suffix == right.serial_suffix &&
      left.device_instance_id == right.device_instance_id &&
      left.is_system_disk == right.is_system_disk;
}

bool same_operation_id(
    const operationcore::OperationId& left,
    const operationcore::OperationId& right) noexcept {
  return left == right;
}

bool same_identities(
    const operationcore::ResumeIdentityBinding& left,
    const operationcore::ResumeIdentityBinding& right) noexcept {
  return left.source_identity_hash == right.source_identity_hash &&
      left.target_identity_hash == right.target_identity_hash &&
      left.output_identity_hash == right.output_identity_hash;
}

bool same_binding(
    const operationcore::ResumeSlotBinding& left,
    const operationcore::ResumeSlotBinding& right) noexcept {
  return left.capability == right.capability &&
      same_operation_id(left.operation_id, right.operation_id) &&
      same_identities(left.identities, right.identities) &&
      left.checkpoint_record_hash == right.checkpoint_record_hash &&
      left.partial_file_object_identity_hash ==
          right.partial_file_object_identity_hash;
}

bool same_payload_segment(
    const imageformat::TsumugiPhysicalResumePayloadSegmentV1& left,
    const imageformat::TsumugiPhysicalResumePayloadSegmentV1& right) noexcept {
  return left == right;
}

bool chunk_is_zero_filled(
    const imageformat::TsumugiChunkFlags flags) noexcept {
  return (static_cast<std::uint32_t>(flags) &
          static_cast<std::uint32_t>(
              imageformat::TsumugiChunkFlags::zero_filled)) != 0U;
}

bool chunk_is_unreadable_zero_filled(
    const imageformat::TsumugiChunkFlags flags) noexcept {
  return flags ==
      imageformat::TsumugiChunkFlags::unreadable_zero_filled;
}

bool same_ranges(
    const std::span<const clonecore::ByteRange> left,
    const std::span<const clonecore::ByteRange> right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.size(); ++index) {
    if (left[index].offset != right[index].offset ||
        left[index].length != right[index].length) {
      return false;
    }
  }
  return true;
}

clonecore::Result<std::vector<
    imageformat::TsumugiPhysicalResumePreparationSectorV1>>
to_physical_preparation_sectors(
    const operationcore::CheckpointPreparationEvidence& evidence) {
  if (evidence.original_sectors.size() >
      imageformat::kTsumugiPhysicalResumeMaximumPreparationSectorsV1) {
    return failure<std::vector<
        imageformat::TsumugiPhysicalResumePreparationSectorV1>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_BUFFER_OVERFLOW,
        L"PE Resume preparation evidence conversion",
        L"checkpoint sector件数がlow-level上限を超えます");
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
          L"PE Resume preparation evidence conversion",
          L"checkpoint sector lengthがlow-level表現上限を超えます");
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

operationcore::CheckpointPreparationEvidence to_checkpoint_preparation(
    const operationcore::Sha256Digest& initial_layout_hash,
    const std::uint32_t logical_sector_size,
    const std::span<const
        imageformat::TsumugiPhysicalResumePreparationSectorV1> sectors) {
  operationcore::CheckpointPreparationEvidence result{
      .initial_layout_hash = initial_layout_hash,
      .logical_sector_size = logical_sector_size,
  };
  result.original_sectors.reserve(sectors.size());
  for (const auto& sector : sectors) {
    result.original_sectors.push_back({
        .offset = sector.offset,
        .length = sector.length,
        .original_hash = sector.original_hash,
    });
  }
  return result;
}

clonecore::Result<imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase>
to_physical_phase(const operationcore::InterruptionCheckpoint& checkpoint) {
  if (checkpoint.schema_version ==
          operationcore::kCheckpointSchemaVersionV1 &&
      checkpoint.phase == operationcore::CheckpointPhase::executing) {
    return clonecore::Result<
        imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase>::success(
        checkpoint.verified_chunk_count == 0U
            ? imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase::
                  preparing
            : imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase::
                  prepared);
  }
  switch (checkpoint.phase) {
    case operationcore::CheckpointPhase::preparing:
      return clonecore::Result<
          imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase>::success(
          imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase::
              preparing);
    case operationcore::CheckpointPhase::prepared:
      return clonecore::Result<
          imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase>::success(
          imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase::
              prepared);
    case operationcore::CheckpointPhase::commit_ready:
      return clonecore::Result<
          imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase>::success(
          imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase::
              commit_ready);
    case operationcore::CheckpointPhase::executing:
    case operationcore::CheckpointPhase::verifying:
      break;
  }
  return failure<
      imageformat::TsumugiPhysicalResumeCursorV1::DurablePhase>(
      clonecore::ErrorCode::invalid_data,
      ERROR_INVALID_STATE,
      L"PE Resume durable phase",
      L"checkpoint phaseをphysical resume stateへ変換できません");
}

clonecore::Result<std::vector<clonecore::ByteRange>>
authenticated_unreadable_ranges(
    const imageformat::TsumugiVerifiedImage& image) {
  const bool rescue =
      image.manifest.mode == imageformat::TsumugiManifestMode::rescue;
  std::vector<clonecore::ByteRange> ranges;
  for (const auto& record : image.container.records) {
    if (!chunk_is_unreadable_zero_filled(record.flags)) {
      continue;
    }
    std::uint64_t end{};
    if (!rescue || record.logical_length == 0U ||
        image.manifest.logical_sector_size == 0U ||
        record.logical_offset % image.manifest.logical_sector_size != 0U ||
        record.logical_length % image.manifest.logical_sector_size != 0U ||
        !checked_add(record.logical_offset, record.logical_length, end) ||
        end > image.manifest.source_disk_size) {
      return failure<std::vector<clonecore::ByteRange>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"PE whole-disk Resume authenticated bad range",
          L"欠損zero-fill recordがrescue mode、整列、範囲、または容量条件に一致しません");
    }
    if (!ranges.empty()) {
      std::uint64_t previous_end{};
      if (!checked_add(
              ranges.back().offset, ranges.back().length, previous_end) ||
          record.logical_offset < previous_end) {
        return failure<std::vector<clonecore::ByteRange>>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"PE whole-disk Resume bad range order",
            L"欠損zero-fill範囲が重複、逆順、またはoverflowしています");
      }
      if (record.logical_offset == previous_end) {
        if (!checked_add(
                ranges.back().length,
                record.logical_length,
                ranges.back().length)) {
          return failure<std::vector<clonecore::ByteRange>>(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"PE whole-disk Resume bad range merge",
              L"連続欠損範囲の合計がoverflowします");
        }
        continue;
      }
    }
    ranges.push_back({
        .offset = record.logical_offset,
        .length = record.logical_length,
    });
  }
  if (image.partial_loss != !ranges.empty() ||
      !same_ranges(ranges, image.unreadable_ranges)) {
    return failure<std::vector<clonecore::ByteRange>>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE whole-disk Resume bad range evidence",
        L"完全検証済みrecordから再構成した欠損範囲とpartial-loss証跡が一致しません");
  }
  return clonecore::Result<std::vector<clonecore::ByteRange>>::success(
      std::move(ranges));
}

clonecore::Status validate_storage_proof(
    const DirectImageRestoreResumeStorageProof& proof) {
  if (!proof.all_identities_from_open_handles ||
      all_zero(proof.checkpoint_storage_identity_hash) ||
      all_zero(proof.image_storage_identity_hash) ||
      all_zero(proof.target_storage_identity_hash) ||
      all_zero(proof.active_rescue_storage_identity_hash) ||
      all_zero(proof.image_file_object_identity_hash) ||
      proof.checkpoint_storage_identity_hash ==
          proof.image_storage_identity_hash ||
      proof.checkpoint_storage_identity_hash ==
          proof.target_storage_identity_hash ||
      proof.image_storage_identity_hash ==
          proof.target_storage_identity_hash ||
      proof.target_storage_identity_hash ==
          proof.active_rescue_storage_identity_hash) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE whole-disk復元Resume配置証明",
        L"checkpointとimage/targetの分離、imageとtargetの分離、またはtargetの起動中rescue storage非一致をopened handleから証明できません");
  }
  return clonecore::success_status();
}

clonecore::Result<operationcore::Sha256Digest> hash_source_identity(
    const DirectImageRestoreResumeEvidence& evidence) {
  std::vector<std::byte> bytes;
  bytes.reserve(256U);
  append_domain(bytes, "YTEC-PE-TSUMUGI-WHOLE-DISK-RESUME-SOURCE-V1");
  append_array(bytes, evidence.image_file_object_identity_hash);
  append_array(bytes, evidence.image.container.global_hash);
  append_array(bytes, evidence.image.manifest.source_state_hash);
  append_array(bytes, evidence.image.container.header.header_hash);
  append_array(bytes, evidence.image.container.header.image_id);
  append_array(bytes, evidence.image.container.header.base_nonce);
  append_u32(bytes, evidence.image.container.header.required_features);
  append_u64(bytes, evidence.image.container.header.chunk_count);
  append_u64(bytes, evidence.image.container.header.source_disk_size);
  append_u32(bytes, evidence.image.container.header.logical_sector_size);
  return hash_bytes(bytes);
}

clonecore::Result<operationcore::Sha256Digest> hash_target_identity_v1(
    const clonecore::StableDiskIdentity& target,
    const operationcore::Sha256Digest& initial_layout_hash) {
  const auto valid = clonecore::validate_stable_identity(
      target,
      target,
      L"PE whole-disk復元Resume対象");
  if (!valid) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        valid.error());
  }
  std::vector<std::byte> bytes;
  bytes.reserve(256U);
  append_domain(bytes, "YTEC-PE-TSUMUGI-WHOLE-DISK-RESUME-TARGET-V1");
  append_wstring(bytes, target.model);
  append_u64(bytes, target.size_bytes);
  append_u32(bytes, target.logical_sector_size);
  append_string(bytes, target.serial_suffix);
  append_wstring(bytes, target.device_instance_id);
  bytes.push_back(target.is_system_disk ? std::byte{1} : std::byte{0});
  append_array(bytes, initial_layout_hash);
  return hash_bytes(bytes);
}

clonecore::StableDiskIdentity persistent_target_shape(
    clonecore::StableDiskIdentity identity) {
  // Disk number is a transient routing hint and is deliberately excluded from
  // the durable plan. Stable re-identification still checks every model,
  // size, sector, serial and device-instance field.
  identity.disk_number = 0U;
  return identity;
}

clonecore::Result<operationcore::Sha256Digest> hash_target_identity_v2(
    const clonecore::StableDiskIdentity& target) {
  const auto valid = clonecore::validate_stable_identity(
      target, target, L"PE whole-disk復元Resume対象");
  if (!valid) {
    return clonecore::Result<operationcore::Sha256Digest>::failure(
        valid.error());
  }
  std::vector<std::byte> bytes;
  bytes.reserve(256U);
  append_domain(bytes, "YTEC-PE-TSUMUGI-WHOLE-DISK-RESUME-TARGET-V2");
  append_wstring(bytes, target.model);
  append_u64(bytes, target.size_bytes);
  append_u32(bytes, target.logical_sector_size);
  append_string(bytes, target.serial_suffix);
  append_wstring(bytes, target.device_instance_id);
  bytes.push_back(target.is_system_disk ? std::byte{1} : std::byte{0});
  return hash_bytes(bytes);
}

clonecore::Result<operationcore::Sha256Digest> hash_output_identity(
    const operationcore::Sha256Digest& source,
    const operationcore::Sha256Digest& target) {
  std::vector<std::byte> bytes;
  bytes.reserve(96U);
  append_domain(bytes, "YTEC-PE-TSUMUGI-WHOLE-DISK-RESUME-OUTPUT-V1");
  append_array(bytes, source);
  append_array(bytes, target);
  return hash_bytes(bytes);
}

clonecore::Result<operationcore::Sha256Digest> hash_immutable_payload(
    const operationcore::ResumeIdentityBinding& identities,
    const DirectImageRestoreResumeStorageProof& storage,
    const operationcore::Sha256Digest* initial_layout_hash,
    const imageformat::TsumugiManifestMode mode,
    const std::span<const clonecore::ByteRange> unreadable_ranges,
    const std::uint64_t logical_bytes,
    const std::span<const imageformat::TsumugiChunkRecord> records,
    const std::span<const imageformat::
        TsumugiPhysicalResumePayloadSegmentV1> segments) {
  std::vector<std::byte> bytes;
  bytes.reserve(128U + segments.size() * 144U);
  append_domain(
      bytes,
      initial_layout_hash == nullptr
          ? "YTEC-PE-TSUMUGI-WHOLE-DISK-RESUME-PLAN-V1"
          : "YTEC-PE-TSUMUGI-WHOLE-DISK-RESUME-PLAN-V2");
  append_array(bytes, identities.source_identity_hash);
  append_array(bytes, identities.target_identity_hash);
  append_array(bytes, identities.output_identity_hash);
  append_array(bytes, storage.checkpoint_storage_identity_hash);
  append_array(bytes, storage.image_storage_identity_hash);
  append_array(bytes, storage.target_storage_identity_hash);
  append_array(bytes, storage.active_rescue_storage_identity_hash);
  if (initial_layout_hash != nullptr) {
    append_array(bytes, *initial_layout_hash);
  }
  append_u32(bytes, static_cast<std::uint32_t>(mode));
  append_u64(bytes, static_cast<std::uint64_t>(unreadable_ranges.size()));
  for (const auto& range : unreadable_ranges) {
    append_u64(bytes, range.offset);
    append_u64(bytes, range.length);
  }
  append_u64(bytes, logical_bytes);
  append_u64(bytes, static_cast<std::uint64_t>(segments.size()));
  for (const auto& segment : segments) {
    const auto& record = records[static_cast<std::size_t>(
        segment.record_index)];
    append_u64(bytes, segment.record_index);
    append_u64(bytes, segment.record_plaintext_offset);
    append_u64(bytes, segment.target_offset);
    append_u64(bytes, segment.length);
    append_u64(bytes, record.logical_offset);
    append_u64(bytes, record.logical_length);
    append_u32(bytes, static_cast<std::uint32_t>(record.flags));
    append_u64(bytes, record.nonce_counter);
    append_array(bytes, record.plaintext_hash);
    append_array(bytes, record.authentication_tag);
  }
  return hash_bytes(bytes);
}

std::wstring hex_digest(const operationcore::Sha256Digest& digest) {
  constexpr wchar_t kHex[] = L"0123456789ABCDEF";
  std::wstring value;
  value.reserve(digest.size() * 2U);
  for (const std::byte byte : digest) {
    const auto number = std::to_integer<unsigned int>(byte);
    value.push_back(kHex[(number >> 4U) & 0x0fU]);
    value.push_back(kHex[number & 0x0fU]);
  }
  return value;
}

struct DerivedResumePlan final {
  operationcore::OperationPlan plan;
  operationcore::Sha256Digest plan_hash{};
  operationcore::ResumeIdentityBinding identities{};
  std::wstring continuity_token;
  std::uint64_t logical_bytes{};
  std::uint64_t chunk_count{};
  bool rescue_mode{};
  bool partial_loss{};
  std::vector<clonecore::ByteRange> unreadable_ranges;
};

clonecore::Result<DerivedResumePlan> derive_resume_plan(
    const DirectImageRestoreRequest& request,
    const DirectImageRestoreResumeEvidence& evidence,
    const DirectImageRestoreResumeStorageProof& storage,
    const operationcore::OperationId& operation_id,
    const std::optional<operationcore::ResumeSlotRecord>& existing) {
  const auto& image = evidence.image;
  const bool exact =
      image.manifest.mode == imageformat::TsumugiManifestMode::exact;
  const bool rescue =
      image.manifest.mode == imageformat::TsumugiManifestMode::rescue;
  const bool matching_payload_kind =
      (exact && image.container.header.payload_kind ==
                    imageformat::TsumugiPayloadKind::exact_disk) ||
      (rescue && image.container.header.payload_kind ==
                     imageformat::TsumugiPayloadKind::rescue_disk);
  const bool encrypted =
      (image.container.header.required_features &
       static_cast<std::uint32_t>(
           imageformat::TsumugiRequiredFeature::encrypted)) != 0U;
  if (!evidence.complete_image_verified_on_one_immutable_handle ||
      !evidence.image_file_identity_from_that_handle ||
      !evidence.target_state_from_locked_handle ||
      !evidence.active_rescue_media_excluded_by_stable_identity ||
      !evidence.exact_or_rescue_whole_disk_layout_only ||
      all_zero(evidence.image_file_object_identity_hash) ||
      (!exact && !rescue) || !matching_payload_kind ||
      image.container.global_hash != request.expected_image_global_hash ||
      image.manifest.source_state_hash !=
          request.expected_source_state_hash ||
      !image.container.header_hash_verified ||
      !image.container.all_chunks_verified ||
      !image.container.global_hash_verified ||
      (encrypted && !image.container.metadata_authenticated) ||
      image.container.records.empty() ||
      image.container.header.chunk_count !=
          image.container.records.size() ||
      evidence.payload_segments.empty() ||
      !evidence.observed_operation.target ||
      evidence.observed_operation.source ||
      !same_stable_identity(
          request.expected_target,
          *evidence.observed_operation.target)) {
    return failure<DerivedResumePlan>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"PE whole-disk復元Resume完全証明",
        L"同一画像file objectの完全検証、exact/rescue whole-disk、対象再識別、または起動媒体除外を証明できません");
  }

  auto bad_ranges = authenticated_unreadable_ranges(image);
  if (!bad_ranges) {
    return clonecore::Result<DerivedResumePlan>::failure(
        bad_ranges.error());
  }

  std::uint64_t authenticated_container_bytes = 0U;
  for (std::uint64_t index = 0U;
       index < image.container.records.size();
       ++index) {
    const auto& record = image.container.records[
        static_cast<std::size_t>(index)];
    const bool zero = chunk_is_zero_filled(record.flags);
    const std::uint64_t expected_nonce = encrypted && !zero
        ? index + 1U
        : 0U;
    if (record.logical_length == 0U ||
        record.nonce_counter != expected_nonce ||
        !checked_add(
            authenticated_container_bytes,
            record.logical_length,
            authenticated_container_bytes)) {
      return failure<DerivedResumePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"PE exact復元Resume chunk/Nonce列",
          L"認証済みchunk列、暗号化chunk indexからのNonce状態、または処理量が不正です");
    }
  }
  if (authenticated_container_bytes == 0U ||
      (encrypted && all_zero(image.container.header.base_nonce))) {
    return failure<DerivedResumePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE exact復元Resume処理量",
        L"処理量または暗号化base Nonceが不正です");
  }

  std::uint64_t logical_bytes = 0U;
  std::optional<imageformat::TsumugiPhysicalResumePayloadSegmentV1>
      previous_segment;
  for (const auto& segment : evidence.payload_segments) {
    if (segment.record_index >= image.container.records.size()) {
      return failure<DerivedResumePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"PE exact復元Resume payload mapping",
          L"payload segmentが認証済みcontainer record外です");
    }
    const auto& record = image.container.records[
        static_cast<std::size_t>(segment.record_index)];
    std::uint64_t segment_record_end{};
    std::uint64_t expected_target{};
    if (segment.length == 0U ||
        segment.record_plaintext_offset %
                image.manifest.logical_sector_size !=
            0U ||
        segment.length % image.manifest.logical_sector_size != 0U ||
        !checked_add(
            segment.record_plaintext_offset,
            segment.length,
            segment_record_end) ||
        segment_record_end > record.logical_length ||
        !checked_add(
            record.logical_offset,
            segment.record_plaintext_offset,
            expected_target) ||
        segment.target_offset != expected_target ||
        !checked_add(logical_bytes, segment.length, logical_bytes) ||
        (previous_segment &&
         (segment.record_index < previous_segment->record_index ||
          (segment.record_index == previous_segment->record_index &&
           segment.record_plaintext_offset <
               previous_segment->record_plaintext_offset +
                   previous_segment->length)))) {
      return failure<DerivedResumePlan>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"PE exact復元Resume payload mapping",
          L"metadata延期後のpayload segmentが空、未整列、重複、overflow、またはrecord対応外です");
    }
    previous_segment = segment;
  }
  if (logical_bytes == 0U) {
    return failure<DerivedResumePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"PE exact復元Resume payload segment合計",
        L"実際に復元するpayload segmentが空または件数上限外です");
  }

  auto source = hash_source_identity(evidence);
  if (!source) {
    return clonecore::Result<DerivedResumePlan>::failure(source.error());
  }
  if (existing &&
      (!existing->checkpoint.checkpoint.target ||
       (existing->checkpoint.checkpoint.schema_version ==
            operationcore::kCheckpointSchemaVersionV2 &&
        !existing->checkpoint.checkpoint.preparation_evidence))) {
    return failure<DerivedResumePlan>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE Resume durable target shape",
        L"checkpointに安定targetまたはv2 initial-layout証跡がありません");
  }
  const clonecore::StableDiskIdentity durable_target = existing
      ? *existing->checkpoint.checkpoint.target
      : request.expected_target;
  const auto stable_request = clonecore::validate_stable_identity(
      durable_target, request.expected_target, L"PE Resume durable target");
  if (!stable_request) {
    return clonecore::Result<DerivedResumePlan>::failure(
        stable_request.error());
  }
  const operationcore::Sha256Digest durable_initial_layout = existing &&
          existing->checkpoint.checkpoint.preparation_evidence
      ? existing->checkpoint.checkpoint.preparation_evidence
            ->initial_layout_hash
      : request.expected_target_layout_hash;

  const auto build_candidate = [&](const bool legacy)
      -> clonecore::Result<DerivedResumePlan> {
    clonecore::StableDiskIdentity plan_target = legacy
        ? durable_target
        : persistent_target_shape(durable_target);
    auto target_hash = legacy
        ? hash_target_identity_v1(
              plan_target, durable_initial_layout)
        : hash_target_identity_v2(plan_target);
    if (!target_hash) {
      return clonecore::Result<DerivedResumePlan>::failure(
          target_hash.error());
    }
    auto output = hash_output_identity(source.value(), target_hash.value());
    if (!output) {
      return clonecore::Result<DerivedResumePlan>::failure(output.error());
    }
    const operationcore::ResumeIdentityBinding identities{
        .source_identity_hash = source.value(),
        .target_identity_hash = target_hash.value(),
        .output_identity_hash = output.value(),
    };
    if (existing && !same_identities(existing->identities, identities)) {
      return failure<DerivedResumePlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"PE Resume durable identity algorithm",
          L"checkpointのimage/target/output identityがv1/v2候補と一致しません");
    }
    auto immutable = hash_immutable_payload(
        identities,
        storage,
        legacy ? nullptr : &durable_initial_layout,
        image.manifest.mode,
        bad_ranges.value(),
        logical_bytes,
        image.container.records,
        evidence.payload_segments);
    if (!immutable) {
      return clonecore::Result<DerivedResumePlan>::failure(
          immutable.error());
    }
    operationcore::OperationPlan plan{
        .schema_version = operationcore::kOperationPlanSchemaVersion,
        .operation_id = operation_id,
        .kind = operationcore::OperationKind::image_restore,
        .environment = operationcore::OperationEnvironment::winpe,
        .source = std::nullopt,
        .target = std::move(plan_target),
        .expected_work_bytes = logical_bytes,
        .immutable_payload_hash = immutable.value(),
    };
    auto plan_hash = operationcore::hash_operation_plan(plan);
    if (!plan_hash) {
      return clonecore::Result<DerivedResumePlan>::failure(
          plan_hash.error());
    }
    if (existing && plan_hash.value() !=
            existing->checkpoint.checkpoint.plan_hash) {
      return failure<DerivedResumePlan>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"PE Resume durable plan algorithm",
          L"checkpoint plan Hashがv1/v2候補と一致しません");
    }
    return clonecore::Result<DerivedResumePlan>::success({
        .plan = std::move(plan),
        .plan_hash = plan_hash.value(),
        .identities = identities,
        .continuity_token =
            L"TSUMUGI-WHOLE-DISK-V1-" + hex_digest(source.value()) + L"-" +
            hex_digest(immutable.value()),
        .logical_bytes = logical_bytes,
        .chunk_count =
            static_cast<std::uint64_t>(evidence.payload_segments.size()),
        .rescue_mode = rescue,
        .partial_loss = image.partial_loss,
        .unreadable_ranges = bad_ranges.value(),
    });
  };

  if (existing && existing->checkpoint.checkpoint.schema_version ==
          operationcore::kCheckpointSchemaVersionV1) {
    return build_candidate(true);
  }
  auto current = build_candidate(false);
  if (current || !existing) {
    return current;
  }
  // A v1 checkpoint upgraded in-place to schema v2 retains its immutable v1
  // plan/identity hashes. Trying both exact algorithms avoids an out-of-band
  // migration marker while never accepting a hash that was not already bound
  // by the authenticated slot.
  return build_candidate(true);
}

clonecore::Result<operationcore::ParsedCheckpoint> make_parsed_checkpoint(
    const operationcore::InterruptionCheckpoint& checkpoint) {
  auto bytes = operationcore::serialize_checkpoint(checkpoint);
  if (!bytes) {
    return clonecore::Result<operationcore::ParsedCheckpoint>::failure(
        bytes.error());
  }
  return operationcore::parse_checkpoint(bytes.value());
}

clonecore::Status validate_verified_prefix(
    const operationcore::InterruptionCheckpoint& checkpoint,
    const DirectImageRestoreResumeEvidence& evidence,
    const DerivedResumePlan& derived) {
  const bool legacy_executing =
      checkpoint.schema_version == operationcore::kCheckpointSchemaVersionV1 &&
      checkpoint.phase == operationcore::CheckpointPhase::executing;
  const bool preparing =
      checkpoint.schema_version == operationcore::kCheckpointSchemaVersionV2 &&
      checkpoint.phase == operationcore::CheckpointPhase::preparing &&
      checkpoint.verified_chunk_count == 0U &&
      checkpoint.verified_work_bytes == 0U;
  const bool prepared =
      checkpoint.schema_version == operationcore::kCheckpointSchemaVersionV2 &&
      checkpoint.phase == operationcore::CheckpointPhase::prepared;
  const bool commit_ready =
      checkpoint.schema_version == operationcore::kCheckpointSchemaVersionV2 &&
      checkpoint.phase == operationcore::CheckpointPhase::commit_ready &&
      checkpoint.verified_chunk_count == derived.chunk_count &&
      checkpoint.verified_work_bytes == derived.logical_bytes;
  if ((!legacy_executing && !preparing && !prepared && !commit_ready) ||
      checkpoint.verified_chunk_count > derived.chunk_count) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"PE exact復元Resume prefix",
        L"preparing/prepared/commit-readyまたはlegacy executingの連続chunk prefix以外は再開できません");
  }
  std::uint64_t bytes = 0U;
  for (std::uint64_t index = 0U;
       index < checkpoint.verified_chunk_count;
       ++index) {
    if (!checked_add(
            bytes,
            evidence.payload_segments[
                static_cast<std::size_t>(index)].length,
            bytes)) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE exact復元Resume prefix",
          L"検証済みchunk prefix容量がオーバーフローします");
    }
  }
  if (bytes != checkpoint.verified_work_bytes ||
      bytes > derived.logical_bytes) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE exact復元Resume prefix",
        L"checkpointの検証済み容量が認証済み先頭chunk列と一致しません");
  }
  return clonecore::success_status();
}

clonecore::Result<DirectImageRestoreResumeEvidence> collect_evidence(
    const DirectImageRestoreRequest& request,
    const std::optional<operationcore::ResumeSlotRecord>& existing,
    const DirectImageRestoreResumeDependencies& dependencies) {
  if (!dependencies.collect_evidence) {
    return failure<DirectImageRestoreResumeEvidence>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_FUNCTION,
        L"PE exact復元Resume証拠収集",
        L"同一handle完全検証と対象状態を収集する依存がありません");
  }
  try {
    return dependencies.collect_evidence(request, existing);
  } catch (...) {
    return failure<DirectImageRestoreResumeEvidence>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"PE exact復元Resume証拠収集",
        L"証拠収集adapterが例外で停止しました");
  }
}

clonecore::Result<DirectImageRestoreResumeTransferReport> execute_transfer(
    const DirectImageRestoreRequest& request,
    const DirectImageRestoreResumeEvidence& evidence,
    const DirectImageRestoreResumeCursor& cursor,
    const DirectImageRestoreResumePhaseCommit& preparation_commit,
    const DirectImageRestoreChunkReadbackCommit& commit,
    const DirectImageRestoreResumePhaseCommit& commit_ready_commit,
    const DirectImageRestoreResumeDependencies& dependencies) {
  if (!dependencies.execute_transfer) {
    return failure<DirectImageRestoreResumeTransferReport>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE exact復元Resume転送adapter",
        L"未完成レイアウトを再開できる転送adapterが未接続です");
  }
  try {
    return dependencies.execute_transfer(
        request,
        evidence,
        cursor,
        preparation_commit,
        commit,
        commit_ready_commit);
  } catch (...) {
    return failure<DirectImageRestoreResumeTransferReport>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"PE exact復元Resume転送adapter",
        L"転送adapterが例外で停止したためcheckpointを保持します");
  }
}

}  // namespace

clonecore::Result<std::wstring>
format_direct_image_restore_resume_startup_review_v1(
    const DirectImageRestoreResumeOutcome& outcome) {
  try {
    if (outcome.kind !=
            DirectImageRestoreResumeOutcomeKind::decision_required ||
        !outcome.existing_slot || !outcome.checkpoint_phase ||
        !outcome.capability ||
        (*outcome.capability !=
             operationcore::ResumeCapability::persistent_exact_restore &&
         *outcome.capability !=
             operationcore::ResumeCapability::persistent_rescue_restore) ||
        outcome.verified_logical_bytes > outcome.expected_logical_bytes) {
      return failure<std::wstring>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"PE exact復元Resume startup表示",
          L"bounded startup summaryの必須項目が不正です");
    }

    const wchar_t* mode = *outcome.capability ==
            operationcore::ResumeCapability::persistent_rescue_restore
        ? L"救出イメージのディスク全体復元"
        : L"通常イメージのディスク全体復元";
    const wchar_t* phase = [&]() noexcept {
      switch (*outcome.checkpoint_phase) {
        case operationcore::CheckpointPhase::preparing:
          return L"復元先の準備中";
        case operationcore::CheckpointPhase::prepared:
          return L"復元データの書込み待ち";
        case operationcore::CheckpointPhase::executing:
          return L"復元データの書込み中";
        case operationcore::CheckpointPhase::verifying:
          return L"書込み内容の確認中";
        case operationcore::CheckpointPhase::commit_ready:
          return L"パーティション情報の最終確定待ち";
      }
      return L"不明";
    }();

    std::wostringstream text;
    text << L"前回中断した復元の安全な再開情報があります。\r\n\r\n"
         << L"種類: " << mode << L"\r\n"
         << L"段階: " << phase << L"\r\n"
         << L"読戻し確認済み: " << outcome.verified_logical_bytes
         << L" / " << outcome.expected_logical_bytes << L" bytes\r\n"
         << L"確認済みチャンク: " << outcome.verified_chunk_count
         << L"\r\n\r\n"
         << L"［はい］同じイメージと復元先を選び直して再開準備へ\r\n"
         << L"［いいえ］この再開情報を破棄する確認へ\r\n"
         << L"［キャンセル］今回は何も変更せず保持";
    return clonecore::Result<std::wstring>::success(text.str());
  } catch (const std::bad_alloc&) {
    return failure<std::wstring>(
        clonecore::ErrorCode::io_failed,
        ERROR_NOT_ENOUGH_MEMORY,
        L"PE exact復元Resume startup表示",
        L"startup summaryの作成に必要なメモリを確保できません");
  } catch (...) {
    return failure<std::wstring>(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"PE exact復元Resume startup表示",
        L"startup summaryを安全に作成できません");
  }
}

clonecore::Result<DirectImageRestoreResumeOutcome>
control_direct_image_restore_resume_v1(
    const DirectImageRestoreRequest& request,
    const DirectImageRestoreResumeCommand& command,
    const DirectImageRestoreResumeDependencies& dependencies) {
  if (command.action == DirectImageRestoreResumeAction::cancel) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::success({
        .kind = DirectImageRestoreResumeOutcomeKind::cancelled,
    });
  }
  if (dependencies.slot_platform == nullptr) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_FUNCTION,
        L"PE exact復元Resume controller",
        L"単一slot platformがありません");
  }

  operationcore::SingleResumeSlot slot(*dependencies.slot_platform);
  auto inspected = slot.inspect();
  if (!inspected) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        inspected.error());
  }
  std::optional<operationcore::ResumeSlotRecord> existing =
      inspected.take_value();
  std::optional<operationcore::ResumeSlotBinding> existing_binding;
  if (existing) {
    auto binding = operationcore::make_resume_slot_binding(*existing);
    if (!binding) {
      return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
          binding.error());
    }
    existing_binding = binding.take_value();
  }

  if (command.action == DirectImageRestoreResumeAction::inspect_only) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::success({
        .kind = existing
            ? DirectImageRestoreResumeOutcomeKind::decision_required
            : DirectImageRestoreResumeOutcomeKind::no_slot,
        .existing_slot = existing_binding,
        .verified_logical_bytes = existing
            ? existing->checkpoint.checkpoint.verified_work_bytes
            : 0U,
        .verified_chunk_count = existing
            ? existing->checkpoint.checkpoint.verified_chunk_count
            : 0U,
        .expected_logical_bytes = existing
            ? existing->checkpoint.checkpoint.expected_work_bytes
            : 0U,
        .checkpoint_phase = existing
            ? std::optional<operationcore::CheckpointPhase>(
                  existing->checkpoint.checkpoint.phase)
            : std::nullopt,
        .capability = existing
            ? std::optional<operationcore::ResumeCapability>(
                  existing->capability)
            : std::nullopt,
    });
  }

  if (existing &&
      command.action == DirectImageRestoreResumeAction::start_new) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::access_denied,
        ERROR_FILE_EXISTS,
        L"PE exact復元Resume新規開始",
        L"既存checkpointを再開または安全に破棄するまで新しい処理を開始できません");
  }
  if (!existing &&
      (command.action ==
           DirectImageRestoreResumeAction::resume_existing ||
       command.action ==
           DirectImageRestoreResumeAction::discard_existing)) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_NOT_FOUND,
        L"PE exact復元Resume既存slot",
        L"レビューした既存checkpointがありません");
  }

  if (command.action ==
      DirectImageRestoreResumeAction::discard_existing) {
    if (!command.reviewed_existing_slot || !existing_binding ||
        !same_binding(
            *command.reviewed_existing_slot, *existing_binding)) {
      return failure<DirectImageRestoreResumeOutcome>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_FILE_INVALID,
          L"PE exact復元Resume破棄確認",
          L"表示後にslotが変化したため破棄しません");
    }
    const auto discarded = slot.discard(*existing_binding);
    if (!discarded) {
      return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
          discarded.error());
    }
    return clonecore::Result<DirectImageRestoreResumeOutcome>::success({
        .kind = DirectImageRestoreResumeOutcomeKind::discarded,
    });
  }

  if (command.action != DirectImageRestoreResumeAction::start_new &&
      command.action !=
          DirectImageRestoreResumeAction::resume_existing) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"PE exact復元Resume操作",
        L"未対応のcontroller操作です");
  }
  if (!request.administrator ||
      !request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token != L"OK") {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"PE exact復元Resume最終確認",
        L"管理者権限、消去内容確認、または大文字OKが不足しています");
  }
  if (dependencies.physical_dependencies == nullptr ||
      !dependencies.physical_dependencies
           ->persistent_exact_resume_capable) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"PE exact復元Resume物理adapter",
        L"現在の物理adapterは同じ未完成レイアウトを再開できないため、対象I/O前に停止しました");
  }
  if (!dependencies.prove_storage_separation) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_FUNCTION,
        L"PE exact復元Resume controller",
        L"opened storage配置証明がありません");
  }
  if (existing &&
      (!command.reviewed_existing_slot || !existing_binding ||
       !same_binding(
           *command.reviewed_existing_slot, *existing_binding))) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_FILE_INVALID,
        L"PE exact復元Resume表示内容",
        L"表示後にslotが変化したため再開しません");
  }

  clonecore::Result<DirectImageRestoreResumeStorageProof> storage = [&]() {
    try {
      return dependencies.prove_storage_separation(request);
    } catch (...) {
      return failure<DirectImageRestoreResumeStorageProof>(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"PE exact復元Resume配置証明",
          L"配置証明adapterが例外で停止しました");
    }
  }();
  if (!storage) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        storage.error());
  }
  const auto storage_valid = validate_storage_proof(storage.value());
  if (!storage_valid) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        storage_valid.error());
  }

  const auto operation_id = existing
      ? existing->checkpoint.checkpoint.operation_id
      : command.new_operation_id;
  auto evidence = collect_evidence(request, existing, dependencies);
  if (!evidence) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        evidence.error());
  }
  if (evidence.value().image_file_object_identity_hash !=
          storage.value().image_file_object_identity_hash ||
      evidence.value().target_storage_identity_hash !=
          storage.value().target_storage_identity_hash) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"PE Resume opened storage/evidence binding",
        L"完全検証したimage file objectまたはoffline targetのopened storageが配置証明と一致しません");
  }
  if (existing &&
      existing->checkpoint.checkpoint.schema_version ==
          operationcore::kCheckpointSchemaVersionV1 &&
      evidence.value().target_state ==
          DirectImageRestoreTargetResumeState::
              preparation_original_or_zero_bound_to_operation) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE Resume legacy partial invalidation",
        L"元sector digestを持たないv1 checkpointは部分zero状態からv2へ昇格できません");
  }
  auto derived = derive_resume_plan(
      request, evidence.value(), storage.value(), operation_id, existing);
  if (!derived) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        derived.error());
  }

  operationcore::ResumeSlotRecord current_record;
  operationcore::ResumeSlotBinding current_binding;
  const auto expected_capability = derived.value().rescue_mode
      ? operationcore::ResumeCapability::persistent_rescue_restore
      : operationcore::ResumeCapability::persistent_exact_restore;
  if (existing) {
    const auto& existing_checkpoint =
        existing->checkpoint.checkpoint;
    const bool plan_bound =
        evidence.value().incomplete_layout_plan_hash ==
            existing_checkpoint.plan_hash &&
        evidence.value().incomplete_layout_plan_hash ==
            derived.value().plan_hash;
    const bool exact_incomplete_layout = plan_bound &&
        evidence.value().target_state ==
            DirectImageRestoreTargetResumeState::
                incomplete_layout_bound_to_operation;
    const bool zero_cursor_on_reviewed_initial =
        existing_checkpoint.verified_work_bytes == 0U &&
        existing_checkpoint.verified_chunk_count == 0U &&
        evidence.value().target_state ==
            DirectImageRestoreTargetResumeState::reviewed_initial_layout &&
        evidence.value().observed_initial_target_layout_hash ==
            (existing_checkpoint.preparation_evidence
                 ? existing_checkpoint.preparation_evidence
                       ->initial_layout_hash
                 : request.expected_target_layout_hash);
    bool phase_state_matches{};
    if (existing_checkpoint.schema_version ==
        operationcore::kCheckpointSchemaVersionV1) {
      phase_state_matches =
          (zero_cursor_on_reviewed_initial || exact_incomplete_layout) &&
          plan_bound &&
          !evidence.value().preparation_sectors.empty();
    } else if (existing_checkpoint.preparation_evidence) {
      auto durable_sectors = to_physical_preparation_sectors(
          *existing_checkpoint.preparation_evidence);
      if (!durable_sectors) {
        return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
            durable_sectors.error());
      }
      const bool evidence_matches =
          durable_sectors.value() == evidence.value().preparation_sectors &&
          evidence.value().observed_initial_target_layout_hash ==
              existing_checkpoint.preparation_evidence->initial_layout_hash;
      switch (existing_checkpoint.phase) {
        case operationcore::CheckpointPhase::preparing:
          phase_state_matches = evidence_matches && plan_bound &&
              (zero_cursor_on_reviewed_initial ||
               evidence.value().target_state ==
                   DirectImageRestoreTargetResumeState::
                       preparation_original_or_zero_bound_to_operation);
          break;
        case operationcore::CheckpointPhase::prepared:
          phase_state_matches = evidence_matches && exact_incomplete_layout;
          break;
        case operationcore::CheckpointPhase::commit_ready:
          phase_state_matches = evidence_matches && plan_bound &&
              (evidence.value().target_state ==
                   DirectImageRestoreTargetResumeState::
                       incomplete_layout_bound_to_operation ||
               evidence.value().target_state ==
                   DirectImageRestoreTargetResumeState::
                       commit_publication_bound_to_operation ||
               evidence.value().target_state ==
                   DirectImageRestoreTargetResumeState::
                       completed_layout_bound_to_operation);
          break;
        case operationcore::CheckpointPhase::executing:
        case operationcore::CheckpointPhase::verifying:
          phase_state_matches = false;
          break;
      }
    }
    if (existing->capability !=
            expected_capability ||
        existing->owned_partial.has_value() ||
        !same_identities(existing->identities, derived.value().identities) ||
        !phase_state_matches) {
      return failure<DirectImageRestoreResumeOutcome>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"PE exact復元Resume未完成layout",
          L"同じoperation、画像file identity、対象、出力、または未完成layout plan Hashを証明できません");
    }
    const auto resumable = operationcore::validate_checkpoint_for_resume(
        existing->checkpoint.checkpoint,
        derived.value().plan,
        evidence.value().observed_operation,
        derived.value().continuity_token,
        derived.value().identities.output_identity_hash);
    const auto prefix = validate_verified_prefix(
        existing->checkpoint.checkpoint,
        evidence.value(),
        derived.value());
    if (!resumable || !prefix) {
      return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
          !resumable ? resumable.error() : prefix.error());
    }
    current_record = *existing;
    current_binding = *existing_binding;
    if (existing_checkpoint.schema_version ==
        operationcore::kCheckpointSchemaVersionV1) {
      if (existing_checkpoint.revision ==
          (std::numeric_limits<std::uint64_t>::max)()) {
        return failure<DirectImageRestoreResumeOutcome>(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"PE Resume legacy checkpoint upgrade",
            L"checkpoint revisionが上限に達しています");
      }
      auto upgraded = existing_checkpoint;
      upgraded.schema_version = operationcore::kCheckpointSchemaVersionV2;
      ++upgraded.revision;
      upgraded.phase = evidence.value().target_state ==
              DirectImageRestoreTargetResumeState::reviewed_initial_layout
          ? operationcore::CheckpointPhase::preparing
          : operationcore::CheckpointPhase::prepared;
      upgraded.preparation_evidence = to_checkpoint_preparation(
          request.expected_target_layout_hash,
          request.expected_target.logical_sector_size,
          evidence.value().preparation_sectors);
      auto parsed = make_parsed_checkpoint(upgraded);
      if (!parsed) {
        return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
            parsed.error());
      }
      const auto replaced = slot.replace(current_binding, parsed.value());
      if (!replaced) {
        return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
            replaced.error());
      }
      current_record.checkpoint = parsed.take_value();
      auto upgraded_binding =
          operationcore::make_resume_slot_binding(current_record);
      if (!upgraded_binding) {
        return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
            upgraded_binding.error());
      }
      current_binding = upgraded_binding.take_value();
    }
  } else {
    if (evidence.value().target_state !=
            DirectImageRestoreTargetResumeState::reviewed_initial_layout ||
        evidence.value().observed_initial_target_layout_hash !=
            request.expected_target_layout_hash ||
        evidence.value().preparation_sectors.empty()) {
      return failure<DirectImageRestoreResumeOutcome>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DEVICE_REINITIALIZATION_NEEDED,
          L"PE exact復元Resume新規layout",
          L"最終確認した初期target layoutと実行直前状態が一致しません");
    }
    const auto reidentified = operationcore::validate_reidentified_operation(
        derived.value().plan, evidence.value().observed_operation);
    if (!reidentified) {
      return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
          reidentified.error());
    }
    operationcore::InterruptionCheckpoint checkpoint{
        .schema_version = operationcore::kCheckpointSchemaVersion,
        .operation_id = derived.value().plan.operation_id,
        .kind = derived.value().plan.kind,
        .environment = derived.value().plan.environment,
        .phase = operationcore::CheckpointPhase::preparing,
        .revision = 1U,
        .expected_work_bytes = derived.value().logical_bytes,
        .verified_work_bytes = 0U,
        .verified_chunk_count = 0U,
        .plan_hash = derived.value().plan_hash,
        .output_identity_hash =
            derived.value().identities.output_identity_hash,
        .source = derived.value().plan.source,
        .target = derived.value().plan.target,
        .continuity_token = derived.value().continuity_token,
        .preparation_evidence = to_checkpoint_preparation(
            request.expected_target_layout_hash,
            request.expected_target.logical_sector_size,
            evidence.value().preparation_sectors),
    };
    auto parsed = make_parsed_checkpoint(checkpoint);
    if (!parsed) {
      return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
          parsed.error());
    }
    current_record = operationcore::ResumeSlotRecord{
        .capability = expected_capability,
        .checkpoint = parsed.take_value(),
        .identities = derived.value().identities,
        .owned_partial = std::nullopt,
    };
    const auto created = slot.create(current_record);
    if (!created) {
      return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
          created.error());
    }
    auto binding = operationcore::make_resume_slot_binding(current_record);
    if (!binding) {
      return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
          binding.error());
    }
    current_binding = binding.take_value();
  }

  auto rebound = slot.open_bound(current_binding);
  if (!rebound) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        rebound.error());
  }
  current_record = rebound.take_value();
  const std::uint64_t starting_bytes =
      current_record.checkpoint.checkpoint.verified_work_bytes;
  const std::uint64_t starting_chunks =
      current_record.checkpoint.checkpoint.verified_chunk_count;
  auto durable_phase = to_physical_phase(
      current_record.checkpoint.checkpoint);
  if (!durable_phase) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        durable_phase.error());
  }
  auto durable_preparation = current_record.checkpoint.checkpoint
          .preparation_evidence
      ? to_physical_preparation_sectors(
            *current_record.checkpoint.checkpoint.preparation_evidence)
      : clonecore::Result<std::vector<
            imageformat::TsumugiPhysicalResumePreparationSectorV1>>::success(
            evidence.value().preparation_sectors);
  if (!durable_preparation) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        durable_preparation.error());
  }

  const auto replace_checkpoint = [&](operationcore::InterruptionCheckpoint next) {
    auto parsed = make_parsed_checkpoint(next);
    if (!parsed) {
      return clonecore::Status::failure(parsed.error());
    }
    auto replaced = slot.replace(current_binding, parsed.value());
    if (!replaced) {
      return replaced;
    }
    current_record.checkpoint = parsed.take_value();
    auto binding = operationcore::make_resume_slot_binding(current_record);
    if (!binding) {
      return clonecore::Status::failure(binding.error());
    }
    current_binding = binding.take_value();
    return clonecore::success_status();
  };

  const DirectImageRestoreResumePhaseCommit preparation_commit = [&]() {
    const auto& checkpoint = current_record.checkpoint.checkpoint;
    if (checkpoint.schema_version !=
            operationcore::kCheckpointSchemaVersionV2 ||
        checkpoint.phase != operationcore::CheckpointPhase::preparing ||
        checkpoint.revision ==
            (std::numeric_limits<std::uint64_t>::max)()) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_STATE,
          L"PE Resume preparation phase commit",
          L"durable preparing checkpointだけをpreparedへ進められます");
    }
    auto next = checkpoint;
    ++next.revision;
    next.phase = operationcore::CheckpointPhase::prepared;
    return replace_checkpoint(std::move(next));
  };

  const DirectImageRestoreChunkReadbackCommit commit =
      [&](const std::uint64_t index,
          const imageformat::TsumugiPhysicalResumePayloadSegmentV1& segment) {
    const auto& checkpoint = current_record.checkpoint.checkpoint;
    if (checkpoint.phase != operationcore::CheckpointPhase::prepared ||
        index != checkpoint.verified_chunk_count ||
        index >= derived.value().chunk_count ||
        !same_payload_segment(
            segment,
            evidence.value().payload_segments[
                static_cast<std::size_t>(index)])) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"PE exact復元Resume readback順序",
          L"次の認証済みchunkとflush/readback完了報告が一致しません");
    }
    std::uint64_t next_bytes{};
    if (!checked_add(
            checkpoint.verified_work_bytes,
            segment.length,
            next_bytes) ||
        next_bytes > derived.value().logical_bytes ||
        checkpoint.revision ==
            (std::numeric_limits<std::uint64_t>::max)()) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"PE exact復元Resume checkpoint進行",
          L"検証済み容量またはrevisionが上限を超えます");
    }
    auto next = checkpoint;
    ++next.revision;
    next.verified_work_bytes = next_bytes;
    next.verified_chunk_count = index + 1U;
    return replace_checkpoint(std::move(next));
  };

  const DirectImageRestoreResumePhaseCommit commit_ready_commit = [&]() {
    const auto& checkpoint = current_record.checkpoint.checkpoint;
    if (checkpoint.schema_version !=
            operationcore::kCheckpointSchemaVersionV2 ||
        checkpoint.phase != operationcore::CheckpointPhase::prepared ||
        checkpoint.verified_work_bytes != checkpoint.expected_work_bytes ||
        checkpoint.verified_chunk_count != derived.value().chunk_count ||
        checkpoint.revision ==
            (std::numeric_limits<std::uint64_t>::max)()) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_STATE,
          L"PE Resume commit-ready phase commit",
          L"全payload読戻し済みprepared checkpointだけをcommit-readyへ進められます");
    }
    auto next = checkpoint;
    ++next.revision;
    next.phase = operationcore::CheckpointPhase::commit_ready;
    return replace_checkpoint(std::move(next));
  };

  const DirectImageRestoreResumeCursor cursor{
      .operation_id = derived.value().plan.operation_id,
      .verified_logical_bytes = starting_bytes,
      .verified_chunk_count = starting_chunks,
      .expected_logical_bytes = derived.value().logical_bytes,
      .expected_chunk_count = derived.value().chunk_count,
      .plan_hash = derived.value().plan_hash,
      .durable_phase = durable_phase.take_value(),
      .identities = derived.value().identities,
      .continuity_token = derived.value().continuity_token,
      .payload_segments = evidence.value().payload_segments,
      .preparation_sectors = durable_preparation.take_value(),
  };
  auto transferred = execute_transfer(
      request,
      evidence.value(),
      cursor,
      preparation_commit,
      commit,
      commit_ready_commit,
      dependencies);
  if (!transferred) {
    return clonecore::Result<DirectImageRestoreResumeOutcome>::failure(
        transferred.error());
  }
  const auto& report = transferred.value();
  const auto& final_checkpoint = current_record.checkpoint.checkpoint;
  if (report.resumed_verified_logical_bytes != starting_bytes ||
      report.resumed_verified_chunk_count != starting_chunks ||
      report.final_verified_logical_bytes != derived.value().logical_bytes ||
      report.final_verified_chunk_count != derived.value().chunk_count ||
      final_checkpoint.verified_work_bytes != derived.value().logical_bytes ||
      final_checkpoint.verified_chunk_count != derived.value().chunk_count ||
      final_checkpoint.schema_version !=
          operationcore::kCheckpointSchemaVersionV2 ||
      final_checkpoint.phase != operationcore::CheckpointPhase::commit_ready ||
      !report.full_image_reverified_on_same_handle_before_first_write ||
      !report.target_and_incomplete_layout_reidentified_before_first_write ||
      !report.verified_prefix_was_not_rewritten ||
      !report.every_new_chunk_flushed_and_read_back ||
      !report.final_layout_committed || !report.target_left_offline ||
      report.rescue_mode != derived.value().rescue_mode ||
      report.partial_loss != derived.value().partial_loss ||
      !same_ranges(
          report.unreadable_ranges,
          derived.value().unreadable_ranges)) {
    return failure<DirectImageRestoreResumeOutcome>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"PE whole-disk復元Resume完了証跡",
        L"完全再検証、prefix非再書込み、全chunk読戻し、最終layout確定、offline、またはrescue partial-loss証跡が不足しています");
  }

  DirectImageRestoreResumeOutcome outcome{
      .kind = DirectImageRestoreResumeOutcomeKind::
          completed_checkpoint_retained,
      .existing_slot = current_binding,
      .verified_logical_bytes = final_checkpoint.verified_work_bytes,
      .verified_chunk_count = final_checkpoint.verified_chunk_count,
      .transfer = transferred.take_value(),
      .rescue_mode = derived.value().rescue_mode,
      .partial_loss = derived.value().partial_loss,
      .unreadable_ranges = derived.value().unreadable_ranges,
  };
  // Allocate/copy every user-visible success field before deleting the only
  // durable recovery record. From this point onward successful cleanup uses
  // only no-throw scalar/optional updates.
  const auto discarded = slot.discard(current_binding);
  if (discarded) {
    outcome.kind = DirectImageRestoreResumeOutcomeKind::completed;
    outcome.existing_slot.reset();
  }
  if (!discarded) {
    outcome.checkpoint_cleanup_error = discarded.error();
  }
  return clonecore::Result<DirectImageRestoreResumeOutcome>::success(
      std::move(outcome));
}

}  // namespace ytec::winpeapp

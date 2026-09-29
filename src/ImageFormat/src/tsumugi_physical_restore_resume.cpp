#include "ytec/imageformat/tsumugi_physical_restore_resume.h"

#include "ytec/imageformat/sha256.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

clonecore::Error resume_error(
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

bool chunk_is_zero(const TsumugiChunkFlags flags) noexcept {
  return (static_cast<std::uint32_t>(flags) &
          static_cast<std::uint32_t>(TsumugiChunkFlags::zero_filled)) != 0U;
}

bool chunk_is_unreadable(const TsumugiChunkFlags flags) noexcept {
  return flags == TsumugiChunkFlags::unreadable_zero_filled;
}

bool same_byte_ranges(
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

clonecore::Status validate_whole_disk_mode_and_loss_evidence(
    const TsumugiVerifiedImage& image) {
  const bool exact =
      image.manifest.mode == TsumugiManifestMode::exact &&
      image.container.header.payload_kind == TsumugiPayloadKind::exact_disk;
  const bool rescue =
      image.manifest.mode == TsumugiManifestMode::rescue &&
      image.container.header.payload_kind == TsumugiPayloadKind::rescue_disk;
  if (!exact && !rescue) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi physical Resume mode",
        L"exact/rescue whole-disk以外のmodeまたはpayload kindはこのbackendへ渡せません");
  }

  std::vector<clonecore::ByteRange> ranges;
  for (const auto& record : image.container.records) {
    if (!chunk_is_unreadable(record.flags)) {
      continue;
    }
    std::uint64_t record_end{};
    if (!rescue || record.logical_length == 0U ||
        image.manifest.logical_sector_size == 0U ||
        record.logical_offset % image.manifest.logical_sector_size != 0U ||
        record.logical_length % image.manifest.logical_sector_size != 0U ||
        !checked_add(record.logical_offset, record.logical_length, record_end) ||
        record_end > image.manifest.source_disk_size) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi physical Resume rescue evidence",
          L"欠損zero-fill recordがrescue mode、整列、範囲、または容量条件に一致しません");
    }
    if (!ranges.empty()) {
      std::uint64_t previous_end{};
      if (!checked_add(
              ranges.back().offset, ranges.back().length, previous_end) ||
          record.logical_offset < previous_end) {
        return status_failure(
            clonecore::ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"Tsumugi physical Resume rescue range order",
            L"欠損zero-fill範囲が重複、逆順、またはoverflowしています");
      }
      if (record.logical_offset == previous_end) {
        if (!checked_add(
                ranges.back().length,
                record.logical_length,
                ranges.back().length)) {
          return status_failure(
              clonecore::ErrorCode::invalid_data,
              ERROR_ARITHMETIC_OVERFLOW,
              L"Tsumugi physical Resume rescue range merge",
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
  if ((exact && (image.partial_loss || !image.unreadable_ranges.empty())) ||
      image.partial_loss != !ranges.empty() ||
      !same_byte_ranges(ranges, image.unreadable_ranges)) {
    return status_failure(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Tsumugi physical Resume partial-loss evidence",
        L"完全検証済みrecordから再構成した欠損範囲とpartial-loss証跡が一致しません");
  }
  return clonecore::success_status();
}

clonecore::Status validate_resume_target_identity(
    const TsumugiRestoreDiskIdentity& target) {
  if (all_zero(target.stable_identity_hash) || target.disk_size == 0U ||
      target.logical_sector_size != 512U ||
      target.is_running_windows_system_disk || target.is_usb_memory ||
      target.is_active_rescue_media || target.is_dynamic_disk ||
      target.is_storage_spaces || target.is_windows_software_raid ||
      target.has_unresolved_hardware_raid ||
      (target.is_usb_attached &&
       all_zero(target.connection_instance_hash))) {
    return status_failure(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi physical Resume target boundary",
        L"安定識別済み512-byte fixed target、非system/非rescue/非dynamic/非RAID、およびUSB connection token条件を満たしません");
  }
  return clonecore::success_status();
}

clonecore::Result<std::uint64_t> validate_preparation_layout_shape(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    const clonecore::ITargetDiskWriter& target) {
  if (layout.target_size_bytes == 0U ||
      layout.logical_sector_size == 0U ||
      target.size_bytes() != layout.target_size_bytes ||
      target.logical_sector_size() != layout.logical_sector_size ||
      layout.invalidation_ranges.size() != 2U) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_DEVICE_REINITIALIZATION_NEEDED,
        L"Tsumugi Resume preparation target",
        L"layoutとopened targetの容量、sector、または無効化range数が一致しません");
  }
  std::uint64_t count{};
  std::uint64_t previous_end{};
  bool have_previous{};
  for (const auto& range : layout.invalidation_ranges) {
    std::uint64_t end{};
    if (range.length == 0U ||
        range.offset % layout.logical_sector_size != 0U ||
        range.length % layout.logical_sector_size != 0U ||
        !checked_add(range.offset, range.length, end) ||
        end > layout.target_size_bytes ||
        (have_previous && range.offset < previous_end)) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi Resume preparation ranges",
          L"無効化rangeが空、未整列、重複、overflow、またはtarget外です");
    }
    const auto range_count =
        range.length / layout.logical_sector_size;
    if (!checked_add(count, range_count, count) ||
        count > kTsumugiPhysicalResumeMaximumPreparationSectorsV1) {
      return failure<std::uint64_t>(
          clonecore::ErrorCode::unsupported_layout,
          ERROR_BUFFER_OVERFLOW,
          L"Tsumugi Resume preparation sector count",
          L"永続preparation証跡のsector件数上限を超えます");
    }
    previous_end = end;
    have_previous = true;
  }
  if (count == 0U) {
    return failure<std::uint64_t>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi Resume preparation sector count",
        L"永続preparation証跡が空です");
  }
  return clonecore::Result<std::uint64_t>::success(count);
}

bool bytes_all_zero(const std::span<const std::byte> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

clonecore::Result<TsumugiPhysicalResumePreparationInspectionV1>
inspect_preparation_impl(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    const std::span<const TsumugiPhysicalResumePreparationSectorV1>
        evidence,
    clonecore::ITargetDiskWriter& target) {
  auto expected_count = validate_preparation_layout_shape(layout, target);
  if (!expected_count) {
    return clonecore::Result<
        TsumugiPhysicalResumePreparationInspectionV1>::failure(
        expected_count.error());
  }
  if (evidence.size() != expected_count.value()) {
    return failure<TsumugiPhysicalResumePreparationInspectionV1>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi Resume preparation evidence count",
        L"checkpoint sector証跡が決定的な無効化range全体を覆っていません");
  }

  std::size_t index{};
  std::uint64_t original_count{};
  std::uint64_t zero_count{};
  for (const auto& range : layout.invalidation_ranges) {
    for (std::uint64_t offset = range.offset;
         offset < range.offset + range.length;
         offset += layout.logical_sector_size, ++index) {
      const auto& item = evidence[index];
      if (item.offset != offset ||
          item.length != layout.logical_sector_size ||
          all_zero(item.original_hash)) {
        return failure<TsumugiPhysicalResumePreparationInspectionV1>(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Tsumugi Resume preparation evidence shape",
            L"sector証跡のoffset、length、順序、またはdigestが不正です");
      }
      auto observed = target.read_back(offset, item.length);
      if (!observed) {
        return clonecore::Result<
            TsumugiPhysicalResumePreparationInspectionV1>::failure(
            observed.error());
      }
      if (observed.value().size() != item.length) {
        return failure<TsumugiPhysicalResumePreparationInspectionV1>(
            clonecore::ErrorCode::io_failed,
            ERROR_HANDLE_EOF,
            L"Tsumugi Resume preparation sector read",
            L"opened targetからのsector読戻し長が一致しません");
      }
      if (bytes_all_zero(observed.value())) {
        ++zero_count;
        continue;
      }
      auto current_hash = sha256(observed.value());
      if (!current_hash) {
        return clonecore::Result<
            TsumugiPhysicalResumePreparationInspectionV1>::failure(
            current_hash.error());
      }
      if (current_hash.value() != item.original_hash) {
        return failure<TsumugiPhysicalResumePreparationInspectionV1>(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"Tsumugi Resume preparation sector proof",
            L"metadata sectorが元digestにも全zeroにも一致せず、foreignまたはtorn状態です");
      }
      ++original_count;
    }
  }
  return clonecore::Result<
      TsumugiPhysicalResumePreparationInspectionV1>::success({
      .state = zero_count == evidence.size()
          ? TsumugiPhysicalResumePreparationStateV1::all_zero
          : original_count == evidence.size()
              ? TsumugiPhysicalResumePreparationStateV1::all_original
              : TsumugiPhysicalResumePreparationStateV1::original_or_zero,
      .original_sector_count = original_count,
      .zero_sector_count = zero_count,
  });
}

bool same_record(
    const TsumugiChunkRecord& left,
    const TsumugiChunkRecord& right) noexcept {
  return left.logical_offset == right.logical_offset &&
      left.logical_length == right.logical_length &&
      left.stored_offset == right.stored_offset &&
      left.stored_length == right.stored_length &&
      left.flags == right.flags && left.compression == right.compression &&
      left.nonce_counter == right.nonce_counter &&
      left.plaintext_hash == right.plaintext_hash &&
      left.authentication_tag == right.authentication_tag &&
      left.rescue_read_evidence == right.rescue_read_evidence;
}

bool same_header(
    const TsumugiHeader& left,
    const TsumugiHeader& right) noexcept {
  return left.major_version == right.major_version &&
      left.minor_version == right.minor_version &&
      left.required_features == right.required_features &&
      left.payload_kind == right.payload_kind &&
      left.compression == right.compression &&
      left.source_disk_size == right.source_disk_size &&
      left.logical_sector_size == right.logical_sector_size &&
      left.physical_sector_size == right.physical_sector_size &&
      left.chunk_size == right.chunk_size &&
      left.chunk_count == right.chunk_count &&
      left.data.offset == right.data.offset &&
      left.data.length == right.data.length &&
      left.metadata.offset == right.metadata.offset &&
      left.metadata.length == right.metadata.length &&
      left.footer.offset == right.footer.offset &&
      left.footer.length == right.footer.length &&
      left.argon2.memory_kib == right.argon2.memory_kib &&
      left.argon2.iterations == right.argon2.iterations &&
      left.argon2.parallelism == right.argon2.parallelism &&
      left.argon2.salt == right.argon2.salt &&
      left.base_nonce == right.base_nonce &&
      left.image_id == right.image_id &&
      left.metadata_tag == right.metadata_tag &&
      left.header_hash == right.header_hash;
}

bool same_inspection(
    const TsumugiStreamInspection& left,
    const TsumugiStreamInspection& right) noexcept {
  if (!left.opened_file.identity_from_open_handle ||
      !right.opened_file.identity_from_open_handle ||
      left.opened_file != right.opened_file ||
      !same_header(left.header, right.header) ||
      left.manifest != right.manifest || left.global_hash != right.global_hash ||
      left.header_hash_verified != right.header_hash_verified ||
      left.metadata_authenticated != right.metadata_authenticated ||
      left.all_chunks_verified != right.all_chunks_verified ||
      left.global_hash_verified != right.global_hash_verified ||
      left.records.size() != right.records.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.records.size(); ++index) {
    if (!same_record(left.records[index], right.records[index])) {
      return false;
    }
  }
  return true;
}

bool same_identity(
    const TsumugiRestoreDiskIdentity& left,
    const TsumugiRestoreDiskIdentity& right) noexcept {
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

class DeterministicGuidGenerator final : public clonecore::IGuidGenerator {
 public:
  explicit DeterministicGuidGenerator(std::vector<std::byte> seed)
      : seed_(std::move(seed)) {}

  [[nodiscard]] clonecore::Result<clonecore::GptGuid> next_guid() override {
    if (counter_ == (std::numeric_limits<std::uint64_t>::max)()) {
      return failure<clonecore::GptGuid>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi Resume GUID counter",
          L"決定的GUID counterが上限に達しました");
    }
    std::vector<std::byte> material = seed_;
    append_domain(material, "YTEC-TSUMUGI-RESUME-GUID-V1");
    append_u64(material, counter_++);
    auto digest = sha256(material);
    if (!digest) {
      return clonecore::Result<clonecore::GptGuid>::failure(digest.error());
    }
    clonecore::GptGuid guid{};
    std::copy_n(digest.value().begin(), guid.bytes.size(), guid.bytes.begin());
    // RFC 4122 variant/version bits keep the generated identifiers regular
    // GUIDs without introducing any platform RNG state across restarts.
    guid.bytes[6] = static_cast<std::byte>(
        (std::to_integer<unsigned int>(guid.bytes[6]) & 0x0fU) | 0x40U);
    guid.bytes[8] = static_cast<std::byte>(
        (std::to_integer<unsigned int>(guid.bytes[8]) & 0x3fU) | 0x80U);
    return clonecore::Result<clonecore::GptGuid>::success(guid);
  }

 private:
  std::vector<std::byte> seed_;
  std::uint64_t counter_{};
};

class DeterministicMbrSignatureGenerator final
    : public clonecore::IMbrSignatureGenerator {
 public:
  DeterministicMbrSignatureGenerator(
      std::vector<std::byte> seed,
      const std::span<const std::uint32_t> disallowed)
      : seed_(std::move(seed)), disallowed_(disallowed.begin(), disallowed.end()) {}

  [[nodiscard]] clonecore::Result<std::uint32_t>
  next_signature() override {
    if (used_) {
      return failure<std::uint32_t>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_STATE,
          L"Tsumugi Resume MBR signature",
          L"1 operationで複数のMBR signatureを生成できません");
    }
    used_ = true;
    append_domain(seed_, "YTEC-TSUMUGI-RESUME-MBR-SIGNATURE-V1");
    auto digest = sha256(seed_);
    if (!digest) {
      return clonecore::Result<std::uint32_t>::failure(digest.error());
    }
    std::uint32_t value{};
    for (unsigned int index = 0U; index < 4U; ++index) {
      value |= std::to_integer<std::uint32_t>(digest.value()[index])
          << (index * 8U);
    }
    if (value == 0U ||
        std::find(disallowed_.begin(), disallowed_.end(), value) !=
            disallowed_.end()) {
      return failure<std::uint32_t>(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_DUP_NAME,
          L"Tsumugi Resume MBR signature衝突",
          L"durable operationから導出した単一MBR signatureが接続済みdiskと衝突するため、別値へ変更せず停止します");
    }
    return clonecore::Result<std::uint32_t>::success(value);
  }

 private:
  std::vector<std::byte> seed_;
  std::vector<std::uint32_t> disallowed_;
  bool used_{};
};

std::vector<std::byte> layout_seed_material(
    const TsumugiVerifiedImage& image,
    const TsumugiRestoreDiskIdentity& target,
    const TsumugiPhysicalResumeLayoutSeedV1& seed) {
  std::vector<std::byte> material;
  material.reserve(192U);
  append_domain(material, "YTEC-TSUMUGI-PHYSICAL-RESUME-LAYOUT-V1");
  append_array(material, seed.operation_id);
  append_array(material, seed.plan_hash);
  append_array(material, image.container.global_hash);
  append_array(material, image.container.header.header_hash);
  append_array(material, target.stable_identity_hash);
  append_u64(material, target.disk_size);
  append_u32(material, target.logical_sector_size);
  return material;
}

struct EngineGuard final {
  TsumugiWholeDiskRestoreLayoutTransaction* layout{};
  bool committed{};

  ~EngineGuard() {
    if (layout != nullptr && !committed) {
      layout->abort();
    }
  }
};

clonecore::Status validate_nonce_sequence(
    const TsumugiVerifiedImage& image) {
  const bool encrypted =
      (image.container.header.required_features &
       static_cast<std::uint32_t>(TsumugiRequiredFeature::encrypted)) != 0U;
  if (encrypted && all_zero(image.container.header.base_nonce)) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi Resume base Nonce",
        L"暗号化画像のbase Nonceがzeroです");
  }
  for (std::uint64_t index = 0U;
       index < image.container.records.size();
       ++index) {
    if (index == (std::numeric_limits<std::uint64_t>::max)()) {
      return status_failure(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi Resume Nonce counter",
          L"chunk indexがNonce counter上限を超えます");
    }
    const auto& record =
        image.container.records[static_cast<std::size_t>(index)];
    const std::uint64_t expected =
        encrypted && !chunk_is_zero(record.flags) ? index + 1U : 0U;
    if (record.nonce_counter != expected) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"Tsumugi Resume Nonce列",
          L"認証済みchunk indexとNonce counterが一致しないため再利用または置換として拒否します");
    }
  }
  return clonecore::success_status();
}

clonecore::Status compare_or_write_segment(
    clonecore::ITargetDiskWriter& target,
    const TsumugiPhysicalResumePayloadSegmentV1& segment,
    const TsumugiChunkRecord& record,
    const std::span<const std::byte> plaintext,
    const bool write,
    const std::size_t block_bytes) {
  const bool zero = chunk_is_zero(record.flags);
  std::uint64_t segment_end{};
  if (segment.length == 0U ||
      !checked_add(segment.target_offset, segment.length, segment_end) ||
      segment_end > target.size_bytes() ||
      segment.target_offset % target.logical_sector_size() != 0U ||
      segment.length % target.logical_sector_size() != 0U ||
      (!zero && (segment.record_plaintext_offset > plaintext.size() ||
                 segment.length >
                     plaintext.size() - segment.record_plaintext_offset))) {
    return status_failure(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi Resume payload segment",
        L"segmentの範囲、整列、またはauthenticated plaintext対応が不正です");
  }
  const std::size_t aligned_block = (std::max<std::size_t>)(
      target.logical_sector_size(),
      block_bytes - (block_bytes % target.logical_sector_size()));
  std::vector<std::byte> zeroes;
  if (zero) {
    zeroes.assign(aligned_block, std::byte{0});
  }
  std::uint64_t completed = 0U;
  while (completed < segment.length) {
    const auto amount = static_cast<std::size_t>((std::min<std::uint64_t>)(
        segment.length - completed, aligned_block));
    const auto expected = zero
        ? std::span<const std::byte>(zeroes).first(amount)
        : plaintext.subspan(
              static_cast<std::size_t>(
                  segment.record_plaintext_offset + completed),
              amount);
    if (write) {
      auto status = target.write_target(
          segment.target_offset + completed, expected);
      if (!status) {
        return status;
      }
      status = target.flush_target();
      if (!status) {
        return status;
      }
    }
    auto observed = target.read_back(
        segment.target_offset + completed, amount);
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    if (observed.value().size() != amount ||
        !std::equal(
            expected.begin(), expected.end(), observed.value().begin())) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          write ? L"Tsumugi Resume suffix読戻し"
                : L"Tsumugi Resume prefix再照合",
          write
              ? L"flush後の同一target handle読戻しがauthenticated plaintextと一致しません"
              : L"checkpoint済みtarget prefixがauthenticated image由来の期待値と一致しません");
    }
    completed += amount;
  }
  return clonecore::success_status();
}

}  // namespace

clonecore::Result<std::vector<
    TsumugiPhysicalResumePreparationSectorV1>>
capture_tsumugi_physical_resume_preparation_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    clonecore::ITargetDiskWriter& target) {
  auto expected_count = validate_preparation_layout_shape(layout, target);
  if (!expected_count) {
    return clonecore::Result<std::vector<
        TsumugiPhysicalResumePreparationSectorV1>>::failure(
        expected_count.error());
  }
  std::vector<TsumugiPhysicalResumePreparationSectorV1> evidence;
  evidence.reserve(static_cast<std::size_t>(expected_count.value()));
  for (const auto& range : layout.invalidation_ranges) {
    for (std::uint64_t offset = range.offset;
         offset < range.offset + range.length;
         offset += layout.logical_sector_size) {
      auto observed = target.read_back(offset, layout.logical_sector_size);
      if (!observed) {
        return clonecore::Result<std::vector<
            TsumugiPhysicalResumePreparationSectorV1>>::failure(
            observed.error());
      }
      if (observed.value().size() != layout.logical_sector_size) {
        return failure<std::vector<
            TsumugiPhysicalResumePreparationSectorV1>>(
            clonecore::ErrorCode::io_failed,
            ERROR_HANDLE_EOF,
            L"Tsumugi Resume preparation capture",
            L"opened targetからのsector読取り長が一致しません");
      }
      auto original_hash = sha256(observed.value());
      if (!original_hash) {
        return clonecore::Result<std::vector<
            TsumugiPhysicalResumePreparationSectorV1>>::failure(
            original_hash.error());
      }
      evidence.push_back({
          .offset = offset,
          .length = layout.logical_sector_size,
          .original_hash = original_hash.take_value(),
      });
    }
  }
  return clonecore::Result<std::vector<
      TsumugiPhysicalResumePreparationSectorV1>>::success(
      std::move(evidence));
}

clonecore::Result<TsumugiPhysicalResumePreparationInspectionV1>
inspect_tsumugi_physical_resume_preparation_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    const std::span<const TsumugiPhysicalResumePreparationSectorV1>
        evidence,
    clonecore::ITargetDiskWriter& target) {
  return inspect_preparation_impl(layout, evidence, target);
}

clonecore::Status
verify_tsumugi_physical_resume_nonpublication_zero_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    const std::span<const TsumugiPhysicalResumePreparationSectorV1>
        evidence,
    clonecore::ITargetDiskWriter& target) {
  auto expected_count = validate_preparation_layout_shape(layout, target);
  if (!expected_count) {
    return clonecore::Status::failure(expected_count.error());
  }
  if (evidence.size() != expected_count.value()) {
    return status_failure(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi Resume nonpublication evidence",
        L"checkpoint sector証跡が無効化range全体を覆っていません");
  }
  std::vector<clonecore::ByteRange> publication_ranges;
  publication_ranges.reserve(
      layout.staged_writes.size() + layout.commit_writes.size());
  const auto append_write = [&](const TsumugiRestoreLayoutWrite& write) {
    publication_ranges.push_back({
        .offset = write.offset,
        .length = static_cast<std::uint64_t>(write.bytes.size()),
    });
  };
  for (const auto& write : layout.staged_writes) {
    append_write(write);
  }
  for (const auto& write : layout.commit_writes) {
    append_write(write);
  }
  std::size_t index{};
  for (const auto& invalidation : layout.invalidation_ranges) {
    for (std::uint64_t offset = invalidation.offset;
         offset < invalidation.offset + invalidation.length;
         offset += layout.logical_sector_size, ++index) {
      const auto& sector = evidence[index];
      if (sector.offset != offset ||
          sector.length != layout.logical_sector_size ||
          all_zero(sector.original_hash)) {
        return status_failure(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Tsumugi Resume nonpublication evidence shape",
            L"sector証跡のoffset、length、順序、またはdigestが不正です");
      }
      const std::uint64_t sector_end = offset + sector.length;
      const bool publication_sector = std::any_of(
          publication_ranges.begin(),
          publication_ranges.end(),
          [&](const clonecore::ByteRange& range) {
            return offset < range.offset + range.length &&
                range.offset < sector_end;
          });
      if (publication_sector) {
        continue;
      }
      auto observed = target.read_back(offset, sector.length);
      if (!observed) {
        return clonecore::Status::failure(observed.error());
      }
      if (observed.value().size() != sector.length ||
          !bytes_all_zero(observed.value())) {
        return status_failure(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"Tsumugi Resume nonpublication zero",
            L"commit-ready checkpoint外の無効化metadata sectorがzeroではありません");
      }
    }
  }
  return clonecore::success_status();
}

clonecore::Status prepare_tsumugi_physical_resume_layout_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    const std::span<const TsumugiPhysicalResumePreparationSectorV1>
        evidence,
    clonecore::ITargetDiskWriter& target,
    const clonecore::DiskOperationCallbacks& callbacks) {
  const auto inspected = inspect_preparation_impl(layout, evidence, target);
  if (!inspected) {
    return clonecore::Status::failure(inspected.error());
  }
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::invalidating_target,
          .cancellation_allowed = true,
      });
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return status_failure(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"Tsumugi Resume persistent preparation",
        L"最初のtarget write前に取り消しました");
  }
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = clonecore::DiskOperationStage::invalidating_target,
          .cancellation_allowed = false,
      });
  std::vector<std::byte> zeroes(
      layout.logical_sector_size, std::byte{0});
  for (const auto& item : evidence) {
    auto observed = target.read_back(item.offset, item.length);
    if (!observed) {
      return clonecore::Status::failure(observed.error());
    }
    if (observed.value().size() != item.length) {
      return status_failure(
          clonecore::ErrorCode::io_failed,
          ERROR_HANDLE_EOF,
          L"Tsumugi Resume preparation recheck",
          L"zero化直前のsector読戻し長が一致しません");
    }
    if (bytes_all_zero(observed.value())) {
      continue;
    }
    auto current_hash = sha256(observed.value());
    if (!current_hash || current_hash.value() != item.original_hash) {
      return current_hash
          ? status_failure(
                clonecore::ErrorCode::verification_failed,
                ERROR_CRC,
                L"Tsumugi Resume preparation recheck",
                L"zero化直前にsectorが元digest/zero以外へ変化しました")
          : clonecore::Status::failure(current_hash.error());
    }
    auto status = target.write_target(item.offset, zeroes);
    if (!status) {
      return status;
    }
    status = target.flush_target();
    if (!status) {
      return status;
    }
    auto readback = target.read_back(item.offset, item.length);
    if (!readback) {
      return clonecore::Status::failure(readback.error());
    }
    if (readback.value().size() != item.length ||
        !bytes_all_zero(readback.value())) {
      return status_failure(
          clonecore::ErrorCode::verification_failed,
          ERROR_CRC,
          L"Tsumugi Resume preparation readback",
          L"flush後の同一target handleからsector zeroを証明できません");
    }
  }
  const auto final_state = inspect_preparation_impl(layout, evidence, target);
  if (!final_state || final_state.value().state !=
          TsumugiPhysicalResumePreparationStateV1::all_zero) {
    return final_state
        ? status_failure(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"Tsumugi Resume preparation completion",
              L"payload許可前に全metadata sectorのzero化を証明できません")
        : clonecore::Status::failure(final_state.error());
  }
  return clonecore::success_status();
}

clonecore::Result<std::vector<TsumugiPhysicalResumePayloadSegmentV1>>
make_tsumugi_physical_resume_payload_segments_v1(
    const std::span<const TsumugiChunkRecord> records,
    const TsumugiWholeDiskRestoreLayoutPlan& layout) {
  if (layout.target_size_bytes == 0U || layout.logical_sector_size == 0U ||
      records.empty() || records.size() > kTsumugiMaximumChunkCount) {
    return failure<std::vector<TsumugiPhysicalResumePayloadSegmentV1>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi Resume payload mapping",
        L"target寸法、sector、またはrecord件数が不正です");
  }
  std::vector<clonecore::ByteRange> protected_ranges;
  protected_ranges.reserve(
      layout.staged_writes.size() + layout.commit_writes.size());
  const auto append_write = [&](const TsumugiRestoreLayoutWrite& write) {
    protected_ranges.push_back({
        .offset = write.offset,
        .length = static_cast<std::uint64_t>(write.bytes.size()),
    });
  };
  for (const auto& write : layout.staged_writes) {
    append_write(write);
  }
  for (const auto& write : layout.commit_writes) {
    append_write(write);
  }
  if (protected_ranges.empty()) {
    return failure<std::vector<TsumugiPhysicalResumePayloadSegmentV1>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi Resume protected metadata",
        L"延期するGPT/MBR metadata範囲がありません");
  }
  std::sort(
      protected_ranges.begin(),
      protected_ranges.end(),
      [](const auto& left, const auto& right) {
        return left.offset < right.offset;
      });
  std::vector<clonecore::ByteRange> merged;
  for (const auto& range : protected_ranges) {
    std::uint64_t end{};
    if (range.length == 0U ||
        range.offset % layout.logical_sector_size != 0U ||
        range.length % layout.logical_sector_size != 0U ||
        !checked_add(range.offset, range.length, end) ||
        end > layout.target_size_bytes) {
      return failure<std::vector<TsumugiPhysicalResumePayloadSegmentV1>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi Resume protected metadata範囲",
          L"延期metadataが空、未整列、overflow、またはtarget外です");
    }
    if (merged.empty() ||
        merged.back().offset + merged.back().length < range.offset) {
      merged.push_back(range);
    } else {
      const auto previous_end =
          merged.back().offset + merged.back().length;
      merged.back().length = (std::max)(previous_end, end) -
          merged.back().offset;
    }
  }

  std::vector<TsumugiPhysicalResumePayloadSegmentV1> segments;
  std::uint64_t previous_record_end{};
  bool have_previous{};
  for (std::uint64_t record_index = 0U;
       record_index < records.size();
       ++record_index) {
    const auto& record = records[static_cast<std::size_t>(record_index)];
    std::uint64_t record_end{};
    if (record.logical_length == 0U ||
        record.logical_offset % layout.logical_sector_size != 0U ||
        record.logical_length % layout.logical_sector_size != 0U ||
        !checked_add(
            record.logical_offset, record.logical_length, record_end) ||
        record_end > layout.target_size_bytes ||
        (have_previous && record.logical_offset < previous_record_end)) {
      return failure<std::vector<TsumugiPhysicalResumePayloadSegmentV1>>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi Resume record範囲",
          L"recordが空、未整列、重複、overflow、またはtarget外です");
    }
    have_previous = true;
    previous_record_end = record_end;
    std::uint64_t cursor = record.logical_offset;
    const auto append_segment = [&](const std::uint64_t first,
                                    const std::uint64_t last) {
      if (last <= first) {
        return;
      }
      segments.push_back({
          .record_index = record_index,
          .record_plaintext_offset = first - record.logical_offset,
          .target_offset = first,
          .length = last - first,
      });
    };
    for (const auto& protected_range : merged) {
      const auto protected_end =
          protected_range.offset + protected_range.length;
      if (protected_end <= cursor || protected_range.offset >= record_end) {
        continue;
      }
      append_segment(cursor, (std::min)(protected_range.offset, record_end));
      cursor = (std::max)(cursor, (std::min)(protected_end, record_end));
      if (cursor == record_end) {
        break;
      }
    }
    append_segment(cursor, record_end);
  }
  return clonecore::Result<
      std::vector<TsumugiPhysicalResumePayloadSegmentV1>>::success(
      std::move(segments));
}

clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>
make_tsumugi_physical_resume_layout_plan_v1(
    const TsumugiVerifiedImage& image,
    const TsumugiRestoreDiskIdentity& target,
    const TsumugiPhysicalResumeLayoutSeedV1& seed,
    const std::span<const std::uint32_t> disallowed_mbr_signatures) {
  if ((image.manifest.mode != TsumugiManifestMode::exact &&
       image.manifest.mode != TsumugiManifestMode::rescue) ||
      target.logical_sector_size != 512U ||
      target.disk_size < image.manifest.source_disk_size ||
      target.logical_sector_size != image.manifest.logical_sector_size ||
      all_zero(target.stable_identity_hash) || all_zero(seed.operation_id) ||
      all_zero(seed.plan_hash)) {
    return failure<TsumugiWholeDiskRestoreLayoutPlan>(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"Tsumugi physical Resume layout",
        L"exact/rescue whole-disk、512-byte logical sector、安定target、およびdurable operation seedだけを受け付けます");
  }
  auto material = layout_seed_material(image, target, seed);
  DeterministicGuidGenerator guid_generator(material);
  DeterministicMbrSignatureGenerator signature_generator(
      std::move(material), disallowed_mbr_signatures);
  return make_tsumugi_whole_disk_restore_layout_plan_v1(
      image.manifest,
      target.disk_size,
      target.logical_sector_size,
      guid_generator,
      signature_generator,
      disallowed_mbr_signatures);
}

clonecore::Status verify_tsumugi_physical_resume_layout_withheld_v1(
    const TsumugiWholeDiskRestoreLayoutPlan& layout,
    clonecore::ITargetDiskWriter& target,
    const std::size_t verification_block_bytes) {
  TsumugiWholeDiskRestoreLayoutTransaction transaction(layout, target);
  const auto resumed = transaction.resume_prepared(
      verification_block_bytes);
  if (!resumed) {
    return clonecore::Status::failure(resumed.error());
  }
  // This is an evidence-only probe.  Let the temporary transaction go out of
  // scope without abort(): abort() deliberately flushes a prepared transfer,
  // while this helper promises target reads only.
  return clonecore::success_status();
}

clonecore::Result<TsumugiPhysicalResumeEngineReportV1>
execute_tsumugi_physical_whole_disk_resume_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    const std::span<const std::uint32_t> disallowed_mbr_signatures,
    const TsumugiPhysicalResumeCursorV1& cursor,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const TsumugiPhysicalResumePhaseCommitV1& preparation_commit,
    const TsumugiPhysicalResumeCheckpointCommitV1& checkpoint_commit,
    const TsumugiPhysicalResumePhaseCommitV1& commit_ready_commit,
    const TsumugiPhysicalResumeEngineDependenciesV1& dependencies,
    const clonecore::DiskOperationCallbacks& callbacks) {
  const bool preparing = cursor.durable_phase ==
      TsumugiPhysicalResumeCursorV1::DurablePhase::preparing;
  const bool prepared = cursor.durable_phase ==
      TsumugiPhysicalResumeCursorV1::DurablePhase::prepared;
  const bool commit_ready = cursor.durable_phase ==
      TsumugiPhysicalResumeCursorV1::DurablePhase::commit_ready;
  if (!target || !revalidate_locked_target || !checkpoint_commit ||
      !preparation_commit || !commit_ready_commit ||
      !dependencies.read_verified_image ||
      target->size_bytes() != target_identity.disk_size ||
      target_identity.logical_sector_size != 512U ||
      target->logical_sector_size() != target_identity.logical_sector_size ||
      (!preparing && !prepared && !commit_ready) ||
      cursor.expected_segment_count != cursor.segments.size() ||
      cursor.verified_segment_count > cursor.expected_segment_count ||
      cursor.verified_payload_bytes > cursor.expected_payload_bytes ||
      (preparing && (cursor.verified_segment_count != 0U ||
                     cursor.verified_payload_bytes != 0U)) ||
      (commit_ready &&
       (cursor.verified_segment_count != cursor.expected_segment_count ||
        cursor.verified_payload_bytes != cursor.expected_payload_bytes))) {
    return failure<TsumugiPhysicalResumeEngineReportV1>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"Tsumugi physical Resume engine開始",
        L"target、strict reader、durable phase/cursor、またはcheckpoint callbackが不正です");
  }
  const auto target_boundary = validate_resume_target_identity(
      target_identity);
  if (!target_boundary) {
    return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
        target_boundary.error());
  }
  const auto mode_and_loss =
      validate_whole_disk_mode_and_loss_evidence(initially_verified);
  if (!mode_and_loss) {
    return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
        mode_and_loss.error());
  }
  const auto nonce_status = validate_nonce_sequence(initially_verified);
  if (!nonce_status) {
    return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
        nonce_status.error());
  }
  auto layout = make_tsumugi_physical_resume_layout_plan_v1(
      initially_verified,
      target_identity,
      cursor.layout_seed,
      disallowed_mbr_signatures);
  if (!layout) {
    return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
        layout.error());
  }
  auto expected_segments = make_tsumugi_physical_resume_payload_segments_v1(
      initially_verified.container.records, layout.value());
  if (!expected_segments || expected_segments.value() != cursor.segments) {
    return failure<TsumugiPhysicalResumeEngineReportV1>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Tsumugi Resume authenticated segment mapping",
        L"checkpointで認証したpayload境界が現在のcontainerと決定的layoutから再構成した境界に一致しません");
  }
  std::uint64_t expected_bytes{};
  std::uint64_t prefix_bytes{};
  for (std::uint64_t index = 0U; index < cursor.segments.size(); ++index) {
    if (!checked_add(
            expected_bytes,
            cursor.segments[static_cast<std::size_t>(index)].length,
            expected_bytes) ||
        (index < cursor.verified_segment_count &&
         !checked_add(
             prefix_bytes,
             cursor.segments[static_cast<std::size_t>(index)].length,
             prefix_bytes))) {
      return failure<TsumugiPhysicalResumeEngineReportV1>(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"Tsumugi Resume segment合計",
          L"payload segment合計がoverflowします");
    }
  }
  if (expected_bytes != cursor.expected_payload_bytes ||
      prefix_bytes != cursor.verified_payload_bytes || expected_bytes == 0U) {
    return failure<TsumugiPhysicalResumeEngineReportV1>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Tsumugi Resume cursor合計",
        L"checkpoint bytes/countが認証済みpayload segment prefixと一致しません");
  }

  auto deterministic_layout = layout.take_value();
  TsumugiWholeDiskRestoreLayoutTransaction layout_transaction(
      deterministic_layout, *target);
  EngineGuard guard{.layout = &layout_transaction};
  bool gate_passed{};
  bool target_reidentified{};
  std::optional<TsumugiRestoreLayoutPublicationInspectionV1>
      publication_at_gate;
  std::uint64_t next_segment{};
  std::uint64_t next_record{};
  std::uint64_t final_bytes = cursor.verified_payload_bytes;
  const std::size_t block_bytes = (std::max<std::size_t>)(
      target_identity.logical_sector_size,
      (std::min<std::size_t>)(
          image_request.verification_block_bytes,
          32U * 1024U * 1024U));

  auto restored = dependencies.read_verified_image(
      TsumugiStreamVerifyRequest{
          .image_path = image_request.image_path,
          .password = image_request.password,
          .verification_block_bytes =
              image_request.verification_block_bytes,
      },
      [&](const TsumugiChunkRecord& record,
          const std::span<const std::byte> plaintext) {
        if (!gate_passed ||
            next_record >= initially_verified.container.records.size() ||
            next_segment > cursor.segments.size() ||
            !same_record(
                initially_verified.container.records[
                    static_cast<std::size_t>(next_record)],
                record)) {
          return status_failure(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_STATE,
              L"Tsumugi Resume restore pass順序",
              L"完全検証gate前、record順序外、置換record、またはcursor外でpayload callbackが呼ばれました");
        }
        const std::uint64_t record_index = next_record++;
        while (next_segment < cursor.segments.size() &&
               cursor.segments[static_cast<std::size_t>(next_segment)]
                       .record_index == record_index) {
          const auto& segment =
              cursor.segments[static_cast<std::size_t>(next_segment)];
          const bool prefix = next_segment < cursor.verified_segment_count;
          const auto status = compare_or_write_segment(
              *target,
              segment,
              record,
              plaintext,
              !prefix,
              block_bytes);
          if (!status) {
            return status;
          }
          if (!prefix) {
            const auto committed = checkpoint_commit(next_segment, segment);
            if (!committed) {
              return committed;
            }
            if (!checked_add(final_bytes, segment.length, final_bytes)) {
              return status_failure(
                  clonecore::ErrorCode::invalid_data,
                  ERROR_ARITHMETIC_OVERFLOW,
                  L"Tsumugi Resume verified payload",
                  L"readback済みpayload合計がoverflowします");
            }
          }
          ++next_segment;
        }
        return clonecore::success_status();
      },
      callbacks,
      [&](const TsumugiStreamInspection& inspection) {
        if (!same_inspection(inspection, initially_verified.container)) {
          return status_failure(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_FILE_INVALID,
              L"Tsumugi Resume image handle再照合",
              L"完全検証passとrestore passのopened file identity、size、time、container、または認証済みrecordが一致しません");
        }
        auto current = revalidate_locked_target();
        if (!current || !same_identity(target_identity, current.value())) {
          return current
              ? status_failure(
                    clonecore::ErrorCode::identity_mismatch,
                    ERROR_DEVICE_REINITIALIZATION_NEEDED,
                    L"Tsumugi Resume target再識別",
                    L"書込み前のtarget identity、connection、寸法、sector、または属性が変化しました")
              : clonecore::Status::failure(current.error());
        }
        const auto current_boundary = validate_resume_target_identity(
            current.value());
        if (!current_boundary) {
          return current_boundary;
        }
        if (preparing) {
          if (cursor.preparation_sectors.empty()) {
            // Legacy v1 checkpoint: evidence collection has already proved
            // that the complete reviewed initial layout is unchanged.
            const auto invalidated = layout_transaction.prepare(callbacks);
            if (!invalidated) {
              return clonecore::Status::failure(invalidated.error());
            }
          } else {
            const auto invalidated =
                prepare_tsumugi_physical_resume_layout_v1(
                    deterministic_layout,
                    cursor.preparation_sectors,
                    *target,
                    callbacks);
            if (!invalidated) {
              return invalidated;
            }
            const auto reopened =
                layout_transaction.resume_prepared(block_bytes);
            if (!reopened) {
              return clonecore::Status::failure(reopened.error());
            }
          }
          const auto phase_committed = preparation_commit();
          if (!phase_committed) {
            return phase_committed;
          }
        } else if (prepared) {
          const auto reopened = layout_transaction.resume_prepared(
              block_bytes);
          if (!reopened) {
            return clonecore::Status::failure(reopened.error());
          }
        } else {
          const auto nonpublication_zero =
              verify_tsumugi_physical_resume_nonpublication_zero_v1(
                  deterministic_layout,
                  cursor.preparation_sectors,
                  *target);
          if (!nonpublication_zero) {
            return nonpublication_zero;
          }
          auto inspected =
              inspect_tsumugi_whole_disk_restore_layout_publication_v1(
                  deterministic_layout, *target, block_bytes);
          if (!inspected) {
            return clonecore::Status::failure(inspected.error());
          }
          publication_at_gate = inspected.take_value();
          if (publication_at_gate->state ==
              TsumugiRestoreLayoutPublicationStateV1::all_zero) {
            const auto reopened = layout_transaction.resume_prepared(
                block_bytes);
            if (!reopened) {
              return clonecore::Status::failure(reopened.error());
            }
          }
        }
        target_reidentified = true;
        gate_passed = true;
        return clonecore::success_status();
      });
  if (!restored) {
    return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
        restored.error());
  }
  if (!gate_passed || !target_reidentified ||
      !restored.value().callbacks_started_after_complete_verification ||
      !same_inspection(
          restored.value().inspection, initially_verified.container) ||
      next_record != initially_verified.container.records.size() ||
      next_segment != cursor.expected_segment_count ||
      final_bytes != cursor.expected_payload_bytes) {
    return failure<TsumugiPhysicalResumeEngineReportV1>(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        L"Tsumugi Resume payload完了証跡",
        L"完全image再検証、target再識別、segment prefix/suffix、または合計証跡が不足しています");
  }
  if (!commit_ready) {
    const auto durable = commit_ready_commit();
    if (!durable) {
      return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
          durable.error());
    }
  }
  auto final_target = revalidate_locked_target();
  if (!final_target || !same_identity(target_identity, final_target.value())) {
    return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
        final_target
            ? resume_error(
                  clonecore::ErrorCode::identity_mismatch,
                  ERROR_DEVICE_REINITIALIZATION_NEEDED,
                  L"Tsumugi Resume commit前target再識別",
                  L"layout commit直前にtarget identityまたはconnectionが変化しました")
            : final_target.error());
  }
  const auto final_boundary = validate_resume_target_identity(
      final_target.value());
  if (!final_boundary) {
    return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
        final_boundary.error());
  }

  if (commit_ready) {
    const auto nonpublication_zero =
        verify_tsumugi_physical_resume_nonpublication_zero_v1(
            deterministic_layout,
            cursor.preparation_sectors,
            *target);
    if (!nonpublication_zero) {
      return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
          nonpublication_zero.error());
    }
    auto current_publication =
        inspect_tsumugi_whole_disk_restore_layout_publication_v1(
            deterministic_layout, *target, block_bytes);
    if (!current_publication || !publication_at_gate ||
        current_publication.value().state != publication_at_gate->state ||
        current_publication.value().published_write_count !=
            publication_at_gate->published_write_count ||
        current_publication.value().total_write_count !=
            publication_at_gate->total_write_count) {
      return current_publication
          ? failure<TsumugiPhysicalResumeEngineReportV1>(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_DEVICE_REINITIALIZATION_NEEDED,
                L"Tsumugi Resume publication reinspection",
                L"payload prefix再照合中にmetadata publication状態が変化しました")
          : clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
                current_publication.error());
    }
    if (current_publication.value().state ==
        TsumugiRestoreLayoutPublicationStateV1::all_final) {
      guard.committed = true;
      return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::success({
          .resumed_verified_payload_bytes = cursor.verified_payload_bytes,
          .resumed_verified_segment_count = cursor.verified_segment_count,
          .final_verified_payload_bytes = final_bytes,
          .final_verified_segment_count = next_segment,
          .full_image_reverified_on_same_handle_before_first_write = true,
          .target_and_incomplete_layout_reidentified_before_first_write = true,
          .verified_prefix_was_not_rewritten = true,
          .every_new_segment_flushed_and_read_back = true,
          .final_layout_committed = true,
      });
    }
    if (current_publication.value().state ==
        TsumugiRestoreLayoutPublicationStateV1::known_write_prefix) {
      const auto reinvalidated =
          layout_transaction.reinvalidate_publication_prefix(block_bytes);
      if (!reinvalidated) {
        return clonecore::Result<
            TsumugiPhysicalResumeEngineReportV1>::failure(
            reinvalidated.error());
      }
    }
  }
  const auto committed = layout_transaction.commit(callbacks);
  if (!committed) {
    return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
        committed.error());
  }
  auto final_publication =
      inspect_tsumugi_whole_disk_restore_layout_publication_v1(
          deterministic_layout, *target, block_bytes);
  if (!final_publication || final_publication.value().state !=
          TsumugiRestoreLayoutPublicationStateV1::all_final) {
    return final_publication
        ? failure<TsumugiPhysicalResumeEngineReportV1>(
              clonecore::ErrorCode::verification_failed,
              ERROR_CRC,
              L"Tsumugi Resume final layout readback",
              L"commit完了後にoperation期待metadata全体を再照合できません")
        : clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::failure(
              final_publication.error());
  }
  guard.committed = true;
  return clonecore::Result<TsumugiPhysicalResumeEngineReportV1>::success({
      .resumed_verified_payload_bytes = cursor.verified_payload_bytes,
      .resumed_verified_segment_count = cursor.verified_segment_count,
      .final_verified_payload_bytes = final_bytes,
      .final_verified_segment_count = next_segment,
      .full_image_reverified_on_same_handle_before_first_write = true,
      .target_and_incomplete_layout_reidentified_before_first_write = true,
      .verified_prefix_was_not_rewritten = true,
      .every_new_segment_flushed_and_read_back = true,
      .final_layout_committed = true,
  });
}

clonecore::Result<TsumugiPhysicalResumeEngineReportV1>
execute_tsumugi_physical_whole_disk_resume_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    const std::span<const std::uint32_t> disallowed_mbr_signatures,
    const TsumugiPhysicalResumeCursorV1& cursor,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const TsumugiPhysicalResumePhaseCommitV1& preparation_commit,
    const TsumugiPhysicalResumeCheckpointCommitV1& checkpoint_commit,
    const TsumugiPhysicalResumePhaseCommitV1& commit_ready_commit,
    const clonecore::DiskOperationCallbacks& callbacks) {
  return execute_tsumugi_physical_whole_disk_resume_engine_v1(
      image_request,
      initially_verified,
      target_identity,
      std::move(target),
      disallowed_mbr_signatures,
      cursor,
      revalidate_locked_target,
      preparation_commit,
      checkpoint_commit,
      commit_ready_commit,
      TsumugiPhysicalResumeEngineDependenciesV1{
          .read_verified_image = read_verified_tsumugi_file_v1,
      },
      callbacks);
}

}  // namespace ytec::imageformat

#include "ytec/imageformat/tsumugi_manifest.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'Y'}, std::byte{'T'}, std::byte{'M'}, std::byte{'A'},
    std::byte{'N'}, std::byte{'1'}, std::byte{0x0D}, std::byte{0x0A}};
constexpr std::uint32_t kKnownManifestFlags =
    static_cast<std::uint32_t>(TsumugiManifestFlags::source_contains_windows) |
    static_cast<std::uint32_t>(
        TsumugiManifestFlags::bitlocker_source_was_unlocked) |
    static_cast<std::uint32_t>(TsumugiManifestFlags::partition_selection) |
    static_cast<std::uint32_t>(
        TsumugiManifestFlags::automatic_surplus_allocation);
constexpr std::uint32_t kKnownPartitionFlags =
    static_cast<std::uint32_t>(TsumugiManifestPartitionFlags::selected) |
    static_cast<std::uint32_t>(TsumugiManifestPartitionFlags::required) |
    static_cast<std::uint32_t>(TsumugiManifestPartitionFlags::active) |
    static_cast<std::uint32_t>(
        TsumugiManifestPartitionFlags::contains_windows) |
    static_cast<std::uint32_t>(
        TsumugiManifestPartitionFlags::bitlocker_was_unlocked);
constexpr std::size_t kFixedTextBytes = 32U;
constexpr std::size_t kPartitionTextBytes = 128U;
constexpr std::size_t kMaximumManifestBytes = 16U * 1024U * 1024U;

clonecore::Error manifest_error(
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
    std::wstring message) {
  return clonecore::Result<T>::failure(manifest_error(
      code,
      native_code,
      L"Tsumugi v1マニフェスト検証",
      std::move(message)));
}

template <typename T>
T read_little(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

template <typename T>
void write_little(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) noexcept {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

bool all_zero(const std::span<const std::byte> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
    return value == std::byte{0};
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

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0U &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool flag_set(
    const TsumugiManifestFlags value,
    const TsumugiManifestFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

bool flag_set(
    const TsumugiManifestPartitionFlags value,
    const TsumugiManifestPartitionFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0U;
}

bool valid_mode(const TsumugiManifestMode mode) noexcept {
  return mode == TsumugiManifestMode::exact ||
      mode == TsumugiManifestMode::shrink ||
      mode == TsumugiManifestMode::rescue;
}

bool valid_style(const TsumugiManifestPartitionStyle style) noexcept {
  return style == TsumugiManifestPartitionStyle::mbr ||
      style == TsumugiManifestPartitionStyle::gpt;
}

bool valid_role(const TsumugiManifestPartitionRole role) noexcept {
  return role >= TsumugiManifestPartitionRole::other &&
      role <= TsumugiManifestPartitionRole::data;
}

bool valid_file_system(const TsumugiManifestFileSystem file_system) noexcept {
  return file_system >= TsumugiManifestFileSystem::unknown &&
      file_system <= TsumugiManifestFileSystem::fat32;
}

bool valid_payload_encoding(
    const TsumugiManifestPayloadEncoding encoding) noexcept {
  return encoding == TsumugiManifestPayloadEncoding::exact_raw ||
      encoding ==
          TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
}

bool file_archive_capable(
    const TsumugiManifestFileSystem file_system) noexcept {
  return file_system == TsumugiManifestFileSystem::ntfs ||
      file_system == TsumugiManifestFileSystem::exfat ||
      file_system == TsumugiManifestFileSystem::fat32;
}

bool valid_cluster_size(
    const std::uint64_t cluster_size,
    const std::uint32_t logical_sector_size) noexcept {
  return cluster_size >= logical_sector_size &&
      cluster_size <= 2ULL * 1024ULL * 1024ULL &&
      cluster_size % logical_sector_size == 0U;
}

bool valid_sector_geometry(
    const std::uint32_t logical,
    const std::uint32_t physical) noexcept {
  const auto is_power_of_two = [](const std::uint32_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
  };
  return logical >= 512U && logical <= 4096U && is_power_of_two(logical) &&
      physical >= logical && physical <= 65536U &&
      is_power_of_two(physical) && physical % logical == 0U;
}

bool valid_ascii_text(
    const std::string_view value,
    const std::size_t maximum) noexcept {
  return !value.empty() && value.size() < maximum &&
      std::all_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x20U && byte <= 0x7EU;
      });
}

bool valid_utf8_text(
    const std::string_view value,
    const std::size_t maximum) noexcept {
  if (value.size() >= maximum) {
    return false;
  }
  std::size_t index = 0U;
  while (index < value.size()) {
    const auto first = static_cast<std::uint8_t>(value[index]);
    std::uint32_t code_point{};
    std::size_t continuation_count{};
    if (first <= 0x7FU) {
      code_point = first;
    } else if (first >= 0xC2U && first <= 0xDFU) {
      code_point = first & 0x1FU;
      continuation_count = 1U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      code_point = first & 0x0FU;
      continuation_count = 2U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      code_point = first & 0x07U;
      continuation_count = 3U;
    } else {
      return false;
    }
    if (index + continuation_count >= value.size()) {
      return false;
    }
    for (std::size_t continuation = 0U;
         continuation < continuation_count;
         ++continuation) {
      const auto byte = static_cast<std::uint8_t>(
          value[index + continuation + 1U]);
      if ((byte & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (byte & 0x3FU);
    }
    const bool overlong =
        (continuation_count == 1U && code_point < 0x80U) ||
        (continuation_count == 2U && code_point < 0x800U) ||
        (continuation_count == 3U && code_point < 0x10000U);
    if (overlong || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU) ||
        code_point == 0x7FU || code_point < 0x20U) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

clonecore::Status validate_snapshot(
    const TsumugiManifest& manifest) {
  if (manifest.partition_snapshot.empty() ||
      manifest.partition_snapshot.size() > kMaximumManifestBytes) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi v1マニフェストのパーティション表",
        L"パーティション表snapshotが空または上限超過です"));
  }
  const auto snapshot = inspect_partition_snapshot_v1(
      manifest.partition_snapshot);
  if (!snapshot) {
    return clonecore::Status::failure(snapshot.error());
  }
  const auto expected_style =
      manifest.partition_style == TsumugiManifestPartitionStyle::gpt
          ? PartitionTableStyle::gpt
          : PartitionTableStyle::mbr;
  if (snapshot.value().style != expected_style ||
      snapshot.value().source_disk_size != manifest.source_disk_size ||
      snapshot.value().logical_sector_size != manifest.logical_sector_size) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"Tsumugi v1マニフェストのパーティション表",
        L"snapshotの形式、ディスク容量、または論理セクターが一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_manifest(const TsumugiManifest& manifest) {
  if (!valid_mode(manifest.mode) || !valid_style(manifest.partition_style) ||
      (static_cast<std::uint32_t>(manifest.flags) & ~kKnownManifestFlags) !=
          0U ||
      !valid_sector_geometry(
          manifest.logical_sector_size, manifest.physical_sector_size) ||
      manifest.source_disk_size == 0U ||
      manifest.source_disk_size % manifest.logical_sector_size != 0U ||
      all_zero(manifest.source_model_hash) ||
      all_zero(manifest.source_serial_hash) ||
      all_zero(manifest.source_state_hash) ||
      !valid_ascii_text(manifest.created_utc, kFixedTextBytes) ||
      manifest.created_utc.find('T') == std::string::npos ||
      !manifest.created_utc.ends_with('Z') ||
      !valid_ascii_text(manifest.app_version, kFixedTextBytes) ||
      manifest.partitions.empty() ||
      manifest.partitions.size() > kTsumugiManifestMaximumPartitions) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi v1マニフェスト共通フィールド",
        L"形式、flag、セクター、時刻、版、またはパーティション数が不正です"));
  }

  std::set<std::uint32_t> table_indexes;
  std::set<std::uint32_t> partition_numbers;
  std::uint64_t previous_end = 0U;
  std::uint64_t previous_payload_end = 0U;
  std::size_t selected_count = 0U;
  std::size_t windows_count = 0U;
  std::size_t unlocked_bitlocker_count = 0U;
  bool has_unselected_partition = false;
  for (const auto& partition : manifest.partitions) {
    const auto raw_flags = static_cast<std::uint32_t>(partition.flags);
    std::uint64_t source_end{};
    std::uint64_t payload_end{};
    const bool selected = flag_set(
        partition.flags, TsumugiManifestPartitionFlags::selected);
    const bool required = flag_set(
        partition.flags, TsumugiManifestPartitionFlags::required);
    const bool contains_windows = flag_set(
        partition.flags, TsumugiManifestPartitionFlags::contains_windows);
    if (partition.source_table_index == 0U ||
        partition.source_partition_number == 0U ||
        !valid_role(partition.role) ||
        !valid_file_system(partition.file_system) ||
        !valid_payload_encoding(partition.payload_encoding) ||
        (raw_flags & ~kKnownPartitionFlags) != 0U ||
        partition.source_size == 0U ||
        partition.source_offset % manifest.logical_sector_size != 0U ||
        partition.source_size % manifest.logical_sector_size != 0U ||
        !checked_add(
            partition.source_offset, partition.source_size, source_end) ||
        source_end > manifest.source_disk_size ||
        partition.source_offset < previous_end ||
        partition.used_bytes > partition.source_size ||
        !valid_utf8_text(partition.name_utf8, kPartitionTextBytes) ||
        !valid_utf8_text(partition.label_utf8, kPartitionTextBytes) ||
        !table_indexes.insert(partition.source_table_index).second ||
        !partition_numbers.insert(partition.source_partition_number).second ||
        (required && !selected) ||
        (contains_windows && partition.role !=
                                 TsumugiManifestPartitionRole::windows)) {
      return clonecore::Status::failure(manifest_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi v1パーティションレコード",
          L"識別、範囲、flag、文字列、役割、または並び順が不正です"));
    }
    previous_end = source_end;

    if (manifest.partition_style == TsumugiManifestPartitionStyle::mbr &&
        (!all_zero(std::span(partition.type_id).subspan(1U)) ||
         !all_zero(partition.unique_id))) {
      return clonecore::Status::failure(manifest_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi v1 MBRパーティション識別",
          L"MBR形式で使用しないtype／unique ID領域が非ゼロです"));
    }

    if (!selected) {
      has_unselected_partition = true;
      if (partition.minimum_target_bytes != 0U ||
          partition.planned_target_bytes != 0U ||
          partition.payload_logical_offset != 0U ||
          partition.payload_logical_length != 0U ||
          partition.payload_encoding !=
              TsumugiManifestPayloadEncoding::exact_raw ||
          partition.payload_format_version != 0U ||
          partition.cluster_size != 0U) {
        return clonecore::Status::failure(manifest_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_INVALID_DATA,
            L"Tsumugi v1未選択パーティション",
            L"未選択領域に対象容量またはpayload範囲があります"));
      }
      continue;
    }
    ++selected_count;
    if (contains_windows) {
      ++windows_count;
    }
    if (flag_set(
            partition.flags,
            TsumugiManifestPartitionFlags::bitlocker_was_unlocked)) {
      ++unlocked_bitlocker_count;
    }
    const bool byte_stream_archive =
        manifest.mode == TsumugiManifestMode::shrink &&
        partition.payload_encoding ==
            TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
    if (partition.minimum_target_bytes == 0U ||
        partition.planned_target_bytes < partition.minimum_target_bytes ||
        partition.payload_logical_length == 0U ||
        (!byte_stream_archive &&
         partition.payload_logical_length >
             partition.minimum_target_bytes) ||
        !checked_add(
            partition.payload_logical_offset,
            partition.payload_logical_length,
            payload_end) ||
        payload_end > manifest.source_disk_size ||
        partition.payload_logical_offset < previous_payload_end) {
      return clonecore::Status::failure(manifest_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi v1選択パーティション",
          L"必要容量、予定容量、またはpayload範囲が不正です"));
    }
    previous_payload_end = payload_end;
    if (!byte_stream_archive &&
        (partition.payload_logical_offset % manifest.logical_sector_size != 0U ||
         partition.payload_logical_length % manifest.logical_sector_size != 0U)) {
      return clonecore::Status::failure(manifest_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi v1セクターpayload境界",
          L"exact RAW payloadは論理セクター境界へ整列している必要があります"));
    }
    if (manifest.mode != TsumugiManifestMode::shrink &&
        (partition.payload_logical_offset != partition.source_offset ||
         partition.payload_logical_length != partition.source_size ||
         partition.minimum_target_bytes != partition.source_size ||
         partition.planned_target_bytes != partition.source_size ||
         partition.payload_encoding !=
             TsumugiManifestPayloadEncoding::exact_raw ||
         partition.payload_format_version != 0U ||
         partition.cluster_size != 0U)) {
      return clonecore::Status::failure(manifest_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"Tsumugi v1通常／救出payload",
          L"通常・救出モードは元領域と同じoffset／長さ／対象容量が必要です"));
    }
    if (manifest.mode == TsumugiManifestMode::shrink) {
      const bool exact_raw = partition.payload_encoding ==
          TsumugiManifestPayloadEncoding::exact_raw;
      const bool valid_raw = exact_raw &&
          partition.payload_format_version == 0U &&
          partition.cluster_size == 0U &&
          partition.payload_logical_length == partition.source_size &&
          partition.minimum_target_bytes == partition.source_size &&
          partition.planned_target_bytes >= partition.source_size;
      const bool valid_archive = !exact_raw &&
          partition.payload_encoding ==
              TsumugiManifestPayloadEncoding::microsoft_wim_single_image &&
          partition.payload_format_version ==
              kTsumugiWimPayloadFormatVersion &&
          file_archive_capable(partition.file_system) &&
          valid_cluster_size(
              partition.cluster_size, manifest.logical_sector_size) &&
          partition.minimum_target_bytes >= partition.used_bytes;
      if (!valid_raw && !valid_archive) {
        return clonecore::Status::failure(manifest_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"Tsumugi v1縮小payload契約",
            L"NTFS・exFAT・FAT32の単一WIM、または元サイズを拘束したexact RAWだけを許可します"));
      }
    }
  }

  const bool manifest_has_windows = flag_set(
      manifest.flags, TsumugiManifestFlags::source_contains_windows);
  const bool manifest_has_unlocked_bitlocker = flag_set(
      manifest.flags,
      TsumugiManifestFlags::bitlocker_source_was_unlocked);
  const bool manifest_has_partition_selection = flag_set(
      manifest.flags, TsumugiManifestFlags::partition_selection);
  if (selected_count == 0U || manifest_has_windows != (windows_count != 0U) ||
      manifest_has_unlocked_bitlocker !=
          (unlocked_bitlocker_count != 0U) ||
      manifest_has_partition_selection != has_unselected_partition ||
      (manifest.mode == TsumugiManifestMode::rescue &&
       (has_unselected_partition ||
        flag_set(
            manifest.flags,
            TsumugiManifestFlags::automatic_surplus_allocation)))) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"Tsumugi v1選択整合性",
        L"選択数、Windows／BitLocker／個別選択flag、または救出モードの指定が矛盾しています"));
  }
  return validate_snapshot(manifest);
}

void write_fixed_text(
    const std::span<std::byte> output,
    const std::size_t offset,
    const std::string_view value) {
  std::memcpy(output.data() + offset, value.data(), value.size());
}

clonecore::Result<std::string> read_fixed_text(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::size_t field_size,
    const std::size_t length,
    const bool ascii_only) {
  if (length >= field_size ||
      !all_zero(bytes.subspan(offset + length, field_size - length))) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"固定長文字列の長さまたはpaddingが不正です");
  }
  std::string value(length, '\0');
  std::memcpy(value.data(), bytes.data() + offset, length);
  const bool valid = ascii_only
      ? valid_ascii_text(value, field_size)
      : valid_utf8_text(value, field_size);
  if (!valid && !(value.empty() && !ascii_only)) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        ERROR_NO_UNICODE_TRANSLATION,
        L"固定長文字列が正規ASCII／UTF-8ではありません");
  }
  return clonecore::Result<std::string>::success(std::move(value));
}

}  // namespace

clonecore::Result<std::vector<std::byte>> build_tsumugi_manifest_v1(
    const TsumugiManifest& manifest) {
  const auto valid = validate_manifest(manifest);
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(valid.error());
  }
  std::uint64_t records_length{};
  std::uint64_t snapshot_offset{};
  std::uint64_t total_length{};
  if (!checked_multiply(
          manifest.partitions.size(),
          kTsumugiManifestPartitionRecordSize,
          records_length) ||
      !checked_add(
          kTsumugiManifestHeaderSize, records_length, snapshot_offset) ||
      !checked_add(
          snapshot_offset, manifest.partition_snapshot.size(), total_length) ||
      total_length > kMaximumManifestBytes ||
      total_length > (std::numeric_limits<std::size_t>::max)()) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"マニフェスト長が上限を超えます");
  }
  std::vector<std::byte> output(
      static_cast<std::size_t>(total_length), std::byte{0});
  const std::span<std::byte> bytes(output);
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  write_little(bytes, 8U, kTsumugiManifestMajorVersion);
  write_little(bytes, 10U, kTsumugiManifestMinorVersion);
  write_little(bytes, 12U, kTsumugiManifestHeaderSize);
  write_little(bytes, 16U, kTsumugiManifestPartitionRecordSize);
  write_little<std::uint32_t>(bytes, 20U, 0U);
  write_little(bytes, 24U, static_cast<std::uint32_t>(manifest.flags));
  write_little(bytes, 28U, static_cast<std::uint16_t>(manifest.mode));
  write_little(
      bytes, 30U, static_cast<std::uint16_t>(manifest.partition_style));
  write_little(bytes, 32U, manifest.logical_sector_size);
  write_little(bytes, 36U, manifest.physical_sector_size);
  write_little(
      bytes, 40U, static_cast<std::uint32_t>(manifest.partitions.size()));
  write_little<std::uint32_t>(bytes, 44U, 0U);
  write_little(bytes, 48U, manifest.source_disk_size);
  write_little<std::uint64_t>(bytes, 56U, kTsumugiManifestHeaderSize);
  write_little(bytes, 64U, records_length);
  write_little(bytes, 72U, snapshot_offset);
  write_little<std::uint64_t>(
      bytes, 80U, manifest.partition_snapshot.size());
  write_little(
      bytes, 88U, static_cast<std::uint16_t>(manifest.created_utc.size()));
  write_little(
      bytes, 90U, static_cast<std::uint16_t>(manifest.app_version.size()));
  std::copy(
      manifest.source_model_hash.begin(),
      manifest.source_model_hash.end(),
      bytes.begin() + 96U);
  std::copy(
      manifest.source_serial_hash.begin(),
      manifest.source_serial_hash.end(),
      bytes.begin() + 128U);
  std::copy(
      manifest.source_state_hash.begin(),
      manifest.source_state_hash.end(),
      bytes.begin() + 224U);
  write_fixed_text(bytes, 160U, manifest.created_utc);
  write_fixed_text(bytes, 192U, manifest.app_version);

  for (std::size_t index = 0U; index < manifest.partitions.size(); ++index) {
    const auto& partition = manifest.partitions[index];
    const std::size_t offset = kTsumugiManifestHeaderSize +
        index * kTsumugiManifestPartitionRecordSize;
    const auto record = bytes.subspan(
        offset, kTsumugiManifestPartitionRecordSize);
    write_little(record, 0U, partition.source_table_index);
    write_little(record, 4U, partition.source_partition_number);
    write_little(record, 8U, static_cast<std::uint16_t>(partition.role));
    write_little(
        record, 10U, static_cast<std::uint16_t>(partition.file_system));
    write_little(record, 12U, static_cast<std::uint32_t>(partition.flags));
    write_little(record, 16U, partition.source_offset);
    write_little(record, 24U, partition.source_size);
    write_little(record, 32U, partition.used_bytes);
    write_little(record, 40U, partition.minimum_target_bytes);
    write_little(record, 48U, partition.planned_target_bytes);
    write_little(record, 56U, partition.payload_logical_offset);
    write_little(record, 64U, partition.payload_logical_length);
    std::copy(partition.type_id.begin(), partition.type_id.end(), record.begin() + 72U);
    std::copy(
        partition.unique_id.begin(), partition.unique_id.end(), record.begin() + 88U);
    write_little(
        record, 104U, static_cast<std::uint16_t>(partition.name_utf8.size()));
    write_little(
        record, 106U, static_cast<std::uint16_t>(partition.label_utf8.size()));
    write_little(
        record, 108U, static_cast<std::uint16_t>(partition.payload_encoding));
    write_little(record, 110U, partition.payload_format_version);
    write_fixed_text(record, 112U, partition.name_utf8);
    write_fixed_text(record, 240U, partition.label_utf8);
    write_little(record, 368U, partition.cluster_size);
  }
  std::copy(
      manifest.partition_snapshot.begin(),
      manifest.partition_snapshot.end(),
      bytes.begin() + static_cast<std::size_t>(snapshot_offset));
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(output));
}

clonecore::Result<TsumugiManifest> inspect_tsumugi_manifest_v1(
    const std::span<const std::byte> bytes) {
  if (bytes.size() < kTsumugiManifestHeaderSize ||
      bytes.size() > kMaximumManifestBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
      read_little<std::uint16_t>(bytes, 8U) !=
          kTsumugiManifestMajorVersion ||
      read_little<std::uint16_t>(bytes, 10U) !=
          kTsumugiManifestMinorVersion ||
      read_little<std::uint32_t>(bytes, 12U) !=
          kTsumugiManifestHeaderSize ||
      read_little<std::uint32_t>(bytes, 16U) !=
          kTsumugiManifestPartitionRecordSize ||
      read_little<std::uint32_t>(bytes, 20U) != 0U ||
      read_little<std::uint32_t>(bytes, 44U) != 0U ||
      !all_zero(bytes.subspan(92U, 4U))) {
    return failure<TsumugiManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"固定ヘッダー、版、必須機能、または予約領域が不正です");
  }
  const auto partition_count = read_little<std::uint32_t>(bytes, 40U);
  const auto records_offset = read_little<std::uint64_t>(bytes, 56U);
  const auto records_length = read_little<std::uint64_t>(bytes, 64U);
  const auto snapshot_offset = read_little<std::uint64_t>(bytes, 72U);
  const auto snapshot_length = read_little<std::uint64_t>(bytes, 80U);
  std::uint64_t expected_records_length{};
  std::uint64_t expected_snapshot_offset{};
  std::uint64_t expected_total{};
  if (partition_count == 0U ||
      partition_count > kTsumugiManifestMaximumPartitions ||
      !checked_multiply(
          partition_count,
          kTsumugiManifestPartitionRecordSize,
          expected_records_length) ||
      !checked_add(
          kTsumugiManifestHeaderSize,
          expected_records_length,
          expected_snapshot_offset) ||
      !checked_add(snapshot_offset, snapshot_length, expected_total) ||
      records_offset != kTsumugiManifestHeaderSize ||
      records_length != expected_records_length ||
      snapshot_offset != expected_snapshot_offset ||
      snapshot_length == 0U || expected_total != bytes.size()) {
    return failure<TsumugiManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"レコードまたはsnapshotの範囲が非正規です");
  }

  const auto created = read_fixed_text(
      bytes,
      160U,
      kFixedTextBytes,
      read_little<std::uint16_t>(bytes, 88U),
      true);
  const auto app_version = read_fixed_text(
      bytes,
      192U,
      kFixedTextBytes,
      read_little<std::uint16_t>(bytes, 90U),
      true);
  if (!created || !app_version) {
    return clonecore::Result<TsumugiManifest>::failure(
        created ? app_version.error() : created.error());
  }

  TsumugiManifest manifest{
      .mode = static_cast<TsumugiManifestMode>(
          read_little<std::uint16_t>(bytes, 28U)),
      .partition_style = static_cast<TsumugiManifestPartitionStyle>(
          read_little<std::uint16_t>(bytes, 30U)),
      .flags = static_cast<TsumugiManifestFlags>(
          read_little<std::uint32_t>(bytes, 24U)),
      .source_disk_size = read_little<std::uint64_t>(bytes, 48U),
      .logical_sector_size = read_little<std::uint32_t>(bytes, 32U),
      .physical_sector_size = read_little<std::uint32_t>(bytes, 36U),
      .created_utc = created.value(),
      .app_version = app_version.value(),
  };
  std::copy_n(bytes.begin() + 96U, manifest.source_model_hash.size(),
              manifest.source_model_hash.begin());
  std::copy_n(bytes.begin() + 128U, manifest.source_serial_hash.size(),
              manifest.source_serial_hash.begin());
  std::copy_n(bytes.begin() + 224U, manifest.source_state_hash.size(),
              manifest.source_state_hash.begin());
  manifest.partitions.reserve(partition_count);
  for (std::size_t index = 0U; index < partition_count; ++index) {
    const std::size_t offset = kTsumugiManifestHeaderSize +
        index * kTsumugiManifestPartitionRecordSize;
    const auto record = bytes.subspan(
        offset, kTsumugiManifestPartitionRecordSize);
    if (!all_zero(record.subspan(376U, 8U))) {
      return failure<TsumugiManifest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"パーティションレコードの予約領域が非ゼロです");
    }
    const auto name = read_fixed_text(
        record,
        112U,
        kPartitionTextBytes,
        read_little<std::uint16_t>(record, 104U),
        false);
    const auto label = read_fixed_text(
        record,
        240U,
        kPartitionTextBytes,
        read_little<std::uint16_t>(record, 106U),
        false);
    if (!name || !label) {
      return clonecore::Result<TsumugiManifest>::failure(
          name ? label.error() : name.error());
    }
    TsumugiManifestPartition partition{
        .source_table_index = read_little<std::uint32_t>(record, 0U),
        .source_partition_number = read_little<std::uint32_t>(record, 4U),
        .role = static_cast<TsumugiManifestPartitionRole>(
            read_little<std::uint16_t>(record, 8U)),
        .file_system = static_cast<TsumugiManifestFileSystem>(
            read_little<std::uint16_t>(record, 10U)),
        .flags = static_cast<TsumugiManifestPartitionFlags>(
            read_little<std::uint32_t>(record, 12U)),
        .source_offset = read_little<std::uint64_t>(record, 16U),
        .source_size = read_little<std::uint64_t>(record, 24U),
        .used_bytes = read_little<std::uint64_t>(record, 32U),
        .minimum_target_bytes = read_little<std::uint64_t>(record, 40U),
        .planned_target_bytes = read_little<std::uint64_t>(record, 48U),
        .payload_logical_offset = read_little<std::uint64_t>(record, 56U),
        .payload_logical_length = read_little<std::uint64_t>(record, 64U),
        .payload_encoding = static_cast<TsumugiManifestPayloadEncoding>(
            read_little<std::uint16_t>(record, 108U)),
        .payload_format_version =
            read_little<std::uint16_t>(record, 110U),
        .cluster_size = read_little<std::uint64_t>(record, 368U),
        .name_utf8 = name.value(),
        .label_utf8 = label.value(),
    };
    std::copy_n(record.begin() + 72U, partition.type_id.size(),
                partition.type_id.begin());
    std::copy_n(record.begin() + 88U, partition.unique_id.size(),
                partition.unique_id.begin());
    manifest.partitions.push_back(std::move(partition));
  }
  manifest.partition_snapshot.assign(
      bytes.begin() + static_cast<std::size_t>(snapshot_offset), bytes.end());

  const auto valid = validate_manifest(manifest);
  if (!valid) {
    return clonecore::Result<TsumugiManifest>::failure(valid.error());
  }
  const auto canonical = build_tsumugi_manifest_v1(manifest);
  if (!canonical || canonical.value().size() != bytes.size() ||
      !std::equal(canonical.value().begin(), canonical.value().end(),
                  bytes.begin())) {
    return failure<TsumugiManifest>(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"マニフェストは正規再符号化と一致しません");
  }
  return clonecore::Result<TsumugiManifest>::success(std::move(manifest));
}

bool tsumugi_manifest_requires_shrink_archive_adapter(
    const TsumugiManifest& manifest) noexcept {
  if (manifest.mode != TsumugiManifestMode::shrink) {
    return false;
  }
  return std::any_of(
      manifest.partitions.begin(),
      manifest.partitions.end(),
      [](const TsumugiManifestPartition& partition) {
        return flag_set(
                   partition.flags,
                   TsumugiManifestPartitionFlags::selected) &&
            partition.payload_encoding ==
                TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
      });
}

}  // namespace ytec::imageformat

#include "ytec/imageformat/backup_manifest.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'D'}, std::byte{'C'}, std::byte{'M'}, std::byte{'A'},
    std::byte{'N'}, std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}};
constexpr std::size_t kMaximumPartitions = 128;
constexpr std::size_t kMaximumAppVersionBytes = 64;
constexpr std::size_t kMaximumArchitectureBytes = 32;
constexpr std::size_t kMaximumModelCharacters = 256;
constexpr std::size_t kMaximumSerialBytes = 64;
constexpr std::size_t kMaximumDeviceIdCharacters = 1024;
constexpr std::size_t kMaximumPartitionNameCharacters = 256;
constexpr std::uint32_t kFlagSystemDisk = 1U << 0U;
constexpr std::uint32_t kFlagBitLockerDecrypted = 1U << 1U;
constexpr std::uint32_t kKnownFlags =
    kFlagSystemDisk | kFlagBitLockerDecrypted;

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
    std::wstring operation,
    std::wstring message) {
  return clonecore::Result<T>::failure(manifest_error(
      code,
      native_code,
      std::move(operation),
      std::move(message)));
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

bool all_zero(const std::span<const std::byte> bytes) noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
    return value == std::byte{0};
  });
}

bool is_visible_ascii(const std::string_view value) noexcept {
  return std::all_of(
      value.begin(), value.end(), [](const unsigned char value) {
        return value >= 0x20U && value <= 0x7EU;
      });
}

bool is_valid_utc_timestamp(const std::string_view value) {
  if (value.size() != 20 || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != 'Z') {
    return false;
  }
  constexpr std::array<std::size_t, 14> positions{
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  if (!std::all_of(
          positions.begin(), positions.end(), [&](const std::size_t index) {
            return value[index] >= '0' && value[index] <= '9';
          })) {
    return false;
  }
  const auto two = [&](const std::size_t index) {
    return static_cast<unsigned int>((value[index] - '0') * 10) +
           static_cast<unsigned int>(value[index + 1] - '0');
  };
  return two(5) >= 1 && two(5) <= 12 &&
         two(8) >= 1 && two(8) <= 31 &&
         two(11) <= 23 && two(14) <= 59 && two(17) <= 60;
}

clonecore::Result<std::string> to_utf8(const std::wstring_view value) {
  if (value.empty()) {
    return clonecore::Result<std::string>::success({});
  }
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        L"バックアップマニフェストUTF-8生成",
        L"UTF-16文字列が長すぎます");
  }
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"バックアップマニフェストUTF-8生成",
        L"UTF-16文字列が不正です");
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          required,
          nullptr,
          nullptr) != required) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"バックアップマニフェストUTF-8生成",
        L"UTF-16文字列を完全に変換できません");
  }
  return clonecore::Result<std::string>::success(std::move(result));
}

clonecore::Result<std::wstring> from_utf8(const std::string_view value) {
  if (value.empty()) {
    return clonecore::Result<std::wstring>::success({});
  }
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"バックアップマニフェストUTF-8解析",
        L"UTF-8文字列が長すぎます");
  }
  const int required = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0);
  if (required <= 0) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"バックアップマニフェストUTF-8解析",
        L"UTF-8文字列が不正です");
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          required) != required) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        GetLastError(),
        L"バックアップマニフェストUTF-8解析",
        L"UTF-8文字列を完全に変換できません");
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

template <typename T>
T read_little(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
void write_little(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) noexcept {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

struct EncodedStrings final {
  std::string model;
  std::string device_id;
  std::vector<std::string> partition_names;
};

clonecore::Result<EncodedStrings> encode_strings(
    const BackupImageManifest& manifest) {
  auto model = to_utf8(manifest.source.model);
  auto device = to_utf8(manifest.source.device_instance_id);
  if (!model) {
    return clonecore::Result<EncodedStrings>::failure(model.error());
  }
  if (!device) {
    return clonecore::Result<EncodedStrings>::failure(device.error());
  }
  EncodedStrings result{
      .model = model.take_value(),
      .device_id = device.take_value(),
  };
  result.partition_names.reserve(manifest.partitions.size());
  for (const auto& partition : manifest.partitions) {
    auto name = to_utf8(partition.name);
    if (!name) {
      return clonecore::Result<EncodedStrings>::failure(name.error());
    }
    result.partition_names.push_back(name.take_value());
  }
  return clonecore::Result<EncodedStrings>::success(std::move(result));
}

clonecore::Status validate_manifest(
    const BackupImageManifest& manifest,
    const EncodedStrings& strings) {
  const auto identity = clonecore::validate_stable_identity(
      manifest.source, manifest.source, L"バックアップコピー元");
  if (!identity) {
    return identity;
  }
  const bool compression_supported =
      (manifest.compression == DcimgCompression::none &&
       manifest.compression_version == 0U) ||
      (manifest.compression == DcimgCompression::zstandard &&
       manifest.compression_version == 1U);
  const bool legacy_system_manifest =
      manifest.format_minor == kLegacyBackupManifestMinorVersion;
  const bool current_manifest =
      manifest.format_minor == kBackupManifestMinorVersion;
  if ((!legacy_system_manifest && !current_manifest) ||
      (!manifest.source.is_system_disk && !current_manifest) ||
      !is_supported_sector_size_pair(
          manifest.source.logical_sector_size,
          manifest.physical_sector_size) ||
      manifest.source.size_bytes %
              manifest.source.logical_sector_size !=
          0 ||
      !manifest.bitlocker_fully_decrypted ||
      !compression_supported ||
      (manifest.chunk_size != kDcimgChunkSize16MiB &&
       manifest.chunk_size != kDcimgChunkSize32MiB) ||
      !is_valid_utc_timestamp(manifest.created_utc) ||
      manifest.app_version.empty() ||
      manifest.app_version.size() > kMaximumAppVersionBytes ||
      !is_visible_ascii(manifest.app_version) ||
      manifest.windows_major != 10 ||
      manifest.windows_build < 10240 ||
      manifest.windows_architecture != "AMD64" ||
      manifest.partitions.empty() ||
      manifest.partitions.size() > kMaximumPartitions ||
      manifest.source.model.size() > kMaximumModelCharacters ||
      strings.model.size() > kMaximumBackupManifestBytes ||
      manifest.source.serial_suffix.size() > kMaximumSerialBytes ||
      !is_visible_ascii(manifest.source.serial_suffix) ||
      manifest.source.device_instance_id.size() >
          kMaximumDeviceIdCharacters ||
      manifest.windows_architecture.size() >
          kMaximumArchitectureBytes) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"バックアップマニフェスト共通条件",
        L"ディスク、Windows、BitLocker、圧縮、日時、または識別情報が対応条件外です"));
  }
  if ((manifest.source.is_system_disk &&
       manifest.partition_style == BackupPartitionStyle::gpt &&
       manifest.boot_mode != BackupBootMode::uefi) ||
      (manifest.source.is_system_disk &&
       manifest.partition_style == BackupPartitionStyle::mbr &&
       manifest.boot_mode != BackupBootMode::legacy_bios) ||
      (!manifest.source.is_system_disk &&
       manifest.boot_mode != BackupBootMode::none) ||
      (manifest.partition_style != BackupPartitionStyle::gpt &&
       manifest.partition_style != BackupPartitionStyle::mbr)) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"バックアップ起動方式",
        L"システムディスクの起動方式またはデータディスクの非起動指定が一致しません"));
  }

  std::uint64_t previous_end = 0;
  std::size_t efi_count = 0;
  std::size_t msr_count = 0;
  std::size_t windows_count = 0;
  std::size_t data_count = 0;
  for (std::size_t index = 0; index < manifest.partitions.size(); ++index) {
    const auto& partition = manifest.partitions[index];
    std::uint64_t end{};
    if (partition.length_bytes == 0 ||
        partition.offset_bytes %
                manifest.source.logical_sector_size !=
            0 ||
        partition.length_bytes %
                manifest.source.logical_sector_size !=
            0 ||
        !checked_add(
            partition.offset_bytes, partition.length_bytes, end) ||
        end > manifest.source.size_bytes ||
        (index != 0 && partition.offset_bytes < previous_end) ||
        partition.name.size() > kMaximumPartitionNameCharacters ||
        strings.partition_names[index].size() >
            kMaximumBackupManifestBytes) {
      return clonecore::Status::failure(manifest_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"バックアップパーティション境界",
          L"パーティションが空、未整列、重複、境界外、または名前上限外です"));
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (manifest.partitions[previous].table_index ==
          partition.table_index) {
        return clonecore::Status::failure(manifest_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"バックアップパーティション番号",
            L"パーティション表番号が重複しています"));
      }
    }
    previous_end = end;

    switch (partition.role) {
      case BackupPartitionRole::efi_system:
        ++efi_count;
        if (manifest.partition_style != BackupPartitionStyle::gpt ||
            partition.file_system != BackupFileSystem::fat32) {
          return clonecore::Status::failure(manifest_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_DATA,
              L"EFIマニフェスト",
              L"EFIはGPT上のFAT32である必要があります"));
        }
        break;
      case BackupPartitionRole::microsoft_reserved:
        ++msr_count;
        if (manifest.partition_style != BackupPartitionStyle::gpt ||
            partition.file_system != BackupFileSystem::none ||
            partition.cluster_size != 0) {
          return clonecore::Status::failure(manifest_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_DATA,
              L"MSRマニフェスト",
              L"MSRはGPT上のファイルシステムなしである必要があります"));
        }
        break;
      case BackupPartitionRole::windows_ntfs:
        ++windows_count;
        if (!manifest.source.is_system_disk ||
            partition.file_system != BackupFileSystem::ntfs) {
          return clonecore::Status::failure(manifest_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_DATA,
              L"Windowsパーティションマニフェスト",
              L"WindowsパーティションはNTFSである必要があります"));
        }
        break;
      case BackupPartitionRole::recovery_ntfs:
        if (!manifest.source.is_system_disk ||
            partition.file_system != BackupFileSystem::ntfs) {
          return clonecore::Status::failure(manifest_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_DATA,
              L"回復パーティションマニフェスト",
              L"回復パーティションはNTFSである必要があります"));
        }
        break;
      case BackupPartitionRole::fat32_data:
        ++data_count;
        if (partition.file_system != BackupFileSystem::fat32 ||
            (manifest.source.is_system_disk &&
             manifest.partition_style != BackupPartitionStyle::mbr)) {
          return clonecore::Status::failure(manifest_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_DATA,
              L"FAT32パーティションマニフェスト",
              L"汎用FAT32はデータ専用ディスク、またはシステムMBR上である必要があります"));
        }
        break;
      case BackupPartitionRole::ntfs_data:
        ++data_count;
        if (manifest.source.is_system_disk ||
            partition.file_system != BackupFileSystem::ntfs) {
          return clonecore::Status::failure(manifest_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_DATA,
              L"NTFSデータパーティションマニフェスト",
              L"汎用NTFSはデータ専用ディスクである必要があります"));
        }
        break;
      default:
        return clonecore::Status::failure(manifest_error(
            clonecore::ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"バックアップパーティション役割",
            L"未知のパーティション役割です"));
    }
    if (partition.file_system != BackupFileSystem::none &&
        (partition.cluster_size <
             manifest.source.logical_sector_size ||
         partition.cluster_size % manifest.source.logical_sector_size !=
             0 ||
         partition.cluster_size > 2ULL * 1024ULL * 1024ULL)) {
      return clonecore::Status::failure(manifest_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"バックアップクラスタサイズ",
          L"ファイルシステムのクラスタサイズが対応条件外です"));
    }
  }
  const bool system_layout_invalid =
      manifest.source.is_system_disk &&
      (windows_count == 0 ||
       (manifest.partition_style == BackupPartitionStyle::gpt &&
        (efi_count != 1 || msr_count != 1)) ||
       (manifest.partition_style == BackupPartitionStyle::mbr &&
        (efi_count != 0 || msr_count != 0)));
  const bool data_layout_invalid =
      !manifest.source.is_system_disk &&
      (windows_count != 0 || data_count == 0 || efi_count != 0 ||
       (manifest.partition_style == BackupPartitionStyle::gpt
            ? msr_count > 1
            : msr_count != 0));
  if (system_layout_invalid || data_layout_invalid) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"バックアップ起動パーティション構成",
        L"システム/データ種別とWindows、データ、EFI、MSRの構成が一致しません"));
  }
  return clonecore::success_status();
}

struct StringDescriptor final {
  std::uint64_t offset{};
  std::uint32_t length{};
};

void write_descriptor(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const StringDescriptor descriptor) {
  write_little(bytes, offset, descriptor.offset);
  write_little(bytes, offset + 8, descriptor.length);
}

StringDescriptor read_descriptor(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  return StringDescriptor{
      .offset = read_little<std::uint64_t>(bytes, offset),
      .length = read_little<std::uint32_t>(bytes, offset + 8),
  };
}

clonecore::Result<std::string> read_string(
    const std::span<const std::byte> bytes,
    const StringDescriptor descriptor,
    std::uint64_t& expected_offset) {
  std::uint64_t end{};
  if (descriptor.offset != expected_offset ||
      !checked_add(descriptor.offset, descriptor.length, end) ||
      end > bytes.size()) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"バックアップマニフェスト文字列境界",
        L"文字列が非正規配置または境界外です");
  }
  std::string result(descriptor.length, '\0');
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<char>(
        std::to_integer<unsigned char>(
            bytes[static_cast<std::size_t>(descriptor.offset) + index]));
  }
  expected_offset = end;
  return clonecore::Result<std::string>::success(std::move(result));
}

bool equal_bytes(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

}  // namespace

clonecore::Result<std::vector<std::byte>>
build_backup_manifest_v1(const BackupImageManifest& manifest) {
  auto strings = encode_strings(manifest);
  if (!strings) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        strings.error());
  }
  const auto valid = validate_manifest(manifest, strings.value());
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        valid.error());
  }

  const std::array<std::string_view, 6> header_strings{
      manifest.created_utc,
      manifest.app_version,
      manifest.windows_architecture,
      strings.value().model,
      manifest.source.serial_suffix,
      strings.value().device_id,
  };
  std::uint64_t total = kBackupManifestHeaderSize +
      static_cast<std::uint64_t>(manifest.partitions.size()) *
          kBackupManifestPartitionRecordSize;
  for (const auto value : header_strings) {
    if (!checked_add(total, value.size(), total)) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_ARITHMETIC_OVERFLOW,
          L"バックアップマニフェスト全長",
          L"文字列全長がオーバーフローしました");
    }
  }
  for (const auto& value : strings.value().partition_names) {
    if (!checked_add(total, value.size(), total)) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_ARITHMETIC_OVERFLOW,
          L"バックアップマニフェスト全長",
          L"パーティション名全長がオーバーフローしました");
    }
  }
  if (total > kMaximumBackupManifestBytes ||
      total > std::numeric_limits<std::size_t>::max()) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILE_TOO_LARGE,
        L"バックアップマニフェスト全長",
        L"マニフェストが許容上限を超えています");
  }

  std::vector<std::byte> output(
      static_cast<std::size_t>(total), std::byte{0});
  const std::span<std::byte> bytes(output);
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  write_little(bytes, 8, kBackupManifestMajorVersion);
  write_little(bytes, 10, manifest.format_minor);
  write_little(bytes, 12, kBackupManifestHeaderSize);
  write_little(bytes, 16, total);
  write_little(bytes, 24, manifest.source.disk_number);
  const std::uint32_t flags =
      (manifest.source.is_system_disk ? kFlagSystemDisk : 0U) |
      (manifest.bitlocker_fully_decrypted
           ? kFlagBitLockerDecrypted
           : 0U);
  write_little(bytes, 28, flags);
  write_little(bytes, 32, manifest.source.size_bytes);
  write_little(bytes, 40, manifest.source.logical_sector_size);
  write_little(bytes, 44, manifest.physical_sector_size);
  write_little(bytes, 48, static_cast<std::uint16_t>(
                              manifest.partition_style));
  write_little(
      bytes, 50, static_cast<std::uint16_t>(manifest.boot_mode));
  write_little(
      bytes, 52, static_cast<std::uint16_t>(manifest.compression));
  write_little(bytes, 54, manifest.compression_version);
  write_little(bytes, 56, manifest.chunk_size);
  write_little(bytes, 60, manifest.windows_major);
  write_little(bytes, 64, manifest.windows_minor);
  write_little(bytes, 68, manifest.windows_build);
  write_little(
      bytes,
      72,
      static_cast<std::uint32_t>(manifest.partitions.size()));
  write_little(bytes, 76, kBackupManifestPartitionRecordSize);
  write_little<std::uint64_t>(bytes, 80, kBackupManifestHeaderSize);
  const std::uint64_t strings_offset =
      kBackupManifestHeaderSize +
      static_cast<std::uint64_t>(manifest.partitions.size()) *
          kBackupManifestPartitionRecordSize;
  write_little(bytes, 88, strings_offset);

  std::uint64_t next_string = strings_offset;
  for (std::size_t index = 0; index < header_strings.size(); ++index) {
    write_descriptor(
        bytes,
        96 + index * 16,
        StringDescriptor{
            .offset = next_string,
            .length =
                static_cast<std::uint32_t>(header_strings[index].size()),
        });
    std::copy(
        std::as_bytes(std::span(header_strings[index])).begin(),
        std::as_bytes(std::span(header_strings[index])).end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(next_string));
    next_string += header_strings[index].size();
  }
  for (std::size_t index = 0; index < manifest.partitions.size(); ++index) {
    const auto& partition = manifest.partitions[index];
    const std::size_t record_offset =
        kBackupManifestHeaderSize +
        index * kBackupManifestPartitionRecordSize;
    const auto record =
        bytes.subspan(record_offset, kBackupManifestPartitionRecordSize);
    write_little(record, 0, partition.table_index);
    write_little(
        record, 4, static_cast<std::uint16_t>(partition.role));
    write_little(
        record, 6, static_cast<std::uint16_t>(partition.file_system));
    write_little(record, 8, partition.offset_bytes);
    write_little(record, 16, partition.length_bytes);
    write_little(record, 24, partition.cluster_size);
    const auto& name = strings.value().partition_names[index];
    write_descriptor(
        record,
        32,
        StringDescriptor{
            .offset = next_string,
            .length = static_cast<std::uint32_t>(name.size()),
        });
    std::copy(
        std::as_bytes(std::span(name)).begin(),
        std::as_bytes(std::span(name)).end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(next_string));
    next_string += name.size();
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(output));
}

clonecore::Result<BackupImageManifest>
inspect_backup_manifest_v1(const std::span<const std::byte> bytes) {
  if (bytes.size() < kBackupManifestHeaderSize ||
      bytes.size() > kMaximumBackupManifestBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
      read_little<std::uint16_t>(bytes, 8) !=
          kBackupManifestMajorVersion ||
      (read_little<std::uint16_t>(bytes, 10) !=
           kLegacyBackupManifestMinorVersion &&
       read_little<std::uint16_t>(bytes, 10) !=
           kBackupManifestMinorVersion) ||
      read_little<std::uint32_t>(bytes, 12) !=
          kBackupManifestHeaderSize ||
      read_little<std::uint64_t>(bytes, 16) != bytes.size() ||
      read_little<std::uint32_t>(bytes, 76) !=
          kBackupManifestPartitionRecordSize ||
      !all_zero(bytes.subspan(108, 4)) ||
      !all_zero(bytes.subspan(124, 4)) ||
      !all_zero(bytes.subspan(140, 4)) ||
      !all_zero(bytes.subspan(156, 4)) ||
      !all_zero(bytes.subspan(172, 4)) ||
      !all_zero(bytes.subspan(188, 4))) {
    return failure<BackupImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"バックアップマニフェストヘッダー",
        L"マジック、版、固定長、全長、または予約領域が不正です");
  }
  const std::uint32_t flags = read_little<std::uint32_t>(bytes, 28);
  const std::uint32_t partition_count =
      read_little<std::uint32_t>(bytes, 72);
  const std::uint64_t expected_strings =
      kBackupManifestHeaderSize +
      static_cast<std::uint64_t>(partition_count) *
          kBackupManifestPartitionRecordSize;
  if ((flags & ~kKnownFlags) != 0 ||
      partition_count == 0 || partition_count > kMaximumPartitions ||
      read_little<std::uint64_t>(bytes, 80) !=
          kBackupManifestHeaderSize ||
      read_little<std::uint64_t>(bytes, 88) != expected_strings ||
      expected_strings > bytes.size()) {
    return failure<BackupImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"バックアップマニフェスト配置",
        L"フラグ、パーティション数、または文字列開始位置が不正です");
  }

  std::uint64_t next_string = expected_strings;
  std::array<std::string, 6> header_strings;
  for (std::size_t index = 0; index < header_strings.size(); ++index) {
    auto value = read_string(
        bytes, read_descriptor(bytes, 96 + index * 16), next_string);
    if (!value) {
      return clonecore::Result<BackupImageManifest>::failure(
          value.error());
    }
    header_strings[index] = value.take_value();
  }
  auto model = from_utf8(header_strings[3]);
  auto device = from_utf8(header_strings[5]);
  if (!model) {
    return clonecore::Result<BackupImageManifest>::failure(model.error());
  }
  if (!device) {
    return clonecore::Result<BackupImageManifest>::failure(device.error());
  }

  BackupImageManifest manifest{
      .format_minor = read_little<std::uint16_t>(bytes, 10),
      .source =
          clonecore::StableDiskIdentity{
              .disk_number = read_little<std::uint32_t>(bytes, 24),
              .model = model.take_value(),
              .size_bytes = read_little<std::uint64_t>(bytes, 32),
              .logical_sector_size =
                  read_little<std::uint32_t>(bytes, 40),
              .serial_suffix = header_strings[4],
              .device_instance_id = device.take_value(),
              .is_system_disk = (flags & kFlagSystemDisk) != 0,
          },
      .physical_sector_size = read_little<std::uint32_t>(bytes, 44),
      .partition_style = static_cast<BackupPartitionStyle>(
          read_little<std::uint16_t>(bytes, 48)),
      .boot_mode = static_cast<BackupBootMode>(
          read_little<std::uint16_t>(bytes, 50)),
      .windows_major = read_little<std::uint32_t>(bytes, 60),
      .windows_minor = read_little<std::uint32_t>(bytes, 64),
      .windows_build = read_little<std::uint32_t>(bytes, 68),
      .windows_architecture = header_strings[2],
      .bitlocker_fully_decrypted =
          (flags & kFlagBitLockerDecrypted) != 0,
      .compression = static_cast<DcimgCompression>(
          read_little<std::uint16_t>(bytes, 52)),
      .compression_version =
          read_little<std::uint16_t>(bytes, 54),
      .chunk_size = read_little<std::uint32_t>(bytes, 56),
      .created_utc = header_strings[0],
      .app_version = header_strings[1],
  };
  manifest.partitions.reserve(partition_count);
  for (std::uint32_t index = 0; index < partition_count; ++index) {
    const std::size_t record_offset =
        kBackupManifestHeaderSize +
        static_cast<std::size_t>(index) *
            kBackupManifestPartitionRecordSize;
    const auto record =
        bytes.subspan(record_offset, kBackupManifestPartitionRecordSize);
    if (!all_zero(record.subspan(44, 36))) {
      return failure<BackupImageManifest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"バックアップパーティション予約領域",
          L"パーティションレコードの予約領域が0ではありません");
    }
    auto name_bytes =
        read_string(bytes, read_descriptor(record, 32), next_string);
    if (!name_bytes) {
      return clonecore::Result<BackupImageManifest>::failure(
          name_bytes.error());
    }
    auto name = from_utf8(name_bytes.value());
    if (!name) {
      return clonecore::Result<BackupImageManifest>::failure(
          name.error());
    }
    manifest.partitions.push_back(BackupManifestPartition{
        .table_index = read_little<std::uint32_t>(record, 0),
        .offset_bytes = read_little<std::uint64_t>(record, 8),
        .length_bytes = read_little<std::uint64_t>(record, 16),
        .role = static_cast<BackupPartitionRole>(
            read_little<std::uint16_t>(record, 4)),
        .file_system = static_cast<BackupFileSystem>(
            read_little<std::uint16_t>(record, 6)),
        .cluster_size = read_little<std::uint64_t>(record, 24),
        .name = name.take_value(),
    });
  }
  if (next_string != bytes.size()) {
    return failure<BackupImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"バックアップマニフェスト末尾",
        L"未参照データまたは末尾追加があります");
  }
  auto encoded_strings = encode_strings(manifest);
  if (!encoded_strings) {
    return clonecore::Result<BackupImageManifest>::failure(
        encoded_strings.error());
  }
  const auto valid = validate_manifest(manifest, encoded_strings.value());
  if (!valid) {
    return clonecore::Result<BackupImageManifest>::failure(valid.error());
  }
  const auto canonical = build_backup_manifest_v1(manifest);
  if (!canonical) {
    return clonecore::Result<BackupImageManifest>::failure(
        canonical.error());
  }
  if (!equal_bytes(bytes, canonical.value())) {
    return failure<BackupImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"バックアップマニフェスト正規形",
        L"項目順、文字列表現、または予約領域が正規形と一致しません");
  }
  return clonecore::Result<BackupImageManifest>::success(
      std::move(manifest));
}

}  // namespace ytec::imageformat

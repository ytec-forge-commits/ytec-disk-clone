#include "ytec/imageformat/shrink_image_manifest.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'D'}, std::byte{'C'}, std::byte{'M'}, std::byte{'I'},
    std::byte{'G'}, std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}};
constexpr std::uint32_t kFlagSystemDisk = 1U << 0U;
constexpr std::uint32_t kFlagBitLockerDecrypted = 1U << 1U;
constexpr std::uint32_t kKnownHeaderFlags =
    kFlagSystemDisk | kFlagBitLockerDecrypted;
constexpr std::uint32_t kFlagActive = 1U << 0U;
constexpr std::uint32_t kFlagPayloadPresent = 1U << 1U;
constexpr std::uint32_t kKnownPartitionFlags =
    kFlagActive | kFlagPayloadPresent;
constexpr std::size_t kMaximumPartitions = 128U;
constexpr std::size_t kMaximumModelCharacters = 256U;
constexpr std::size_t kMaximumDeviceIdCharacters = 1024U;
constexpr std::size_t kMaximumSerialBytes = 64U;
constexpr std::size_t kMaximumLabelCharacters = 32U;
constexpr std::size_t kMaximumPayloadNameBytes = 64U;
constexpr std::size_t kMaximumAppVersionBytes = 64U;
constexpr std::size_t kMaximumArchitectureBytes = 32U;

struct StringDescriptor final {
  std::uint64_t offset{};
  std::uint32_t length{};
};

struct EncodedStrings final {
  std::string model;
  std::string device_id;
  std::vector<std::string> labels;
};

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
      code, native_code, std::move(operation), std::move(message)));
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
      value.begin(), value.end(), [](const unsigned char character) {
        return character >= 0x20U && character <= 0x7EU;
      });
}

bool is_valid_utc_timestamp(const std::string_view value) {
  if (value.size() != 20U || value[4] != '-' || value[7] != '-' ||
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
  return two(5) >= 1U && two(5) <= 12U &&
      two(8) >= 1U && two(8) <= 31U &&
      two(11) <= 23U && two(14) <= 59U && two(17) <= 60U;
}

clonecore::Result<std::string> to_utf8(const std::wstring_view value) {
  if (value.empty()) {
    return clonecore::Result<std::string>::success({});
  }
  if (value.size() > static_cast<std::size_t>(
                         (std::numeric_limits<int>::max)())) {
    return failure<std::string>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小移行マニフェストUTF-8生成",
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
        L"縮小移行マニフェストUTF-8生成",
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
        L"縮小移行マニフェストUTF-8生成",
        L"UTF-16文字列を完全に変換できません");
  }
  return clonecore::Result<std::string>::success(std::move(result));
}

clonecore::Result<std::wstring> from_utf8(const std::string_view value) {
  if (value.empty()) {
    return clonecore::Result<std::wstring>::success({});
  }
  if (value.size() > static_cast<std::size_t>(
                         (std::numeric_limits<int>::max)())) {
    return failure<std::wstring>(
        clonecore::ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小移行マニフェストUTF-8解析",
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
        L"縮小移行マニフェストUTF-8解析",
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
        L"縮小移行マニフェストUTF-8解析",
        L"UTF-8文字列を完全に変換できません");
  }
  return clonecore::Result<std::wstring>::success(std::move(result));
}

bool valid_payload_name(const std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaximumPayloadNameBytes ||
      !is_visible_ascii(value) || value == "." || value == ".." ||
      value.find('/') != std::string_view::npos ||
      value.find('\\') != std::string_view::npos ||
      value.find(':') != std::string_view::npos) {
    return false;
  }
  constexpr std::string_view extension = ".wim";
  return value.size() > extension.size() &&
      value.ends_with(extension);
}

bool digest_all_zero(const Sha256Digest& digest) noexcept {
  return all_zero(digest);
}

bool valid_volume_label(const std::wstring_view value) noexcept {
  return std::none_of(value.begin(), value.end(), [](const wchar_t character) {
    return character == L'\0' || character < L' ';
  });
}

clonecore::Result<EncodedStrings> encode_strings(
    const ShrinkImageManifest& manifest) {
  auto model = to_utf8(manifest.source.model);
  auto device = to_utf8(manifest.source.device_instance_id);
  if (!model) {
    return clonecore::Result<EncodedStrings>::failure(model.error());
  }
  if (!device) {
    return clonecore::Result<EncodedStrings>::failure(device.error());
  }
  EncodedStrings strings{
      .model = model.take_value(),
      .device_id = device.take_value(),
  };
  strings.labels.reserve(manifest.partitions.size());
  for (const auto& partition : manifest.partitions) {
    auto label = to_utf8(partition.label);
    if (!label) {
      return clonecore::Result<EncodedStrings>::failure(label.error());
    }
    strings.labels.push_back(label.take_value());
  }
  return clonecore::Result<EncodedStrings>::success(std::move(strings));
}

clonecore::Status validate_manifest(
    const ShrinkImageManifest& manifest,
    const EncodedStrings& strings) {
  const auto identity = clonecore::validate_stable_identity(
      manifest.source, manifest.source, L"縮小移行イメージコピー元");
  if (!identity) {
    return identity;
  }
  const bool bootstrap_all_zero = all_zero(manifest.mbr_bootstrap);
  if (manifest.source.model.size() > kMaximumModelCharacters ||
      manifest.source.device_instance_id.size() >
          kMaximumDeviceIdCharacters ||
      manifest.source.serial_suffix.size() > kMaximumSerialBytes ||
      !is_visible_ascii(manifest.source.serial_suffix) ||
      manifest.physical_sector_size < manifest.source.logical_sector_size ||
      manifest.physical_sector_size > 64U * 1024U ||
      (manifest.physical_sector_size &
       (manifest.physical_sector_size - 1U)) != 0U ||
      manifest.source.size_bytes % manifest.source.logical_sector_size != 0U ||
      !manifest.bitlocker_fully_decrypted ||
      (manifest.source.is_system_disk &&
       (manifest.windows_major != 10U || manifest.windows_build < 10240U ||
        manifest.windows_architecture != "AMD64")) ||
      (!manifest.source.is_system_disk &&
       (manifest.windows_major != 0U || manifest.windows_minor != 0U ||
        manifest.windows_build != 0U ||
        !manifest.windows_architecture.empty())) ||
      manifest.windows_architecture.size() > kMaximumArchitectureBytes ||
      manifest.app_version.empty() ||
      manifest.app_version.size() > kMaximumAppVersionBytes ||
      !is_visible_ascii(manifest.app_version) ||
      !is_valid_utc_timestamp(manifest.created_utc) ||
      manifest.partitions.empty() ||
      manifest.partitions.size() > kMaximumPartitions ||
      strings.labels.size() != manifest.partitions.size()) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"縮小移行マニフェスト共通条件",
        L"識別情報、Windows、BitLocker、日時、またはパーティション条件が対応外です"));
  }
  if ((manifest.partition_style ==
           migrationcore::MigrationPartitionStyle::gpt &&
       !bootstrap_all_zero) ||
      (manifest.partition_style ==
           migrationcore::MigrationPartitionStyle::mbr &&
       manifest.source.is_system_disk && bootstrap_all_zero)) {
    return clonecore::Status::failure(manifest_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_DATA,
        L"縮小移行MBRブートコード",
        L"パーティション形式とMBRブートコードが一致しません"));
  }

  migrationcore::ShrinkMigrationRequest plan_request{
      .source_style = manifest.partition_style,
      .target_style = manifest.partition_style,
      .target_size_bytes = manifest.source.size_bytes,
      .target_logical_sector_size = manifest.source.logical_sector_size,
      .source_is_windows_system = manifest.source.is_system_disk,
      .windows_is_amd64 = manifest.windows_architecture == "AMD64",
      .bitlocker_fully_decrypted = manifest.bitlocker_fully_decrypted,
  };
  plan_request.source_partitions.reserve(manifest.partitions.size());
  std::uint64_t declared_partition_bytes = 0U;
  for (std::size_t index = 0; index < manifest.partitions.size(); ++index) {
    const auto& partition = manifest.partitions[index];
    const bool content =
        partition.role == migrationcore::MigrationPartitionRole::bios_system ||
        partition.role == migrationcore::MigrationPartitionRole::windows ||
        partition.role == migrationcore::MigrationPartitionRole::recovery ||
        partition.role == migrationcore::MigrationPartitionRole::data;
    const bool payload_expected = content && partition.used_bytes != 0U;
    const bool fixed_role_valid =
        partition.role == migrationcore::MigrationPartitionRole::efi_system
        ? partition.file_system == migrationcore::MigrationFileSystem::fat32 &&
              partition.used_bytes == 0U && partition.cluster_size == 4096U &&
              !partition.active
        : partition.role ==
                migrationcore::MigrationPartitionRole::microsoft_reserved
            ? partition.file_system == migrationcore::MigrationFileSystem::none &&
                  partition.used_bytes == 0U && partition.cluster_size == 0U &&
                  partition.label.empty() && !partition.active
            : true;
    std::uint64_t next_declared_partition_bytes = 0U;
    if (partition.label.size() > kMaximumLabelCharacters ||
        !valid_volume_label(partition.label) || !fixed_role_valid ||
        (manifest.partition_style ==
             migrationcore::MigrationPartitionStyle::gpt &&
         partition.active) ||
        !checked_add(
            declared_partition_bytes,
            partition.source_size_bytes,
            next_declared_partition_bytes) ||
        next_declared_partition_bytes > manifest.source.size_bytes ||
        strings.labels[index].size() > kMaximumShrinkImageManifestBytes ||
        (payload_expected &&
         (!valid_payload_name(partition.payload_file_name) ||
          partition.payload_length_bytes == 0U ||
          digest_all_zero(partition.payload_sha256))) ||
        (!payload_expected &&
         (!partition.payload_file_name.empty() ||
          partition.payload_length_bytes != 0U ||
          !digest_all_zero(partition.payload_sha256)))) {
      return clonecore::Status::failure(manifest_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_DATA,
          L"縮小移行ペイロード情報",
          L"WIMファイル名、長さ、SHA-256、または空領域指定が一致しません"));
    }
    declared_partition_bytes = next_declared_partition_bytes;
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (manifest.partitions[previous].source_table_index ==
              partition.source_table_index ||
          (!partition.payload_file_name.empty() &&
           _stricmp(
               manifest.partitions[previous].payload_file_name.c_str(),
               partition.payload_file_name.c_str()) == 0)) {
        return clonecore::Status::failure(manifest_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_DUP_NAME,
            L"縮小移行ペイロード一意性",
            L"パーティション番号またはWIMファイル名が重複しています"));
      }
    }
    plan_request.source_partitions.push_back(
        migrationcore::ShrinkSourcePartition{
            .source_table_index = partition.source_table_index,
            .role = partition.role,
            .file_system = partition.file_system,
            .source_size_bytes = partition.source_size_bytes,
            .used_bytes = partition.used_bytes,
            .cluster_size = partition.cluster_size,
            .label = partition.label,
            .active = partition.active,
        });
  }
  const auto plan = migrationcore::plan_shrink_migration(plan_request);
  if (!plan) {
    return clonecore::Status::failure(plan.error());
  }
  return clonecore::success_status();
}

void write_descriptor(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const StringDescriptor descriptor) noexcept {
  write_little(bytes, offset, descriptor.offset);
  write_little(bytes, offset + 8U, descriptor.length);
}

StringDescriptor read_descriptor(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
  return StringDescriptor{
      .offset = read_little<std::uint64_t>(bytes, offset),
      .length = read_little<std::uint32_t>(bytes, offset + 8U),
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
        L"縮小移行マニフェスト文字列境界",
        L"文字列が非正規配置または境界外です");
  }
  std::string value(descriptor.length, '\0');
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<char>(std::to_integer<unsigned char>(
        bytes[static_cast<std::size_t>(descriptor.offset) + index]));
  }
  expected_offset = end;
  return clonecore::Result<std::string>::success(std::move(value));
}

bool equal_bytes(
    const std::span<const std::byte> left,
    const std::span<const std::byte> right) noexcept {
  return left.size() == right.size() &&
      std::equal(left.begin(), left.end(), right.begin());
}

}  // namespace

clonecore::Result<std::vector<std::byte>>
build_shrink_image_manifest_v1(const ShrinkImageManifest& manifest) {
  auto strings = encode_strings(manifest);
  if (!strings) {
    return clonecore::Result<std::vector<std::byte>>::failure(strings.error());
  }
  const auto valid = validate_manifest(manifest, strings.value());
  if (!valid) {
    return clonecore::Result<std::vector<std::byte>>::failure(valid.error());
  }

  const std::array<std::string_view, 6> header_strings{
      manifest.created_utc,
      manifest.app_version,
      manifest.windows_architecture,
      strings.value().model,
      manifest.source.serial_suffix,
      strings.value().device_id,
  };
  std::uint64_t total = kShrinkImageManifestHeaderSize +
      static_cast<std::uint64_t>(manifest.partitions.size()) *
          kShrinkImageManifestRecordSize;
  for (const auto value : header_strings) {
    if (!checked_add(total, value.size(), total)) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_ARITHMETIC_OVERFLOW,
          L"縮小移行マニフェスト全長",
          L"文字列全長がオーバーフローしました");
    }
  }
  if (manifest.partition_style ==
          migrationcore::MigrationPartitionStyle::mbr &&
      !checked_add(total, manifest.mbr_bootstrap.size(), total)) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ARITHMETIC_OVERFLOW,
        L"縮小移行マニフェスト全長",
        L"MBRブートコード長がオーバーフローしました");
  }
  for (std::size_t index = 0; index < manifest.partitions.size(); ++index) {
    if (!checked_add(
            total, manifest.partitions[index].payload_file_name.size(), total) ||
        !checked_add(total, strings.value().labels[index].size(), total)) {
      return failure<std::vector<std::byte>>(
          clonecore::ErrorCode::invalid_argument,
          ERROR_ARITHMETIC_OVERFLOW,
          L"縮小移行マニフェスト全長",
          L"パーティション文字列全長がオーバーフローしました");
    }
  }
  if (total > kMaximumShrinkImageManifestBytes ||
      total > (std::numeric_limits<std::size_t>::max)()) {
    return failure<std::vector<std::byte>>(
        clonecore::ErrorCode::invalid_argument,
        ERROR_FILE_TOO_LARGE,
        L"縮小移行マニフェスト全長",
        L"マニフェストが許容上限を超えています");
  }

  std::vector<std::byte> output(static_cast<std::size_t>(total), std::byte{0});
  const std::span<std::byte> bytes(output);
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  write_little(bytes, 8, kShrinkImageManifestMajorVersion);
  write_little(bytes, 10, kShrinkImageManifestMinorVersion);
  write_little(bytes, 12, kShrinkImageManifestHeaderSize);
  write_little(bytes, 16, total);
  const std::uint32_t flags =
      (manifest.source.is_system_disk ? kFlagSystemDisk : 0U) |
      (manifest.bitlocker_fully_decrypted ? kFlagBitLockerDecrypted : 0U);
  write_little(bytes, 24, flags);
  write_little(bytes, 28, manifest.source.disk_number);
  write_little(bytes, 32, manifest.source.size_bytes);
  write_little(bytes, 40, manifest.source.logical_sector_size);
  write_little(bytes, 44, manifest.physical_sector_size);
  write_little(
      bytes, 48, static_cast<std::uint16_t>(manifest.partition_style));
  write_little(bytes, 52, manifest.windows_major);
  write_little(bytes, 56, manifest.windows_minor);
  write_little(bytes, 60, manifest.windows_build);
  write_little(
      bytes, 64, static_cast<std::uint32_t>(manifest.partitions.size()));
  write_little(bytes, 68, kShrinkImageManifestRecordSize);
  write_little<std::uint64_t>(bytes, 72, kShrinkImageManifestHeaderSize);
  const std::uint64_t strings_offset = kShrinkImageManifestHeaderSize +
      static_cast<std::uint64_t>(manifest.partitions.size()) *
          kShrinkImageManifestRecordSize;
  write_little(bytes, 80, strings_offset);

  std::uint64_t next_string = strings_offset;
  for (std::size_t index = 0; index < header_strings.size(); ++index) {
    write_descriptor(
        bytes,
        88U + index * 16U,
        StringDescriptor{
            .offset = next_string,
            .length = static_cast<std::uint32_t>(header_strings[index].size()),
        });
    std::copy(
        std::as_bytes(std::span(header_strings[index])).begin(),
        std::as_bytes(std::span(header_strings[index])).end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(next_string));
    next_string += header_strings[index].size();
  }
  const bool has_mbr_bootstrap =
      manifest.partition_style == migrationcore::MigrationPartitionStyle::mbr;
  write_descriptor(
      bytes,
      184,
      StringDescriptor{
          .offset = next_string,
          .length = has_mbr_bootstrap
              ? static_cast<std::uint32_t>(manifest.mbr_bootstrap.size())
              : 0U,
      });
  if (has_mbr_bootstrap) {
    std::copy(
        manifest.mbr_bootstrap.begin(),
        manifest.mbr_bootstrap.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(next_string));
    next_string += manifest.mbr_bootstrap.size();
  }

  for (std::size_t index = 0; index < manifest.partitions.size(); ++index) {
    const auto& partition = manifest.partitions[index];
    const auto record = bytes.subspan(
        kShrinkImageManifestHeaderSize +
            index * kShrinkImageManifestRecordSize,
        kShrinkImageManifestRecordSize);
    write_little(record, 0, partition.source_table_index);
    write_little(record, 4, static_cast<std::uint16_t>(partition.role));
    write_little(
        record, 6, static_cast<std::uint16_t>(partition.file_system));
    write_little(record, 8, partition.source_size_bytes);
    write_little(record, 16, partition.used_bytes);
    write_little(record, 24, partition.cluster_size);
    const bool payload = !partition.payload_file_name.empty();
    write_little(
        record,
        32,
        (partition.active ? kFlagActive : 0U) |
            (payload ? kFlagPayloadPresent : 0U));
    write_little(record, 40, partition.payload_length_bytes);
    std::copy(
        partition.payload_sha256.begin(),
        partition.payload_sha256.end(),
        record.begin() + 48);
    write_descriptor(
        record,
        80,
        StringDescriptor{
            .offset = next_string,
            .length = static_cast<std::uint32_t>(
                partition.payload_file_name.size()),
        });
    std::copy(
        std::as_bytes(std::span(partition.payload_file_name)).begin(),
        std::as_bytes(std::span(partition.payload_file_name)).end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(next_string));
    next_string += partition.payload_file_name.size();
    write_descriptor(
        record,
        96,
        StringDescriptor{
            .offset = next_string,
            .length = static_cast<std::uint32_t>(
                strings.value().labels[index].size()),
        });
    std::copy(
        std::as_bytes(std::span(strings.value().labels[index])).begin(),
        std::as_bytes(std::span(strings.value().labels[index])).end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(next_string));
    next_string += strings.value().labels[index].size();
  }
  return clonecore::Result<std::vector<std::byte>>::success(std::move(output));
}

clonecore::Result<ShrinkImageManifest> inspect_shrink_image_manifest_v1(
    const std::span<const std::byte> bytes) {
  if (bytes.size() < kShrinkImageManifestHeaderSize ||
      bytes.size() > kMaximumShrinkImageManifestBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
      read_little<std::uint16_t>(bytes, 8) !=
          kShrinkImageManifestMajorVersion ||
      read_little<std::uint16_t>(bytes, 10) !=
          kShrinkImageManifestMinorVersion ||
      read_little<std::uint32_t>(bytes, 12) !=
          kShrinkImageManifestHeaderSize ||
      read_little<std::uint64_t>(bytes, 16) != bytes.size() ||
      read_little<std::uint32_t>(bytes, 68) !=
          kShrinkImageManifestRecordSize ||
      !all_zero(bytes.subspan(50, 2)) ||
      !all_zero(bytes.subspan(200, 56))) {
    return failure<ShrinkImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"縮小移行マニフェストヘッダー",
        L"マジック、版、長さ、または予約領域が不正です");
  }
  const std::uint32_t flags = read_little<std::uint32_t>(bytes, 24);
  const std::uint32_t partition_count =
      read_little<std::uint32_t>(bytes, 64);
  const std::uint64_t expected_strings = kShrinkImageManifestHeaderSize +
      static_cast<std::uint64_t>(partition_count) *
          kShrinkImageManifestRecordSize;
  if ((flags & ~kKnownHeaderFlags) != 0U || partition_count == 0U ||
      partition_count > kMaximumPartitions ||
      read_little<std::uint64_t>(bytes, 72) !=
          kShrinkImageManifestHeaderSize ||
      read_little<std::uint64_t>(bytes, 80) != expected_strings ||
      expected_strings > bytes.size()) {
    return failure<ShrinkImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"縮小移行マニフェスト配置",
        L"フラグ、件数、または文字列位置が不正です");
  }

  std::uint64_t next_string = expected_strings;
  std::array<std::string, 6> header_strings;
  for (std::size_t index = 0; index < header_strings.size(); ++index) {
    auto value = read_string(
        bytes, read_descriptor(bytes, 88U + index * 16U), next_string);
    if (!value) {
      return clonecore::Result<ShrinkImageManifest>::failure(value.error());
    }
    header_strings[index] = value.take_value();
  }
  const auto bootstrap_descriptor = read_descriptor(bytes, 184);
  const auto style = static_cast<migrationcore::MigrationPartitionStyle>(
      read_little<std::uint16_t>(bytes, 48));
  const std::uint32_t expected_bootstrap_length =
      style == migrationcore::MigrationPartitionStyle::mbr ? 440U : 0U;
  if (bootstrap_descriptor.offset != next_string ||
      bootstrap_descriptor.length != expected_bootstrap_length ||
      bootstrap_descriptor.offset > bytes.size() ||
      bootstrap_descriptor.length >
          bytes.size() - bootstrap_descriptor.offset) {
    return failure<ShrinkImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"縮小移行MBRブートコード境界",
        L"MBRブートコードの位置または長さが不正です");
  }
  std::array<std::byte, 440> mbr_bootstrap{};
  if (expected_bootstrap_length != 0U) {
    std::copy_n(
        bytes.begin() +
            static_cast<std::ptrdiff_t>(bootstrap_descriptor.offset),
        mbr_bootstrap.size(),
        mbr_bootstrap.begin());
    next_string += mbr_bootstrap.size();
  }
  auto model = from_utf8(header_strings[3]);
  auto device = from_utf8(header_strings[5]);
  if (!model) {
    return clonecore::Result<ShrinkImageManifest>::failure(model.error());
  }
  if (!device) {
    return clonecore::Result<ShrinkImageManifest>::failure(device.error());
  }
  ShrinkImageManifest manifest{
      .source = clonecore::StableDiskIdentity{
          .disk_number = read_little<std::uint32_t>(bytes, 28),
          .model = model.take_value(),
          .size_bytes = read_little<std::uint64_t>(bytes, 32),
          .logical_sector_size = read_little<std::uint32_t>(bytes, 40),
          .serial_suffix = header_strings[4],
          .device_instance_id = device.take_value(),
          .is_system_disk = (flags & kFlagSystemDisk) != 0U,
      },
      .physical_sector_size = read_little<std::uint32_t>(bytes, 44),
      .partition_style = style,
      .windows_major = read_little<std::uint32_t>(bytes, 52),
      .windows_minor = read_little<std::uint32_t>(bytes, 56),
      .windows_build = read_little<std::uint32_t>(bytes, 60),
      .windows_architecture = header_strings[2],
      .bitlocker_fully_decrypted =
          (flags & kFlagBitLockerDecrypted) != 0U,
      .created_utc = header_strings[0],
      .app_version = header_strings[1],
      .mbr_bootstrap = mbr_bootstrap,
  };
  manifest.partitions.reserve(partition_count);
  for (std::uint32_t index = 0; index < partition_count; ++index) {
    const auto record = bytes.subspan(
        kShrinkImageManifestHeaderSize +
            static_cast<std::size_t>(index) * kShrinkImageManifestRecordSize,
        kShrinkImageManifestRecordSize);
    const std::uint32_t partition_flags =
        read_little<std::uint32_t>(record, 32);
    if ((partition_flags & ~kKnownPartitionFlags) != 0U ||
        !all_zero(record.subspan(36, 4)) ||
        !all_zero(record.subspan(112, 48))) {
      return failure<ShrinkImageManifest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"縮小移行パーティションレコード",
          L"フラグまたは予約領域が不正です");
    }
    auto payload = read_string(
        bytes, read_descriptor(record, 80), next_string);
    auto label_bytes = read_string(
        bytes, read_descriptor(record, 96), next_string);
    if (!payload) {
      return clonecore::Result<ShrinkImageManifest>::failure(payload.error());
    }
    if (!label_bytes) {
      return clonecore::Result<ShrinkImageManifest>::failure(
          label_bytes.error());
    }
    auto label = from_utf8(label_bytes.value());
    if (!label) {
      return clonecore::Result<ShrinkImageManifest>::failure(label.error());
    }
    ShrinkImagePartition partition{
        .source_table_index = read_little<std::uint32_t>(record, 0),
        .role = static_cast<migrationcore::MigrationPartitionRole>(
            read_little<std::uint16_t>(record, 4)),
        .file_system = static_cast<migrationcore::MigrationFileSystem>(
            read_little<std::uint16_t>(record, 6)),
        .source_size_bytes = read_little<std::uint64_t>(record, 8),
        .used_bytes = read_little<std::uint64_t>(record, 16),
        .cluster_size = read_little<std::uint64_t>(record, 24),
        .active = (partition_flags & kFlagActive) != 0U,
        .label = label.take_value(),
        .payload_file_name = payload.take_value(),
        .payload_length_bytes = read_little<std::uint64_t>(record, 40),
    };
    std::copy_n(
        record.begin() + 48,
        partition.payload_sha256.size(),
        partition.payload_sha256.begin());
    if (((partition_flags & kFlagPayloadPresent) != 0U) !=
        !partition.payload_file_name.empty()) {
      return failure<ShrinkImageManifest>(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"縮小移行ペイロードフラグ",
          L"WIMファイル名と存在フラグが一致しません");
    }
    manifest.partitions.push_back(std::move(partition));
  }
  if (next_string != bytes.size()) {
    return failure<ShrinkImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"縮小移行マニフェスト末尾",
        L"未参照データまたは末尾追加があります");
  }
  auto encoded_strings = encode_strings(manifest);
  if (!encoded_strings) {
    return clonecore::Result<ShrinkImageManifest>::failure(
        encoded_strings.error());
  }
  const auto valid = validate_manifest(manifest, encoded_strings.value());
  if (!valid) {
    return clonecore::Result<ShrinkImageManifest>::failure(valid.error());
  }
  const auto canonical = build_shrink_image_manifest_v1(manifest);
  if (!canonical) {
    return clonecore::Result<ShrinkImageManifest>::failure(canonical.error());
  }
  if (!equal_bytes(bytes, canonical.value())) {
    return failure<ShrinkImageManifest>(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"縮小移行マニフェスト正規形",
        L"項目順、文字列配置、または予約領域が正規形と一致しません");
  }
  return clonecore::Result<ShrinkImageManifest>::success(std::move(manifest));
}

}  // namespace ytec::imageformat

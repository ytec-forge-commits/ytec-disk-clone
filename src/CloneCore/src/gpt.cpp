#include "ytec/clonecore/gpt.h"

#include "ytec/clonecore/crc32.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace ytec::clonecore {
namespace {

constexpr std::size_t kMinimumHeaderSize = 92;
constexpr std::uint32_t kSupportedEntrySize = 128;
constexpr std::uint32_t kMaximumEntryCount = 16'384;
constexpr std::array<std::byte, 8> kGptSignature{
    std::byte{'E'}, std::byte{'F'}, std::byte{'I'}, std::byte{' '},
    std::byte{'P'}, std::byte{'A'}, std::byte{'R'}, std::byte{'T'}};

struct ParsedHeader final {
  std::uint32_t header_size{};
  std::uint64_t current_lba{};
  std::uint64_t backup_lba{};
  std::uint64_t first_usable_lba{};
  std::uint64_t last_usable_lba{};
  GptGuid disk_guid;
  std::uint64_t entry_lba{};
  std::uint32_t entry_count{};
  std::uint32_t entry_size{};
  std::uint32_t entry_crc{};
};

Error data_error(std::wstring operation, std::wstring message) {
  return Error{
      .code = ErrorCode::invalid_data,
      .native_code = ERROR_INVALID_DATA,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

Error unsupported_error(std::wstring operation, std::wstring message) {
  return Error{
      .code = ErrorCode::unsupported_layout,
      .native_code = ERROR_NOT_SUPPORTED,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

template <typename T>
T read_little(const std::span<const std::byte> bytes, const std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

template <typename T>
void write_little(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

Result<std::vector<std::byte>> read_exact(
    const ISourceDiskReader& reader,
    const std::uint64_t offset,
    const std::size_t length,
    const std::wstring_view operation) {
  const auto data = reader.read(offset, length);
  if (!data) {
    return Result<std::vector<std::byte>>::failure(data.error());
  }
  if (data.value().size() != length) {
    return Result<std::vector<std::byte>>::failure(data_error(
        std::wstring(operation), L"要求したバイト数を読み取れませんでした"));
  }
  return Result<std::vector<std::byte>>::success(data.value());
}

Result<ParsedHeader> parse_header(
    const ISourceDiskReader& reader,
    const std::uint64_t lba,
    const std::uint32_t sector_size) {
  std::uint64_t offset{};
  if (!checked_multiply(lba, sector_size, offset)) {
    return Result<ParsedHeader>::failure(
        data_error(L"GPTヘッダー位置", L"LBAからバイト位置への変換がオーバーフローしました"));
  }
  const auto sector_result =
      read_exact(reader, offset, sector_size, L"GPTヘッダー読取り");
  if (!sector_result) {
    return Result<ParsedHeader>::failure(sector_result.error());
  }
  const auto& sector = sector_result.value();
  if (!std::equal(kGptSignature.begin(), kGptSignature.end(), sector.begin())) {
    return Result<ParsedHeader>::failure(
        data_error(L"GPT署名検証", L"EFI PART署名がありません"));
  }
  const std::span<const std::byte> bytes(sector);
  const std::uint32_t revision = read_little<std::uint32_t>(bytes, 8);
  const std::uint32_t header_size = read_little<std::uint32_t>(bytes, 12);
  if (revision != 0x00010000U || header_size < kMinimumHeaderSize ||
      header_size > sector_size) {
    return Result<ParsedHeader>::failure(data_error(
        L"GPTヘッダー形式", L"未対応のGPTリビジョンまたはヘッダーサイズです"));
  }
  const std::uint32_t stored_crc = read_little<std::uint32_t>(bytes, 16);
  std::vector<std::byte> crc_bytes(
      sector.begin(), sector.begin() + static_cast<std::ptrdiff_t>(header_size));
  std::fill(crc_bytes.begin() + 16, crc_bytes.begin() + 20, std::byte{0});
  if (crc32(crc_bytes) != stored_crc) {
    return Result<ParsedHeader>::failure(
        data_error(L"GPTヘッダーCRC", L"GPTヘッダーCRCが一致しません"));
  }
  if (read_little<std::uint32_t>(bytes, 20) != 0) {
    return Result<ParsedHeader>::failure(
        data_error(L"GPT予約領域", L"GPTヘッダーの予約領域が0ではありません"));
  }

  ParsedHeader header;
  header.header_size = header_size;
  header.current_lba = read_little<std::uint64_t>(bytes, 24);
  header.backup_lba = read_little<std::uint64_t>(bytes, 32);
  header.first_usable_lba = read_little<std::uint64_t>(bytes, 40);
  header.last_usable_lba = read_little<std::uint64_t>(bytes, 48);
  std::copy_n(bytes.begin() + 56, 16, header.disk_guid.bytes.begin());
  header.entry_lba = read_little<std::uint64_t>(bytes, 72);
  header.entry_count = read_little<std::uint32_t>(bytes, 80);
  header.entry_size = read_little<std::uint32_t>(bytes, 84);
  header.entry_crc = read_little<std::uint32_t>(bytes, 88);
  return Result<ParsedHeader>::success(std::move(header));
}

Result<std::vector<std::byte>> read_entry_array(
    const ISourceDiskReader& reader,
    const ParsedHeader& header,
    const std::uint32_t sector_size,
    const std::uint64_t sector_count) {
  if (header.entry_count == 0 || header.entry_count > kMaximumEntryCount ||
      header.entry_size != kSupportedEntrySize) {
    return Result<std::vector<std::byte>>::failure(unsupported_error(
        L"GPTパーティション配列",
        L"パーティション項目数または項目サイズが安全な対応範囲外です"));
  }
  std::uint64_t byte_count{};
  std::uint64_t offset{};
  std::uint64_t end_offset{};
  if (!checked_multiply(header.entry_count, header.entry_size, byte_count) ||
      byte_count > std::numeric_limits<std::size_t>::max() ||
      !checked_multiply(header.entry_lba, sector_size, offset) ||
      !checked_add(offset, byte_count, end_offset) ||
      end_offset > reader.size_bytes() || header.entry_lba >= sector_count) {
    return Result<std::vector<std::byte>>::failure(
        data_error(L"GPTパーティション配列境界", L"パーティション配列がディスク境界外です"));
  }
  const auto result = read_exact(
      reader,
      offset,
      static_cast<std::size_t>(byte_count),
      L"GPTパーティション配列読取り");
  if (!result) {
    return result;
  }
  if (crc32(result.value()) != header.entry_crc) {
    return Result<std::vector<std::byte>>::failure(
        data_error(L"GPTパーティション配列CRC", L"パーティション配列CRCが一致しません"));
  }
  return result;
}

std::u16string read_partition_name(const std::span<const std::byte> entry) {
  std::u16string name;
  name.reserve(36);
  for (std::size_t offset = 56; offset < 128; offset += sizeof(char16_t)) {
    const char16_t character = read_little<char16_t>(entry, offset);
    if (character == u'\0') {
      break;
    }
    name.push_back(character);
  }
  return name;
}

Result<std::vector<GptPartition>> parse_partitions(
    const std::vector<std::byte>& entries,
    const ParsedHeader& header) {
  std::vector<GptPartition> partitions;
  std::set<std::array<std::byte, 16>> unique_guids;
  for (std::uint32_t index = 0; index < header.entry_count; ++index) {
    const std::size_t offset = static_cast<std::size_t>(index) * header.entry_size;
    const std::span<const std::byte> entry(
        entries.data() + offset, header.entry_size);
    GptGuid type;
    std::copy_n(entry.begin(), 16, type.bytes.begin());
    if (type.is_zero()) {
      continue;
    }

    GptPartition partition;
    partition.entry_index = index;
    partition.type_guid = type;
    std::copy_n(entry.begin() + 16, 16, partition.unique_guid.bytes.begin());
    partition.first_lba = read_little<std::uint64_t>(entry, 32);
    partition.last_lba = read_little<std::uint64_t>(entry, 40);
    partition.attributes = read_little<std::uint64_t>(entry, 48);
    partition.name = read_partition_name(entry);
    if (partition.unique_guid.is_zero() ||
        !unique_guids.insert(partition.unique_guid.bytes).second) {
      return Result<std::vector<GptPartition>>::failure(data_error(
          L"GPTパーティションGUID", L"パーティションGUIDが空または重複しています"));
    }
    if (partition.first_lba > partition.last_lba ||
        partition.first_lba < header.first_usable_lba ||
        partition.last_lba > header.last_usable_lba) {
      return Result<std::vector<GptPartition>>::failure(data_error(
          L"GPTパーティション境界", L"パーティションが使用可能LBA範囲外です"));
    }
    partitions.push_back(std::move(partition));
  }
  std::vector<const GptPartition*> ordered;
  ordered.reserve(partitions.size());
  for (const auto& partition : partitions) {
    ordered.push_back(&partition);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
    return left->first_lba < right->first_lba;
  });
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    if (ordered[index - 1]->last_lba >= ordered[index]->first_lba) {
      return Result<std::vector<GptPartition>>::failure(
          data_error(L"GPTパーティション重複", L"パーティション範囲が重複しています"));
    }
  }
  return Result<std::vector<GptPartition>>::success(std::move(partitions));
}

std::vector<std::byte> build_entry_array(const GptDisk& disk) {
  const std::size_t byte_count =
      static_cast<std::size_t>(disk.partition_entry_count) *
      disk.partition_entry_size;
  std::vector<std::byte> entries(byte_count, std::byte{0});
  for (const auto& partition : disk.partitions) {
    const std::size_t offset =
        static_cast<std::size_t>(partition.entry_index) *
        disk.partition_entry_size;
    std::span<std::byte> entry(
        entries.data() + offset, disk.partition_entry_size);
    std::copy(
        partition.type_guid.bytes.begin(),
        partition.type_guid.bytes.end(),
        entry.begin());
    std::copy(
        partition.unique_guid.bytes.begin(),
        partition.unique_guid.bytes.end(),
        entry.begin() + 16);
    write_little(entry, 32, partition.first_lba);
    write_little(entry, 40, partition.last_lba);
    write_little(entry, 48, partition.attributes);
    const std::size_t name_length = std::min<std::size_t>(36, partition.name.size());
    for (std::size_t index = 0; index < name_length; ++index) {
      write_little(entry, 56 + index * sizeof(char16_t), partition.name[index]);
    }
  }
  return entries;
}

std::vector<std::byte> build_header(
    const GptDisk& disk,
    const std::uint64_t current_lba,
    const std::uint64_t backup_lba,
    const std::uint64_t entry_lba,
    const std::uint32_t entry_crc) {
  std::vector<std::byte> sector(disk.logical_sector_size, std::byte{0});
  std::span<std::byte> bytes(sector);
  std::copy(kGptSignature.begin(), kGptSignature.end(), bytes.begin());
  write_little(bytes, 8, 0x00010000U);
  write_little(bytes, 12, static_cast<std::uint32_t>(kMinimumHeaderSize));
  write_little(bytes, 16, 0U);
  write_little(bytes, 20, 0U);
  write_little(bytes, 24, current_lba);
  write_little(bytes, 32, backup_lba);
  write_little(bytes, 40, disk.first_usable_lba);
  write_little(bytes, 48, disk.last_usable_lba);
  std::copy(disk.disk_guid.bytes.begin(), disk.disk_guid.bytes.end(), bytes.begin() + 56);
  write_little(bytes, 72, entry_lba);
  write_little(bytes, 80, disk.partition_entry_count);
  write_little(bytes, 84, disk.partition_entry_size);
  write_little(bytes, 88, entry_crc);
  const std::uint32_t header_crc = crc32(
      std::span<const std::byte>(sector.data(), kMinimumHeaderSize));
  write_little(bytes, 16, header_crc);
  return sector;
}

std::vector<std::byte> build_protective_mbr(const GptDisk& disk) {
  std::vector<std::byte> mbr(disk.logical_sector_size, std::byte{0});
  std::span<std::byte> bytes(mbr);
  bytes[446 + 4] = std::byte{0xEE};
  write_little(bytes, 446 + 8, 1U);
  const std::uint64_t protected_sectors = disk.sector_count - 1;
  write_little(
      bytes,
      446 + 12,
      static_cast<std::uint32_t>(std::min<std::uint64_t>(
          protected_sectors, std::numeric_limits<std::uint32_t>::max())));
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};
  return mbr;
}

const GptGuid kEfiSystem{{
    std::byte{0x28}, std::byte{0x73}, std::byte{0x2A}, std::byte{0xC1},
    std::byte{0x1F}, std::byte{0xF8}, std::byte{0xD2}, std::byte{0x11},
    std::byte{0xBA}, std::byte{0x4B}, std::byte{0x00}, std::byte{0xA0},
    std::byte{0xC9}, std::byte{0x3E}, std::byte{0xC9}, std::byte{0x3B}}};
const GptGuid kMicrosoftReserved{{
    std::byte{0x16}, std::byte{0xE3}, std::byte{0xC9}, std::byte{0xE3},
    std::byte{0x5C}, std::byte{0x0B}, std::byte{0xB8}, std::byte{0x4D},
    std::byte{0x81}, std::byte{0x7D}, std::byte{0xF9}, std::byte{0x2D},
    std::byte{0xF0}, std::byte{0x02}, std::byte{0x15}, std::byte{0xAE}}};
const GptGuid kBasicData{{
    std::byte{0xA2}, std::byte{0xA0}, std::byte{0xD0}, std::byte{0xEB},
    std::byte{0xE5}, std::byte{0xB9}, std::byte{0x33}, std::byte{0x44},
    std::byte{0x87}, std::byte{0xC0}, std::byte{0x68}, std::byte{0xB6},
    std::byte{0xB7}, std::byte{0x26}, std::byte{0x99}, std::byte{0xC7}}};
const GptGuid kWindowsRecovery{{
    std::byte{0xA4}, std::byte{0xBB}, std::byte{0x94}, std::byte{0xDE},
    std::byte{0xD1}, std::byte{0x06}, std::byte{0x40}, std::byte{0x4D},
    std::byte{0xA1}, std::byte{0x6A}, std::byte{0xBF}, std::byte{0xD5},
    std::byte{0x01}, std::byte{0x79}, std::byte{0xD6}, std::byte{0xAC}}};

}  // namespace

bool GptGuid::is_zero() const noexcept {
  return std::all_of(bytes.begin(), bytes.end(), [](const std::byte byte) {
    return byte == std::byte{0};
  });
}

Result<GptDisk> parse_gpt(const ISourceDiskReader& reader) {
  const std::uint32_t sector_size = reader.logical_sector_size();
  if ((sector_size != 512 && sector_size != 4096) ||
      reader.size_bytes() < static_cast<std::uint64_t>(sector_size) * 68U ||
      reader.size_bytes() % sector_size != 0) {
    return Result<GptDisk>::failure(unsupported_error(
        L"GPTディスク寸法", L"対応外のセクターサイズまたはディスク寸法です"));
  }
  const std::uint64_t sector_count = reader.size_bytes() / sector_size;
  const auto mbr_result = read_exact(reader, 0, sector_size, L"保護MBR読取り");
  if (!mbr_result) {
    return Result<GptDisk>::failure(mbr_result.error());
  }
  const auto& mbr = mbr_result.value();
  if (mbr[510] != std::byte{0x55} || mbr[511] != std::byte{0xAA} ||
      mbr[446 + 4] != std::byte{0xEE} ||
      read_little<std::uint32_t>(mbr, 446 + 8) != 1 ||
      read_little<std::uint32_t>(mbr, 446 + 12) == 0) {
    return Result<GptDisk>::failure(
        data_error(L"保護MBR検証", L"有効なGPT保護MBRではありません"));
  }

  const auto primary_result = parse_header(reader, 1, sector_size);
  if (!primary_result) {
    return Result<GptDisk>::failure(primary_result.error());
  }
  const ParsedHeader& primary = primary_result.value();
  if (primary.entry_count == 0 || primary.entry_count > kMaximumEntryCount ||
      primary.entry_size != kSupportedEntrySize) {
    return Result<GptDisk>::failure(unsupported_error(
        L"GPTパーティション配列形式",
        L"パーティション項目数または項目サイズが安全な対応範囲外です"));
  }
  std::uint64_t entry_bytes{};
  std::uint64_t rounded_entry_bytes{};
  if (!checked_multiply(
          primary.entry_count, primary.entry_size, entry_bytes) ||
      !checked_add(entry_bytes, sector_size - 1, rounded_entry_bytes)) {
    return Result<GptDisk>::failure(data_error(
        L"GPTパーティション配列寸法",
        L"パーティション配列の寸法がオーバーフローしました"));
  }
  const std::uint64_t entry_sectors =
      rounded_entry_bytes / sector_size;
  if (primary.current_lba != 1 || primary.backup_lba != sector_count - 1 ||
      primary.first_usable_lba > primary.last_usable_lba ||
      primary.last_usable_lba >= primary.backup_lba ||
      primary.disk_guid.is_zero() || primary.entry_lba != 2 ||
      primary.first_usable_lba < primary.entry_lba + entry_sectors) {
    return Result<GptDisk>::failure(
        data_error(L"プライマリGPT境界", L"プライマリGPTヘッダーのLBA関係が不正です"));
  }
  const auto primary_entries_result =
      read_entry_array(reader, primary, sector_size, sector_count);
  if (!primary_entries_result) {
    return Result<GptDisk>::failure(primary_entries_result.error());
  }
  const auto backup_result =
      parse_header(reader, primary.backup_lba, sector_size);
  if (!backup_result) {
    return Result<GptDisk>::failure(backup_result.error());
  }
  const ParsedHeader& backup = backup_result.value();
  const bool headers_match =
      backup.current_lba == primary.backup_lba && backup.backup_lba == 1 &&
      backup.first_usable_lba == primary.first_usable_lba &&
      backup.last_usable_lba == primary.last_usable_lba &&
      backup.disk_guid == primary.disk_guid &&
      backup.entry_count == primary.entry_count &&
      backup.entry_size == primary.entry_size &&
      backup.entry_crc == primary.entry_crc &&
      backup.entry_lba == primary.backup_lba - entry_sectors &&
      primary.last_usable_lba < backup.entry_lba;
  if (!headers_match) {
    return Result<GptDisk>::failure(data_error(
        L"バックアップGPT照合", L"プライマリとバックアップGPTが一致しません"));
  }
  const auto backup_entries_result =
      read_entry_array(reader, backup, sector_size, sector_count);
  if (!backup_entries_result) {
    return Result<GptDisk>::failure(backup_entries_result.error());
  }
  if (backup_entries_result.value() != primary_entries_result.value()) {
    return Result<GptDisk>::failure(data_error(
        L"GPT配列二重化", L"プライマリとバックアップのパーティション配列が一致しません"));
  }
  const auto partitions_result =
      parse_partitions(primary_entries_result.value(), primary);
  if (!partitions_result) {
    return Result<GptDisk>::failure(partitions_result.error());
  }

  GptDisk disk;
  disk.logical_sector_size = sector_size;
  disk.sector_count = sector_count;
  disk.disk_guid = primary.disk_guid;
  disk.first_usable_lba = primary.first_usable_lba;
  disk.last_usable_lba = primary.last_usable_lba;
  disk.partition_entry_count = primary.entry_count;
  disk.partition_entry_size = primary.entry_size;
  disk.partitions = partitions_result.value();
  return Result<GptDisk>::success(std::move(disk));
}

Result<GptWritePlan> make_gpt_write_plan(
    const GptDisk& source,
    const std::uint64_t target_size_bytes,
    const std::uint32_t target_sector_size,
    IGuidGenerator& guid_generator) {
  if ((target_sector_size != 512 && target_sector_size != 4096) ||
      target_sector_size != source.logical_sector_size ||
      target_size_bytes % target_sector_size != 0 ||
      source.partition_entry_size != kSupportedEntrySize ||
      source.partition_entry_count == 0 ||
      source.partition_entry_count > kMaximumEntryCount) {
    return Result<GptWritePlan>::failure(unsupported_error(
        L"コピー先GPT計画", L"コピー先寸法またはGPT配列形式が対応条件外です"));
  }
  std::uint64_t source_size_bytes{};
  if (!checked_multiply(
          source.sector_count,
          source.logical_sector_size,
          source_size_bytes) ||
      target_size_bytes < source_size_bytes) {
    return Result<GptWritePlan>::failure(unsupported_error(
        L"コピー先GPT容量", L"コピー先ディスクがコピー元より小さいか寸法が不正です"));
  }

  GptDisk target = source;
  target.sector_count = target_size_bytes / target_sector_size;
  const std::uint64_t entry_bytes =
      static_cast<std::uint64_t>(target.partition_entry_count) *
      target.partition_entry_size;
  const std::uint64_t entry_sectors =
      (entry_bytes + target_sector_size - 1) / target_sector_size;
  if (target.sector_count <= 3 + entry_sectors * 2) {
    return Result<GptWritePlan>::failure(
        data_error(L"コピー先GPT領域", L"GPTメタデータを配置する容量がありません"));
  }
  target.first_usable_lba = std::max<std::uint64_t>(
      source.first_usable_lba, 2 + entry_sectors);
  const std::uint64_t backup_entry_lba =
      target.sector_count - 1 - entry_sectors;
  target.last_usable_lba = backup_entry_lba - 1;
  for (const auto& partition : target.partitions) {
    if (partition.first_lba < target.first_usable_lba ||
        partition.last_lba > target.last_usable_lba) {
      return Result<GptWritePlan>::failure(data_error(
          L"コピー先パーティション配置",
          L"コピー元パーティションをコピー先の使用可能範囲へ配置できません"));
    }
  }

  const auto disk_guid_result = guid_generator.next_guid();
  if (!disk_guid_result || disk_guid_result.value().is_zero()) {
    return Result<GptWritePlan>::failure(
        disk_guid_result ? data_error(L"コピー先Disk GUID", L"空のGUIDは使用できません")
                         : disk_guid_result.error());
  }
  target.disk_guid = disk_guid_result.value();
  std::set<std::array<std::byte, 16>> generated_guids;
  generated_guids.insert(target.disk_guid.bytes);
  for (auto& partition : target.partitions) {
    const auto guid_result = guid_generator.next_guid();
    if (!guid_result || guid_result.value().is_zero() ||
        !generated_guids.insert(guid_result.value().bytes).second) {
      return Result<GptWritePlan>::failure(
          guid_result ? data_error(
                            L"コピー先Partition GUID",
                            L"空または重複したGUIDは使用できません")
                      : guid_result.error());
    }
    partition.unique_guid = guid_result.value();
  }

  std::vector<std::byte> entries = build_entry_array(target);
  const std::uint32_t entry_crc = crc32(entries);
  std::vector<std::byte> primary_header =
      build_header(target, 1, target.sector_count - 1, 2, entry_crc);
  std::vector<std::byte> backup_header = build_header(
      target, target.sector_count - 1, 1, backup_entry_lba, entry_crc);
  std::vector<std::byte> padded_entries(
      static_cast<std::size_t>(entry_sectors * target_sector_size),
      std::byte{0});
  std::copy(entries.begin(), entries.end(), padded_entries.begin());

  GptWritePlan plan;
  plan.target_disk = target;
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::primary_entries,
      .offset = static_cast<std::uint64_t>(target_sector_size) * 2,
      .bytes = padded_entries,
  });
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::backup_entries,
      .offset = backup_entry_lba * target_sector_size,
      .bytes = padded_entries,
  });
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::backup_header,
      .offset = (target.sector_count - 1) * target_sector_size,
      .bytes = std::move(backup_header),
  });
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::protective_mbr,
      .offset = 0,
      .bytes = build_protective_mbr(target),
  });
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::primary_header_commit,
      .offset = target_sector_size,
      .bytes = std::move(primary_header),
  });
  return Result<GptWritePlan>::success(std::move(plan));
}

Result<GptWritePlan> make_gpt_add_partition_plan(
    const GptDisk& current,
    const GptAddPartitionRequest& request,
    IGuidGenerator& guid_generator) {
  if ((current.logical_sector_size != 512U &&
       current.logical_sector_size != 4096U) ||
      current.sector_count == 0U || current.disk_guid.is_zero() ||
      current.partition_entry_size != kSupportedEntrySize ||
      current.partition_entry_count == 0U ||
      current.partition_entry_count > kMaximumEntryCount ||
      request.sector_count == 0U || request.type_guid.is_zero() ||
      request.name.size() > 36U) {
    return Result<GptWritePlan>::failure(unsupported_error(
        L"GPT保持型パーティション追加",
        L"既存GPT形式または追加する区画の種類・寸法・名前が対応条件外です"));
  }
  std::uint64_t requested_end{};
  if (!checked_add(
          request.first_lba, request.sector_count - 1U, requested_end) ||
      request.first_lba < current.first_usable_lba ||
      requested_end > current.last_usable_lba) {
    return Result<GptWritePlan>::failure(data_error(
        L"GPT保持型パーティション範囲",
        L"追加する区画が既存GPTの使用可能LBA範囲外です"));
  }

  std::set<std::uint32_t> used_entries;
  std::set<std::array<std::byte, 16>> used_guids;
  used_guids.insert(current.disk_guid.bytes);
  for (const auto& partition : current.partitions) {
    if (partition.entry_index >= current.partition_entry_count ||
        !used_entries.insert(partition.entry_index).second ||
        partition.unique_guid.is_zero() ||
        !used_guids.insert(partition.unique_guid.bytes).second ||
        partition.first_lba > partition.last_lba ||
        partition.first_lba < current.first_usable_lba ||
        partition.last_lba > current.last_usable_lba ||
        (request.first_lba <= partition.last_lba &&
         partition.first_lba <= requested_end)) {
      return Result<GptWritePlan>::failure(data_error(
          L"GPT保持型既存区画照合",
          L"既存entryの識別子・範囲が不正か、追加範囲と重なります"));
    }
  }
  std::optional<std::uint32_t> free_entry;
  for (std::uint32_t index = 0U;
       index < current.partition_entry_count;
       ++index) {
    if (!used_entries.contains(index)) {
      free_entry = index;
      break;
    }
  }
  if (!free_entry.has_value()) {
    return Result<GptWritePlan>::failure(unsupported_error(
        L"GPT保持型空entry",
        L"既存GPTに新しいパーティションentryの空きがありません"));
  }

  GptGuid new_guid{};
  constexpr std::size_t kMaximumAttempts = 32U;
  for (std::size_t attempt = 0U; attempt < kMaximumAttempts; ++attempt) {
    auto generated = guid_generator.next_guid();
    if (!generated) {
      return Result<GptWritePlan>::failure(generated.error());
    }
    if (!generated.value().is_zero() &&
        used_guids.insert(generated.value().bytes).second) {
      new_guid = generated.value();
      break;
    }
  }
  if (new_guid.is_zero()) {
    return Result<GptWritePlan>::failure(data_error(
        L"GPT保持型Partition GUID",
        L"既存識別子と衝突しないPartition GUIDを生成できませんでした"));
  }

  GptDisk target = current;
  target.partitions.push_back(GptPartition{
      .entry_index = free_entry.value(),
      .type_guid = request.type_guid,
      .unique_guid = new_guid,
      .first_lba = request.first_lba,
      .last_lba = requested_end,
      .attributes = request.attributes,
      .name = request.name,
  });
  const std::uint64_t entry_bytes =
      static_cast<std::uint64_t>(target.partition_entry_count) *
      target.partition_entry_size;
  const std::uint64_t entry_sectors =
      (entry_bytes + target.logical_sector_size - 1U) /
      target.logical_sector_size;
  if (target.sector_count <= 2U + entry_sectors ||
      target.last_usable_lba >= target.sector_count - 1U - entry_sectors) {
    return Result<GptWritePlan>::failure(data_error(
        L"GPT保持型メタデータ境界",
        L"既存GPTのbackup entryまたはheader位置を安全に再構成できません"));
  }

  std::vector<std::byte> entries = build_entry_array(target);
  const std::uint32_t entry_crc = crc32(entries);
  const std::uint64_t backup_entry_lba =
      target.sector_count - 1U - entry_sectors;
  std::vector<std::byte> padded_entries(
      static_cast<std::size_t>(
          entry_sectors * target.logical_sector_size),
      std::byte{0});
  std::copy(entries.begin(), entries.end(), padded_entries.begin());

  GptWritePlan plan;
  plan.target_disk = target;
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::primary_entries,
      .offset = 2ULL * target.logical_sector_size,
      .bytes = padded_entries,
  });
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::backup_entries,
      .offset = backup_entry_lba * target.logical_sector_size,
      .bytes = padded_entries,
  });
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::backup_header,
      .offset = (target.sector_count - 1U) * target.logical_sector_size,
      .bytes = build_header(
          target,
          target.sector_count - 1U,
          1U,
          backup_entry_lba,
          entry_crc),
  });
  plan.writes.push_back(GptMetadataWrite{
      .kind = GptMetadataKind::primary_header_commit,
      .offset = target.logical_sector_size,
      .bytes = build_header(
          target,
          1U,
          target.sector_count - 1U,
          2U,
          entry_crc),
  });
  return Result<GptWritePlan>::success(std::move(plan));
}

const GptGuid& gpt_type_efi_system() noexcept { return kEfiSystem; }
const GptGuid& gpt_type_microsoft_reserved() noexcept {
  return kMicrosoftReserved;
}
const GptGuid& gpt_type_basic_data() noexcept { return kBasicData; }
const GptGuid& gpt_type_windows_recovery() noexcept {
  return kWindowsRecovery;
}

}  // namespace ytec::clonecore

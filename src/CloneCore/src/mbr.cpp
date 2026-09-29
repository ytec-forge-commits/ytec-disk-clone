#include "ytec/clonecore/mbr.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace ytec::clonecore {
namespace {

constexpr std::size_t kMbrSize = 512;
constexpr std::size_t kDiskSignatureOffset = 440;
constexpr std::size_t kReservedOffset = 444;
constexpr std::size_t kPartitionTableOffset = 446;
constexpr std::size_t kPartitionEntrySize = 16;
constexpr std::size_t kPartitionEntryCount = 4;
constexpr std::uint64_t kMaximumMbrSectorCount =
    static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;

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

bool is_extended_partition_type(const std::uint8_t type) noexcept {
  return type == 0x05 || type == 0x0F || type == 0x85;
}

class WindowsMbrSignatureGenerator final : public IMbrSignatureGenerator {
 public:
  Result<std::uint32_t> next_signature() override {
    std::uint32_t signature{};
    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(&signature),
        sizeof(signature),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
      return Result<std::uint32_t>::failure(Error{
          .code = ErrorCode::internal_error,
          .native_code = static_cast<DWORD>(status),
          .operation = L"MBRディスク署名生成",
          .message = L"Windows CNG乱数生成に失敗しました",
      });
    }
    return Result<std::uint32_t>::success(signature);
  }
};

}  // namespace

std::unique_ptr<IMbrSignatureGenerator> make_windows_mbr_signature_generator() {
  return std::make_unique<WindowsMbrSignatureGenerator>();
}

Result<MbrDisk> parse_mbr(const ISourceDiskReader& reader) {
  if (reader.logical_sector_size() != kMbrSize ||
      reader.size_bytes() < kMbrSize * 2U ||
      reader.size_bytes() % kMbrSize != 0) {
    return Result<MbrDisk>::failure(unsupported_error(
        L"MBRディスク寸法",
        L"Legacy BIOS用MBRは512バイト論理セクターだけを扱います"));
  }
  const std::uint64_t sector_count = reader.size_bytes() / kMbrSize;
  if (sector_count > kMaximumMbrSectorCount) {
    return Result<MbrDisk>::failure(unsupported_error(
        L"MBRディスク容量", L"32bit LBAで表現できる容量を超えています"));
  }
  const auto read_result = reader.read(0, kMbrSize);
  if (!read_result) {
    return Result<MbrDisk>::failure(read_result.error());
  }
  if (read_result.value().size() != kMbrSize) {
    return Result<MbrDisk>::failure(
        data_error(L"MBR読取り", L"512バイトを読み取れませんでした"));
  }
  const std::span<const std::byte> bytes(read_result.value());
  if (bytes[510] != std::byte{0x55} || bytes[511] != std::byte{0xAA}) {
    return Result<MbrDisk>::failure(
        data_error(L"MBR署名", L"0x55AA署名がありません"));
  }
  if (bytes[kReservedOffset] != std::byte{0} ||
      bytes[kReservedOffset + 1] != std::byte{0}) {
    return Result<MbrDisk>::failure(data_error(
        L"MBR予約領域", L"ディスク署名後の予約領域が0ではありません"));
  }

  MbrDisk disk;
  disk.logical_sector_size = kMbrSize;
  disk.sector_count = sector_count;
  disk.disk_signature = read_little<std::uint32_t>(bytes, kDiskSignatureOffset);
  std::copy_n(bytes.begin(), disk.bootstrap.size(), disk.bootstrap.begin());

  std::size_t active_count = 0;
  for (std::size_t index = 0; index < kPartitionEntryCount; ++index) {
    const std::size_t offset =
        kPartitionTableOffset + index * kPartitionEntrySize;
    const auto entry = bytes.subspan(offset, kPartitionEntrySize);
    const std::uint8_t boot_indicator =
        std::to_integer<std::uint8_t>(entry[0]);
    const std::uint8_t type = std::to_integer<std::uint8_t>(entry[4]);
    if (type == 0) {
      if (!std::all_of(entry.begin(), entry.end(), [](const std::byte value) {
            return value == std::byte{0};
          })) {
        return Result<MbrDisk>::failure(data_error(
            L"MBR空エントリ", L"空エントリに残留データがあります"));
      }
      continue;
    }
    if (boot_indicator != 0 && boot_indicator != 0x80) {
      return Result<MbrDisk>::failure(data_error(
          L"MBR Activeフラグ", L"ブートインジケーターが0または0x80ではありません"));
    }
    if (type == 0xEE) {
      return Result<MbrDisk>::failure(unsupported_error(
          L"MBR保護エントリ", L"GPT保護MBRはMBRクローンとして扱いません"));
    }
    if (is_extended_partition_type(type)) {
      return Result<MbrDisk>::failure(unsupported_error(
          L"MBR拡張パーティション",
          L"EBRチェーン解析が未実装のため拡張パーティションを拒否します"));
    }

    MbrPartition partition;
    partition.table_index = static_cast<std::uint8_t>(index);
    partition.active = boot_indicator == 0x80;
    std::copy_n(entry.begin() + 1, partition.first_chs.size(),
                partition.first_chs.begin());
    partition.type = type;
    std::copy_n(entry.begin() + 5, partition.last_chs.size(),
                partition.last_chs.begin());
    partition.first_lba = read_little<std::uint32_t>(entry, 8);
    partition.sector_count = read_little<std::uint32_t>(entry, 12);

    const std::uint64_t end =
        static_cast<std::uint64_t>(partition.first_lba) +
        partition.sector_count;
    if (partition.first_lba == 0 || partition.sector_count == 0 ||
        end > sector_count) {
      return Result<MbrDisk>::failure(data_error(
          L"MBRパーティション境界", L"パーティションがディスク境界外です"));
    }
    if (partition.active) {
      ++active_count;
    }
    disk.partitions.push_back(partition);
  }
  if (active_count > 1) {
    return Result<MbrDisk>::failure(data_error(
        L"MBR Activeパーティション",
        L"複数のActiveパーティションがあり起動対象を確定できません"));
  }

  std::vector<const MbrPartition*> ordered;
  ordered.reserve(disk.partitions.size());
  for (const auto& partition : disk.partitions) {
    ordered.push_back(&partition);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
    return left->first_lba < right->first_lba;
  });
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    const std::uint64_t previous_end =
        static_cast<std::uint64_t>(ordered[index - 1]->first_lba) +
        ordered[index - 1]->sector_count;
    if (previous_end > ordered[index]->first_lba) {
      return Result<MbrDisk>::failure(data_error(
          L"MBRパーティション重複", L"パーティション範囲が重複しています"));
    }
  }
  return Result<MbrDisk>::success(std::move(disk));
}

Result<MbrWritePlan> make_mbr_write_plan(
    const MbrDisk& source,
    const std::uint64_t target_size_bytes,
    const std::uint32_t target_sector_size,
    IMbrSignatureGenerator& signature_generator,
    const std::span<const std::uint32_t> disallowed_signatures,
    const bool require_single_active_partition) {
  if (source.logical_sector_size != kMbrSize ||
      target_sector_size != kMbrSize ||
      target_size_bytes % kMbrSize != 0 ||
      target_size_bytes / kMbrSize < source.sector_count ||
      target_size_bytes / kMbrSize > kMaximumMbrSectorCount) {
    return Result<MbrWritePlan>::failure(unsupported_error(
        L"コピー先MBR寸法",
        L"コピー先は512バイトセクターでコピー元以上かつ32bit LBA範囲が必要です"));
  }
  const std::size_t active_count = static_cast<std::size_t>(std::count_if(
      source.partitions.begin(), source.partitions.end(),
      [](const MbrPartition& partition) { return partition.active; }));
  if (source.partitions.empty() ||
      (require_single_active_partition
           ? active_count != 1U
           : active_count != 0U)) {
    return Result<MbrWritePlan>::failure(unsupported_error(
        L"MBR BIOS起動構成",
        require_single_active_partition
            ? L"BIOS起動MBRには1個のActiveプライマリパーティションが必要です"
            : L"データ専用MBRにActiveパーティションを指定できません"));
  }

  std::set<std::uint32_t> forbidden(
      disallowed_signatures.begin(), disallowed_signatures.end());
  forbidden.insert(0);
  forbidden.insert(source.disk_signature);
  std::uint32_t target_signature{};
  constexpr std::size_t kMaximumAttempts = 32;
  for (std::size_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
    const auto generated = signature_generator.next_signature();
    if (!generated) {
      return Result<MbrWritePlan>::failure(generated.error());
    }
    if (!forbidden.contains(generated.value())) {
      target_signature = generated.value();
      break;
    }
  }
  if (target_signature == 0) {
    return Result<MbrWritePlan>::failure(Error{
        .code = ErrorCode::identity_mismatch,
        .native_code = ERROR_DUP_NAME,
        .operation = L"コピー先MBRディスク署名",
        .message = L"接続中ディスクと衝突しない署名を生成できませんでした",
    });
  }

  MbrDisk target = source;
  target.sector_count = target_size_bytes / kMbrSize;
  target.disk_signature = target_signature;
  std::vector<std::byte> sector(kMbrSize, std::byte{0});
  std::span<std::byte> bytes(sector);
  std::copy(target.bootstrap.begin(), target.bootstrap.end(), bytes.begin());
  write_little(bytes, kDiskSignatureOffset, target.disk_signature);

  for (const auto& partition : target.partitions) {
    if (partition.table_index >= kPartitionEntryCount) {
      return Result<MbrWritePlan>::failure(data_error(
          L"コピー先MBRエントリ", L"パーティション表インデックスが不正です"));
    }
    const std::size_t offset = kPartitionTableOffset +
                               partition.table_index * kPartitionEntrySize;
    bytes[offset] = partition.active ? std::byte{0x80} : std::byte{0};
    std::copy(partition.first_chs.begin(), partition.first_chs.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1));
    bytes[offset + 4] = std::byte{partition.type};
    std::copy(partition.last_chs.begin(), partition.last_chs.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + 5));
    write_little(bytes, offset + 8, partition.first_lba);
    write_little(bytes, offset + 12, partition.sector_count);
  }
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};

  return Result<MbrWritePlan>::success(MbrWritePlan{
      .target_disk = std::move(target),
      .sector = std::move(sector),
  });
}

Result<MbrWritePlan> make_mbr_add_partition_plan(
    const MbrDisk& current,
    const MbrAddPartitionRequest& request) {
  if (current.logical_sector_size != kMbrSize ||
      current.sector_count < 2U ||
      current.sector_count > kMaximumMbrSectorCount ||
      request.first_lba == 0U || request.sector_count == 0U ||
      request.type == 0U || request.type == 0xEEU ||
      is_extended_partition_type(request.type)) {
    return Result<MbrWritePlan>::failure(unsupported_error(
        L"MBR保持型パーティション追加",
        L"既存MBR形式または追加する基本区画の種類・寸法が対応条件外です"));
  }
  const std::uint64_t requested_end =
      static_cast<std::uint64_t>(request.first_lba) +
      request.sector_count;
  if (requested_end > current.sector_count) {
    return Result<MbrWritePlan>::failure(data_error(
        L"MBR保持型パーティション範囲",
        L"追加する基本区画が32bit LBAまたはディスク境界を超えます"));
  }

  std::set<std::uint8_t> used_entries;
  for (const auto& partition : current.partitions) {
    const std::uint64_t partition_end =
        static_cast<std::uint64_t>(partition.first_lba) +
        partition.sector_count;
    if (partition.table_index >= kPartitionEntryCount ||
        !used_entries.insert(partition.table_index).second ||
        partition.first_lba == 0U || partition.sector_count == 0U ||
        partition_end > current.sector_count ||
        partition.type == 0U || partition.type == 0xEEU ||
        is_extended_partition_type(partition.type) ||
        (request.first_lba < partition_end &&
         partition.first_lba < requested_end)) {
      return Result<MbrWritePlan>::failure(data_error(
          L"MBR保持型既存区画照合",
          L"既存entryの種類・範囲が不正か、追加範囲と重なります"));
    }
  }
  std::optional<std::uint8_t> free_entry;
  for (std::size_t index = 0U; index < kPartitionEntryCount; ++index) {
    const auto table_index = static_cast<std::uint8_t>(index);
    if (!used_entries.contains(table_index)) {
      free_entry = table_index;
      break;
    }
  }
  if (!free_entry.has_value()) {
    return Result<MbrWritePlan>::failure(unsupported_error(
        L"MBR保持型空entry",
        L"既存MBRに新しい基本パーティションentryの空きがありません"));
  }

  MbrDisk target = current;
  target.partitions.push_back(MbrPartition{
      .table_index = free_entry.value(),
      .active = false,
      .first_chs = {
          std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}},
      .type = request.type,
      .last_chs = {
          std::byte{0xFE}, std::byte{0xFF}, std::byte{0xFF}},
      .first_lba = request.first_lba,
      .sector_count = request.sector_count,
  });
  std::vector<std::byte> sector(kMbrSize, std::byte{0});
  std::span<std::byte> bytes(sector);
  std::copy(target.bootstrap.begin(), target.bootstrap.end(), bytes.begin());
  write_little(bytes, kDiskSignatureOffset, target.disk_signature);
  for (const auto& partition : target.partitions) {
    const std::size_t offset = kPartitionTableOffset +
        partition.table_index * kPartitionEntrySize;
    bytes[offset] = partition.active ? std::byte{0x80} : std::byte{0};
    std::copy(
        partition.first_chs.begin(),
        partition.first_chs.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1U));
    bytes[offset + 4U] = std::byte{partition.type};
    std::copy(
        partition.last_chs.begin(),
        partition.last_chs.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + 5U));
    write_little(bytes, offset + 8U, partition.first_lba);
    write_little(bytes, offset + 12U, partition.sector_count);
  }
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};
  return Result<MbrWritePlan>::success(MbrWritePlan{
      .target_disk = std::move(target),
      .sector = std::move(sector),
  });
}

}  // namespace ytec::clonecore

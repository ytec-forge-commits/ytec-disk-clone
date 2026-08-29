#include "ytec/clonecore/offline_gpt_clone.h"

#include "verified_write_digest.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace ytec::clonecore {
namespace {

constexpr std::size_t kTargetMetadataInvalidationBytes = 1024U * 1024U;

Error clone_error(
    const ErrorCode code,
    const DWORD native_code,
    std::wstring operation,
    std::wstring message) {
  return Error{
      .code = code,
      .native_code = native_code,
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

bool is_power_of_two(const std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
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

Result<ByteRange> partition_byte_range(
    const GptPartition& partition,
    const std::uint32_t sector_size) {
  std::uint64_t offset{};
  std::uint64_t sector_count{};
  std::uint64_t length{};
  if (!checked_multiply(partition.first_lba, sector_size, offset) ||
      !checked_add(partition.last_lba - partition.first_lba, 1, sector_count) ||
      !checked_multiply(sector_count, sector_size, length)) {
    return Result<ByteRange>::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"パーティション範囲計算",
        L"パーティションのバイト範囲がオーバーフローしました"));
  }
  return Result<ByteRange>::success(ByteRange{.offset = offset, .length = length});
}

Status validate_used_ranges(
    std::vector<ByteRange>& ranges,
    const ByteRange& partition_range,
    const NtfsGeometry& geometry) {
  if (ranges.empty()) {
    return Status::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"NTFS使用クラスタ",
        L"使用クラスタ一覧が空です"));
  }
  std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
    return left.offset < right.offset;
  });
  const std::uint64_t cluster_size = geometry.cluster_size();
  std::uint64_t previous_end{};
  for (std::size_t index = 0; index < ranges.size(); ++index) {
    auto& range = ranges[index];
    std::uint64_t end{};
    if (range.length == 0 || range.offset % cluster_size != 0 ||
        range.length % cluster_size != 0 ||
        !checked_add(range.offset, range.length, end) ||
        end > partition_range.length || (index != 0 && range.offset < previous_end)) {
      return Status::failure(clone_error(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"NTFS使用クラスタ境界",
          L"使用クラスタ範囲が未整列、重複、またはパーティション境界外です"));
    }
    previous_end = end;
    range.offset += partition_range.offset;
  }
  if (ranges.front().offset != partition_range.offset) {
    return Status::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"NTFSブートクラスタ",
        L"使用クラスタ一覧にパーティション先頭クラスタが含まれていません"));
  }
  return success_status();
}

Status write_and_verify(
    ITargetDiskWriter& target,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes,
    const std::wstring_view operation,
    detail::VerifiedWriteDigestBuilder* const digest) {
  const Status write_status = target.write_target(offset, bytes);
  if (!write_status) {
    return write_status;
  }
  const auto read_result = target.read_back(offset, bytes.size());
  if (!read_result) {
    return Status::failure(read_result.error());
  }
  if (read_result.value().size() != bytes.size() ||
      !std::equal(bytes.begin(), bytes.end(), read_result.value().begin())) {
    return Status::failure(clone_error(
        ErrorCode::verification_failed,
        ERROR_CRC,
        std::wstring(operation),
        L"書込み後の読戻し内容が一致しません"));
  }
  if (digest != nullptr) {
    return digest->append_verified_write(offset, read_result.value());
  }
  return success_status();
}

Status cancelled_status(const std::wstring_view operation) {
  return Status::failure(clone_error(
      ErrorCode::cancelled,
      ERROR_CANCELLED,
      std::wstring(operation),
      L"利用者の操作により安全に中止しました"));
}

void publish_progress(
    const DiskOperationCallbacks& callbacks,
    DiskOperationProgress& progress,
    const DiskOperationStage stage,
    const std::optional<std::uint32_t> partition_index,
    const bool cancellation_allowed) noexcept {
  progress.stage = stage;
  progress.partition_index = partition_index;
  progress.cancellation_allowed = cancellation_allowed;
  progress.pause_allowed = stage == DiskOperationStage::copying_data;
  report_disk_operation_progress(callbacks, progress);
}

Status copy_range(
    const ISourceDiskReader& source,
    ITargetDiskWriter& target,
    const ByteRange& range,
    const std::size_t maximum_chunk_bytes,
    const std::uint32_t partition_index,
    const DiskOperationCallbacks& callbacks,
    DiskOperationProgress& progress,
    detail::VerifiedWriteDigestBuilder& digest,
    std::uint64_t& copied_bytes,
    std::uint64_t& verified_chunk_count) {
  std::uint64_t position = 0;
  while (position < range.length) {
    if (disk_operation_cancellation_requested(callbacks)) {
      const Status flush_status = target.flush_target();
      if (!flush_status) {
        return flush_status;
      }
      return cancelled_status(L"GPTクローンのデータコピー");
    }
    const std::uint64_t remaining = range.length - position;
    const std::size_t chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, maximum_chunk_bytes));
    const auto read_result = source.read(range.offset + position, chunk);
    if (!read_result) {
      return Status::failure(read_result.error());
    }
    if (read_result.value().size() != chunk) {
      return Status::failure(clone_error(
          ErrorCode::io_failed,
          ERROR_HANDLE_EOF,
          L"コピー元チャンク読取り",
          L"コピー元から要求したバイト数を読み取れませんでした"));
    }
    const Status write_status = write_and_verify(
        target,
        range.offset + position,
        read_result.value(),
        L"パーティションデータ読戻し検証",
        &digest);
    if (!write_status) {
      return write_status;
    }
    position += chunk;
    copied_bytes += chunk;
    progress.read_bytes = copied_bytes;
    progress.written_bytes = copied_bytes;
    progress.verified_bytes = copied_bytes;
    ++verified_chunk_count;
    publish_progress(
        callbacks,
        progress,
        DiskOperationStage::copying_data,
        partition_index,
        true);
    if (disk_operation_control_at_safe_boundary(
            callbacks,
            DiskOperationSafeBoundary{
                .kind = DiskOperationSafeBoundaryKind::verified_chunk,
                .stage = DiskOperationStage::copying_data,
                .partition_index = partition_index,
                .completed_bytes = copied_bytes,
                .completed_units = verified_chunk_count,
            }) == DiskOperationControlDecision::cancel_operation) {
      const Status flush_status = target.flush_target();
      if (!flush_status) {
        return flush_status;
      }
      return cancelled_status(L"GPTクローンの安全境界");
    }
  }
  return success_status();
}

Result<std::vector<std::byte>> read_boot_sector(
    const ISourceDiskReader& source,
    const ByteRange& partition_range) {
  const auto result = source.read(
      partition_range.offset, source.logical_sector_size());
  if (!result) {
    return result;
  }
  if (result.value().size() != source.logical_sector_size()) {
    return Result<std::vector<std::byte>>::failure(clone_error(
        ErrorCode::io_failed,
        ERROR_HANDLE_EOF,
        L"パーティションブートセクター読取り",
        L"ブートセクターを完全に読み取れませんでした"));
  }
  return result;
}

}  // namespace

Result<NtfsGeometry> parse_ntfs_geometry(
    const std::span<const std::byte> boot_sector,
    const std::uint32_t expected_sector_size,
    const std::uint64_t partition_size_bytes) {
  constexpr char kNtfsSignature[] = "NTFS    ";
  constexpr char kBitLockerSignature[] = "-FVE-FS-";
  if (boot_sector.size() >= 512 &&
      std::memcmp(boot_sector.data() + 3, kBitLockerSignature, 8) == 0) {
    return Result<NtfsGeometry>::failure(clone_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"BitLocker完全復号の確認",
        L"BitLocker形式を検出しました。保護の中断ではなく完全復号が必要です"));
  }
  if (boot_sector.size() < 512 || boot_sector[510] != std::byte{0x55} ||
      boot_sector[511] != std::byte{0xAA} ||
      std::memcmp(boot_sector.data() + 3, kNtfsSignature, 8) != 0) {
    return Result<NtfsGeometry>::failure(clone_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"NTFSブートセクター",
        L"NTFSとして安全に識別できません"));
  }
  NtfsGeometry geometry;
  geometry.bytes_per_sector = read_little<std::uint16_t>(boot_sector, 11);
  geometry.sectors_per_cluster =
      std::to_integer<std::uint8_t>(boot_sector[13]);
  geometry.total_sectors = read_little<std::uint64_t>(boot_sector, 40);
  std::uint64_t volume_bytes{};
  if (geometry.bytes_per_sector != expected_sector_size ||
      !is_power_of_two(geometry.sectors_per_cluster) ||
      geometry.sectors_per_cluster > 128 || geometry.total_sectors == 0 ||
      !checked_multiply(
          geometry.total_sectors, geometry.bytes_per_sector, volume_bytes) ||
      volume_bytes > partition_size_bytes) {
    return Result<NtfsGeometry>::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"NTFSジオメトリ検証",
        L"NTFSのセクター、クラスタ、または容量情報が不正です"));
  }
  return Result<NtfsGeometry>::success(geometry);
}

Result<Fat32Geometry> parse_fat32_geometry(
    const std::span<const std::byte> boot_sector,
    const std::uint32_t expected_sector_size,
    const std::uint64_t partition_size_bytes) {
  constexpr char kFat32Signature[] = "FAT32   ";
  if (boot_sector.size() < 512 || boot_sector[510] != std::byte{0x55} ||
      boot_sector[511] != std::byte{0xAA} ||
      std::memcmp(boot_sector.data() + 82, kFat32Signature, 8) != 0) {
    return Result<Fat32Geometry>::failure(clone_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"EFIパーティション形式",
        L"EFIシステムパーティションをFAT32として識別できません"));
  }
  Fat32Geometry geometry;
  geometry.bytes_per_sector =
      read_little<std::uint16_t>(boot_sector, 11);
  geometry.sectors_per_cluster =
      std::to_integer<std::uint8_t>(boot_sector[13]);
  geometry.total_sectors =
      read_little<std::uint32_t>(boot_sector, 32);
  std::uint64_t volume_bytes{};
  if (geometry.bytes_per_sector != expected_sector_size ||
      !is_power_of_two(geometry.sectors_per_cluster) ||
      geometry.sectors_per_cluster > 128 ||
      geometry.total_sectors == 0 ||
      !checked_multiply(
          geometry.total_sectors,
          geometry.bytes_per_sector,
          volume_bytes) ||
      volume_bytes > partition_size_bytes) {
    return Result<Fat32Geometry>::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"FAT32ジオメトリ検証",
        L"FAT32のセクター、クラスタ、または容量情報が不正です"));
  }
  return Result<Fat32Geometry>::success(geometry);
}

Result<ExFatGeometry> parse_exfat_geometry(
    const std::span<const std::byte> boot_sector,
    const std::uint32_t expected_sector_size,
    const std::uint64_t partition_size_bytes) {
  constexpr char kExFatSignature[] = "EXFAT   ";
  if (boot_sector.size() < 512 ||
      std::memcmp(boot_sector.data() + 3, kExFatSignature, 8) != 0 ||
      boot_sector[510] != std::byte{0x55} ||
      boot_sector[511] != std::byte{0xAA} ||
      !std::all_of(
          boot_sector.begin() + 11,
          boot_sector.begin() + 64,
          [](const std::byte value) { return value == std::byte{0}; }) ||
      !std::all_of(
          boot_sector.begin() + 113,
          boot_sector.begin() + 120,
          [](const std::byte value) { return value == std::byte{0}; })) {
    return Result<ExFatGeometry>::failure(clone_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"exFATブートセクター",
        L"exFATとして安全に識別できません"));
  }

  const std::uint8_t sector_shift =
      std::to_integer<std::uint8_t>(boot_sector[108]);
  const std::uint8_t cluster_shift =
      std::to_integer<std::uint8_t>(boot_sector[109]);
  const std::uint8_t fat_count =
      std::to_integer<std::uint8_t>(boot_sector[110]);
  const std::uint8_t percent_in_use =
      std::to_integer<std::uint8_t>(boot_sector[112]);
  const std::uint16_t revision =
      read_little<std::uint16_t>(boot_sector, 104);
  const std::uint16_t volume_flags =
      read_little<std::uint16_t>(boot_sector, 106);
  if (sector_shift >= 32U || cluster_shift >= 32U ||
      sector_shift + cluster_shift > 25U) {
    return Result<ExFatGeometry>::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"exFATセクター・クラスタ検証",
        L"exFATのセクターまたはクラスタ指数が対応範囲外です"));
  }

  ExFatGeometry geometry;
  geometry.bytes_per_sector = std::uint32_t{1} << sector_shift;
  geometry.sectors_per_cluster = std::uint32_t{1} << cluster_shift;
  geometry.total_sectors = read_little<std::uint64_t>(boot_sector, 72);
  geometry.fat_offset_sectors =
      read_little<std::uint32_t>(boot_sector, 80);
  geometry.fat_length_sectors =
      read_little<std::uint32_t>(boot_sector, 84);
  geometry.cluster_heap_offset_sectors =
      read_little<std::uint32_t>(boot_sector, 88);
  geometry.cluster_count =
      read_little<std::uint32_t>(boot_sector, 92);
  geometry.root_directory_cluster =
      read_little<std::uint32_t>(boot_sector, 96);

  std::uint64_t volume_bytes{};
  std::uint64_t all_fats_length{};
  std::uint64_t fat_end{};
  std::uint64_t heap_length{};
  std::uint64_t heap_end{};
  std::uint64_t fat_bytes{};
  std::uint64_t required_fat_bytes{};
  if (geometry.bytes_per_sector != expected_sector_size ||
      expected_sector_size == 0U || geometry.total_sectors == 0U ||
      !checked_multiply(
          geometry.total_sectors, geometry.bytes_per_sector, volume_bytes) ||
      volume_bytes > partition_size_bytes ||
      geometry.fat_offset_sectors < 24U ||
      geometry.fat_length_sectors == 0U ||
      (fat_count != 1U && fat_count != 2U) ||
      !checked_multiply(
          geometry.fat_length_sectors, fat_count, all_fats_length) ||
      !checked_add(
          geometry.fat_offset_sectors, all_fats_length, fat_end) ||
      geometry.cluster_heap_offset_sectors < fat_end ||
      geometry.cluster_count == 0U ||
      geometry.cluster_count > 0xFFFFFFF5U ||
      geometry.root_directory_cluster < 2U ||
      geometry.root_directory_cluster > geometry.cluster_count + 1ULL ||
      !checked_multiply(
          geometry.fat_length_sectors,
          geometry.bytes_per_sector,
          fat_bytes) ||
      !checked_multiply(
          static_cast<std::uint64_t>(geometry.cluster_count) + 2U,
          sizeof(std::uint32_t),
          required_fat_bytes) ||
      fat_bytes < required_fat_bytes ||
      !checked_multiply(
          geometry.cluster_count,
          geometry.sectors_per_cluster,
          heap_length) ||
      !checked_add(
          geometry.cluster_heap_offset_sectors, heap_length, heap_end) ||
      heap_end > geometry.total_sectors || (revision >> 8U) != 1U ||
      (volume_flags & 0xFFF0U) != 0U ||
      (fat_count == 1U && (volume_flags & 0x0001U) != 0U) ||
      (percent_in_use > 100U && percent_in_use != 0xFFU)) {
    return Result<ExFatGeometry>::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"exFATジオメトリ検証",
        L"exFATの容量、FAT、クラスタヒープ、または版情報が不正です"));
  }
  return Result<ExFatGeometry>::success(geometry);
}

Result<BasicDataFileSystem> classify_basic_data_file_system(
    const std::span<const std::byte> boot_sector,
    const std::uint32_t expected_sector_size,
    const std::uint64_t partition_size_bytes) {
  constexpr char kBitLockerSignature[] = "-FVE-FS-";
  constexpr char kNtfsSignature[] = "NTFS    ";
  constexpr char kExFatSignature[] = "EXFAT   ";
  constexpr char kFat32Signature[] = "FAT32   ";
  if (boot_sector.size() < 512U) {
    return Result<BasicDataFileSystem>::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"基本データ領域ブートセクター",
        L"ファイルシステム識別に必要な先頭512バイトがありません"));
  }
  if (std::memcmp(boot_sector.data() + 3, kBitLockerSignature, 8) == 0) {
    return Result<BasicDataFileSystem>::failure(clone_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"BitLocker完全復号の確認",
        L"BitLocker形式を検出しました。保護の中断ではなく完全復号が必要です"));
  }
  if (std::memcmp(boot_sector.data() + 3, kNtfsSignature, 8) == 0) {
    const auto geometry = parse_ntfs_geometry(
        boot_sector, expected_sector_size, partition_size_bytes);
    return geometry
        ? Result<BasicDataFileSystem>::success(BasicDataFileSystem::ntfs)
        : Result<BasicDataFileSystem>::failure(geometry.error());
  }
  if (std::memcmp(boot_sector.data() + 3, kExFatSignature, 8) == 0) {
    const auto geometry = parse_exfat_geometry(
        boot_sector, expected_sector_size, partition_size_bytes);
    return geometry
        ? Result<BasicDataFileSystem>::success(BasicDataFileSystem::exfat)
        : Result<BasicDataFileSystem>::failure(geometry.error());
  }
  if (std::memcmp(boot_sector.data() + 82, kFat32Signature, 8) == 0) {
    const auto geometry = parse_fat32_geometry(
        boot_sector, expected_sector_size, partition_size_bytes);
    return geometry
        ? Result<BasicDataFileSystem>::success(BasicDataFileSystem::fat32)
        : Result<BasicDataFileSystem>::failure(geometry.error());
  }
  return Result<BasicDataFileSystem>::failure(clone_error(
      ErrorCode::unsupported_layout,
      ERROR_NOT_SUPPORTED,
      L"基本データ領域ファイルシステム",
      L"NTFS、FAT32、exFATのいずれとしても安全に識別できません"));
}

Status validate_fat32_boot_sector(
    const std::span<const std::byte> boot_sector,
    const std::uint32_t expected_sector_size,
    const std::uint64_t partition_size_bytes) {
  const auto geometry = parse_fat32_geometry(
      boot_sector, expected_sector_size, partition_size_bytes);
  if (!geometry) {
    return Status::failure(geometry.error());
  }
  return success_status();
}

Result<OfflineGptClonePlan> build_offline_gpt_clone_plan(
    const ISourceDiskReader& source,
    const ITargetDiskWriter& target,
    INtfsUsedRangeProvider& used_range_provider,
    IGuidGenerator& guid_generator) {
  const auto source_gpt_result = parse_gpt(source);
  if (!source_gpt_result) {
    return Result<OfflineGptClonePlan>::failure(source_gpt_result.error());
  }
  const auto target_gpt_result = make_gpt_write_plan(
      source_gpt_result.value(),
      target.size_bytes(),
      target.logical_sector_size(),
      guid_generator);
  if (!target_gpt_result) {
    return Result<OfflineGptClonePlan>::failure(target_gpt_result.error());
  }

  OfflineGptClonePlan plan;
  plan.source_gpt = source_gpt_result.value();
  plan.target_gpt = target_gpt_result.value();
  for (const auto& partition : plan.source_gpt.partitions) {
    const auto partition_range_result = partition_byte_range(
        partition, plan.source_gpt.logical_sector_size);
    if (!partition_range_result) {
      return Result<OfflineGptClonePlan>::failure(
          partition_range_result.error());
    }
    const ByteRange partition_range = partition_range_result.value();
    PlannedPartitionCopy copy;
    copy.entry_index = partition.entry_index;

    if (partition.type_guid == gpt_type_microsoft_reserved()) {
      copy.mode = PartitionCopyMode::microsoft_reserved_recreate;
      plan.partition_copies.push_back(std::move(copy));
      continue;
    }

    const auto boot_result = read_boot_sector(source, partition_range);
    if (!boot_result) {
      return Result<OfflineGptClonePlan>::failure(boot_result.error());
    }
    if (partition.type_guid == gpt_type_efi_system()) {
      const Status fat_status = validate_fat32_boot_sector(
          boot_result.value(),
          source.logical_sector_size(),
          partition_range.length);
      if (!fat_status) {
        return Result<OfflineGptClonePlan>::failure(fat_status.error());
      }
      copy.mode = PartitionCopyMode::efi_fat32_raw;
      copy.source_ranges.push_back(partition_range);
    } else if (partition.type_guid == gpt_type_windows_recovery()) {
      const auto ntfs_result = parse_ntfs_geometry(
          boot_result.value(),
          source.logical_sector_size(),
          partition_range.length);
      if (!ntfs_result) {
        return Result<OfflineGptClonePlan>::failure(ntfs_result.error());
      }
      copy.mode = PartitionCopyMode::recovery_ntfs_raw;
      copy.source_ranges.push_back(partition_range);
    } else if (partition.type_guid == gpt_type_basic_data()) {
      const auto file_system = classify_basic_data_file_system(
          boot_result.value(),
          source.logical_sector_size(),
          partition_range.length);
      if (!file_system) {
        return Result<OfflineGptClonePlan>::failure(file_system.error());
      }
      if (file_system.value() == BasicDataFileSystem::ntfs) {
        const auto ntfs_result = parse_ntfs_geometry(
            boot_result.value(),
            source.logical_sector_size(),
            partition_range.length);
        if (!ntfs_result) {
          return Result<OfflineGptClonePlan>::failure(ntfs_result.error());
        }
        auto ranges_result = used_range_provider.query_used_ranges(
            partition.entry_index, ntfs_result.value());
        if (!ranges_result) {
          return Result<OfflineGptClonePlan>::failure(ranges_result.error());
        }
        std::vector<ByteRange> ranges = ranges_result.value();
        const Status ranges_status = validate_used_ranges(
            ranges, partition_range, ntfs_result.value());
        if (!ranges_status) {
          return Result<OfflineGptClonePlan>::failure(ranges_status.error());
        }
        copy.mode = PartitionCopyMode::ntfs_used_clusters;
        copy.source_ranges = std::move(ranges);
      } else {
        copy.mode = file_system.value() == BasicDataFileSystem::fat32
            ? PartitionCopyMode::basic_fat32_raw
            : PartitionCopyMode::basic_exfat_raw;
        // Exact clone preserves the complete filesystem container.  Sparse
        // inference is intentionally limited to NTFS bitmap-backed routes.
        copy.source_ranges.push_back(partition_range);
      }
    } else {
      return Result<OfflineGptClonePlan>::failure(clone_error(
          ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"GPTパーティション種別",
          L"未対応または不明なGPTパーティション種別を検出しました"));
    }
    plan.partition_copies.push_back(std::move(copy));
  }
  return Result<OfflineGptClonePlan>::success(std::move(plan));
}

Result<OfflineGptCloneReport> execute_offline_gpt_clone(
    const OfflineGptCloneRequest& request,
    const ISourceDiskReader& source,
    ITargetDiskWriter& target,
    INtfsUsedRangeProvider& used_range_provider,
    IGuidGenerator& guid_generator) {
  const Status identity_status = validate_clone_identities(
      request.expected_source,
      request.observed_source,
      request.expected_target,
      request.observed_target,
      request.confirmation);
  if (!identity_status) {
    return Result<OfflineGptCloneReport>::failure(identity_status.error());
  }
  if (request.maximum_chunk_bytes == 0 ||
      request.maximum_chunk_bytes > 16U * 1024U * 1024U ||
      request.observed_source.size_bytes != source.size_bytes() ||
      request.observed_target.size_bytes != target.size_bytes() ||
      request.observed_source.logical_sector_size !=
          source.logical_sector_size() ||
      request.observed_target.logical_sector_size !=
          target.logical_sector_size() ||
      target.size_bytes() < 2U * kTargetMetadataInvalidationBytes) {
    return Result<OfflineGptCloneReport>::failure(clone_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"ディスクI/O境界の再確認",
        L"識別情報とI/O対象の寸法が一致しません"));
  }
  if (source.logical_sector_size() != 512 ||
      target.logical_sector_size() != 512) {
    return Result<OfflineGptCloneReport>::failure(clone_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"4Kn有効化ゲート",
        L"Phase 1では実機検証前の4Knクローンを有効にしていません"));
  }

  DiskOperationProgress progress;
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::planning,
      std::nullopt,
      true);
  if (disk_operation_cancellation_requested(request.callbacks)) {
    return Result<OfflineGptCloneReport>::failure(
        cancelled_status(L"GPTクローン計画").error());
  }

  const auto plan_result = build_offline_gpt_clone_plan(
      source, target, used_range_provider, guid_generator);
  if (!plan_result) {
    return Result<OfflineGptCloneReport>::failure(plan_result.error());
  }
  const OfflineGptClonePlan& plan = plan_result.value();
  auto digest_result = detail::VerifiedWriteDigestBuilder::create();
  if (!digest_result) {
    return Result<OfflineGptCloneReport>::failure(digest_result.error());
  }
  auto digest = digest_result.take_value();

  std::uint64_t total_copy_bytes = 0;
  for (const auto& partition : plan.partition_copies) {
    for (const auto& range : partition.source_ranges) {
      if (!checked_add(total_copy_bytes, range.length, total_copy_bytes)) {
        return Result<OfflineGptCloneReport>::failure(clone_error(
            ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"GPTクローン進捗合計",
            L"コピー予定バイト数が表現可能な範囲を超えました"));
      }
    }
  }
  progress.total_read_bytes = total_copy_bytes;
  progress.total_write_bytes = total_copy_bytes;
  progress.total_verify_bytes = total_copy_bytes;
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::invalidating_target,
      std::nullopt,
      true);
  if (disk_operation_cancellation_requested(request.callbacks)) {
    return Result<OfflineGptCloneReport>::failure(
        cancelled_status(L"コピー先GPT無効化前").error());
  }

  const std::vector<std::byte> zeroes(
      kTargetMetadataInvalidationBytes, std::byte{0});
  const std::uint64_t trailing_metadata_offset =
      target.size_bytes() - zeroes.size();
  for (const std::uint64_t offset :
       {std::uint64_t{0}, trailing_metadata_offset}) {
    if (disk_operation_cancellation_requested(request.callbacks)) {
      const Status flush_status = target.flush_target();
      if (!flush_status) {
        return Result<OfflineGptCloneReport>::failure(flush_status.error());
      }
      return Result<OfflineGptCloneReport>::failure(
          cancelled_status(L"コピー先GPT無効化").error());
    }
    const Status invalidate_status = write_and_verify(
        target,
        offset,
        zeroes,
        L"コピー先パーティション情報無効化",
        nullptr);
    if (!invalidate_status) {
      return Result<OfflineGptCloneReport>::failure(
          invalidate_status.error());
    }
  }
  const Status invalidation_flush = target.flush_target();
  if (!invalidation_flush) {
    return Result<OfflineGptCloneReport>::failure(invalidation_flush.error());
  }

  OfflineGptCloneReport report;
  std::uint64_t verified_chunk_count = 0U;
  report.source_disk_guid = plan.source_gpt.disk_guid;
  report.target_disk_guid = plan.target_gpt.target_disk.disk_guid;
  for (const auto& partition : plan.partition_copies) {
    if (partition.mode == PartitionCopyMode::microsoft_reserved_recreate) {
      ++report.recreated_partition_count;
      continue;
    }
    for (const auto& range : partition.source_ranges) {
      const Status copy_status = copy_range(
          source,
          target,
          range,
          request.maximum_chunk_bytes,
          partition.entry_index,
          request.callbacks,
          progress,
          digest,
          report.copied_data_bytes,
          verified_chunk_count);
      if (!copy_status) {
        return Result<OfflineGptCloneReport>::failure(copy_status.error());
      }
    }
    ++report.copied_partition_count;
  }
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::flushing_data,
      std::nullopt,
      false);
  const Status data_flush = target.flush_target();
  if (!data_flush) {
    return Result<OfflineGptCloneReport>::failure(data_flush.error());
  }

  for (const auto& write : plan.target_gpt.writes) {
    if (write.kind == GptMetadataKind::primary_header_commit) {
      continue;
    }
    publish_progress(
        request.callbacks,
        progress,
        DiskOperationStage::staging_partition_table,
        std::nullopt,
        true);
    if (disk_operation_cancellation_requested(request.callbacks)) {
      const Status flush_status = target.flush_target();
      if (!flush_status) {
        return Result<OfflineGptCloneReport>::failure(flush_status.error());
      }
      return Result<OfflineGptCloneReport>::failure(
          cancelled_status(L"GPTメタデータ仮配置").error());
    }
    const Status metadata_status = write_and_verify(
        target,
        write.offset,
        write.bytes,
        L"コピー先GPTメタデータ検証",
        &digest);
    if (!metadata_status) {
      return Result<OfflineGptCloneReport>::failure(metadata_status.error());
    }
  }
  const Status metadata_flush = target.flush_target();
  if (!metadata_flush) {
    return Result<OfflineGptCloneReport>::failure(metadata_flush.error());
  }
  if (disk_operation_cancellation_requested(request.callbacks)) {
    return Result<OfflineGptCloneReport>::failure(
        cancelled_status(L"プライマリGPT確定前").error());
  }
  const auto commit = std::find_if(
      plan.target_gpt.writes.begin(),
      plan.target_gpt.writes.end(),
      [](const auto& write) {
        return write.kind == GptMetadataKind::primary_header_commit;
      });
  if (commit == plan.target_gpt.writes.end()) {
    return Result<OfflineGptCloneReport>::failure(clone_error(
        ErrorCode::internal_error,
        ERROR_INVALID_DATA,
        L"GPTコミット計画",
        L"プライマリGPTコミットが計画にありません"));
  }
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::committing_partition_table,
      std::nullopt,
      false);
  const Status commit_status = write_and_verify(
      target,
      commit->offset,
      commit->bytes,
      L"プライマリGPTコミット検証",
      &digest);
  if (!commit_status) {
    return Result<OfflineGptCloneReport>::failure(commit_status.error());
  }
  const Status commit_flush = target.flush_target();
  if (!commit_flush) {
    return Result<OfflineGptCloneReport>::failure(commit_flush.error());
  }
  auto finished_digest = digest.finish();
  if (!finished_digest) {
    return Result<OfflineGptCloneReport>::failure(finished_digest.error());
  }
  report.verified_write_digest = finished_digest.take_value();
  report.read_back_verified = true;
  report.primary_gpt_committed = true;
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::completed,
      std::nullopt,
      false);
  return Result<OfflineGptCloneReport>::success(std::move(report));
}

}  // namespace ytec::clonecore

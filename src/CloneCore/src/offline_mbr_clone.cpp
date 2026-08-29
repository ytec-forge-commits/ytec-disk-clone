#include "ytec/clonecore/offline_mbr_clone.h"

#include "verified_write_digest.h"

#include <Windows.h>

#include <algorithm>
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

bool checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& result) noexcept {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

Result<ByteRange> partition_byte_range(
    const MbrPartition& partition,
    const std::uint32_t sector_size) {
  std::uint64_t offset{};
  std::uint64_t length{};
  if (!checked_multiply(partition.first_lba, sector_size, offset) ||
      !checked_multiply(partition.sector_count, sector_size, length)) {
    return Result<ByteRange>::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        L"MBRパーティション範囲計算",
        L"パーティションのバイト範囲がオーバーフローしました"));
  }
  return Result<ByteRange>::success(ByteRange{.offset = offset, .length = length});
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
        L"MBRパーティションブートセクター読取り",
        L"要求したセクター長を読み取れませんでした"));
  }
  return result;
}

Status validate_used_ranges(
    std::vector<ByteRange>& ranges,
    const ByteRange& partition_range,
    const NtfsGeometry& geometry) {
  if (ranges.empty()) {
    return Status::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"MBR NTFS使用クラスタ",
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
        end > partition_range.length ||
        (index != 0 && range.offset < previous_end)) {
      return Status::failure(clone_error(
          ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"MBR NTFS使用クラスタ境界",
          L"使用クラスタ範囲が未整列、重複、または区画境界外です"));
    }
    previous_end = end;
    range.offset += partition_range.offset;
  }
  if (ranges.front().offset != partition_range.offset) {
    return Status::failure(clone_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"MBR NTFSブートクラスタ",
        L"使用クラスタ一覧にパーティション先頭が含まれていません"));
  }
  return success_status();
}

Status write_and_verify(
    ITargetDiskWriter& target,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes,
    const std::wstring_view operation,
    detail::VerifiedWriteDigestBuilder* const digest) {
  const Status written = target.write_target(offset, bytes);
  if (!written) {
    return written;
  }
  const auto read_back = target.read_back(offset, bytes.size());
  if (!read_back) {
    return Status::failure(read_back.error());
  }
  if (read_back.value().size() != bytes.size() ||
      !std::equal(bytes.begin(), bytes.end(), read_back.value().begin())) {
    return Status::failure(clone_error(
        ErrorCode::verification_failed,
        ERROR_CRC,
        std::wstring(operation),
        L"書込み後の読戻し内容が一致しません"));
  }
  if (digest != nullptr) {
    return digest->append_verified_write(offset, read_back.value());
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
      return cancelled_status(L"MBRクローンのデータコピー");
    }
    const std::size_t chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(range.length - position, maximum_chunk_bytes));
    const auto read_result = source.read(range.offset + position, chunk);
    if (!read_result) {
      return Status::failure(read_result.error());
    }
    if (read_result.value().size() != chunk) {
      return Status::failure(clone_error(
          ErrorCode::io_failed,
          ERROR_HANDLE_EOF,
          L"MBRコピー元チャンク読取り",
          L"要求したバイト数を読み取れませんでした"));
    }
    const Status copied = write_and_verify(
        target,
        range.offset + position,
        read_result.value(),
        L"MBRパーティションデータ読戻し検証",
        &digest);
    if (!copied) {
      return copied;
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
      return cancelled_status(L"MBRクローンの安全境界");
    }
  }
  return success_status();
}

}  // namespace

Result<OfflineMbrClonePlan> build_offline_mbr_clone_plan(
    const ISourceDiskReader& source,
    const ITargetDiskWriter& target,
    INtfsUsedRangeProvider& used_range_provider,
    IMbrSignatureGenerator& signature_generator,
    const std::span<const std::uint32_t> disallowed_signatures) {
  const auto source_mbr = parse_mbr(source);
  if (!source_mbr) {
    return Result<OfflineMbrClonePlan>::failure(source_mbr.error());
  }
  const bool bios_boot_layout = std::any_of(
      source_mbr.value().partitions.begin(),
      source_mbr.value().partitions.end(),
      [](const MbrPartition& partition) { return partition.active; });
  const auto target_mbr = make_mbr_write_plan(
      source_mbr.value(),
      target.size_bytes(),
      target.logical_sector_size(),
      signature_generator,
      disallowed_signatures,
      bios_boot_layout);
  if (!target_mbr) {
    return Result<OfflineMbrClonePlan>::failure(target_mbr.error());
  }

  OfflineMbrClonePlan plan;
  plan.source_mbr = source_mbr.value();
  plan.target_mbr = target_mbr.value();
  for (const auto& partition : plan.source_mbr.partitions) {
    const auto partition_range = partition_byte_range(
        partition, plan.source_mbr.logical_sector_size);
    if (!partition_range) {
      return Result<OfflineMbrClonePlan>::failure(partition_range.error());
    }
    const auto boot_sector = read_boot_sector(source, partition_range.value());
    if (!boot_sector) {
      return Result<OfflineMbrClonePlan>::failure(boot_sector.error());
    }

    PlannedMbrPartitionCopy copy;
    copy.table_index = partition.table_index;
    if (partition.type == 0x07) {
      const auto file_system = classify_basic_data_file_system(
          boot_sector.value(),
          source.logical_sector_size(),
          partition_range.value().length);
      if (!file_system) {
        return Result<OfflineMbrClonePlan>::failure(file_system.error());
      }
      if (file_system.value() == BasicDataFileSystem::ntfs) {
        const auto geometry = parse_ntfs_geometry(
            boot_sector.value(),
            source.logical_sector_size(),
            partition_range.value().length);
        if (!geometry) {
          return Result<OfflineMbrClonePlan>::failure(geometry.error());
        }
        auto ranges = used_range_provider.query_used_ranges(
            partition.table_index, geometry.value());
        if (!ranges) {
          return Result<OfflineMbrClonePlan>::failure(ranges.error());
        }
        std::vector<ByteRange> used = ranges.value();
        const Status valid = validate_used_ranges(
            used, partition_range.value(), geometry.value());
        if (!valid) {
          return Result<OfflineMbrClonePlan>::failure(valid.error());
        }
        copy.mode = MbrPartitionCopyMode::ntfs_used_clusters;
        copy.source_ranges = std::move(used);
      } else if (file_system.value() == BasicDataFileSystem::exfat) {
        copy.mode = MbrPartitionCopyMode::exfat_raw;
        copy.source_ranges.push_back(partition_range.value());
      } else {
        return Result<OfflineMbrClonePlan>::failure(clone_error(
            ErrorCode::unsupported_layout,
            ERROR_NOT_SUPPORTED,
            L"MBR 0x07ファイルシステム",
            L"MBR 0x07上のFAT32は対応せず、型と実形式が一致する構成だけを扱います"));
      }
    } else if (partition.type == 0x27) {
      const auto geometry = parse_ntfs_geometry(
          boot_sector.value(),
          source.logical_sector_size(),
          partition_range.value().length);
      if (!geometry) {
        return Result<OfflineMbrClonePlan>::failure(geometry.error());
      }
      copy.mode = MbrPartitionCopyMode::recovery_ntfs_raw;
      copy.source_ranges.push_back(partition_range.value());
    } else if (partition.type == 0x0B || partition.type == 0x0C) {
      const Status valid = validate_fat32_boot_sector(
          boot_sector.value(),
          source.logical_sector_size(),
          partition_range.value().length);
      if (!valid) {
        return Result<OfflineMbrClonePlan>::failure(valid.error());
      }
      copy.mode = MbrPartitionCopyMode::fat32_raw;
      copy.source_ranges.push_back(partition_range.value());
    } else {
      return Result<OfflineMbrClonePlan>::failure(clone_error(
          ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"MBRパーティション種別",
          L"Phase 3で未対応のMBRパーティション種別です"));
    }
    if (partition.active && partition.type == 0x27) {
      return Result<OfflineMbrClonePlan>::failure(clone_error(
          ErrorCode::unsupported_layout,
          ERROR_NOT_SUPPORTED,
          L"MBR Active回復区画",
          L"回復区画をBIOS起動対象として扱いません"));
    }
    plan.partition_copies.push_back(std::move(copy));
  }
  return Result<OfflineMbrClonePlan>::success(std::move(plan));
}

Result<OfflineMbrCloneReport> execute_offline_mbr_clone(
    const OfflineMbrCloneRequest& request,
    const ISourceDiskReader& source,
    ITargetDiskWriter& target,
    INtfsUsedRangeProvider& used_range_provider,
    IMbrSignatureGenerator& signature_generator) {
  const Status identities = validate_clone_identities(
      request.expected_source,
      request.observed_source,
      request.expected_target,
      request.observed_target,
      request.confirmation);
  if (!identities) {
    return Result<OfflineMbrCloneReport>::failure(identities.error());
  }
  if (request.maximum_chunk_bytes == 0 ||
      request.maximum_chunk_bytes > 16U * 1024U * 1024U ||
      source.logical_sector_size() != 512 ||
      target.logical_sector_size() != 512 ||
      request.observed_source.size_bytes != source.size_bytes() ||
      request.observed_target.size_bytes != target.size_bytes() ||
      request.observed_source.logical_sector_size != 512 ||
      request.observed_target.logical_sector_size != 512 ||
      target.size_bytes() < 2U * kTargetMetadataInvalidationBytes) {
    return Result<OfflineMbrCloneReport>::failure(clone_error(
        ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"MBRディスクI/O境界の再確認",
        L"識別情報、I/O寸法、512バイトセクター条件が一致しません"));
  }

  DiskOperationProgress progress;
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::planning,
      std::nullopt,
      true);
  if (disk_operation_cancellation_requested(request.callbacks)) {
    return Result<OfflineMbrCloneReport>::failure(
        cancelled_status(L"MBRクローン計画").error());
  }

  const auto plan_result = build_offline_mbr_clone_plan(
      source,
      target,
      used_range_provider,
      signature_generator,
      request.connected_mbr_signatures);
  if (!plan_result) {
    return Result<OfflineMbrCloneReport>::failure(plan_result.error());
  }
  const auto& plan = plan_result.value();
  auto digest_result = detail::VerifiedWriteDigestBuilder::create();
  if (!digest_result) {
    return Result<OfflineMbrCloneReport>::failure(digest_result.error());
  }
  auto digest = digest_result.take_value();

  std::uint64_t total_copy_bytes = 0;
  for (const auto& partition : plan.partition_copies) {
    for (const auto& range : partition.source_ranges) {
      if (!checked_add(total_copy_bytes, range.length, total_copy_bytes)) {
        return Result<OfflineMbrCloneReport>::failure(clone_error(
            ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"MBRクローン進捗合計",
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
    return Result<OfflineMbrCloneReport>::failure(
        cancelled_status(L"コピー先MBR無効化前").error());
  }

  const std::vector<std::byte> zeroes(
      kTargetMetadataInvalidationBytes, std::byte{0});
  const std::uint64_t trailing_metadata_offset =
      target.size_bytes() - zeroes.size();
  Status status = success_status();
  for (const std::uint64_t offset :
       {std::uint64_t{0}, trailing_metadata_offset}) {
    if (disk_operation_cancellation_requested(request.callbacks)) {
      const Status flush_status = target.flush_target();
      if (!flush_status) {
        return Result<OfflineMbrCloneReport>::failure(flush_status.error());
      }
      return Result<OfflineMbrCloneReport>::failure(
          cancelled_status(L"コピー先パーティション情報無効化").error());
    }
    status = write_and_verify(
        target,
        offset,
        zeroes,
        L"コピー先パーティション情報無効化",
        nullptr);
    if (!status) {
      return Result<OfflineMbrCloneReport>::failure(status.error());
    }
  }
  status = target.flush_target();
  if (!status) {
    return Result<OfflineMbrCloneReport>::failure(status.error());
  }

  OfflineMbrCloneReport report;
  std::uint64_t verified_chunk_count = 0U;
  report.source_disk_signature = plan.source_mbr.disk_signature;
  report.target_disk_signature = plan.target_mbr.target_disk.disk_signature;
  for (const auto& partition : plan.partition_copies) {
    for (const auto& range : partition.source_ranges) {
      status = copy_range(
          source,
          target,
          range,
          request.maximum_chunk_bytes,
          partition.table_index,
          request.callbacks,
          progress,
          digest,
          report.copied_data_bytes,
          verified_chunk_count);
      if (!status) {
        return Result<OfflineMbrCloneReport>::failure(status.error());
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
  status = target.flush_target();
  if (!status) {
    return Result<OfflineMbrCloneReport>::failure(status.error());
  }
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::staging_partition_table,
      std::nullopt,
      true);
  if (disk_operation_cancellation_requested(request.callbacks)) {
    return Result<OfflineMbrCloneReport>::failure(
        cancelled_status(L"コピー先MBR確定前").error());
  }
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::committing_partition_table,
      std::nullopt,
      false);
  status = write_and_verify(
      target,
      0,
      plan.target_mbr.sector,
      L"コピー先MBR最終確定",
      &digest);
  if (!status) {
    return Result<OfflineMbrCloneReport>::failure(status.error());
  }
  status = target.flush_target();
  if (!status) {
    return Result<OfflineMbrCloneReport>::failure(status.error());
  }
  auto finished_digest = digest.finish();
  if (!finished_digest) {
    return Result<OfflineMbrCloneReport>::failure(finished_digest.error());
  }
  report.verified_write_digest = finished_digest.take_value();
  report.read_back_verified = true;
  report.target_mbr_committed = true;
  publish_progress(
      request.callbacks,
      progress,
      DiskOperationStage::completed,
      std::nullopt,
      false);
  return Result<OfflineMbrCloneReport>::success(std::move(report));
}

}  // namespace ytec::clonecore

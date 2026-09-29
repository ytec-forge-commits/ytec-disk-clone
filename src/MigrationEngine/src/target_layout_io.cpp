#include "ytec/migrationengine/target_layout_io.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ytec::migrationengine {
namespace {

constexpr std::size_t kMetadataInvalidationBytes = 1024U * 1024U;

clonecore::Error io_error(
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

clonecore::Status cancelled(const std::wstring_view operation) {
  return clonecore::Status::failure(io_error(
      clonecore::ErrorCode::cancelled,
      ERROR_CANCELLED,
      std::wstring(operation),
      L"安全な確定境界で取り消しました"));
}

clonecore::Status write_and_verify(
    clonecore::ITargetDiskWriter& target,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes,
    const std::wstring_view operation) {
  const auto written = target.write_target(offset, bytes);
  if (!written) {
    return written;
  }
  const auto observed = target.read_back(offset, bytes.size());
  if (!observed) {
    return clonecore::Status::failure(observed.error());
  }
  if (observed.value().size() != bytes.size() ||
      !std::equal(bytes.begin(), bytes.end(), observed.value().begin())) {
    return clonecore::Status::failure(io_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        std::wstring(operation),
        L"コピー先の読戻しが書込み内容と一致しません"));
  }
  return clonecore::success_status();
}

void publish(
    const clonecore::DiskOperationCallbacks& callbacks,
    const clonecore::DiskOperationStage stage,
    const bool cancellable) {
  clonecore::report_disk_operation_progress(
      callbacks,
      clonecore::DiskOperationProgress{
          .stage = stage,
          .cancellation_allowed = cancellable,
      });
}

}  // namespace

clonecore::Result<ShrinkTargetMetadataWriteReport>
write_shrink_target_metadata(
    const ShrinkTargetLayout& layout,
    clonecore::ITargetDiskWriter& target,
    const clonecore::DiskOperationCallbacks& callbacks) {
  if (target.size_bytes() != layout.migration.target_size_bytes ||
      target.logical_sector_size() == 0U ||
      target.size_bytes() < 2U * kMetadataInvalidationBytes) {
    return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
        io_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"縮小移行コピー先Writer寸法",
            L"計画とコピー先Writerの容量またはセクターが一致しません"));
  }
  const std::uint32_t planned_sector = layout.is_gpt
      ? layout.gpt.target_disk.logical_sector_size
      : layout.mbr.target_disk.logical_sector_size;
  if (planned_sector != target.logical_sector_size()) {
    return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
        io_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"縮小移行コピー先セクター",
            L"計画とコピー先Writerの論理セクターが一致しません"));
  }
  if (clonecore::disk_operation_cancellation_requested(callbacks)) {
    return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
        cancelled(L"縮小移行コピー先無効化前").error());
  }
  publish(
      callbacks, clonecore::DiskOperationStage::invalidating_target, true);
  const std::vector<std::byte> zeroes(
      kMetadataInvalidationBytes, std::byte{0});
  auto status = write_and_verify(
      target, 0U, zeroes, L"縮小移行コピー先先頭無効化");
  if (!status) {
    return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
        status.error());
  }
  std::uint64_t invalidated = zeroes.size();
  status = write_and_verify(
      target,
      target.size_bytes() - zeroes.size(),
      zeroes,
      L"縮小移行コピー先末尾無効化");
  if (!status) {
    return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
        status.error());
  }
  invalidated += zeroes.size();
  status = target.flush_target();
  if (!status) {
    return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
        status.error());
  }

  ShrinkTargetMetadataWriteReport report{
      .invalidated_bytes = invalidated,
  };
  if (layout.is_gpt) {
    for (const auto& write : layout.gpt.writes) {
      if (write.kind == clonecore::GptMetadataKind::primary_header_commit) {
        continue;
      }
      publish(
          callbacks, clonecore::DiskOperationStage::staging_partition_table,
          true);
      if (clonecore::disk_operation_cancellation_requested(callbacks)) {
        return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
            cancelled(L"縮小移行GPT仮配置").error());
      }
      status = write_and_verify(
          target, write.offset, write.bytes, L"縮小移行GPTメタデータ仮配置");
      if (!status) {
        return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
            status.error());
      }
      report.metadata_bytes += write.bytes.size();
    }
    status = target.flush_target();
    if (!status) {
      return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
          status.error());
    }
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
          cancelled(L"縮小移行GPT最終確定前").error());
    }
    const auto commit = std::find_if(
        layout.gpt.writes.begin(),
        layout.gpt.writes.end(),
        [](const auto& write) {
          return write.kind ==
              clonecore::GptMetadataKind::primary_header_commit;
        });
    if (commit == layout.gpt.writes.end()) {
      return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
          io_error(
              clonecore::ErrorCode::internal_error,
              ERROR_INVALID_DATA,
              L"縮小移行GPT最終確定",
              L"プライマリGPTヘッダーが計画にありません"));
    }
    publish(
        callbacks, clonecore::DiskOperationStage::committing_partition_table,
        false);
    status = write_and_verify(
        target, commit->offset, commit->bytes, L"縮小移行GPT最終確定");
    if (!status) {
      return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
          status.error());
    }
    report.metadata_bytes += commit->bytes.size();
  } else {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
          cancelled(L"縮小移行MBR最終確定前").error());
    }
    publish(
        callbacks, clonecore::DiskOperationStage::committing_partition_table,
        false);
    status = write_and_verify(
        target, 0U, layout.mbr.sector, L"縮小移行MBR最終確定");
    if (!status) {
      return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
          status.error());
    }
    report.metadata_bytes = layout.mbr.sector.size();
  }
  status = target.flush_target();
  if (!status) {
    return clonecore::Result<ShrinkTargetMetadataWriteReport>::failure(
        status.error());
  }
  report.every_write_read_back_verified = true;
  report.partition_table_committed = true;
  return clonecore::Result<ShrinkTargetMetadataWriteReport>::success(report);
}

}  // namespace ytec::migrationengine

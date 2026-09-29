#include "ytec/imageformat/restore.h"

#include "ytec/imageformat/compression.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/image_inspection.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::imageformat {
namespace {

clonecore::Error restore_error(
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

clonecore::Status write_and_verify(
    clonecore::ITargetDiskWriter& target,
    const std::uint64_t offset,
    const std::span<const std::byte> data,
    const std::wstring_view operation) {
  const auto written = target.write_target(offset, data);
  if (!written) {
    return written;
  }
  const auto read_back = target.read_back(offset, data.size());
  if (!read_back) {
    return clonecore::Status::failure(read_back.error());
  }
  if (read_back.value().size() != data.size() ||
      !std::equal(data.begin(), data.end(), read_back.value().begin())) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_CRC,
        std::wstring(operation),
        L"書込み後の読戻し内容が一致しません"));
  }
  return clonecore::success_status();
}

clonecore::Status cancelled_status(const std::wstring_view operation) {
  return clonecore::Status::failure(restore_error(
      clonecore::ErrorCode::cancelled,
      ERROR_CANCELLED,
      std::wstring(operation),
      L"利用者の操作により安全に中止しました"));
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

void publish_progress(
    const clonecore::DiskOperationCallbacks& callbacks,
    clonecore::DiskOperationProgress& progress,
    const clonecore::DiskOperationStage stage,
    const bool cancellation_allowed) noexcept {
  progress.stage = stage;
  progress.partition_index.reset();
  progress.cancellation_allowed = cancellation_allowed;
  clonecore::report_disk_operation_progress(callbacks, progress);
}

clonecore::Result<std::vector<std::byte>> invoke_reader_exact(
    const Sha256ReadCallback& reader,
    const std::uint64_t image_length,
    const std::uint64_t offset,
    const std::size_t length,
    const std::wstring_view operation) {
  if (!reader || offset > image_length ||
      length > image_length - offset) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        restore_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            std::wstring(operation),
            L"dcimg読取り範囲またはCallbackが不正です"));
  }
  try {
    auto bytes = reader(offset, length);
    if (!bytes) {
      return bytes;
    }
    if (bytes.value().size() != length) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          restore_error(
              clonecore::ErrorCode::io_failed,
              ERROR_HANDLE_EOF,
              std::wstring(operation),
              L"要求したdcimg範囲を完全に読み取れませんでした"));
    }
    return bytes;
  } catch (...) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        restore_error(
            clonecore::ErrorCode::io_failed,
            ERROR_GEN_FAILURE,
            std::wstring(operation),
            L"dcimg読取りCallbackで予期しない例外が発生しました"));
  }
}

clonecore::Result<std::vector<std::byte>>
read_and_verify_chunk_from_reader(
    const Sha256ReadCallback& reader,
    const std::uint64_t image_length,
    const DcimgChunkRecord& chunk,
    const std::size_t maximum_block_bytes,
    const clonecore::DiskOperationCallbacks& callbacks,
    clonecore::DiskOperationProgress& progress,
    clonecore::ITargetDiskWriter& target) {
  if (chunk.stored_length >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::size_t>::max)()) ||
      chunk.stored_length > kDcimgChunkSize32MiB) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        restore_error(
            clonecore::ErrorCode::invalid_data,
            ERROR_FILE_TOO_LARGE,
            L"dcimgデータチャンク再読取り",
            L"チャンク長が復元時の安全上限を超えています"));
  }

  std::vector<std::byte> bytes;
  try {
    bytes.resize(static_cast<std::size_t>(chunk.stored_length));
  } catch (const std::bad_alloc&) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        restore_error(
            clonecore::ErrorCode::io_failed,
            ERROR_NOT_ENOUGH_MEMORY,
            L"dcimgデータチャンク再読取り",
            L"チャンク検証用メモリを確保できませんでした"));
  }

  std::size_t position = 0;
  while (position < bytes.size()) {
    if (clonecore::disk_operation_cancellation_requested(callbacks)) {
      const auto flush_status = target.flush_target();
      if (!flush_status) {
        return clonecore::Result<std::vector<std::byte>>::failure(
            flush_status.error());
      }
      return clonecore::Result<std::vector<std::byte>>::failure(
          cancelled_status(L"dcimgデータチャンク再読取り").error());
    }
    const std::size_t length =
        (std::min)(maximum_block_bytes, bytes.size() - position);
    auto block = invoke_reader_exact(
        reader,
        image_length,
        chunk.stored_offset + position,
        length,
        L"dcimgデータチャンク再読取り");
    if (!block) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          block.error());
    }
    std::copy(
        block.value().begin(),
        block.value().end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(position));
    position += length;
    progress.read_bytes += length;
    clonecore::report_disk_operation_progress(callbacks, progress);
  }

  std::vector<std::byte> uncompressed;
  if (chunk.compression == DcimgCompression::zstandard) {
    auto decompressed = decompress_zstandard_dcimg_v1(
        bytes, static_cast<std::size_t>(chunk.uncompressed_length));
    if (!decompressed) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          decompressed.error());
    }
    uncompressed = decompressed.take_value();
  } else {
    uncompressed = std::move(bytes);
  }

  const auto actual_hash = sha256(uncompressed);
  if (!actual_hash) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        actual_hash.error());
  }
  if (actual_hash.value() != chunk.sha256) {
    return clonecore::Result<std::vector<std::byte>>::failure(
        restore_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_CRC,
            L"dcimgデータチャンク再検証",
            L"復元直前に再読取りしたチャンクのSHA-256が一致しません"));
  }
  return clonecore::Result<std::vector<std::byte>>::success(
      std::move(uncompressed));
}

clonecore::Status write_span_in_chunks(
    clonecore::ITargetDiskWriter& target,
    const std::uint64_t target_offset,
    const std::span<const std::byte> data,
    const std::size_t maximum_chunk_bytes,
    const std::wstring_view operation,
    const clonecore::DiskOperationCallbacks& callbacks,
    clonecore::DiskOperationProgress* const progress,
    const bool cancellation_allowed,
    const bool count_source_read) {
  std::size_t position = 0;
  while (position < data.size()) {
    if (cancellation_allowed &&
        clonecore::disk_operation_cancellation_requested(callbacks)) {
      const auto flush_status = target.flush_target();
      if (!flush_status) {
        return flush_status;
      }
      return cancelled_status(operation);
    }
    const std::size_t length =
        (std::min)(maximum_chunk_bytes, data.size() - position);
    const auto status = write_and_verify(
        target,
        target_offset + position,
        data.subspan(position, length),
        operation);
    if (!status) {
      return status;
    }
    position += length;
    if (progress != nullptr) {
      if (count_source_read) {
        progress->read_bytes += length;
      }
      progress->written_bytes += length;
      progress->verified_bytes += length;
      clonecore::report_disk_operation_progress(callbacks, *progress);
    }
  }
  return clonecore::success_status();
}

clonecore::Status write_zeroes_in_chunks(
    clonecore::ITargetDiskWriter& target,
    const std::uint64_t target_offset,
    const std::uint64_t length,
    const std::size_t maximum_chunk_bytes,
    const std::wstring_view operation,
    const clonecore::DiskOperationCallbacks& callbacks,
    clonecore::DiskOperationProgress* const progress,
    const bool cancellation_allowed) {
  std::vector<std::byte> zeroes(maximum_chunk_bytes, std::byte{0});
  std::uint64_t position = 0;
  while (position < length) {
    if (cancellation_allowed &&
        clonecore::disk_operation_cancellation_requested(callbacks)) {
      const auto flush_status = target.flush_target();
      if (!flush_status) {
        return flush_status;
      }
      return cancelled_status(operation);
    }
    const std::size_t chunk = static_cast<std::size_t>(
        (std::min<std::uint64_t>)(length - position, maximum_chunk_bytes));
    const auto status = write_and_verify(
        target,
        target_offset + position,
        std::span<const std::byte>(zeroes).first(chunk),
        operation);
    if (!status) {
      return status;
    }
    position += chunk;
    if (progress != nullptr) {
      progress->written_bytes += chunk;
      progress->verified_bytes += chunk;
      clonecore::report_disk_operation_progress(callbacks, *progress);
    }
  }
  return clonecore::success_status();
}

clonecore::Status validate_target(
    const DcimgHeader& header,
    const DcimgRestoreRequest& request,
    const clonecore::ITargetDiskWriter& target) {
  const auto identity = clonecore::validate_stable_identity(
      request.expected_target, request.observed_target, L"復元先");
  if (!identity) {
    return identity;
  }
  if (request.expected_target.is_system_disk ||
      request.observed_target.is_system_disk ||
      request.expected_target.size_bytes != target.size_bytes() ||
      request.observed_target.size_bytes != target.size_bytes() ||
      request.expected_target.logical_sector_size !=
          target.logical_sector_size() ||
      request.observed_target.logical_sector_size !=
          target.logical_sector_size() ||
      target.size_bytes() < header.source_disk_size ||
      target.logical_sector_size() != header.logical_sector_size) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_INVALID_DATA,
        L"dcimg復元先寸法・システム判定",
        L"復元先の識別、容量、セクター、またはシステムディスク判定が一致しません"));
  }
  if (!request.confirmation.first_step_acknowledged ||
      request.confirmation.typed_token !=
          clonecore::make_target_confirmation_token(
              request.observed_target)) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_ACCESS_DENIED,
        L"dcimg復元先二段階確認",
        L"消去確認または確認文字列が一致しません"));
  }
  if (request.maximum_chunk_bytes < target.logical_sector_size() ||
      request.maximum_chunk_bytes > kDcimgChunkSize16MiB ||
      request.maximum_chunk_bytes % target.logical_sector_size() != 0) {
    return clonecore::Status::failure(restore_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"dcimg復元チャンク寸法",
        L"書込みチャンク寸法がセクター整列または安全上限外です"));
  }
  return clonecore::success_status();
}

}  // namespace

struct DcimgRestorePreparedSource::Impl final {
  std::uint64_t image_length{};
  Sha256ReadCallback reader;
  DcimgReadInspection inspection;
  std::uint64_t verified_work_bytes{};
};

DcimgRestorePreparedSource::DcimgRestorePreparedSource(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DcimgRestorePreparedSource::DcimgRestorePreparedSource(
    DcimgRestorePreparedSource&&) noexcept = default;

DcimgRestorePreparedSource& DcimgRestorePreparedSource::operator=(
    DcimgRestorePreparedSource&&) noexcept = default;

DcimgRestorePreparedSource::~DcimgRestorePreparedSource() = default;

clonecore::Result<DcimgRestorePreparedSource>
prepare_verified_dcimg_v1_from_reader(
    const std::uint64_t image_length,
    const Sha256ReadCallback& reader,
    const DcimgRestoreRequest& request) {
  clonecore::DiskOperationProgress progress;
  publish_progress(
      request.callbacks,
      progress,
      clonecore::DiskOperationStage::planning,
      true);
  if (clonecore::disk_operation_cancellation_requested(request.callbacks)) {
    return clonecore::Result<DcimgRestorePreparedSource>::failure(
        cancelled_status(L"dcimg復元計画").error());
  }

  const Sha256ReadCallback safe_reader =
      [&reader, image_length, &request](
          const std::uint64_t offset,
          const std::size_t length) {
        if (clonecore::disk_operation_cancellation_requested(
                request.callbacks)) {
          return clonecore::Result<std::vector<std::byte>>::failure(
              cancelled_status(L"dcimg復元前完全検証").error());
        }
        return invoke_reader_exact(
            reader,
            image_length,
            offset,
            length,
            L"dcimg復元前完全検証");
      };

  // This complete verification is deliberately before every target write.
  std::uint64_t verified_work_bytes = 0;
  auto inspection = inspect_dcimg_v1_from_reader(
      image_length,
      request.maximum_chunk_bytes,
      safe_reader,
      DcimgReadInspectionCallbacks{
          .progress =
              [&request, &progress, &verified_work_bytes](
                  const DcimgReadInspectionProgress& observed) {
                verified_work_bytes = observed.verified_bytes;
                progress.total_read_bytes =
                    observed.total_verify_bytes;
                progress.total_verify_bytes =
                    observed.total_verify_bytes;
                progress.read_bytes = observed.verified_bytes;
                progress.written_bytes = 0;
                progress.verified_bytes = observed.verified_bytes;
                publish_progress(
                    request.callbacks,
                    progress,
                    clonecore::DiskOperationStage::verifying_source,
                    true);
              },
      });
  if (!inspection) {
    return clonecore::Result<DcimgRestorePreparedSource>::failure(
        inspection.error());
  }
  if (verified_work_bytes == 0 ||
      verified_work_bytes != progress.total_verify_bytes) {
    return clonecore::Result<DcimgRestorePreparedSource>::failure(
        restore_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_DATA,
            L"dcimg復元前完全検証進捗",
            L"完全検証の最終進捗を確認できませんでした"));
  }
  if ((request.expected_image_length != 0 &&
       request.expected_image_length != image_length) ||
      (request.expected_global_hash.has_value() &&
       request.expected_global_hash.value() !=
           inspection.value().container.global_hash)) {
    return clonecore::Result<DcimgRestorePreparedSource>::failure(
        restore_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_CRC,
            L"dcimg復元ジョブ識別",
            L"復元対象dcimgがジョブ記録済みの長さまたは全体SHA-256に一致しません"));
  }
  return clonecore::Result<DcimgRestorePreparedSource>::success(
      DcimgRestorePreparedSource(std::make_unique<
          DcimgRestorePreparedSource::Impl>(
          DcimgRestorePreparedSource::Impl{
              .image_length = image_length,
              .reader = reader,
              .inspection = inspection.take_value(),
              .verified_work_bytes = verified_work_bytes,
          })));
}

clonecore::Result<DcimgRestoreReport>
restore_prepared_dcimg_v1(
    DcimgRestorePreparedSource&& source,
    const DcimgRestoreRequest& request,
    clonecore::ITargetDiskWriter& target) {
  if (source.impl_ == nullptr) {
    return clonecore::Result<DcimgRestoreReport>::failure(
        restore_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_INVALID_PARAMETER,
            L"dcimg準備済み復元元",
            L"完全検証済みの復元元がありません"));
  }
  auto prepared = std::move(source.impl_);
  const std::uint64_t image_length = prepared->image_length;
  const Sha256ReadCallback reader = std::move(prepared->reader);
  auto inspection =
      clonecore::Result<DcimgReadInspection>::success(
          std::move(prepared->inspection));
  clonecore::DiskOperationProgress progress{
      .total_read_bytes = prepared->verified_work_bytes,
      .total_verify_bytes = prepared->verified_work_bytes,
      .read_bytes = prepared->verified_work_bytes,
      .verified_bytes = prepared->verified_work_bytes,
      .cancellation_allowed = true,
  };

  if ((request.expected_image_length != 0 &&
       request.expected_image_length != image_length) ||
      (request.expected_global_hash.has_value() &&
       request.expected_global_hash.value() !=
           inspection.value().container.global_hash)) {
    return clonecore::Result<DcimgRestoreReport>::failure(
        restore_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_CRC,
            L"dcimg復元ジョブ識別",
            L"復元対象dcimgがジョブ記録済みの長さまたは全体SHA-256に一致しません"));
  }
  const auto& header = inspection.value().container.header;
  const auto metadata = inspect_dcimg_metadata_v1(
      header,
      inspection.value().manifest,
      inspection.value().partition_table_snapshot);
  if (!metadata) {
    return clonecore::Result<DcimgRestoreReport>::failure(metadata.error());
  }
  const auto& snapshot = metadata.value().partition_snapshot;
  const auto layout = validate_dcimg_restore_layout_v1(
      inspection.value().container, metadata.value());
  if (!layout) {
    return clonecore::Result<DcimgRestoreReport>::failure(
        layout.error());
  }
  const auto target_status = validate_target(header, request, target);
  if (!target_status) {
    return clonecore::Result<DcimgRestoreReport>::failure(
        target_status.error());
  }

  // Keep the complete-image verification counters as the first phase of the
  // same operation.  Resetting here would make the user-visible counters move
  // backwards when the operation changes from verification to restoration.
  for (const auto& chunk : inspection.value().container.chunks) {
    if (!checked_add(
            progress.total_write_bytes,
            chunk.uncompressed_length,
            progress.total_write_bytes) ||
        !checked_add(
            progress.total_verify_bytes,
            chunk.uncompressed_length,
            progress.total_verify_bytes) ||
        (chunk.flags != DcimgChunkFlags::zero_filled &&
         !checked_add(
             progress.total_read_bytes,
             chunk.stored_length,
             progress.total_read_bytes))) {
      return clonecore::Result<DcimgRestoreReport>::failure(restore_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"dcimg復元進捗合計",
          L"復元予定バイト数が表現可能な範囲を超えました"));
    }
  }
  publish_progress(
      request.callbacks,
      progress,
      clonecore::DiskOperationStage::invalidating_target,
      true);
  if (clonecore::disk_operation_cancellation_requested(request.callbacks)) {
    return clonecore::Result<DcimgRestoreReport>::failure(
        cancelled_status(L"dcimg復元先パーティション表無効化前").error());
  }

  for (const auto& region : snapshot.regions) {
    const auto invalidated = write_zeroes_in_chunks(
        target,
        region.disk_offset,
        region.data.size(),
        request.maximum_chunk_bytes,
        L"dcimg復元先パーティション表無効化",
        request.callbacks,
        nullptr,
        true);
    if (!invalidated) {
      return clonecore::Result<DcimgRestoreReport>::failure(
          invalidated.error());
    }
  }
  auto status = target.flush_target();
  if (!status) {
    return clonecore::Result<DcimgRestoreReport>::failure(status.error());
  }

  DcimgRestoreReport report;
  report.complete_image_verified_before_write = true;
  report.backup_manifest_verified_before_write = true;
  report.partition_style = metadata.value().manifest.partition_style;
  report.boot_mode = metadata.value().manifest.boot_mode;
  report.contains_windows = metadata.value().manifest.source.is_system_disk;
  const auto windows_partition = std::find_if(
      metadata.value().manifest.partitions.begin(),
      metadata.value().manifest.partitions.end(),
      [](const BackupManifestPartition& partition) {
        return partition.role == BackupPartitionRole::windows_ntfs;
      });
  if (report.contains_windows &&
      windows_partition == metadata.value().manifest.partitions.end()) {
    return clonecore::Result<DcimgRestoreReport>::failure(restore_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_NOT_FOUND,
        L"dcimg復元後起動対象Windows領域",
        L"検証済みバックアップ情報からWindows領域を特定できません"));
  }
  if (windows_partition != metadata.value().manifest.partitions.end()) {
    report.windows_partition_offset = windows_partition->offset_bytes;
  }
  for (const auto& chunk : inspection.value().container.chunks) {
    publish_progress(
        request.callbacks,
        progress,
        clonecore::DiskOperationStage::copying_data,
        true);
    if (chunk.flags == DcimgChunkFlags::zero_filled) {
      status = write_zeroes_in_chunks(
          target,
          chunk.logical_offset,
          chunk.uncompressed_length,
          request.maximum_chunk_bytes,
          L"dcimgゼロチャンク復元・読戻し",
          request.callbacks,
          &progress,
          true);
    } else {
      auto chunk_bytes = read_and_verify_chunk_from_reader(
          reader,
          image_length,
          chunk,
          request.maximum_chunk_bytes,
          request.callbacks,
          progress,
          target);
      if (!chunk_bytes) {
        return clonecore::Result<DcimgRestoreReport>::failure(
            chunk_bytes.error());
      }
      status = write_span_in_chunks(
          target,
          chunk.logical_offset,
          chunk_bytes.value(),
          request.maximum_chunk_bytes,
          L"dcimgデータチャンク復元・読戻し",
          request.callbacks,
          &progress,
          true,
          false);
    }
    if (!status) {
      return clonecore::Result<DcimgRestoreReport>::failure(status.error());
    }
    report.restored_data_bytes += chunk.uncompressed_length;
    ++report.restored_chunk_count;
  }
  publish_progress(
      request.callbacks,
      progress,
      clonecore::DiskOperationStage::flushing_data,
      false);
  status = target.flush_target();
  if (!status) {
    return clonecore::Result<DcimgRestoreReport>::failure(status.error());
  }

  // Regions are canonical and sorted. Reverse commit writes a GPT backup
  // before the primary region at offset zero; MBR has one final sector.
  for (auto region = snapshot.regions.rbegin();
       region != snapshot.regions.rend();
       ++region) {
    const bool is_primary_commit = region->disk_offset == 0;
    if (is_primary_commit) {
      if (clonecore::disk_operation_cancellation_requested(
              request.callbacks)) {
        const auto flush_status = target.flush_target();
        if (!flush_status) {
          return clonecore::Result<DcimgRestoreReport>::failure(
              flush_status.error());
        }
        return clonecore::Result<DcimgRestoreReport>::failure(
            cancelled_status(L"dcimgパーティション表最終確定前").error());
      }
      publish_progress(
          request.callbacks,
          progress,
          clonecore::DiskOperationStage::committing_partition_table,
          false);
    } else {
      publish_progress(
          request.callbacks,
          progress,
          clonecore::DiskOperationStage::staging_partition_table,
          true);
    }
    status = write_span_in_chunks(
        target,
        region->disk_offset,
        region->data,
        request.maximum_chunk_bytes,
        L"dcimgパーティション表確定・読戻し",
        request.callbacks,
        nullptr,
        !is_primary_commit,
        false);
    if (!status) {
      return clonecore::Result<DcimgRestoreReport>::failure(status.error());
    }
    report.committed_partition_table_bytes += region->data.size();
  }
  status = target.flush_target();
  if (!status) {
    return clonecore::Result<DcimgRestoreReport>::failure(status.error());
  }
  report.read_back_verified = true;
  report.partition_table_committed = true;
  publish_progress(
      request.callbacks,
      progress,
      clonecore::DiskOperationStage::completed,
      false);
  return clonecore::Result<DcimgRestoreReport>::success(report);
}

clonecore::Result<DcimgRestoreReport>
restore_verified_dcimg_v1_from_reader(
    const std::uint64_t image_length,
    const Sha256ReadCallback& reader,
    const DcimgRestoreRequest& request,
    clonecore::ITargetDiskWriter& target) {
  auto prepared = prepare_verified_dcimg_v1_from_reader(
      image_length, reader, request);
  if (!prepared) {
    return clonecore::Result<DcimgRestoreReport>::failure(
        prepared.error());
  }
  return restore_prepared_dcimg_v1(
      prepared.take_value(), request, target);
}

clonecore::Result<DcimgRestoreReport> restore_verified_dcimg_v1(
    const std::span<const std::byte> image,
    const DcimgRestoreRequest& request,
    clonecore::ITargetDiskWriter& target) {
  const Sha256ReadCallback reader =
      [image](
          const std::uint64_t offset,
          const std::size_t length)
      -> clonecore::Result<std::vector<std::byte>> {
    if (offset > image.size() || length > image.size() - offset) {
      return clonecore::Result<std::vector<std::byte>>::failure(
          restore_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"dcimgメモリイメージ読取り",
              L"メモリイメージの読取り範囲が不正です"));
    }
    const auto source = image.subspan(
        static_cast<std::size_t>(offset), length);
    return clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(source.begin(), source.end()));
  };
  return restore_verified_dcimg_v1_from_reader(
      image.size(), reader, request, target);
}

}  // namespace ytec::imageformat

#include "ytec/clonecore/rescue_copy.h"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ytec::clonecore {
namespace {

constexpr std::size_t kMaximumLargeBlockBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumBookkeepingEntries = 1048576U;

Error rescue_error(
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

bool is_power_of_two(const std::uint64_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

bool checked_add(
    const std::uint64_t left,
    const std::uint64_t right,
    std::uint64_t& output) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  output = left + right;
  return true;
}

Status add_counter(
    std::uint64_t& counter,
    const std::uint64_t amount,
    const std::wstring_view operation) {
  std::uint64_t result{};
  if (!checked_add(counter, amount, result)) {
    return Status::failure(rescue_error(
        ErrorCode::invalid_data,
        ERROR_ARITHMETIC_OVERFLOW,
        std::wstring(operation),
        L"救出処理のカウンターが表現可能な範囲を超えました"));
  }
  counter = result;
  return success_status();
}

bool is_recoverable_media_read_error(const Error& error) noexcept {
  if (error.code != ErrorCode::io_failed) {
    return false;
  }
  switch (error.native_code) {
    case ERROR_CRC:
    case ERROR_SECTOR_NOT_FOUND:
    case ERROR_READ_FAULT:
    case ERROR_GEN_FAILURE:
    case ERROR_IO_DEVICE:
      return true;
    default:
      return false;
  }
}

struct ReadAttempt final {
  bool media_failure{};
  std::uint32_t native_error{};
  std::vector<std::byte> bytes;
};

struct FailedBlock final {
  ByteRange bytes;
  std::uint32_t forward_native_error{};
  std::uint32_t reverse_native_error{};
};

Result<ReadAttempt> attempt_source_read(
    const ISourceDiskReader& source,
    const std::uint64_t offset,
    const std::size_t length) {
  auto read = source.read(offset, length);
  if (!read) {
    if (is_recoverable_media_read_error(read.error())) {
      return Result<ReadAttempt>::success(ReadAttempt{
          .media_failure = true,
          .native_error = read.error().native_code,
      });
    }
    return Result<ReadAttempt>::failure(read.error());
  }
  if (read.value().size() > length) {
    return Result<ReadAttempt>::failure(rescue_error(
        ErrorCode::invalid_data,
        ERROR_INVALID_DATA,
        L"救出コピー元Reader契約",
        L"コピー元Readerが要求量を超えるデータを返しました"));
  }
  if (read.value().size() != length) {
    // A short raw-device read is retried in the next finite rescue pass.  Its
    // partial bytes are never written because their exact boundary is unknown.
    return Result<ReadAttempt>::success(ReadAttempt{
        .media_failure = true,
        .native_error = ERROR_HANDLE_EOF,
    });
  }
  return Result<ReadAttempt>::success(ReadAttempt{
      .media_failure = false,
      .bytes = read.take_value(),
  });
}

Status write_and_read_back_verify(
    ITargetDiskWriter& target,
    const std::uint64_t offset,
    const std::span<const std::byte> bytes,
    bool& target_mutation_may_have_started) {
  // A failed writer may have performed a partial device write, so the mutation
  // boundary starts immediately before invoking it.
  target_mutation_may_have_started = true;
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
    return Status::failure(rescue_error(
        ErrorCode::verification_failed,
        ERROR_CRC,
        L"救出コピー先の読戻し検証",
        L"コピー先へ書き込んだ内容と読戻し内容が一致しません"));
  }
  return success_status();
}

void publish_progress(
    const RescueCopyCallbacks& callbacks,
    const RescueCopyProgress& progress) noexcept {
  if (!callbacks.progress) {
    return;
  }
  try {
    callbacks.progress(progress);
  } catch (...) {
    // Observation must never unwind through the destructive engine.
  }
}

bool cancellation_requested(const RescueCopyCallbacks& callbacks) noexcept {
  if (!callbacks.cancellation_requested) {
    return false;
  }
  try {
    return callbacks.cancellation_requested();
  } catch (...) {
    // A broken observer is a fail-closed cancellation request.
    return true;
  }
}

Error cancelled_error(const std::wstring_view operation) {
  return rescue_error(
      ErrorCode::cancelled,
      ERROR_CANCELLED,
      std::wstring(operation),
      L"読戻し検証済みの安全な境界で救出コピーを中止しました");
}

Status validate_request(
    const RescueRawCopyRequest& request,
    const ISourceDiskReader& source,
    const ITargetDiskWriter& target) {
  if (!request.rescue_mode_explicitly_confirmed) {
    return Status::failure(rescue_error(
        ErrorCode::confirmation_required,
        ERROR_CANCELLED,
        L"救出モードの明示確認",
        L"救出モードが明示選択されていません"));
  }
  if (request.source_kind == RescueSourceKind::system_disk &&
      request.environment != RescueExecutionEnvironment::winpe) {
    return Status::failure(rescue_error(
        ErrorCode::unsupported_platform,
        ERROR_NOT_SUPPORTED,
        L"システムディスク救出の実行環境",
        L"システムディスクの救出はWinPE上でのみ実行できます"));
  }

  const std::uint64_t source_size = source.size_bytes();
  const std::uint64_t target_size = target.size_bytes();
  const std::uint32_t source_sector = source.logical_sector_size();
  const std::uint32_t target_sector = target.logical_sector_size();
  if (source_size == 0U || source_sector < 512U || source_sector > 4096U ||
      !is_power_of_two(source_sector) || source_sector != target_sector ||
      source_size % source_sector != 0U || target_size % target_sector != 0U ||
      target_size < source_size) {
    return Status::failure(rescue_error(
        ErrorCode::unsupported_layout,
        ERROR_NOT_SUPPORTED,
        L"救出RAWコピーのディスク寸法",
        L"同一論理セクターでコピー先が同容量以上のRAWコピーだけを扱います"));
  }
  if (request.large_block_bytes < source_sector ||
      request.large_block_bytes > kMaximumLargeBlockBytes ||
      request.large_block_bytes % source_sector != 0U) {
    return Status::failure(rescue_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"救出コピーの大ブロック設定",
        L"大ブロックは論理セクター整列かつ16MiB以下である必要があります"));
  }
  if (request.maximum_failed_block_count == 0U ||
      request.maximum_failed_block_count > kMaximumBookkeepingEntries ||
      request.maximum_missing_range_count == 0U ||
      request.maximum_missing_range_count > kMaximumBookkeepingEntries) {
    return Status::failure(rescue_error(
        ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"救出コピーのメモリ上限",
        L"失敗ブロック数と欠損範囲数の上限が許可範囲外です"));
  }
  return success_status();
}

Status append_missing_sector(
    std::vector<RescueMissingRange>& ranges,
    const std::size_t maximum_range_count,
    const std::uint64_t offset,
    const std::uint32_t sector_size,
    const std::uint32_t forward_native_error,
    const std::uint32_t reverse_native_error,
    const std::uint32_t sector_native_error) {
  if (!ranges.empty()) {
    auto& previous = ranges.back();
    std::uint64_t previous_end{};
    if (!checked_add(previous.bytes.offset, previous.bytes.length, previous_end)) {
      return Status::failure(rescue_error(
          ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"救出欠損マップ結合",
          L"欠損範囲の終端がオーバーフローしました"));
    }
    if (previous_end == offset &&
        previous.forward_native_error == forward_native_error &&
        previous.reverse_native_error == reverse_native_error &&
        previous.sector_native_error == sector_native_error) {
      const Status length_status = add_counter(
          previous.bytes.length, sector_size, L"救出欠損範囲長");
      if (!length_status) {
        return length_status;
      }
      return add_counter(
          previous.sector_count, 1U, L"救出欠損セクター数");
    }
  }
  if (ranges.size() >= maximum_range_count) {
    return Status::failure(rescue_error(
        ErrorCode::invalid_data,
        ERROR_NOT_ENOUGH_MEMORY,
        L"救出欠損マップ上限",
        L"正確な欠損マップを保持できる上限を超えたため停止しました"));
  }
  ranges.push_back(RescueMissingRange{
      .bytes = ByteRange{.offset = offset, .length = sector_size},
      .first_lba = offset / sector_size,
      .sector_count = 1U,
      .forward_attempts = 1U,
      .reverse_attempts = 1U,
      .sector_attempts = 1U,
      .forward_native_error = forward_native_error,
      .reverse_native_error = reverse_native_error,
      .sector_native_error = sector_native_error,
      .zero_fill_read_back_verified = true,
  });
  return success_status();
}

Status ensure_missing_sector_can_be_recorded(
    const std::vector<RescueMissingRange>& ranges,
    const std::size_t maximum_range_count,
    const std::uint64_t offset,
    const std::uint32_t forward_native_error,
    const std::uint32_t reverse_native_error,
    const std::uint32_t sector_native_error) {
  if (!ranges.empty()) {
    std::uint64_t previous_end{};
    if (!checked_add(
            ranges.back().bytes.offset,
            ranges.back().bytes.length,
            previous_end)) {
      return Status::failure(rescue_error(
          ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"救出欠損マップ事前検証",
          L"欠損範囲の終端がオーバーフローしました"));
    }
    if (previous_end == offset &&
        ranges.back().forward_native_error == forward_native_error &&
        ranges.back().reverse_native_error == reverse_native_error &&
        ranges.back().sector_native_error == sector_native_error) {
      return success_status();
    }
  }
  if (ranges.size() >= maximum_range_count) {
    return Status::failure(rescue_error(
        ErrorCode::invalid_data,
        ERROR_NOT_ENOUGH_MEMORY,
        L"救出欠損マップ上限",
        L"正確な欠損マップを保持できる上限を超えたため停止しました"));
  }
  return success_status();
}

Status normalize_missing_ranges(
    std::vector<RescueMissingRange>& ranges,
    const std::uint32_t sector_size) {
  std::sort(
      ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.bytes.offset < right.bytes.offset;
      });
  std::size_t output_count = 0U;
  for (auto& range : ranges) {
    if (output_count != 0U) {
      auto& previous = ranges[output_count - 1U];
      std::uint64_t previous_end{};
      if (!checked_add(previous.bytes.offset, previous.bytes.length, previous_end)) {
        return Status::failure(rescue_error(
            ErrorCode::invalid_data,
            ERROR_ARITHMETIC_OVERFLOW,
            L"救出欠損マップ整列",
            L"欠損範囲の終端がオーバーフローしました"));
      }
      if (range.bytes.offset < previous_end) {
        return Status::failure(rescue_error(
            ErrorCode::internal_error,
            ERROR_INVALID_DATA,
            L"救出欠損マップ重複",
            L"内部の欠損範囲が重複しています"));
      }
      if (range.bytes.offset == previous_end &&
          previous.forward_native_error == range.forward_native_error &&
          previous.reverse_native_error == range.reverse_native_error &&
          previous.sector_native_error == range.sector_native_error) {
        const Status length_status = add_counter(
            previous.bytes.length, range.bytes.length, L"救出欠損範囲結合");
        if (!length_status) {
          return length_status;
        }
        const Status count_status = add_counter(
            previous.sector_count,
            range.sector_count,
            L"救出欠損セクター結合");
        if (!count_status) {
          return count_status;
        }
        continue;
      }
    }
    ranges[output_count++] = std::move(range);
  }
  ranges.resize(output_count);
  for (const auto& range : ranges) {
    if (range.bytes.length == 0U || range.bytes.offset % sector_size != 0U ||
        range.bytes.length % sector_size != 0U ||
        range.first_lba != range.bytes.offset / sector_size ||
        range.sector_count != range.bytes.length / sector_size ||
        !range.zero_fill_read_back_verified) {
      return Status::failure(rescue_error(
          ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"救出欠損マップ最終検証",
          L"欠損マップの境界または読戻し検証状態が不正です"));
    }
  }
  return success_status();
}

}  // namespace

Result<RescueRawCopyReport> execute_rescue_raw_copy(
    const RescueRawCopyRequest& request,
    const ISourceDiskReader& source,
    ITargetDiskWriter& target) {
  RescueCopyProgress progress{
      .phase = RescueCopyPhase::validating,
      .source_extent_bytes = source.size_bytes(),
      .cancellation_allowed = true,
  };
  publish_progress(request.callbacks, progress);

  const Status request_status = validate_request(request, source, target);
  if (!request_status) {
    return Result<RescueRawCopyReport>::failure(request_status.error());
  }
  if (cancellation_requested(request.callbacks)) {
    return Result<RescueRawCopyReport>::failure(
        cancelled_error(L"救出コピー開始前"));
  }

  bool target_mutation_may_have_started = false;
  auto fail_after_safe_flush = [&](Error error) -> Result<RescueRawCopyReport> {
    if (target_mutation_may_have_started) {
      const Status flushed = target.flush_target();
      if (!flushed) {
        return Result<RescueRawCopyReport>::failure(flushed.error());
      }
    }
    return Result<RescueRawCopyReport>::failure(std::move(error));
  };
  auto stop_if_cancelled = [&](const std::wstring_view operation)
      -> std::optional<Result<RescueRawCopyReport>> {
    if (!cancellation_requested(request.callbacks)) {
      return std::nullopt;
    }
    return fail_after_safe_flush(cancelled_error(operation));
  };
  std::uint64_t verified_boundary_count = 0U;
  auto stop_at_verified_boundary = [&](const std::wstring_view operation)
      -> std::optional<Result<RescueRawCopyReport>> {
    ++verified_boundary_count;
    progress.pause_allowed = true;
    publish_progress(request.callbacks, progress);
    if (rescue_copy_control_at_safe_boundary(
            request.callbacks,
            DiskOperationSafeBoundary{
                .kind = DiskOperationSafeBoundaryKind::verified_chunk,
                .stage = DiskOperationStage::copying_data,
                .completed_bytes = progress.settled_target_bytes,
                .completed_units = verified_boundary_count,
            }) == DiskOperationControlDecision::cancel_operation) {
      return fail_after_safe_flush(cancelled_error(operation));
    }
    return std::nullopt;
  };

  try {
    const std::uint64_t source_size = source.size_bytes();
    const std::uint32_t sector_size = source.logical_sector_size();
    RescueRawCopyReport report{
        .source_extent_bytes = source_size,
        .layout_preserved_without_conversion = true,
    };
    std::vector<FailedBlock> failed_blocks;
    failed_blocks.reserve(std::min<std::size_t>(
        request.maximum_failed_block_count, 4096U));

    auto account_verified_write = [&](const std::uint64_t byte_count,
                                      const bool zero_filled,
                                      const bool recovered) -> Status {
      const Status verified = add_counter(
          report.written_and_read_back_verified_bytes,
          byte_count,
          L"救出コピー読戻し検証済みバイト数");
      if (!verified) {
        return verified;
      }
      const Status settled = add_counter(
          progress.settled_target_bytes,
          byte_count,
          L"救出コピー確定済みバイト数");
      if (!settled) {
        return settled;
      }
      if (zero_filled) {
        const Status zeroes = add_counter(
            report.zero_filled_bytes, byte_count, L"救出コピーゼロ埋め量");
        if (!zeroes) {
          return zeroes;
        }
        progress.zero_filled_bytes = report.zero_filled_bytes;
      } else {
        const Status copied = add_counter(
            report.copied_source_bytes,
            byte_count,
            L"救出コピー元データ量");
        if (!copied) {
          return copied;
        }
        if (recovered) {
          return add_counter(
              report.recovered_bytes,
              byte_count,
              L"救出コピー回復データ量");
        }
      }
      return success_status();
    };
    auto resolve_outstanding = [&](const std::uint64_t byte_count) -> Status {
      if (byte_count > progress.outstanding_failed_bytes) {
        return Status::failure(rescue_error(
            ErrorCode::internal_error,
            ERROR_ARITHMETIC_OVERFLOW,
            L"救出未解決バイト数の減算",
            L"未解決バイト数が内部で不正に減少しました"));
      }
      progress.outstanding_failed_bytes -= byte_count;
      return success_status();
    };

    progress.phase = RescueCopyPhase::forward_read;
    progress.pause_allowed = false;
    publish_progress(request.callbacks, progress);
    for (std::uint64_t offset = 0U; offset < source_size;) {
      if (auto cancelled = stop_if_cancelled(L"救出コピー前方読取り");
          cancelled.has_value()) {
        return std::move(cancelled.value());
      }
      const std::uint64_t remaining = source_size - offset;
      const std::size_t length = static_cast<std::size_t>(
          std::min<std::uint64_t>(remaining, request.large_block_bytes));
      progress.current_offset = offset;
      const auto attempt = attempt_source_read(source, offset, length);
      if (!attempt) {
        return fail_after_safe_flush(attempt.error());
      }
      bool verified_write = false;
      if (attempt.value().media_failure) {
        if (failed_blocks.size() >= request.maximum_failed_block_count) {
          return fail_after_safe_flush(rescue_error(
              ErrorCode::invalid_data,
              ERROR_NOT_ENOUGH_MEMORY,
              L"救出失敗ブロック上限",
              L"失敗ブロックを完全に追跡できる上限を超えたため停止しました"));
        }
        failed_blocks.push_back(FailedBlock{
            .bytes = ByteRange{.offset = offset, .length = length},
            .forward_native_error = attempt.value().native_error,
        });
        const Status failure_count = add_counter(
            report.forward_failed_block_count,
            1U,
            L"救出前方失敗ブロック数");
        if (!failure_count) {
          return fail_after_safe_flush(failure_count.error());
        }
        const Status outstanding = add_counter(
            progress.outstanding_failed_bytes,
            length,
            L"救出未解決バイト数");
        if (!outstanding) {
          return fail_after_safe_flush(outstanding.error());
        }
      } else {
        const Status written = write_and_read_back_verify(
            target,
            offset,
            attempt.value().bytes,
            target_mutation_may_have_started);
        if (!written) {
          return fail_after_safe_flush(written.error());
        }
        const Status accounted = account_verified_write(length, false, false);
        if (!accounted) {
          return fail_after_safe_flush(accounted.error());
        }
        verified_write = true;
      }
      if (verified_write) {
        if (auto cancelled =
                stop_at_verified_boundary(L"救出コピー前方安全境界");
            cancelled.has_value()) {
          return std::move(cancelled.value());
        }
      } else {
        progress.pause_allowed = false;
        publish_progress(request.callbacks, progress);
      }
      offset += length;
    }

    std::vector<FailedBlock> sector_retry_blocks;
    sector_retry_blocks.reserve(failed_blocks.size());
    progress.phase = RescueCopyPhase::reverse_retry;
    progress.pause_allowed = false;
    publish_progress(request.callbacks, progress);
    for (auto failed = failed_blocks.rbegin(); failed != failed_blocks.rend();
         ++failed) {
      if (auto cancelled = stop_if_cancelled(L"救出コピー逆方向再試行");
          cancelled.has_value()) {
        return std::move(cancelled.value());
      }
      progress.current_offset = failed->bytes.offset;
      const std::size_t length =
          static_cast<std::size_t>(failed->bytes.length);
      const auto attempt =
          attempt_source_read(source, failed->bytes.offset, length);
      if (!attempt) {
        return fail_after_safe_flush(attempt.error());
      }
      bool verified_write = false;
      if (attempt.value().media_failure) {
        sector_retry_blocks.push_back(FailedBlock{
            .bytes = failed->bytes,
            .forward_native_error = failed->forward_native_error,
            .reverse_native_error = attempt.value().native_error,
        });
        const Status failure_count = add_counter(
            report.reverse_failed_block_count,
            1U,
            L"救出逆方向失敗ブロック数");
        if (!failure_count) {
          return fail_after_safe_flush(failure_count.error());
        }
      } else {
        const Status written = write_and_read_back_verify(
            target,
            failed->bytes.offset,
            attempt.value().bytes,
            target_mutation_may_have_started);
        if (!written) {
          return fail_after_safe_flush(written.error());
        }
        const Status accounted = account_verified_write(length, false, true);
        if (!accounted) {
          return fail_after_safe_flush(accounted.error());
        }
        const Status recovery_count = add_counter(
            report.reverse_recovered_block_count,
            1U,
            L"救出逆方向回復ブロック数");
        if (!recovery_count) {
          return fail_after_safe_flush(recovery_count.error());
        }
        const Status resolved = resolve_outstanding(failed->bytes.length);
        if (!resolved) {
          return fail_after_safe_flush(resolved.error());
        }
        verified_write = true;
      }
      if (verified_write) {
        if (auto cancelled =
                stop_at_verified_boundary(L"救出コピー逆方向安全境界");
            cancelled.has_value()) {
          return std::move(cancelled.value());
        }
      } else {
        progress.pause_allowed = false;
        publish_progress(request.callbacks, progress);
      }
    }

    report.missing_ranges.reserve(std::min<std::size_t>(
        request.maximum_missing_range_count, 4096U));
    const std::vector<std::byte> zero_sector(sector_size, std::byte{0});
    progress.phase = RescueCopyPhase::sector_retry;
    progress.pause_allowed = false;
    publish_progress(request.callbacks, progress);
    for (const FailedBlock& failed : sector_retry_blocks) {
      std::uint64_t failed_end{};
      if (!checked_add(
              failed.bytes.offset, failed.bytes.length, failed_end) ||
          failed_end > source_size) {
        return fail_after_safe_flush(rescue_error(
            ErrorCode::internal_error,
            ERROR_ARITHMETIC_OVERFLOW,
            L"救出小ブロック範囲",
            L"再試行ブロックの終端がコピー元境界を超えました"));
      }
      for (std::uint64_t offset = failed.bytes.offset; offset < failed_end;
           offset += sector_size) {
        if (auto cancelled = stop_if_cancelled(L"救出コピー小ブロック再試行");
            cancelled.has_value()) {
          return std::move(cancelled.value());
        }
        progress.current_offset = offset;
        const auto attempt = attempt_source_read(source, offset, sector_size);
        if (!attempt) {
          return fail_after_safe_flush(attempt.error());
        }
        if (attempt.value().media_failure) {
          const Status map_capacity = ensure_missing_sector_can_be_recorded(
              report.missing_ranges,
              request.maximum_missing_range_count,
              offset,
              failed.forward_native_error,
              failed.reverse_native_error,
              attempt.value().native_error);
          if (!map_capacity) {
            return fail_after_safe_flush(map_capacity.error());
          }
          const Status written = write_and_read_back_verify(
              target,
              offset,
              zero_sector,
              target_mutation_may_have_started);
          if (!written) {
            return fail_after_safe_flush(written.error());
          }
          const Status missing = append_missing_sector(
              report.missing_ranges,
              request.maximum_missing_range_count,
              offset,
              sector_size,
              failed.forward_native_error,
              failed.reverse_native_error,
              attempt.value().native_error);
          if (!missing) {
            return fail_after_safe_flush(missing.error());
          }
          const Status accounted = account_verified_write(
              sector_size, true, false);
          if (!accounted) {
            return fail_after_safe_flush(accounted.error());
          }
          const Status exhausted_count = add_counter(
              report.exhausted_sector_count,
              1U,
              L"救出不能セクター数");
          if (!exhausted_count) {
            return fail_after_safe_flush(exhausted_count.error());
          }
        } else {
          const Status written = write_and_read_back_verify(
              target,
              offset,
              attempt.value().bytes,
              target_mutation_may_have_started);
          if (!written) {
            return fail_after_safe_flush(written.error());
          }
          const Status accounted = account_verified_write(
              sector_size, false, true);
          if (!accounted) {
            return fail_after_safe_flush(accounted.error());
          }
          const Status recovered_count = add_counter(
              report.sector_recovered_count,
              1U,
              L"救出小ブロック回復数");
          if (!recovered_count) {
            return fail_after_safe_flush(recovered_count.error());
          }
        }
        const Status resolved = resolve_outstanding(sector_size);
        if (!resolved) {
          return fail_after_safe_flush(resolved.error());
        }
        if (auto cancelled =
                stop_at_verified_boundary(L"救出コピー小ブロック安全境界");
            cancelled.has_value()) {
          return std::move(cancelled.value());
        }
      }
    }

    progress.pause_allowed = false;
    publish_progress(request.callbacks, progress);
    const Status normalized =
        normalize_missing_ranges(report.missing_ranges, sector_size);
    if (!normalized) {
      return fail_after_safe_flush(normalized.error());
    }
    if (progress.settled_target_bytes != source_size ||
        progress.outstanding_failed_bytes != 0U ||
        report.written_and_read_back_verified_bytes != source_size ||
        report.copied_source_bytes > source_size ||
        report.zero_filled_bytes > source_size - report.copied_source_bytes ||
        report.copied_source_bytes + report.zero_filled_bytes != source_size ||
        (report.missing_ranges.empty() != (report.zero_filled_bytes == 0U))) {
      return fail_after_safe_flush(rescue_error(
          ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"救出コピー最終不変条件",
          L"確定済みデータ量、欠損マップ、またはゼロ埋め量が一致しません"));
    }

    progress.phase = RescueCopyPhase::flushing;
    progress.cancellation_allowed = false;
    progress.pause_allowed = false;
    publish_progress(request.callbacks, progress);
    const Status flushed = target.flush_target();
    if (!flushed) {
      return Result<RescueRawCopyReport>::failure(flushed.error());
    }
    report.target_flushed = true;
    report.all_writes_read_back_verified = true;
    report.partial_data_loss = !report.missing_ranges.empty();
    report.byte_exact_copy = !report.partial_data_loss;

    // This explicit invariant prevents a caller from ever receiving an
    // unlabelled zero-filled rescue result.
    if ((report.zero_filled_bytes != 0U || report.exhausted_sector_count != 0U) &&
        !report.partial_data_loss) {
      return Result<RescueRawCopyReport>::failure(rescue_error(
          ErrorCode::internal_error,
          ERROR_INVALID_DATA,
          L"救出コピー欠損表示",
          L"欠損を一部欠損として報告できないため結果を拒否しました"));
    }

    progress.phase = RescueCopyPhase::completed;
    publish_progress(request.callbacks, progress);
    return Result<RescueRawCopyReport>::success(std::move(report));
  } catch (const std::bad_alloc&) {
    return fail_after_safe_flush(rescue_error(
        ErrorCode::internal_error,
        ERROR_NOT_ENOUGH_MEMORY,
        L"救出コピーのメモリ確保",
        L"設定した上限内の作業メモリを確保できませんでした"));
  } catch (...) {
    return fail_after_safe_flush(rescue_error(
        ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"救出コピー内部例外",
        L"予期しない例外を安全な境界で停止しました"));
  }
}

}  // namespace ytec::clonecore

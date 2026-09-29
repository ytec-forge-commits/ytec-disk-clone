#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/operation_progress.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ytec::clonecore {

// Rescue copy is deliberately a separate raw-copy engine.  It never shrinks a
// filesystem and never changes a partition style.  Stable-identity checks and
// the destructive target authorization remain caller responsibilities.
enum class RescueExecutionEnvironment : std::uint8_t {
  windows,
  winpe,
};

enum class RescueSourceKind : std::uint8_t {
  data_disk,
  system_disk,
};

enum class RescueCopyPhase : std::uint8_t {
  validating,
  forward_read,
  reverse_retry,
  sector_retry,
  flushing,
  completed,
};

struct RescueCopyProgress final {
  RescueCopyPhase phase{RescueCopyPhase::validating};
  std::uint64_t source_extent_bytes{};
  std::uint64_t current_offset{};
  std::uint64_t settled_target_bytes{};
  std::uint64_t zero_filled_bytes{};
  std::uint64_t outstanding_failed_bytes{};
  bool cancellation_allowed{};
  bool pause_allowed{};
};

struct RescueCopyCallbacks final {
  std::function<void(const RescueCopyProgress&)> progress;
  std::function<bool()> cancellation_requested;
  DiskOperationSafeBoundaryCallback safe_boundary;
};

[[nodiscard]] inline DiskOperationControlDecision
rescue_copy_control_at_safe_boundary(
    const RescueCopyCallbacks& callbacks,
    const DiskOperationSafeBoundary& boundary) noexcept {
  if (!callbacks.safe_boundary) {
    return DiskOperationControlDecision::continue_operation;
  }
  try {
    return callbacks.safe_boundary(boundary);
  } catch (...) {
    return DiskOperationControlDecision::cancel_operation;
  }
}

struct RescueMissingRange final {
  ByteRange bytes;
  std::uint64_t first_lba{};
  std::uint64_t sector_count{};
  std::uint8_t forward_attempts{};
  std::uint8_t reverse_attempts{};
  std::uint8_t sector_attempts{};
  std::uint32_t forward_native_error{};
  std::uint32_t reverse_native_error{};
  std::uint32_t sector_native_error{};
  bool zero_fill_read_back_verified{};
};

struct RescueRawCopyRequest final {
  RescueExecutionEnvironment environment{RescueExecutionEnvironment::windows};
  RescueSourceKind source_kind{RescueSourceKind::data_disk};

  // This is an additional misuse gate, not a replacement for OperationPlan's
  // re-identification and exact "OK" authorization.
  bool rescue_mode_explicitly_confirmed{};

  // One forward read and one reverse-order retry use this size.  A final retry
  // always uses exactly one logical sector so the loss map is sector-precise.
  std::size_t large_block_bytes{4U * 1024U * 1024U};

  // These caps bound attacker-controlled/failing-device bookkeeping.  The
  // operation fails closed instead of emitting an incomplete loss map.
  std::size_t maximum_failed_block_count{262144U};
  std::size_t maximum_missing_range_count{262144U};

  RescueCopyCallbacks callbacks;
};

struct RescueRawCopyReport final {
  std::uint64_t source_extent_bytes{};
  std::uint64_t copied_source_bytes{};
  std::uint64_t recovered_bytes{};
  std::uint64_t zero_filled_bytes{};
  std::uint64_t written_and_read_back_verified_bytes{};
  std::uint64_t forward_failed_block_count{};
  std::uint64_t reverse_recovered_block_count{};
  std::uint64_t reverse_failed_block_count{};
  std::uint64_t sector_recovered_count{};
  std::uint64_t exhausted_sector_count{};
  std::vector<RescueMissingRange> missing_ranges;
  bool layout_preserved_without_conversion{};
  bool byte_exact_copy{};
  bool target_flushed{};
  bool all_writes_read_back_verified{};
  bool partial_data_loss{};
};

[[nodiscard]] Result<RescueRawCopyReport> execute_rescue_raw_copy(
    const RescueRawCopyRequest& request,
    const ISourceDiskReader& source,
    ITargetDiskWriter& target);

}  // namespace ytec::clonecore

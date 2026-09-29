#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace ytec::clonecore {

enum class DiskOperationStage : std::uint8_t {
  planning,
  verifying_source,
  invalidating_target,
  copying_data,
  flushing_data,
  staging_partition_table,
  committing_partition_table,
  validating_conversion,
  converting_partition_style,
  rebuilding_boot,
  verifying_final,
  completed,
};

// A destructive engine may stop its worker only at one of these explicit
// boundaries.  In particular, no pause callback is made while target metadata
// is being invalidated or committed, while boot data is being rebuilt, or
// while an external snapshot provider is frozen/thawed.
enum class DiskOperationSafeBoundaryKind : std::uint8_t {
  read_only_block,
  verified_chunk,
  verified_partition,
};

struct DiskOperationSafeBoundary final {
  DiskOperationSafeBoundaryKind kind{
      DiskOperationSafeBoundaryKind::verified_chunk};
  DiskOperationStage stage{DiskOperationStage::copying_data};
  std::optional<std::uint32_t> partition_index;
  std::uint64_t completed_bytes{};
  std::uint64_t completed_units{};
};

enum class DiskOperationControlDecision : std::uint8_t {
  continue_operation,
  cancel_operation,
};

using DiskOperationSafeBoundaryCallback =
    std::function<DiskOperationControlDecision(
        const DiskOperationSafeBoundary&)>;

struct DiskOperationProgress final {
  DiskOperationStage stage{DiskOperationStage::planning};
  std::optional<std::uint32_t> partition_index;
  std::uint64_t total_read_bytes{};
  std::uint64_t total_write_bytes{};
  std::uint64_t total_verify_bytes{};
  std::uint64_t read_bytes{};
  std::uint64_t written_bytes{};
  std::uint64_t verified_bytes{};
  bool cancellation_allowed{};
  // This is true only when a pause request can be honoured at a bounded safe
  // boundary.  It is independent from cancellation_allowed: cancellation can
  // remain available while pausing is deliberately unavailable.
  bool pause_allowed{};
};

struct DiskOperationCallbacks final {
  std::function<void(const DiskOperationProgress&)> progress;
  std::function<bool()> cancellation_requested;
  DiskOperationSafeBoundaryCallback safe_boundary;
};

inline void report_disk_operation_progress(
    const DiskOperationCallbacks& callbacks,
    const DiskOperationProgress& progress) noexcept {
  if (!callbacks.progress) {
    return;
  }
  try {
    callbacks.progress(progress);
  } catch (...) {
    // Progress observation must never unwind through a destructive engine.
  }
}

[[nodiscard]] inline bool disk_operation_cancellation_requested(
    const DiskOperationCallbacks& callbacks) noexcept {
  if (!callbacks.cancellation_requested) {
    return false;
  }
  try {
    return callbacks.cancellation_requested();
  } catch (...) {
    // A broken cancellation observer is treated as a fail-closed request.
    return true;
  }
}

[[nodiscard]] inline DiskOperationControlDecision
disk_operation_control_at_safe_boundary(
    const DiskOperationCallbacks& callbacks,
    const DiskOperationSafeBoundary& boundary) noexcept {
  if (!callbacks.safe_boundary) {
    return DiskOperationControlDecision::continue_operation;
  }
  try {
    return callbacks.safe_boundary(boundary);
  } catch (...) {
    // A broken control observer must stop the destructive engine at the safe
    // boundary instead of letting it continue without operator control.
    return DiskOperationControlDecision::cancel_operation;
  }
}

}  // namespace ytec::clonecore

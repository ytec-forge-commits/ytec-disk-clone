#pragma once

#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/rescue_copy.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace ytec::clonecore {

enum class ManualPauseState : std::uint8_t {
  running,
  pause_requested,
  paused,
  cancelling,
  completed,
};

enum class ManualPauseAvailability : std::uint8_t {
  unavailable,
  available_at_safe_boundary,
};

struct ManualPauseSnapshot final {
  ManualPauseState state{ManualPauseState::running};
  ManualPauseAvailability availability{
      ManualPauseAvailability::unavailable};
  std::uint64_t sequence{};
  std::optional<DiskOperationSafeBoundary> last_safe_boundary;
};

// The callback may run on either the UI caller or operation worker thread. It
// must enqueue/post its UI update and return promptly; the controller never
// waits for a UI acknowledgement.
using ManualPauseEventCallback =
    std::function<void(const ManualPauseSnapshot&)>;

// One in-memory controller belongs to one currently executing operation. It
// does not provide persistent resume: only the worker blocks, and only when an
// engine explicitly reports a safe boundary after a verified unit/read-only
// block. UI-facing request methods never wait for the worker.
class ManualPauseController final {
 public:
  explicit ManualPauseController(ManualPauseEventCallback event = {});
  ~ManualPauseController();

  ManualPauseController(const ManualPauseController&) = delete;
  ManualPauseController& operator=(const ManualPauseController&) = delete;
  ManualPauseController(ManualPauseController&&) = delete;
  ManualPauseController& operator=(ManualPauseController&&) = delete;

  [[nodiscard]] bool request_pause() noexcept;
  [[nodiscard]] bool resume() noexcept;
  [[nodiscard]] bool request_cancel() noexcept;
  void mark_completed() noexcept;

  [[nodiscard]] bool cancellation_requested() const noexcept;
  [[nodiscard]] ManualPauseSnapshot snapshot() const noexcept;

  // Called by the progress adapter. A false pause_allowed value clears a
  // queued-but-not-yet-observed request, so it cannot cross into metadata,
  // boot-repair, VSS lifecycle, or another non-pausable interval.
  void observe_progress(const DiskOperationProgress& progress) noexcept;

  // Worker-only wait point. request_cancel() wakes a paused worker
  // immediately; an external legacy cancellation probe is polled while the
  // worker is paused and exceptions are treated as cancellation.
  [[nodiscard]] DiskOperationControlDecision wait_at_safe_boundary(
      const DiskOperationSafeBoundary& boundary,
      const std::function<bool()>& external_cancellation = {}) noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

// Preserves existing callbacks and adds the controller as a composable seam.
// The shared_ptr binds controller lifetime to every copied callback.
[[nodiscard]] DiskOperationCallbacks bind_manual_pause_controller(
    DiskOperationCallbacks callbacks,
    std::shared_ptr<ManualPauseController> controller);

[[nodiscard]] RescueCopyCallbacks bind_manual_pause_controller(
    RescueCopyCallbacks callbacks,
    std::shared_ptr<ManualPauseController> controller);

}  // namespace ytec::clonecore

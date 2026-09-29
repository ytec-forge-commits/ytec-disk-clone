#include "ytec/clonecore/manual_pause.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>

namespace ytec::clonecore {
namespace {

using EventHolder = std::shared_ptr<const ManualPauseEventCallback>;

void emit_event(
    const EventHolder& event,
    const ManualPauseSnapshot& snapshot) noexcept {
  if (!event || !*event) {
    return;
  }
  try {
    (*event)(snapshot);
  } catch (...) {
    // UI observation must never unwind through a destructive worker or an UI
    // command handler. Polling snapshot() remains available to the caller.
  }
}

bool external_cancellation_requested(
    const std::function<bool()>& cancellation) noexcept {
  if (!cancellation) {
    return false;
  }
  try {
    return cancellation();
  } catch (...) {
    return true;
  }
}

DiskOperationStage rescue_stage(const RescueCopyPhase phase) noexcept {
  switch (phase) {
    case RescueCopyPhase::validating:
      return DiskOperationStage::verifying_source;
    case RescueCopyPhase::forward_read:
    case RescueCopyPhase::reverse_retry:
    case RescueCopyPhase::sector_retry:
      return DiskOperationStage::copying_data;
    case RescueCopyPhase::flushing:
      return DiskOperationStage::flushing_data;
    case RescueCopyPhase::completed:
      return DiskOperationStage::completed;
  }
  return DiskOperationStage::planning;
}

}  // namespace

class ManualPauseController::Impl final {
 public:
  explicit Impl(ManualPauseEventCallback callback)
      : event(callback
                  ? std::make_shared<const ManualPauseEventCallback>(
                        std::move(callback))
                  : nullptr) {}

  [[nodiscard]] ManualPauseSnapshot snapshot_locked() const noexcept {
    return ManualPauseSnapshot{
        .state = state,
        .availability = availability,
        .sequence = sequence,
        .last_safe_boundary = last_safe_boundary,
    };
  }

  [[nodiscard]] ManualPauseSnapshot changed_locked() noexcept {
    ++sequence;
    return snapshot_locked();
  }

  mutable std::mutex mutex;
  std::condition_variable changed;
  ManualPauseState state{ManualPauseState::running};
  ManualPauseAvailability availability{
      ManualPauseAvailability::unavailable};
  std::uint64_t sequence{};
  std::optional<DiskOperationSafeBoundary> last_safe_boundary;
  EventHolder event;
};

ManualPauseController::ManualPauseController(ManualPauseEventCallback event)
    : impl_(std::make_unique<Impl>(std::move(event))) {}

ManualPauseController::~ManualPauseController() = default;

bool ManualPauseController::request_pause() noexcept {
  EventHolder event;
  ManualPauseSnapshot notification;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != ManualPauseState::running ||
        impl_->availability !=
            ManualPauseAvailability::available_at_safe_boundary) {
      return false;
    }
    impl_->state = ManualPauseState::pause_requested;
    notification = impl_->changed_locked();
    event = impl_->event;
  }
  emit_event(event, notification);
  return true;
}

bool ManualPauseController::resume() noexcept {
  EventHolder event;
  ManualPauseSnapshot notification;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != ManualPauseState::pause_requested &&
        impl_->state != ManualPauseState::paused) {
      return false;
    }
    impl_->state = ManualPauseState::running;
    notification = impl_->changed_locked();
    event = impl_->event;
  }
  impl_->changed.notify_all();
  emit_event(event, notification);
  return true;
}

bool ManualPauseController::request_cancel() noexcept {
  EventHolder event;
  ManualPauseSnapshot notification;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state == ManualPauseState::completed ||
        impl_->state == ManualPauseState::cancelling) {
      return false;
    }
    impl_->state = ManualPauseState::cancelling;
    impl_->availability = ManualPauseAvailability::unavailable;
    impl_->last_safe_boundary.reset();
    notification = impl_->changed_locked();
    event = impl_->event;
  }
  impl_->changed.notify_all();
  emit_event(event, notification);
  return true;
}

void ManualPauseController::mark_completed() noexcept {
  EventHolder event;
  ManualPauseSnapshot notification;
  bool notify = false;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != ManualPauseState::completed ||
        impl_->availability != ManualPauseAvailability::unavailable ||
        impl_->last_safe_boundary.has_value()) {
      impl_->state = ManualPauseState::completed;
      impl_->availability = ManualPauseAvailability::unavailable;
      impl_->last_safe_boundary.reset();
      notification = impl_->changed_locked();
      event = impl_->event;
      notify = true;
    }
  }
  impl_->changed.notify_all();
  if (notify) {
    emit_event(event, notification);
  }
}

bool ManualPauseController::cancellation_requested() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->state == ManualPauseState::cancelling;
}

ManualPauseSnapshot ManualPauseController::snapshot() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->snapshot_locked();
}

void ManualPauseController::observe_progress(
    const DiskOperationProgress& progress) noexcept {
  EventHolder event;
  ManualPauseSnapshot notification;
  bool notify = false;
  bool wake = false;
  {
    std::lock_guard lock(impl_->mutex);
    const auto next_availability = progress.pause_allowed
        ? ManualPauseAvailability::available_at_safe_boundary
        : ManualPauseAvailability::unavailable;
    if (progress.stage == DiskOperationStage::completed) {
      if (impl_->state != ManualPauseState::completed ||
          impl_->availability != ManualPauseAvailability::unavailable ||
          impl_->last_safe_boundary.has_value()) {
        impl_->state = ManualPauseState::completed;
        impl_->availability = ManualPauseAvailability::unavailable;
        impl_->last_safe_boundary.reset();
        notify = true;
        wake = true;
      }
    } else if (impl_->state != ManualPauseState::completed &&
               impl_->state != ManualPauseState::cancelling) {
      if (impl_->availability != next_availability) {
        impl_->availability = next_availability;
        notify = true;
      }
      if (!progress.pause_allowed) {
        if (impl_->last_safe_boundary.has_value()) {
          impl_->last_safe_boundary.reset();
          notify = true;
        }
        // A pause request accepted just after the preceding boundary must not
        // leak into an explicitly non-pausable transaction interval.
        if (impl_->state == ManualPauseState::pause_requested) {
          impl_->state = ManualPauseState::running;
          notify = true;
        }
      }
    }
    if (notify) {
      notification = impl_->changed_locked();
      event = impl_->event;
    }
  }
  if (wake) {
    impl_->changed.notify_all();
  }
  if (notify) {
    emit_event(event, notification);
  }
}

DiskOperationControlDecision ManualPauseController::wait_at_safe_boundary(
    const DiskOperationSafeBoundary& boundary,
    const std::function<bool()>& external_cancellation) noexcept {
  if (external_cancellation_requested(external_cancellation)) {
    static_cast<void>(request_cancel());
    return DiskOperationControlDecision::cancel_operation;
  }

  EventHolder event;
  ManualPauseSnapshot notification;
  bool notify = false;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state == ManualPauseState::cancelling ||
        impl_->state == ManualPauseState::completed) {
      return DiskOperationControlDecision::cancel_operation;
    }
    if (impl_->availability !=
            ManualPauseAvailability::available_at_safe_boundary ||
        !impl_->last_safe_boundary.has_value() ||
        impl_->last_safe_boundary->kind != boundary.kind ||
        impl_->last_safe_boundary->stage != boundary.stage ||
        impl_->last_safe_boundary->partition_index !=
            boundary.partition_index ||
        impl_->last_safe_boundary->completed_bytes !=
            boundary.completed_bytes ||
        impl_->last_safe_boundary->completed_units !=
            boundary.completed_units) {
      impl_->availability =
          ManualPauseAvailability::available_at_safe_boundary;
      impl_->last_safe_boundary = boundary;
      notify = true;
    }
    if (impl_->state == ManualPauseState::pause_requested) {
      impl_->state = ManualPauseState::paused;
      notify = true;
    }
    if (notify) {
      notification = impl_->changed_locked();
      event = impl_->event;
    }
  }
  if (notify) {
    emit_event(event, notification);
  }

  for (;;) {
    {
      std::unique_lock lock(impl_->mutex);
      if (impl_->state == ManualPauseState::cancelling ||
          impl_->state == ManualPauseState::completed) {
        return DiskOperationControlDecision::cancel_operation;
      }
      if (impl_->state != ManualPauseState::paused) {
        return DiskOperationControlDecision::continue_operation;
      }
      impl_->changed.wait_for(
          lock,
          std::chrono::milliseconds(25),
          [&] { return impl_->state != ManualPauseState::paused; });
      if (impl_->state == ManualPauseState::cancelling ||
          impl_->state == ManualPauseState::completed) {
        return DiskOperationControlDecision::cancel_operation;
      }
      if (impl_->state != ManualPauseState::paused) {
        return DiskOperationControlDecision::continue_operation;
      }
    }
    if (external_cancellation_requested(external_cancellation)) {
      static_cast<void>(request_cancel());
      return DiskOperationControlDecision::cancel_operation;
    }
  }
}

DiskOperationCallbacks bind_manual_pause_controller(
    DiskOperationCallbacks callbacks,
    std::shared_ptr<ManualPauseController> controller) {
  if (!controller) {
    return callbacks;
  }

  const auto existing_progress = std::move(callbacks.progress);
  const auto existing_cancellation =
      std::move(callbacks.cancellation_requested);
  const auto existing_boundary = std::move(callbacks.safe_boundary);

  return DiskOperationCallbacks{
      .progress =
          [controller, existing_progress](const DiskOperationProgress& value) {
            controller->observe_progress(value);
            if (existing_progress) {
              existing_progress(value);
            }
          },
      .cancellation_requested =
          [controller, existing_cancellation] {
            if (controller->cancellation_requested()) {
              return true;
            }
            if (!existing_cancellation) {
              return false;
            }
            bool requested = true;
            try {
              requested = existing_cancellation();
            } catch (...) {
              requested = true;
            }
            if (requested) {
              static_cast<void>(controller->request_cancel());
            }
            return requested;
          },
      .safe_boundary =
          [controller, existing_cancellation, existing_boundary](
              const DiskOperationSafeBoundary& boundary) {
            const auto decision = controller->wait_at_safe_boundary(
                boundary, existing_cancellation);
            if (decision == DiskOperationControlDecision::cancel_operation) {
              return decision;
            }
            if (!existing_boundary) {
              return DiskOperationControlDecision::continue_operation;
            }
            try {
              const auto existing = existing_boundary(boundary);
              if (existing ==
                  DiskOperationControlDecision::cancel_operation) {
                static_cast<void>(controller->request_cancel());
              }
              return existing;
            } catch (...) {
              static_cast<void>(controller->request_cancel());
              return DiskOperationControlDecision::cancel_operation;
            }
          },
  };
}

RescueCopyCallbacks bind_manual_pause_controller(
    RescueCopyCallbacks callbacks,
    std::shared_ptr<ManualPauseController> controller) {
  if (!controller) {
    return callbacks;
  }

  const auto existing_progress = std::move(callbacks.progress);
  const auto existing_cancellation =
      std::move(callbacks.cancellation_requested);
  const auto existing_boundary = std::move(callbacks.safe_boundary);

  return RescueCopyCallbacks{
      .progress =
          [controller, existing_progress](const RescueCopyProgress& value) {
            controller->observe_progress(DiskOperationProgress{
                .stage = rescue_stage(value.phase),
                .total_read_bytes = value.source_extent_bytes,
                .total_write_bytes = value.source_extent_bytes,
                .total_verify_bytes = value.source_extent_bytes,
                .read_bytes = value.settled_target_bytes,
                .written_bytes = value.settled_target_bytes,
                .verified_bytes = value.settled_target_bytes,
                .cancellation_allowed = value.cancellation_allowed,
                .pause_allowed = value.pause_allowed,
            });
            if (existing_progress) {
              existing_progress(value);
            }
          },
      .cancellation_requested =
          [controller, existing_cancellation] {
            if (controller->cancellation_requested()) {
              return true;
            }
            if (!existing_cancellation) {
              return false;
            }
            bool requested = true;
            try {
              requested = existing_cancellation();
            } catch (...) {
              requested = true;
            }
            if (requested) {
              static_cast<void>(controller->request_cancel());
            }
            return requested;
          },
      .safe_boundary =
          [controller, existing_cancellation, existing_boundary](
              const DiskOperationSafeBoundary& boundary) {
            const auto decision = controller->wait_at_safe_boundary(
                boundary, existing_cancellation);
            if (decision == DiskOperationControlDecision::cancel_operation) {
              return decision;
            }
            if (!existing_boundary) {
              return DiskOperationControlDecision::continue_operation;
            }
            try {
              const auto existing = existing_boundary(boundary);
              if (existing ==
                  DiskOperationControlDecision::cancel_operation) {
                static_cast<void>(controller->request_cancel());
              }
              return existing;
            } catch (...) {
              static_cast<void>(controller->request_cancel());
              return DiskOperationControlDecision::cancel_operation;
            }
          },
  };
}

}  // namespace ytec::clonecore

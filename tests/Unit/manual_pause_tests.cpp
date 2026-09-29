#include "ytec/clonecore/manual_pause.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Predicate>
bool wait_until(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

ytec::clonecore::DiskOperationProgress pausable_progress() {
  return ytec::clonecore::DiskOperationProgress{
      .stage = ytec::clonecore::DiskOperationStage::copying_data,
      .total_read_bytes = 4096U,
      .total_write_bytes = 4096U,
      .total_verify_bytes = 4096U,
      .read_bytes = 1024U,
      .written_bytes = 1024U,
      .verified_bytes = 1024U,
      .cancellation_allowed = true,
      .pause_allowed = true,
  };
}

ytec::clonecore::DiskOperationSafeBoundary safe_boundary() {
  return ytec::clonecore::DiskOperationSafeBoundary{
      .kind = ytec::clonecore::DiskOperationSafeBoundaryKind::verified_chunk,
      .stage = ytec::clonecore::DiskOperationStage::copying_data,
      .partition_index = 2U,
      .completed_bytes = 1024U,
      .completed_units = 1U,
  };
}

void unavailable_intervals_reject_and_clear_pause_requests() {
  using namespace ytec::clonecore;
  ManualPauseController controller;
  check(
      !controller.request_pause(),
      "Pause must be rejected before a safe boundary is advertised");

  controller.observe_progress(pausable_progress());
  check(
      controller.request_pause(),
      "A pause request must be accepted in a pausable interval");
  controller.observe_progress(DiskOperationProgress{
      .stage = DiskOperationStage::committing_partition_table,
      .cancellation_allowed = false,
      .pause_allowed = false,
  });
  const auto snapshot = controller.snapshot();
  check(
      snapshot.state == ManualPauseState::running &&
          snapshot.availability == ManualPauseAvailability::unavailable &&
          !snapshot.last_safe_boundary.has_value(),
      "A queued request must not cross into an unsafe metadata interval");
  check(
      !controller.request_pause(),
      "Metadata commit must remain visibly non-pausable");
  controller.observe_progress(DiskOperationProgress{
      .stage = DiskOperationStage::rebuilding_boot,
      .cancellation_allowed = false,
      .pause_allowed = false,
  });
  check(
      !controller.request_pause(),
      "BCDBoot/BCD reconstruction must remain visibly non-pausable");
}

void worker_waits_only_at_boundary_and_resume_wakes_it() {
  using namespace ytec::clonecore;
  auto controller = std::make_shared<ManualPauseController>();
  controller->observe_progress(pausable_progress());
  check(controller->request_pause(), "Pause request must be accepted");

  auto worker = std::async(std::launch::async, [controller] {
    return controller->wait_at_safe_boundary(safe_boundary());
  });
  const bool became_paused = wait_until([&] {
    return controller->snapshot().state == ManualPauseState::paused;
  });
  const bool remained_blocked = worker.wait_for(30ms) ==
      std::future_status::timeout;
  const bool resumed = controller->resume();
  const auto decision = worker.get();

  check(became_paused, "Worker must enter paused at the explicit boundary");
  check(remained_blocked, "Worker must stay blocked until resume or cancel");
  check(resumed, "Resume must be accepted while the worker is paused");
  check(
      decision == DiskOperationControlDecision::continue_operation &&
          controller->snapshot().state == ManualPauseState::running,
      "Resume must release the worker to continue");
}

void queued_pause_can_be_withdrawn_before_the_boundary() {
  using namespace ytec::clonecore;
  ManualPauseController controller;
  controller.observe_progress(pausable_progress());
  check(controller.request_pause(), "Pause request must be accepted");
  check(
      controller.snapshot().state == ManualPauseState::pause_requested,
      "The request must remain queued until a verified boundary");
  check(controller.resume(), "A queued pause request must be withdrawable");
  check(
      controller.wait_at_safe_boundary(safe_boundary()) ==
              DiskOperationControlDecision::continue_operation &&
          controller.snapshot().state == ManualPauseState::running,
      "A withdrawn request must not block at the next boundary");
}

void cancel_wakes_a_paused_worker_without_waiting_for_resume() {
  using namespace ytec::clonecore;
  auto controller = std::make_shared<ManualPauseController>();
  controller->observe_progress(pausable_progress());
  check(controller->request_pause(), "Pause request must be accepted");
  auto worker = std::async(std::launch::async, [controller] {
    return controller->wait_at_safe_boundary(safe_boundary());
  });
  const bool became_paused = wait_until([&] {
    return controller->snapshot().state == ManualPauseState::paused;
  });
  const bool cancelled = controller->request_cancel();
  const bool woke = worker.wait_for(500ms) == std::future_status::ready;
  const auto decision = worker.get();

  check(became_paused, "Worker must reach paused before cancellation");
  check(cancelled, "Cancellation must be accepted while paused");
  check(woke, "Cancellation must wake the paused worker immediately");
  check(
      decision == DiskOperationControlDecision::cancel_operation &&
          controller->snapshot().state == ManualPauseState::cancelling,
      "Paused cancellation must advance to the engine's safe cancel path");
}

void adapters_preserve_existing_callbacks_and_fail_closed() {
  using namespace ytec::clonecore;
  auto controller = std::make_shared<ManualPauseController>();
  std::uint32_t progress_calls = 0U;
  std::uint32_t boundary_calls = 0U;
  auto callbacks = bind_manual_pause_controller(
      DiskOperationCallbacks{
          .progress = [&](const DiskOperationProgress&) { ++progress_calls; },
          .cancellation_requested = [] { return false; },
          .safe_boundary = [&](const DiskOperationSafeBoundary&) {
            ++boundary_calls;
            return DiskOperationControlDecision::continue_operation;
          },
      },
      controller);
  report_disk_operation_progress(callbacks, pausable_progress());
  const auto decision =
      disk_operation_control_at_safe_boundary(callbacks, safe_boundary());
  const auto snapshot = controller->snapshot();
  check(
      progress_calls == 1U && boundary_calls == 1U,
      "Binding must preserve existing progress and safe-boundary callbacks");
  check(
      decision == DiskOperationControlDecision::continue_operation &&
          snapshot.last_safe_boundary.has_value() &&
          snapshot.last_safe_boundary->completed_bytes == 1024U,
      "Binding must publish the engine's exact boundary to the controller");

  DiskOperationCallbacks broken{
      .safe_boundary = [](const DiskOperationSafeBoundary&)
          -> DiskOperationControlDecision {
        throw std::runtime_error("broken observer");
      },
  };
  check(
      disk_operation_control_at_safe_boundary(broken, safe_boundary()) ==
          DiskOperationControlDecision::cancel_operation,
      "A throwing control observer must fail closed at the safe boundary");
}

void legacy_cancellation_probe_is_observed_while_paused() {
  using namespace ytec::clonecore;
  auto controller = std::make_shared<ManualPauseController>();
  std::atomic_bool legacy_cancel{false};
  auto callbacks = bind_manual_pause_controller(
      DiskOperationCallbacks{
          .cancellation_requested = [&] { return legacy_cancel.load(); },
      },
      controller);
  report_disk_operation_progress(callbacks, pausable_progress());
  check(controller->request_pause(), "Pause request must be accepted");
  auto worker = std::async(std::launch::async, [&] {
    return disk_operation_control_at_safe_boundary(
        callbacks, safe_boundary());
  });
  const bool became_paused = wait_until([&] {
    return controller->snapshot().state == ManualPauseState::paused;
  });
  legacy_cancel.store(true);
  const bool woke = worker.wait_for(500ms) == std::future_status::ready;
  const auto decision = worker.get();
  check(became_paused, "Worker must pause before legacy cancellation");
  check(
      woke && decision == DiskOperationControlDecision::cancel_operation,
      "Legacy cancellation must be polled and release a paused worker");
}

void event_sequence_is_monotonic_and_completion_is_terminal() {
  using namespace ytec::clonecore;
  std::mutex event_mutex;
  std::vector<std::uint64_t> sequences;
  ManualPauseController controller([&](const ManualPauseSnapshot& snapshot) {
    std::lock_guard lock(event_mutex);
    sequences.push_back(snapshot.sequence);
  });
  controller.observe_progress(pausable_progress());
  check(controller.request_pause(), "Pause request must be accepted");
  controller.mark_completed();
  check(
      !controller.resume() && !controller.request_pause() &&
          !controller.request_cancel(),
      "Completed state must reject further UI control mutations");
  {
    std::lock_guard lock(event_mutex);
    check(sequences.size() >= 3U, "State changes must emit event snapshots");
    for (std::size_t index = 1U; index < sequences.size(); ++index) {
      check(
          sequences[index] > sequences[index - 1U],
          "Event sequence numbers must increase monotonically");
    }
  }
}

}  // namespace

int main() {
  try {
    unavailable_intervals_reject_and_clear_pause_requests();
    worker_waits_only_at_boundary_and_resume_wakes_it();
    queued_pause_can_be_withdrawn_before_the_boundary();
    cancel_wakes_a_paused_worker_without_waiting_for_resume();
    adapters_preserve_existing_callbacks_and_fail_closed();
    legacy_cancellation_probe_is_observed_while_paused();
    event_sequence_is_monotonic_and_completion_is_terminal();
    std::cout << "manual pause tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "manual pause tests: FAIL: " << exception.what() << '\n';
    return 1;
  }
}

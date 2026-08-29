#include "ytec/vssrequester/diff_area_monitor.h"
#include "ytec/vssrequester/windows_diff_area_observer.h"

#include <Windows.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

std::wstring guid(const wchar_t suffix) {
  return std::wstring(L"{00000000-0000-0000-0000-") +
         std::wstring(11U, L'0') + suffix + L"}";
}

std::wstring volume(const wchar_t suffix) {
  return std::wstring(L"\\\\?\\Volume{") +
         std::wstring(L"00000000-0000-0000-0000-") +
         std::wstring(11U, L'0') + suffix + L"}\\";
}

ytec::clonecore::Error mock_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::query_failed,
      .native_code = ERROR_INVALID_DATA,
      .operation = operation,
      .message = L"モック観測失敗",
  };
}

ytec::vssrequester::VssDiffAreaMonitorBinding valid_binding(
    const std::size_t count = 1U) {
  ytec::vssrequester::VssDiffAreaMonitorBinding binding{
      .snapshot_set_id = guid(L'a'),
  };
  for (std::size_t index = 0U; index < count; ++index) {
    const wchar_t suffix = static_cast<wchar_t>(L'1' + index);
    binding.snapshots.push_back(
        ytec::vssrequester::VssDiffAreaSnapshotBinding{
            .snapshot_id = guid(suffix),
            .original_volume_guid_path = volume(suffix),
            .snapshot_device_path =
                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy" +
                std::to_wstring(index + 1U),
            .provider_id = guid(L'e'),
            .creation_timestamp =
                static_cast<std::int64_t>(1'000U + index),
            .expected_source_identity_token =
                L"stable-source-layout-" + std::to_wstring(index + 1U),
        });
  }
  return binding;
}

ytec::vssrequester::VssDiffAreaObservation valid_observation(
    const ytec::vssrequester::VssDiffAreaMonitorBinding& binding,
    const std::size_t index,
    const std::uint64_t maximum = 1'000U,
    const std::uint64_t allocated = 700U,
    const std::uint64_t used = 700U) {
  const auto& expected = binding.snapshots[index];
  return ytec::vssrequester::VssDiffAreaObservation{
      .snapshot_set_id = binding.snapshot_set_id,
      .snapshot_id = expected.snapshot_id,
      .original_volume_guid_path = expected.original_volume_guid_path,
      .snapshot_device_path = expected.snapshot_device_path,
      .provider_id = expected.provider_id,
      .creation_timestamp = expected.creation_timestamp,
      .observed_source_identity_token =
          expected.expected_source_identity_token,
      .diff_area_volume_guid_path = volume(L'f'),
      .backing_volume_guid_path = volume(L'f'),
      .backing_volume_serial_number = 0x1234U,
      .backing_volume_total_bytes = 100ULL * 1024ULL * 1024ULL * 1024ULL,
      .backing_volume_free_bytes = 50ULL * 1024ULL * 1024ULL * 1024ULL,
      .backing_volume_available_bytes =
          40ULL * 1024ULL * 1024ULL * 1024ULL,
      .maximum_kind =
          ytec::vssrequester::VssDiffAreaMaximumKind::bounded,
      .maximum_bytes = maximum,
      .allocated_bytes = allocated,
      .used_bytes = used,
  };
}

class MockObserver final
    : public ytec::vssrequester::IVssDiffAreaObserver {
 public:
  ytec::clonecore::Result<
      std::vector<ytec::vssrequester::VssDiffAreaObservation>>
  observe(
      const ytec::vssrequester::VssDiffAreaMonitorBinding&) override {
    ++calls;
    if (throw_on_observe) {
      throw TestFailure{"observer exception"};
    }
    if (fail_observation) {
      return ytec::clonecore::Result<std::vector<
          ytec::vssrequester::VssDiffAreaObservation>>::failure(
          mock_error(L"モックVSS差分領域query"));
    }
    return ytec::clonecore::Result<std::vector<
        ytec::vssrequester::VssDiffAreaObservation>>::success(observations);
  }

  std::size_t calls{};
  bool fail_observation{};
  bool throw_on_observe{};
  std::vector<ytec::vssrequester::VssDiffAreaObservation> observations;
};

struct MonitorFixture final {
  ytec::vssrequester::VssDiffAreaMonitorBinding binding{valid_binding()};
  MockObserver observer;
  std::vector<ytec::vssrequester::VssDiffAreaPollEvidence> evidence;

  ytec::clonecore::Result<
      std::unique_ptr<ytec::vssrequester::VssDiffAreaMonitor>>
  create(
      ytec::vssrequester::VssDiffAreaMonitorPolicy policy = {
          .poll_interval_ms = 1'000U,
          .danger_used_basis_points = 8'000U,
          .minimum_remaining_bytes = 0U,
      }) {
    return ytec::vssrequester::VssDiffAreaMonitor::create(
        policy,
        binding,
        &observer,
        [&](const ytec::vssrequester::VssDiffAreaPollEvidence& value) {
          evidence.push_back(value);
          return ytec::clonecore::success_status();
        });
  }
};

void test_regular_polling_and_evidence() {
  MonitorFixture fixture;
  fixture.observer.observations = {
      valid_observation(fixture.binding, 0U),
  };
  auto created = fixture.create();
  check(created.has_value(), "Valid monitor should be created");
  auto monitor = created.take_value();

  const auto first = monitor->poll(0U);
  check(first.has_value(), "First observation should pass");
  check(
      first.value().directive ==
          ytec::vssrequester::VssDiffAreaMonitorDirective::
              continue_operation,
      "Below-threshold observation should continue");
  check(first.value().observation_performed,
        "First poll must observe immediately");
  check(fixture.observer.calls == 1U && fixture.evidence.size() == 1U,
        "Performed observation must emit one evidence record");
  check(fixture.evidence[0].samples.size() == 1U,
        "Evidence must preserve one bound snapshot sample");
  check(fixture.evidence[0].samples[0].danger_threshold_bytes == 800U,
        "80 percent threshold should be recorded exactly");
  check(
      fixture.evidence[0].samples[0].effective_backing_volume_free_bytes ==
          40ULL * 1024ULL * 1024ULL * 1024ULL,
      "Evidence must preserve the conservative backing-volume free bytes");

  const auto early = monitor->poll(999U);
  check(early.has_value() && !early.value().observation_performed,
        "A poll before the immutable interval must not query again");
  check(fixture.observer.calls == 1U && fixture.evidence.size() == 1U,
        "A not-due poll must not fabricate evidence");

  const auto due = monitor->poll(1'000U);
  check(due.has_value() && due.value().observation_performed,
        "The next interval boundary must perform another observation");
  check(fixture.observer.calls == 2U && fixture.evidence.size() == 2U,
        "Every due observation must emit evidence");
}

void test_threshold_requires_exact_explicit_review() {
  MonitorFixture fixture;
  fixture.observer.observations = {
      valid_observation(fixture.binding, 0U, 1'000U, 900U, 800U),
  };
  auto created = fixture.create();
  check(created.has_value(), "Threshold monitor should be created");
  auto monitor = created.take_value();

  const auto danger = monitor->poll(0U);
  check(danger.has_value(), "Danger observation should be evidenced");
  check(
      danger.value().directive ==
          ytec::vssrequester::VssDiffAreaMonitorDirective::review_required,
      "Exact threshold must require user review");
  const auto danger_sequence = danger.value().latest_evidence_sequence;
  check(danger_sequence != 0U, "Review must bind a non-zero evidence ID");

  const auto still_blocked = monitor->poll(50'000U);
  check(still_blocked.has_value() &&
            still_blocked.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    review_required &&
            !still_blocked.value().observation_performed,
        "Time passing must never auto-resume a dangerous Snapshot");
  check(fixture.observer.calls == 1U,
        "Unresolved review must block another observer call");

  const auto wrong = monitor->resolve_review(
      danger_sequence + 1U,
      5'000U,
      ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
  check(!wrong.has_value(),
        "A stale or fabricated evidence ID must not resume");
  check(
      monitor->directive() ==
          ytec::vssrequester::VssDiffAreaMonitorDirective::review_required,
      "Rejected review must remain blocked");

  const auto resumed = monitor->resolve_review(
      danger_sequence,
      5'000U,
      ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
  check(resumed.has_value(), "Exact explicit resume should pass once");
  check(resumed.value().observation_performed &&
            fixture.observer.calls == 2U,
        "Resume must perform an immediate fresh observation");
  check(
      resumed.value().directive ==
          ytec::vssrequester::VssDiffAreaMonitorDirective::
              continue_operation,
      "Resume-once review should reopen only the current interval");
  check(fixture.evidence.size() == 2U &&
            fixture.evidence.back().review_action ==
                ytec::vssrequester::VssDiffAreaReviewAction::resume_once,
        "Explicit resume must have its own evidence record");

  const auto immediate = monitor->poll(5'999U);
  check(immediate.has_value() && !immediate.value().observation_performed,
        "Resume once should not bypass the fixed poll cadence");
  const auto danger_again = monitor->poll(6'000U);
  check(danger_again.has_value() &&
            danger_again.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    review_required,
        "Persistent danger must require a new explicit review next interval");

  const auto cancelled = monitor->resolve_review(
      danger_again.value().latest_evidence_sequence,
      6'001U,
      ytec::vssrequester::VssDiffAreaReviewAction::safe_cancel);
  check(cancelled.has_value() &&
            cancelled.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    safe_cancel_required,
        "Explicit safe cancel should make the monitor terminal");
  check(fixture.evidence.back().review_action ==
            ytec::vssrequester::VssDiffAreaReviewAction::safe_cancel,
        "Explicit cancellation must be evidenced");
}

void test_resume_rejects_worse_fresh_observation() {
  {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U, 1'000U, 900U, 800U),
    };
    auto created = fixture.create();
    check(created.has_value(), "Used-growth monitor should be created");
    auto monitor = created.take_value();
    const auto danger = monitor->poll(0U);
    check(danger.has_value(), "Initial threshold evidence should pass");

    fixture.observer.observations[0].used_bytes = 801U;
    const auto rejected = monitor->resolve_review(
        danger.value().latest_evidence_sequence,
        100U,
        ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
    check(rejected.has_value() &&
              rejected.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      review_required &&
              rejected.value().observation_performed,
          "Used-byte growth at resume must create a new review");
    check(fixture.observer.calls == 2U && fixture.evidence.size() == 2U,
          "Rejected resume must be freshly observed and evidenced once");
    check(rejected.value().latest_evidence_sequence !=
              danger.value().latest_evidence_sequence,
          "Worse resume evidence must receive a new evidence sequence");

    const auto accepted = monitor->resolve_review(
        rejected.value().latest_evidence_sequence,
        101U,
        ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
    check(accepted.has_value() &&
              accepted.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      continue_operation &&
              accepted.value().observation_performed,
          "A second explicit review may resume only after a no-worse query");
    check(fixture.observer.calls == 3U,
          "Every resume attempt must issue its own fresh observation");
    const auto early = monitor->poll(1'100U);
    check(early.has_value() && !early.value().observation_performed,
          "Accepted fresh evidence starts exactly one new poll interval");
  }

  {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U, 1'000U, 900U, 800U),
    };
    auto created = fixture.create();
    check(created.has_value(), "Remaining-drop monitor should be created");
    auto monitor = created.take_value();
    const auto danger = monitor->poll(0U);
    check(danger.has_value(), "Initial remaining evidence should pass");

    fixture.observer.observations[0].maximum_bytes = 950U;
    const auto rejected = monitor->resolve_review(
        danger.value().latest_evidence_sequence,
        100U,
        ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
    check(rejected.has_value() &&
              rejected.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      review_required,
          "A lower remaining capacity must create a new review even when used is unchanged");
    check(fixture.evidence.back().samples[0].remaining_bytes == 150U,
          "The new review must expose the freshly reduced remaining bytes");
  }
}

void test_diff_area_volume_is_latched() {
  {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U),
    };
    auto created = fixture.create();
    check(created.has_value(), "Diff-area latch monitor should be created");
    auto monitor = created.take_value();
    check(monitor->poll(0U).has_value(),
          "Initial diff-area association should be latched");
    fixture.observer.observations[0].diff_area_volume_guid_path =
        volume(L'e');
    const auto moved = monitor->poll(1'000U);
    check(moved.has_value() &&
              moved.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "A later diff-area Volume move must fail closed");
  }

  {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U, 1'000U, 900U, 800U),
    };
    auto created = fixture.create();
    check(created.has_value(), "Review latch monitor should be created");
    auto monitor = created.take_value();
    const auto danger = monitor->poll(0U);
    check(danger.has_value(), "Initial review association should be latched");
    fixture.observer.observations[0].diff_area_volume_guid_path =
        volume(L'e');
    const auto moved = monitor->resolve_review(
        danger.value().latest_evidence_sequence,
        100U,
        ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
    check(moved.has_value() &&
              moved.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "A diff-area Volume move during resume must fail closed");
  }
}

void test_resume_drift_invalid_action_and_exception_fail_closed() {
  for (const std::size_t drift_kind : {0U, 1U}) {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U, 1'000U, 900U, 800U),
    };
    auto created = fixture.create();
    check(created.has_value(), "Resume-drift monitor should be created");
    auto monitor = created.take_value();
    const auto danger = monitor->poll(0U);
    check(danger.has_value(), "Resume-drift baseline should pass");
    if (drift_kind == 0U) {
      fixture.observer.observations[0].snapshot_id = guid(L'9');
    } else {
      fixture.observer.observations[0].observed_source_identity_token =
          L"replacement-source";
    }
    const auto drifted = monitor->resolve_review(
        danger.value().latest_evidence_sequence,
        100U,
        ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
    check(drifted.has_value() &&
              drifted.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "Snapshot and source drift at resume must fail closed");
  }

  {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U, 1'000U, 900U, 800U),
    };
    auto created = fixture.create();
    check(created.has_value(), "Invalid-review monitor should be created");
    auto monitor = created.take_value();
    const auto danger = monitor->poll(0U);
    const auto invalid = monitor->resolve_review(
        danger.value().latest_evidence_sequence,
        100U,
        static_cast<ytec::vssrequester::VssDiffAreaReviewAction>(0xffU));
    check(invalid.has_value() &&
              invalid.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "An unknown review action must be terminal safe-cancel");
    check(fixture.observer.calls == 1U &&
              fixture.evidence.back().failure.has_value(),
          "Invalid action must not query or be recorded as a valid review");
  }

  {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U, 1'000U, 900U, 800U),
    };
    auto created = fixture.create();
    check(created.has_value(), "Resume-exception monitor should be created");
    auto monitor = created.take_value();
    const auto danger = monitor->poll(0U);
    fixture.observer.throw_on_observe = true;
    const auto failed = monitor->resolve_review(
        danger.value().latest_evidence_sequence,
        100U,
        ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
    check(failed.has_value() &&
              failed.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "An unexpected exception during fresh resume must fail closed");
  }

  {
    auto binding = valid_binding();
    MockObserver observer;
    observer.observations = {
        valid_observation(binding, 0U, 1'000U, 900U, 800U),
    };
    std::size_t sink_calls = 0U;
    auto created = ytec::vssrequester::VssDiffAreaMonitor::create(
        {
            .poll_interval_ms = 1'000U,
            .danger_used_basis_points = 8'000U,
            .minimum_remaining_bytes = 0U,
        },
        binding,
        &observer,
        [&sink_calls](const ytec::vssrequester::VssDiffAreaPollEvidence&)
            -> ytec::clonecore::Status {
          ++sink_calls;
          if (sink_calls == 2U) {
            throw TestFailure{"resume sink exception"};
          }
          return ytec::clonecore::success_status();
        });
    check(created.has_value(), "Resume sink monitor should be created");
    auto monitor = created.take_value();
    const auto danger = monitor->poll(0U);
    check(danger.has_value(), "Resume sink baseline should be evidenced");
    const auto failed = monitor->resolve_review(
        danger.value().latest_evidence_sequence,
        100U,
        ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
    check(!failed.has_value() &&
              monitor->directive() ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "A sink exception during resume evidence must be terminal");
  }
}

void test_remaining_reserve_threshold() {
  MonitorFixture fixture;
  constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
  fixture.observer.observations = {
      valid_observation(
          fixture.binding, 0U, 10U * gib, 10U * gib, 9U * gib),
  };
  auto created = fixture.create({
      .poll_interval_ms = 1'000U,
      .danger_used_basis_points = 9'500U,
      .minimum_remaining_bytes = gib,
  });
  check(created.has_value(), "Reserve policy should be valid");
  const auto result = created.value()->poll(0U);
  check(result.has_value() &&
            result.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    review_required,
        "Minimum remaining reserve must independently require review");
  check(fixture.evidence[0].samples[0].remaining_reserve_reached,
        "Reserve trigger must be explicit in evidence");
}

void test_backing_volume_free_space_threshold_and_failure_injection() {
  constexpr std::uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
  {
    MonitorFixture fixture;
    auto observation = valid_observation(fixture.binding, 0U);
    observation.backing_volume_total_bytes = 10U * gib;
    observation.backing_volume_free_bytes = 3U * gib;
    observation.backing_volume_available_bytes = gib;
    fixture.observer.observations = {observation};
    auto created = fixture.create({
        .poll_interval_ms = 1'000U,
        .danger_used_basis_points = 9'500U,
        .minimum_remaining_bytes = gib,
    });
    check(created.has_value(), "Backing-reserve monitor should be created");
    const auto result = created.value()->poll(0U);
    check(result.has_value() &&
              result.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      review_required,
          "Actual backing-volume reserve must independently require review");
    check(fixture.evidence[0].samples[0]
                  .effective_backing_volume_free_bytes == gib &&
              fixture.evidence[0].samples[0]
                  .backing_volume_reserve_reached,
          "Backing-volume danger must be explicit in monitor evidence");
  }

  for (const std::size_t failure_kind : {0U, 1U, 2U, 3U}) {
    MonitorFixture fixture;
    auto observation = valid_observation(fixture.binding, 0U);
    switch (failure_kind) {
      case 0U:
        observation.backing_volume_guid_path = volume(L'e');
        break;
      case 1U:
        observation.backing_volume_total_bytes = 0U;
        break;
      case 2U:
        observation.backing_volume_free_bytes =
            observation.backing_volume_total_bytes + 1U;
        break;
      case 3U:
        observation.backing_volume_available_bytes =
            observation.backing_volume_total_bytes + 1U;
        break;
      default:
        throw TestFailure{"invalid backing failure kind"};
    }
    fixture.observer.observations = {observation};
    auto created = fixture.create();
    check(created.has_value(), "Backing failure monitor should be created");
    const auto result = created.value()->poll(0U);
    check(result.has_value() &&
              result.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required &&
              fixture.evidence.size() == 1U &&
              fixture.evidence[0].failure.has_value(),
          "Ambiguous or non-finite backing-volume observation must fail closed");
  }

  {
    MonitorFixture fixture;
    auto observation = valid_observation(fixture.binding, 0U);
    observation.backing_volume_available_bytes = 0U;
    fixture.observer.observations = {observation};
    auto created = fixture.create({
        .poll_interval_ms = 1'000U,
        .danger_used_basis_points = 9'500U,
        .minimum_remaining_bytes = 0U,
    });
    check(created.has_value(), "Zero-free monitor should be created");
    const auto result = created.value()->poll(0U);
    check(result.has_value() &&
              result.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      review_required,
          "A zero-byte effective backing reserve must remain dangerous even when the configurable reserve is zero");
  }
}

void test_backing_volume_identity_and_resume_free_space_are_latched() {
  for (const std::size_t drift_kind : {0U, 1U, 2U}) {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U),
    };
    auto created = fixture.create();
    check(created.has_value() && created.value()->poll(0U).has_value(),
          "Backing identity baseline should be latched");
    if (drift_kind == 0U) {
      fixture.observer.observations[0].backing_volume_guid_path =
          volume(L'e');
      fixture.observer.observations[0].diff_area_volume_guid_path =
          volume(L'e');
    } else if (drift_kind == 1U) {
      ++fixture.observer.observations[0].backing_volume_serial_number;
    } else {
      ++fixture.observer.observations[0].backing_volume_total_bytes;
    }
    const auto drifted = created.value()->poll(1'000U);
    check(drifted.has_value() &&
              drifted.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "Backing Volume GUID, serial, and total-capacity drift must fail closed");
  }

  MonitorFixture fixture;
  auto danger = valid_observation(
      fixture.binding, 0U, 1'000U, 900U, 800U);
  danger.backing_volume_free_bytes = 500U;
  danger.backing_volume_available_bytes = 500U;
  fixture.observer.observations = {danger};
  auto created = fixture.create();
  check(created.has_value(), "Backing free-space resume monitor should be created");
  const auto first = created.value()->poll(0U);
  check(first.has_value() &&
            first.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    review_required,
        "Initial logical danger should require review");
  fixture.observer.observations[0].backing_volume_available_bytes = 499U;
  const auto resumed = created.value()->resolve_review(
      first.value().latest_evidence_sequence,
      100U,
      ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
  check(resumed.has_value() &&
            resumed.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    review_required,
        "A lower fresh backing free-space observation must reject resume-once");

  MonitorFixture crossed_counters_fixture;
  auto crossed = valid_observation(
      crossed_counters_fixture.binding, 0U, 1'000U, 900U, 800U);
  crossed.backing_volume_free_bytes = 600U;
  crossed.backing_volume_available_bytes = 500U;
  crossed_counters_fixture.observer.observations = {crossed};
  auto crossed_monitor = crossed_counters_fixture.create();
  check(crossed_monitor.has_value(),
        "Independent backing counters monitor should be created");
  const auto crossed_first = crossed_monitor.value()->poll(0U);
  check(crossed_first.has_value() &&
            crossed_first.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    review_required,
        "Independent backing counter baseline should require review");
  crossed_counters_fixture.observer.observations[0]
      .backing_volume_free_bytes = 500U;
  crossed_counters_fixture.observer.observations[0]
      .backing_volume_available_bytes = 600U;
  const auto crossed_resume = crossed_monitor.value()->resolve_review(
      crossed_first.value().latest_evidence_sequence,
      100U,
      ytec::vssrequester::VssDiffAreaReviewAction::resume_once);
  check(crossed_resume.has_value() &&
            crossed_resume.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    review_required,
        "An unchanged conservative minimum must not hide regression in either raw backing free-space counter");
}

void test_observer_failures_and_unknown_limits_fail_closed() {
  {
    MonitorFixture fixture;
    fixture.observer.fail_observation = true;
    auto created = fixture.create();
    check(created.has_value(), "Failure monitor should be created");
    const auto result = created.value()->poll(0U);
    check(result.has_value() &&
              result.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "COM/query observer errors must require safe cancellation");
    check(fixture.evidence.size() == 1U &&
              fixture.evidence[0].failure.has_value(),
          "Observer failure must be evidenced");
  }
  {
    MonitorFixture fixture;
    fixture.observer.throw_on_observe = true;
    auto created = fixture.create();
    check(created.has_value(), "Exception monitor should be created");
    const auto result = created.value()->poll(0U);
    check(result.has_value() &&
              result.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "Observer exceptions must be converted to safe cancellation");
  }

  for (const auto kind : {
           ytec::vssrequester::VssDiffAreaMaximumKind::unbounded,
           ytec::vssrequester::VssDiffAreaMaximumKind::unknown,
       }) {
    MonitorFixture fixture;
    auto observation = valid_observation(fixture.binding, 0U);
    observation.maximum_kind = kind;
    fixture.observer.observations = {observation};
    auto created = fixture.create();
    check(created.has_value(), "Limit-kind monitor should be created");
    const auto result = created.value()->poll(0U);
    check(result.has_value() &&
              result.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "Unknown and unbounded diff areas must fail closed");
  }
}

void test_exact_snapshot_and_source_binding() {
  const auto run_drift = [](const std::size_t drift_kind) {
    MonitorFixture fixture;
    auto observation = valid_observation(fixture.binding, 0U);
    switch (drift_kind) {
      case 0U:
        observation.snapshot_set_id = guid(L'b');
        break;
      case 1U:
        observation.snapshot_id = guid(L'9');
        break;
      case 2U:
        observation.original_volume_guid_path = volume(L'9');
        break;
      case 3U:
        observation.snapshot_device_path =
            L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy99";
        break;
      case 4U:
        observation.observed_source_identity_token = L"replacement-source";
        break;
      default:
        throw TestFailure{"invalid drift kind"};
    }
    fixture.observer.observations = {observation};
    auto created = fixture.create();
    check(created.has_value(), "Identity-drift monitor should be created");
    const auto result = created.value()->poll(0U);
    check(result.has_value() &&
              result.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "Every Set/Snapshot/Volume/device/source drift must fail closed");
  };
  for (std::size_t index = 0U; index < 5U; ++index) {
    run_drift(index);
  }

  MonitorFixture fixture;
  fixture.binding = valid_binding(2U);
  fixture.observer.observations = {
      valid_observation(fixture.binding, 1U),
      valid_observation(fixture.binding, 0U),
  };
  auto created = fixture.create();
  check(created.has_value(), "Two-snapshot monitor should be created");
  const auto result = created.value()->poll(0U);
  check(result.has_value() &&
            result.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    continue_operation,
        "Observation order may vary when every exact binding is unique");
  check(fixture.evidence[0].samples[0].observation.snapshot_id ==
            fixture.binding.snapshots[0].snapshot_id,
        "Evidence should be canonicalized to immutable binding order");
}

void test_64_bit_relationships_and_overflow_safe_threshold() {
  for (const std::size_t invalid_kind : {0U, 1U, 2U}) {
    MonitorFixture fixture;
    auto observation = valid_observation(fixture.binding, 0U);
    if (invalid_kind == 0U) {
      observation.maximum_bytes = 0U;
      observation.allocated_bytes = 0U;
      observation.used_bytes = 0U;
    } else if (invalid_kind == 1U) {
      observation.maximum_bytes = 100U;
      observation.allocated_bytes = 101U;
      observation.used_bytes = 100U;
    } else {
      observation.maximum_bytes = 100U;
      observation.allocated_bytes = 90U;
      observation.used_bytes = 91U;
    }
    fixture.observer.observations = {observation};
    auto created = fixture.create();
    check(created.has_value(), "Counter monitor should be created");
    const auto result = created.value()->poll(0U);
    check(result.has_value() &&
              result.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "Invalid 64-bit used/allocated/maximum relation must fail closed");
  }

  MonitorFixture fixture;
  constexpr std::uint64_t maximum =
      static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  fixture.observer.observations = {
      valid_observation(fixture.binding, 0U, maximum, maximum, 0U),
  };
  auto created = fixture.create({
      .poll_interval_ms = 1'000U,
      .danger_used_basis_points = 9'999U,
      .minimum_remaining_bytes = 0U,
  });
  check(created.has_value(), "Large-value monitor should be created");
  const auto first = created.value()->poll(0U);
  check(first.has_value(), "INT64_MAX threshold calculation should pass");
  const auto threshold =
      fixture.evidence[0].samples[0].danger_threshold_bytes;
  check(threshold > 0U && threshold <= maximum,
        "Overflow-safe threshold must remain inside the finite maximum");

  fixture.observer.observations[0].used_bytes = threshold - 1U;
  const auto below = created.value()->poll(1'000U);
  check(below.has_value() &&
            below.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    continue_operation,
        "One byte below the large threshold should continue");
  fixture.observer.observations[0].used_bytes = threshold;
  const auto exact = created.value()->poll(2'000U);
  check(exact.has_value() &&
            exact.value().directive ==
                ytec::vssrequester::VssDiffAreaMonitorDirective::
                    review_required,
        "Exact large threshold should require review without overflow");
}

void test_time_regression_and_evidence_sink_failure() {
  {
    MonitorFixture fixture;
    fixture.observer.observations = {
        valid_observation(fixture.binding, 0U),
    };
    auto created = fixture.create();
    check(created.has_value(), "Time monitor should be created");
    check(created.value()->poll(1'000U).has_value(),
          "Initial monotonic time should pass");
    const auto regressed = created.value()->poll(999U);
    check(regressed.has_value() &&
              regressed.value().directive ==
                  ytec::vssrequester::VssDiffAreaMonitorDirective::
                      safe_cancel_required,
          "Elapsed-time regression must fail closed");
  }
  {
    auto binding = valid_binding();
    MockObserver observer;
    observer.observations = {valid_observation(binding, 0U)};
    auto created = ytec::vssrequester::VssDiffAreaMonitor::create(
        {
            .poll_interval_ms = 1'000U,
            .danger_used_basis_points = 8'000U,
            .minimum_remaining_bytes = 0U,
        },
        binding,
        &observer,
        [](const ytec::vssrequester::VssDiffAreaPollEvidence&) {
          return ytec::clonecore::Status::failure(
              mock_error(L"VSS差分領域証拠保存"));
        });
    check(created.has_value(), "Sink-failure monitor should be created");
    const auto result = created.value()->poll(0U);
    check(!result.has_value(),
          "Evidence sink failure must be returned to the caller");
    check(
        created.value()->directive() ==
            ytec::vssrequester::VssDiffAreaMonitorDirective::
                safe_cancel_required,
        "Evidence sink failure must make the monitor terminal");
  }
  {
    auto binding = valid_binding();
    MockObserver observer;
    observer.observations = {valid_observation(binding, 0U)};
    auto created = ytec::vssrequester::VssDiffAreaMonitor::create(
        {
            .poll_interval_ms = 1'000U,
            .danger_used_basis_points = 8'000U,
            .minimum_remaining_bytes = 0U,
        },
        binding,
        &observer,
        [](const ytec::vssrequester::VssDiffAreaPollEvidence&)
            -> ytec::clonecore::Status {
          throw TestFailure{"sink exception"};
        });
    check(created.has_value(), "Sink-exception monitor should be created");
    const auto result = created.value()->poll(0U);
    check(!result.has_value(),
          "Evidence sink exceptions must be contained at the boundary");
    check(
        created.value()->directive() ==
            ytec::vssrequester::VssDiffAreaMonitorDirective::
                safe_cancel_required,
        "Evidence sink exceptions must make the monitor terminal");
  }
}

void test_creation_rejects_mutable_or_ambiguous_policy_boundary() {
  auto binding = valid_binding();
  MockObserver observer;
  const auto sink = [](const ytec::vssrequester::VssDiffAreaPollEvidence&) {
    return ytec::clonecore::success_status();
  };

  check(!ytec::vssrequester::VssDiffAreaMonitor::create(
             {
                 .poll_interval_ms = 99U,
                 .danger_used_basis_points = 8'000U,
                 .minimum_remaining_bytes = 0U,
             },
             binding,
             &observer,
             sink)
             .has_value(),
        "Too-fast polling policy must be rejected");
  check(!ytec::vssrequester::VssDiffAreaMonitor::create(
             {
                 .poll_interval_ms = 1'000U,
                 .danger_used_basis_points = 10'000U,
                 .minimum_remaining_bytes = 0U,
             },
             binding,
             &observer,
             sink)
             .has_value(),
        "A 100 percent danger threshold must be rejected");

  auto duplicate = valid_binding(2U);
  duplicate.snapshots[1].snapshot_id = duplicate.snapshots[0].snapshot_id;
  check(!ytec::vssrequester::VssDiffAreaMonitor::create(
             {}, duplicate, &observer, sink)
             .has_value(),
        "Duplicate snapshot bindings must be rejected before observation");
  check(!ytec::vssrequester::VssDiffAreaMonitor::create(
             {}, binding, nullptr, sink)
             .has_value(),
        "Missing observer must be rejected");
  check(!ytec::vssrequester::VssDiffAreaMonitor::create(
             {}, binding, &observer, {})
             .has_value(),
        "Missing evidence sink must be rejected");
}

void test_windows_binding_conversion_is_exact_and_fail_closed() {
  const auto expected = valid_binding(2U);
  ytec::vssrequester::SnapshotCopyContext context{
      .snapshot_set_id = expected.snapshot_set_id,
  };
  for (const auto& snapshot : expected.snapshots) {
    context.mappings.push_back(ytec::vssrequester::SnapshotMapping{
        .original_volume_guid_path = snapshot.original_volume_guid_path,
        .snapshot_id = snapshot.snapshot_id,
        .snapshot_device_path = snapshot.snapshot_device_path,
        .provider_id = snapshot.provider_id,
        .creation_timestamp = snapshot.creation_timestamp,
    });
  }
  auto converted = ytec::vssrequester::
      make_windows_vss_diff_area_monitor_binding(
          context, L"reviewed-source-layout");
  check(converted.has_value(),
        "Valid Snapshot callback context should convert");
  check(converted.value().snapshots.size() == context.mappings.size(),
        "Conversion must retain every mapping");
  for (std::size_t index = 0U; index < context.mappings.size(); ++index) {
    const auto& actual = converted.value().snapshots[index];
    const auto& supplied = context.mappings[index];
    check(actual.snapshot_id == supplied.snapshot_id &&
              actual.original_volume_guid_path ==
                  supplied.original_volume_guid_path &&
              actual.snapshot_device_path == supplied.snapshot_device_path &&
              actual.provider_id == supplied.provider_id &&
              actual.creation_timestamp == supplied.creation_timestamp &&
              actual.expected_source_identity_token ==
                  L"reviewed-source-layout",
          "Conversion must copy the exact immutable Snapshot generation");
  }

  auto invalid = context;
  invalid.mappings[0].provider_id =
      L"{00000000-0000-0000-0000-000000000000}";
  check(!ytec::vssrequester::make_windows_vss_diff_area_monitor_binding(
             invalid, L"reviewed-source-layout")
             .has_value(),
        "A null provider GUID must fail closed");
  invalid = context;
  invalid.mappings[0].creation_timestamp = 0;
  check(!ytec::vssrequester::make_windows_vss_diff_area_monitor_binding(
             invalid, L"reviewed-source-layout")
             .has_value(),
        "A missing creation timestamp must fail closed");
  invalid = context;
  invalid.mappings[1].snapshot_id = invalid.mappings[0].snapshot_id;
  check(!ytec::vssrequester::make_windows_vss_diff_area_monitor_binding(
             invalid, L"reviewed-source-layout")
             .has_value(),
        "Duplicate Snapshot generations must fail closed");
}

void test_operation_monitor_lifecycle_and_cancellation_precedence() {
  {
    auto binding = valid_binding();
    auto observer = std::make_unique<MockObserver>();
    auto* const observed = observer.get();
    observed->observations = {valid_observation(binding, 0U)};
    std::uint64_t elapsed{};
    std::size_t review_calls{};
    std::size_t existing_boundary_calls{};
    auto created = ytec::vssrequester::VssDiffAreaOperationMonitor::create(
        {
            .poll_interval_ms = 1'000U,
            .danger_used_basis_points = 8'000U,
            .minimum_remaining_bytes = 0U,
        },
        binding,
        std::move(observer),
        [&review_calls](const auto& evidence) {
          ++review_calls;
          return ytec::clonecore::Result<
              ytec::vssrequester::VssDiffAreaReviewDecision>::success({
              .displayed_evidence_sequence = evidence.sequence,
              .action = ytec::vssrequester::
                  VssDiffAreaReviewAction::safe_cancel,
          });
        },
        [&elapsed]() { return elapsed; });
    check(created.has_value(), "Operation monitor should be created");
    auto monitor = created.take_value();
    check(monitor->initial_poll().has_value() && observed->calls == 1U,
          "Initial poll must observe before output");
    auto callbacks = monitor->callbacks({
        .safe_boundary = [&existing_boundary_calls](const auto&) {
          ++existing_boundary_calls;
          return ytec::clonecore::DiskOperationControlDecision::
              continue_operation;
        },
    });
    elapsed = 1'000U;
    check(ytec::clonecore::disk_operation_control_at_safe_boundary(
              callbacks,
              {
                  .kind = ytec::clonecore::
                      DiskOperationSafeBoundaryKind::verified_chunk,
                  .stage = ytec::clonecore::DiskOperationStage::copying_data,
                  .completed_bytes = 4096U,
                  .completed_units = 1U,
              }) == ytec::clonecore::DiskOperationControlDecision::
                  continue_operation,
          "A safe boundary should continue after a healthy due poll");
    check(observed->calls == 2U && existing_boundary_calls == 1U &&
              review_calls == 0U,
          "The monitor must run before delegating a healthy safe boundary");
    elapsed = 1'001U;
    check(monitor->completion_poll().has_value() && observed->calls == 3U,
          "Completion must force a fresh observation");
    check(ytec::vssrequester::
              validate_completed_vss_diff_area_operation_evidence(
                  monitor->evidence())
              .has_value(),
          "Initial, boundary, and completion evidence should validate");
  }

  {
    auto binding = valid_binding();
    auto observer = std::make_unique<MockObserver>();
    auto* const observed = observer.get();
    observed->observations = {valid_observation(binding, 0U)};
    std::uint64_t elapsed{};
    std::size_t review_calls{};
    auto created = ytec::vssrequester::VssDiffAreaOperationMonitor::create(
        {
            .poll_interval_ms = 1'000U,
            .danger_used_basis_points = 8'000U,
            .minimum_remaining_bytes = 0U,
        },
        binding,
        std::move(observer),
        [&review_calls](const auto& evidence) {
          ++review_calls;
          return ytec::clonecore::Result<
              ytec::vssrequester::VssDiffAreaReviewDecision>::success({
              .displayed_evidence_sequence = evidence.sequence,
              .action = ytec::vssrequester::
                  VssDiffAreaReviewAction::safe_cancel,
          });
        },
        [&elapsed]() { return elapsed; });
    check(created.has_value() && created.value()->initial_poll().has_value(),
          "Cancellation-order monitor should initialize");
    observed->observations[0].used_bytes = 900U;
    auto callbacks = created.value()->callbacks({
        .cancellation_requested = []() { return true; },
    });
    elapsed = 1'000U;
    check(ytec::clonecore::disk_operation_control_at_safe_boundary(
              callbacks,
              {
                  .kind = ytec::clonecore::
                      DiskOperationSafeBoundaryKind::verified_chunk,
                  .stage = ytec::clonecore::DiskOperationStage::copying_data,
              }) == ytec::clonecore::DiskOperationControlDecision::
                  cancel_operation,
          "An existing cancellation must stop before a danger review");
    check(observed->calls == 1U && review_calls == 0U,
          "Cancellation precedence must avoid an extra VSS query or dialog");
  }
}

}  // namespace

int main() {
  try {
    test_regular_polling_and_evidence();
    test_threshold_requires_exact_explicit_review();
    test_resume_rejects_worse_fresh_observation();
    test_diff_area_volume_is_latched();
    test_resume_drift_invalid_action_and_exception_fail_closed();
    test_remaining_reserve_threshold();
    test_backing_volume_free_space_threshold_and_failure_injection();
    test_backing_volume_identity_and_resume_free_space_are_latched();
    test_observer_failures_and_unknown_limits_fail_closed();
    test_exact_snapshot_and_source_binding();
    test_64_bit_relationships_and_overflow_safe_threshold();
    test_time_regression_and_evidence_sink_failure();
    test_creation_rejects_mutable_or_ambiguous_policy_boundary();
    test_windows_binding_conversion_is_exact_and_fail_closed();
    test_operation_monitor_lifecycle_and_cancellation_precedence();
  } catch (const TestFailure& failure) {
    std::cerr << "FAIL: " << failure.message << '\n';
    return 1;
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: unexpected exception: " << exception.what() << '\n';
    return 1;
  }
  std::cout << "VSS diff-area monitor tests passed\n";
  return 0;
}

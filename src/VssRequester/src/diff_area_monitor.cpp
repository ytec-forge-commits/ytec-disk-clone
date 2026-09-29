#include "ytec/vssrequester/diff_area_monitor.h"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <string_view>
#include <utility>

namespace ytec::vssrequester {
namespace {

constexpr std::uint32_t kBasisPointDenominator = 10'000U;
constexpr std::uint64_t kMinimumPollIntervalMs = 100U;
constexpr std::uint64_t kMaximumPollIntervalMs = 60'000U;
constexpr std::size_t kMaximumSnapshots = 128U;
constexpr std::size_t kMaximumIdentityTokenCharacters = 512U;
constexpr std::size_t kMaximumSourceEpochBytes = 256U;

clonecore::Error monitor_error(
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

bool equals_case_insensitive(
    const std::wstring_view left,
    const std::wstring_view right) {
  return left.size() == right.size() &&
         std::equal(
             left.begin(),
             left.end(),
             right.begin(),
             [](const wchar_t lhs, const wchar_t rhs) {
               return std::towlower(lhs) == std::towlower(rhs);
             });
}

bool is_hex(const wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') ||
         (value >= L'a' && value <= L'f') ||
         (value >= L'A' && value <= L'F');
}

bool is_guid_string(const std::wstring_view value) noexcept {
  if (value.size() != 38U || value.front() != L'{' ||
      value.back() != L'}') {
    return false;
  }
  for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
    const bool hyphen =
        index == 9U || index == 14U || index == 19U || index == 24U;
    if ((hyphen && value[index] != L'-') ||
        (!hyphen && !is_hex(value[index]))) {
      return false;
    }
  }
  return true;
}

bool is_volume_guid_path(const std::wstring_view path) noexcept {
  constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
  if (path.size() != 49U || !path.starts_with(prefix) ||
      path[47] != L'}' || path[48] != L'\\') {
    return false;
  }
  for (std::size_t index = prefix.size(); index < 47U; ++index) {
    const std::size_t guid_index = index - prefix.size();
    const bool hyphen = guid_index == 8U || guid_index == 13U ||
                        guid_index == 18U || guid_index == 23U;
    if ((hyphen && path[index] != L'-') ||
        (!hyphen && !is_hex(path[index]))) {
      return false;
    }
  }
  return true;
}

bool is_snapshot_device_path(const std::wstring_view path) noexcept {
  constexpr std::wstring_view prefix =
      L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy";
  if (!path.starts_with(prefix) || path.size() <= prefix.size()) {
    return false;
  }
  std::wstring_view suffix = path.substr(prefix.size());
  if (suffix.ends_with(L'\\')) {
    suffix.remove_suffix(1U);
  }
  return !suffix.empty() &&
         std::all_of(
             suffix.begin(), suffix.end(), [](const wchar_t value) {
               return value >= L'0' && value <= L'9';
             });
}

clonecore::Status validate_policy(
    const VssDiffAreaMonitorPolicy& policy) {
  if (policy.poll_interval_ms < kMinimumPollIntervalMs ||
      policy.poll_interval_ms > kMaximumPollIntervalMs) {
    return clonecore::Status::failure(monitor_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS差分領域監視間隔検証",
        L"監視間隔は100ミリ秒以上60秒以下で指定してください"));
  }
  if (policy.danger_used_basis_points == 0U ||
      policy.danger_used_basis_points >= kBasisPointDenominator) {
    return clonecore::Status::failure(monitor_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS差分領域閾値検証",
        L"使用率閾値は1から9999 basis pointsで指定してください"));
  }
  return clonecore::success_status();
}

clonecore::Status validate_binding(
    const VssDiffAreaMonitorBinding& binding) {
  if (!is_guid_string(binding.snapshot_set_id)) {
    return clonecore::Status::failure(monitor_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS差分領域Snapshot Set検証",
        L"固定済みSnapshot Set GUIDが正規形式ではありません"));
  }
  if (binding.snapshots.empty() ||
      binding.snapshots.size() > kMaximumSnapshots) {
    return clonecore::Status::failure(monitor_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS差分領域Snapshot件数検証",
        L"監視対象Snapshotは1件以上128件以下である必要があります"));
  }

  for (std::size_t index = 0U; index < binding.snapshots.size(); ++index) {
    const auto& snapshot = binding.snapshots[index];
    if (!is_guid_string(snapshot.snapshot_id) ||
        !is_volume_guid_path(snapshot.original_volume_guid_path) ||
        !is_snapshot_device_path(snapshot.snapshot_device_path) ||
        !is_guid_string(snapshot.provider_id) ||
        snapshot.creation_timestamp == 0 ||
        snapshot.expected_source_identity_token.empty() ||
        snapshot.expected_source_identity_token.size() >
            kMaximumIdentityTokenCharacters) {
      return clonecore::Status::failure(monitor_error(
          clonecore::ErrorCode::invalid_argument,
          ERROR_INVALID_PARAMETER,
          L"VSS差分領域Snapshot binding検証",
          L"Snapshot、Source Volume、デバイス、provider、creation timestamp、またはsource identity tokenが不正です"));
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      const auto& earlier = binding.snapshots[previous];
      if (equals_case_insensitive(
              snapshot.snapshot_id, earlier.snapshot_id) ||
          equals_case_insensitive(
              snapshot.original_volume_guid_path,
              earlier.original_volume_guid_path) ||
          equals_case_insensitive(
              snapshot.snapshot_device_path,
              earlier.snapshot_device_path)) {
        return clonecore::Status::failure(monitor_error(
            clonecore::ErrorCode::invalid_argument,
            ERROR_DUP_NAME,
            L"VSS差分領域Snapshot binding重複検証",
            L"Snapshot ID、Source Volume、Snapshotデバイスは一意である必要があります"));
      }
    }
  }
  return clonecore::success_status();
}

std::uint64_t threshold_bytes(
    const std::uint64_t maximum_bytes,
    const std::uint32_t basis_points) noexcept {
  // ceil(maximum * basis_points / 10000), without overflowing uint64_t.
  const std::uint64_t quotient =
      maximum_bytes / kBasisPointDenominator;
  const std::uint64_t remainder =
      maximum_bytes % kBasisPointDenominator;
  const std::uint64_t base = quotient * basis_points;
  const std::uint64_t scaled_remainder = remainder * basis_points;
  const std::uint64_t rounded_remainder =
      (scaled_remainder + (kBasisPointDenominator - 1U)) /
      kBasisPointDenominator;
  return base + rounded_remainder;
}

clonecore::Result<std::vector<VssDiffAreaSampleEvidence>>
validate_and_derive_samples(
    const VssDiffAreaMonitorPolicy& policy,
    const VssDiffAreaMonitorBinding& binding,
    const std::vector<VssDiffAreaObservation>& observations) {
  if (observations.size() != binding.snapshots.size()) {
    return clonecore::Result<
        std::vector<VssDiffAreaSampleEvidence>>::failure(monitor_error(
        clonecore::ErrorCode::identity_mismatch,
        ERROR_NOT_FOUND,
        L"VSS差分領域観測件数検証",
        L"固定済みSnapshotと差分領域観測の件数が一致しません"));
  }

  std::vector<VssDiffAreaSampleEvidence> samples;
  samples.reserve(binding.snapshots.size());
  for (const auto& expected : binding.snapshots) {
    const auto matches = std::count_if(
        observations.begin(), observations.end(), [&](const auto& value) {
          return equals_case_insensitive(
              value.snapshot_id, expected.snapshot_id);
        });
    if (matches != 1) {
      return clonecore::Result<
          std::vector<VssDiffAreaSampleEvidence>>::failure(monitor_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_NOT_FOUND,
          L"VSS差分領域Snapshot対応検証",
          L"固定済みSnapshot IDに一意な観測結果がありません"));
    }
    const auto found = std::find_if(
        observations.begin(), observations.end(), [&](const auto& value) {
          return equals_case_insensitive(
              value.snapshot_id, expected.snapshot_id);
        });
    const auto& observed = *found;
    if (!equals_case_insensitive(
            observed.snapshot_set_id, binding.snapshot_set_id) ||
        !equals_case_insensitive(
            observed.original_volume_guid_path,
            expected.original_volume_guid_path) ||
        !equals_case_insensitive(
            observed.snapshot_device_path,
            expected.snapshot_device_path) ||
        !equals_case_insensitive(
            observed.provider_id, expected.provider_id) ||
        observed.creation_timestamp != expected.creation_timestamp ||
        observed.observed_source_identity_token !=
            expected.expected_source_identity_token) {
      return clonecore::Result<
          std::vector<VssDiffAreaSampleEvidence>>::failure(monitor_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"VSS差分領域Identity再検証",
          L"Snapshot Set、Source Volume、Snapshotデバイス、provider、creation timestamp、またはsource identityが固定値から変化しました"));
    }
    if (!is_volume_guid_path(observed.diff_area_volume_guid_path)) {
      return clonecore::Result<
          std::vector<VssDiffAreaSampleEvidence>>::failure(monitor_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_INVALID_DATA,
          L"VSS差分領域Volume検証",
          L"差分領域Volume GUIDを一意に確認できません"));
    }
    if (!is_volume_guid_path(observed.backing_volume_guid_path) ||
        !equals_case_insensitive(
            observed.backing_volume_guid_path,
            observed.diff_area_volume_guid_path)) {
      return clonecore::Result<
          std::vector<VssDiffAreaSampleEvidence>>::failure(monitor_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"VSS差分領域backing Volume検証",
          L"VSS associationとread-onlyで再識別したbacking Volumeが一意に一致しません"));
    }
    if (observed.backing_volume_total_bytes == 0U ||
        observed.backing_volume_free_bytes >
            observed.backing_volume_total_bytes ||
        observed.backing_volume_available_bytes >
            observed.backing_volume_total_bytes) {
      return clonecore::Result<
          std::vector<VssDiffAreaSampleEvidence>>::failure(monitor_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"VSS差分領域backing Volume容量検証",
          L"backing Volumeの有限なtotal/free/available容量関係が不正です"));
    }
    if (observed.maximum_kind != VssDiffAreaMaximumKind::bounded ||
        observed.maximum_bytes == 0U) {
      return clonecore::Result<
          std::vector<VssDiffAreaSampleEvidence>>::failure(monitor_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_ARITHMETIC_OVERFLOW,
          L"VSS差分領域上限検証",
          L"差分領域の有限な最大容量を確認できないため継続できません"));
    }
    if (observed.used_bytes > observed.allocated_bytes ||
        observed.allocated_bytes > observed.maximum_bytes) {
      return clonecore::Result<
          std::vector<VssDiffAreaSampleEvidence>>::failure(monitor_error(
          clonecore::ErrorCode::invalid_data,
          ERROR_ARITHMETIC_OVERFLOW,
          L"VSS差分領域64bit容量検証",
          L"使用量、割当量、最大容量の64bit関係が不正です"));
    }

    const std::uint64_t threshold = threshold_bytes(
        observed.maximum_bytes, policy.danger_used_basis_points);
    const std::uint64_t remaining =
        observed.maximum_bytes - observed.used_bytes;
    const std::uint64_t effective_backing_free = (std::min)(
        observed.backing_volume_free_bytes,
        observed.backing_volume_available_bytes);
    samples.push_back(VssDiffAreaSampleEvidence{
        .observation = observed,
        .danger_threshold_bytes = threshold,
        .remaining_bytes = remaining,
        .effective_backing_volume_free_bytes = effective_backing_free,
        .used_threshold_reached = observed.used_bytes >= threshold,
        .remaining_reserve_reached =
            policy.minimum_remaining_bytes != 0U &&
            remaining <= policy.minimum_remaining_bytes,
        .backing_volume_reserve_reached =
            effective_backing_free == 0U ||
            (policy.minimum_remaining_bytes != 0U &&
             effective_backing_free <= policy.minimum_remaining_bytes),
    });
  }
  return clonecore::Result<
      std::vector<VssDiffAreaSampleEvidence>>::success(std::move(samples));
}

bool any_threshold_reached(
    const std::vector<VssDiffAreaSampleEvidence>& samples) noexcept {
  return std::any_of(
      samples.begin(), samples.end(), [](const auto& sample) {
        return sample.used_threshold_reached ||
               sample.remaining_reserve_reached ||
               sample.backing_volume_reserve_reached;
      });
}

bool valid_review_action(const VssDiffAreaReviewAction action) noexcept {
  return action == VssDiffAreaReviewAction::resume_once ||
         action == VssDiffAreaReviewAction::safe_cancel;
}

bool observations_are_no_worse(
    const std::vector<VssDiffAreaSampleEvidence>& displayed,
    const std::vector<VssDiffAreaSampleEvidence>& fresh) {
  if (displayed.size() != fresh.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < displayed.size(); ++index) {
    if (!equals_case_insensitive(
            displayed[index].observation.snapshot_id,
            fresh[index].observation.snapshot_id) ||
        fresh[index].observation.used_bytes >
            displayed[index].observation.used_bytes ||
        fresh[index].remaining_bytes < displayed[index].remaining_bytes ||
        fresh[index].observation.backing_volume_free_bytes <
            displayed[index].observation.backing_volume_free_bytes ||
        fresh[index].observation.backing_volume_available_bytes <
            displayed[index].observation.backing_volume_available_bytes ||
        fresh[index].effective_backing_volume_free_bytes <
            displayed[index].effective_backing_volume_free_bytes) {
      return false;
    }
  }
  return true;
}

}  // namespace

class VssDiffAreaMonitor::Impl final {
 public:
  Impl(
      VssDiffAreaMonitorPolicy supplied_policy,
      VssDiffAreaMonitorBinding supplied_binding,
      IVssDiffAreaObserver* supplied_observer,
      VssDiffAreaEvidenceSink supplied_evidence_sink)
      : policy(std::move(supplied_policy)),
        binding(std::move(supplied_binding)),
        observer(supplied_observer),
        evidence_sink(std::move(supplied_evidence_sink)) {}

  [[nodiscard]] clonecore::Result<VssDiffAreaPollResult> current_result(
      const bool performed) const {
    return clonecore::Result<VssDiffAreaPollResult>::success(
        VssDiffAreaPollResult{
            .directive = current_directive,
            .observation_performed = performed,
            .latest_evidence_sequence =
                latest.has_value() ? latest->sequence : 0U,
        });
  }

  [[nodiscard]] clonecore::Result<VssDiffAreaPollResult> publish(
      VssDiffAreaPollEvidence evidence,
      const bool performed) {
    latest = std::move(evidence);
    clonecore::Status saved = clonecore::success_status();
    try {
      saved = evidence_sink(*latest);
    } catch (...) {
      saved = clonecore::Status::failure(monitor_error(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"VSS差分領域証拠記録",
          L"証拠記録コールバックが例外を送出しました"));
    }
    if (!saved) {
      current_directive =
          VssDiffAreaMonitorDirective::safe_cancel_required;
      latest->directive = current_directive;
      latest->failure = saved.error();
      return clonecore::Result<VssDiffAreaPollResult>::failure(
          saved.error());
    }
    return current_result(performed);
  }

  [[nodiscard]] clonecore::Result<VssDiffAreaPollResult> fail_closed(
      const std::uint64_t elapsed_ms,
      clonecore::Error failure,
      const bool performed) {
    current_directive =
        VssDiffAreaMonitorDirective::safe_cancel_required;
    if (sequence == (std::numeric_limits<std::uint64_t>::max)()) {
      return clonecore::Result<VssDiffAreaPollResult>::failure(
          monitor_error(
              clonecore::ErrorCode::internal_error,
              ERROR_ARITHMETIC_OVERFLOW,
              L"VSS差分領域証拠番号",
              L"証拠番号を安全に更新できません"));
    }
    ++sequence;
    return publish(
        VssDiffAreaPollEvidence{
            .sequence = sequence,
            .observed_elapsed_ms = elapsed_ms,
            .policy = policy,
            .directive = current_directive,
            .failure = std::move(failure),
        },
        performed);
  }

  [[nodiscard]] clonecore::Result<VssDiffAreaPollResult>
  fail_closed_for_unexpected_exception(
      const std::uint64_t elapsed_ms,
      const bool performed) {
    current_directive =
        VssDiffAreaMonitorDirective::safe_cancel_required;
    try {
      return fail_closed(
          elapsed_ms,
          monitor_error(
              clonecore::ErrorCode::internal_error,
              ERROR_UNHANDLED_EXCEPTION,
              L"VSS差分領域監視状態機械",
              L"監視状態機械で想定外の例外が発生したため安全に継続できません"),
          performed);
    } catch (...) {
      // Even the evidence/error allocation path may be the source of the
      // exception. Preserve the terminal state and return an allocation-free
      // fallback error rather than allowing an exception across the safety
      // boundary. Evidence could not be guaranteed in this last-resort path.
      current_directive =
          VssDiffAreaMonitorDirective::safe_cancel_required;
      return clonecore::Result<VssDiffAreaPollResult>::failure(
          clonecore::Error{
              .code = clonecore::ErrorCode::internal_error,
              .native_code = ERROR_UNHANDLED_EXCEPTION,
          });
    }
  }

  struct LatchedBackingVolume final {
    std::wstring guid_path;
    std::uint32_t serial_number{};
    std::uint64_t total_bytes{};
    // Free/available are intentionally not immutable identity fields. Their
    // first values remain in poll evidence and every later poll resamples both
    // counters. Review-resume accepts the fresh sample only when neither raw
    // counter nor their conservative minimum is worse than what was shown.
  };

  [[nodiscard]] clonecore::Status validate_or_latch_diff_area_volumes(
      const std::vector<VssDiffAreaSampleEvidence>& samples) {
    if (latched_diff_area_volumes.empty()) {
      latched_diff_area_volumes.reserve(samples.size());
      for (const auto& sample : samples) {
        for (const auto& previous : latched_diff_area_volumes) {
          if (equals_case_insensitive(
                  previous.guid_path,
                  sample.observation.backing_volume_guid_path) &&
              (previous.serial_number !=
                   sample.observation.backing_volume_serial_number ||
               previous.total_bytes !=
                   sample.observation.backing_volume_total_bytes)) {
            return clonecore::Status::failure(monitor_error(
                clonecore::ErrorCode::identity_mismatch,
                ERROR_INVALID_DATA,
                L"VSS差分領域backing Volume一意性検証",
                L"同じbacking Volume GUIDに複数のidentityまたはtotal容量が観測されました"));
          }
        }
        latched_diff_area_volumes.push_back(LatchedBackingVolume{
            .guid_path = sample.observation.backing_volume_guid_path,
            .serial_number =
                sample.observation.backing_volume_serial_number,
            .total_bytes = sample.observation.backing_volume_total_bytes,
        });
      }
      return clonecore::success_status();
    }
    if (latched_diff_area_volumes.size() != samples.size()) {
      return clonecore::Status::failure(monitor_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"VSS差分領域Volume latch件数検証",
          L"固定済みSnapshotと差分領域Volume latchの件数が一致しません"));
    }
    for (std::size_t index = 0U; index < samples.size(); ++index) {
      if (!equals_case_insensitive(
              latched_diff_area_volumes[index].guid_path,
              samples[index].observation.backing_volume_guid_path) ||
          latched_diff_area_volumes[index].serial_number !=
              samples[index].observation.backing_volume_serial_number ||
          latched_diff_area_volumes[index].total_bytes !=
              samples[index].observation.backing_volume_total_bytes) {
        return clonecore::Status::failure(monitor_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"VSS差分領域Volume latch再検証",
            L"Snapshotに対応するbacking Volume GUID、serial、またはtotal容量が初回観測から変化しました"));
      }
    }
    return clonecore::success_status();
  }

  const VssDiffAreaMonitorPolicy policy;
  const VssDiffAreaMonitorBinding binding;
  IVssDiffAreaObserver* observer{};
  VssDiffAreaEvidenceSink evidence_sink;
  VssDiffAreaMonitorDirective current_directive{
      VssDiffAreaMonitorDirective::continue_operation};
  std::optional<std::uint64_t> last_observed_elapsed_ms;
  std::uint64_t sequence{};
  std::optional<VssDiffAreaPollEvidence> latest;
  std::vector<LatchedBackingVolume> latched_diff_area_volumes;
};

VssDiffAreaMonitor::VssDiffAreaMonitor(
    VssDiffAreaMonitorPolicy policy,
    VssDiffAreaMonitorBinding binding,
    IVssDiffAreaObserver* const observer,
    VssDiffAreaEvidenceSink evidence_sink)
    : impl_(std::make_unique<Impl>(
          std::move(policy),
          std::move(binding),
          observer,
          std::move(evidence_sink))) {}

VssDiffAreaMonitor::~VssDiffAreaMonitor() = default;

clonecore::Result<std::unique_ptr<VssDiffAreaMonitor>>
VssDiffAreaMonitor::create(
    VssDiffAreaMonitorPolicy policy,
    VssDiffAreaMonitorBinding binding,
    IVssDiffAreaObserver* const observer,
    VssDiffAreaEvidenceSink evidence_sink) {
  try {
    const auto policy_status = validate_policy(policy);
    if (!policy_status) {
      return clonecore::Result<std::unique_ptr<VssDiffAreaMonitor>>::failure(
          policy_status.error());
    }
    const auto binding_status = validate_binding(binding);
    if (!binding_status) {
      return clonecore::Result<std::unique_ptr<VssDiffAreaMonitor>>::failure(
          binding_status.error());
    }
    if (observer == nullptr || !evidence_sink) {
      return clonecore::Result<std::unique_ptr<VssDiffAreaMonitor>>::failure(
          monitor_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"VSS差分領域監視境界検証",
              L"読取り専用observerと証拠sinkの両方が必要です"));
    }
    return clonecore::Result<std::unique_ptr<VssDiffAreaMonitor>>::success(
        std::unique_ptr<VssDiffAreaMonitor>(new VssDiffAreaMonitor(
            std::move(policy),
            std::move(binding),
            observer,
            std::move(evidence_sink))));
  } catch (...) {
    return clonecore::Result<std::unique_ptr<VssDiffAreaMonitor>>::failure(
        clonecore::Error{
            .code = clonecore::ErrorCode::internal_error,
            .native_code = ERROR_UNHANDLED_EXCEPTION,
        });
  }
}

clonecore::Result<VssDiffAreaPollResult> VssDiffAreaMonitor::poll(
    const std::uint64_t elapsed_ms) {
  return poll_internal(elapsed_ms, false);
}

clonecore::Result<VssDiffAreaPollResult> VssDiffAreaMonitor::poll_now(
    const std::uint64_t elapsed_ms) {
  return poll_internal(elapsed_ms, true);
}

clonecore::Result<VssDiffAreaPollResult> VssDiffAreaMonitor::poll_internal(
    const std::uint64_t elapsed_ms,
    const bool force_observation) {
  bool observation_attempted = false;
  try {
    if (impl_->current_directive !=
        VssDiffAreaMonitorDirective::continue_operation) {
      return impl_->current_result(false);
    }
    if (impl_->last_observed_elapsed_ms.has_value()) {
      if (elapsed_ms < *impl_->last_observed_elapsed_ms) {
        return impl_->fail_closed(
            elapsed_ms,
            monitor_error(
                clonecore::ErrorCode::verification_failed,
                ERROR_INVALID_TIME,
                L"VSS差分領域監視時刻検証",
                L"監視経過時間が後退したため継続できません"),
            false);
      }
      if (!force_observation &&
          elapsed_ms - *impl_->last_observed_elapsed_ms <
          impl_->policy.poll_interval_ms) {
        return impl_->current_result(false);
      }
    }

    observation_attempted = true;
    auto observed = impl_->observer->observe(impl_->binding);
    impl_->last_observed_elapsed_ms = elapsed_ms;
    if (!observed) {
      return impl_->fail_closed(elapsed_ms, observed.error(), true);
    }

    auto derived = validate_and_derive_samples(
        impl_->policy, impl_->binding, observed.value());
    if (!derived) {
      return impl_->fail_closed(elapsed_ms, derived.error(), true);
    }
    auto samples = derived.take_value();
    const auto diff_area_status =
        impl_->validate_or_latch_diff_area_volumes(samples);
    if (!diff_area_status) {
      return impl_->fail_closed(
          elapsed_ms, diff_area_status.error(), true);
    }
    if (impl_->sequence == (std::numeric_limits<std::uint64_t>::max)()) {
      return impl_->fail_closed(
          elapsed_ms,
          monitor_error(
              clonecore::ErrorCode::internal_error,
              ERROR_ARITHMETIC_OVERFLOW,
              L"VSS差分領域証拠番号",
              L"証拠番号を安全に更新できません"),
          true);
    }
    ++impl_->sequence;
    impl_->current_directive = any_threshold_reached(samples)
        ? VssDiffAreaMonitorDirective::review_required
        : VssDiffAreaMonitorDirective::continue_operation;
    return impl_->publish(
        VssDiffAreaPollEvidence{
            .sequence = impl_->sequence,
            .observed_elapsed_ms = elapsed_ms,
            .policy = impl_->policy,
            .directive = impl_->current_directive,
            .samples = std::move(samples),
        },
        true);
  } catch (...) {
    return impl_->fail_closed_for_unexpected_exception(
        elapsed_ms, observation_attempted);
  }
}

clonecore::Result<VssDiffAreaPollResult>
VssDiffAreaMonitor::resolve_review(
    const std::uint64_t evidence_sequence,
    const std::uint64_t elapsed_ms,
    const VssDiffAreaReviewAction action) {
  bool observation_attempted = false;
  try {
    if (!valid_review_action(action)) {
      return impl_->fail_closed(
          elapsed_ms,
          monitor_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"VSS差分領域Review操作検証",
              L"認識できないReview操作を受け取ったため安全に継続できません"),
          false);
    }
    if (impl_->current_directive !=
            VssDiffAreaMonitorDirective::review_required ||
        !impl_->latest.has_value()) {
      return clonecore::Result<VssDiffAreaPollResult>::failure(monitor_error(
          clonecore::ErrorCode::confirmation_required,
          ERROR_INVALID_STATE,
          L"VSS差分領域Review状態検証",
          L"現在は再開または取消のReview待ちではありません"));
    }
    if (evidence_sequence == 0U ||
        evidence_sequence != impl_->latest->sequence) {
      return clonecore::Result<VssDiffAreaPollResult>::failure(monitor_error(
          clonecore::ErrorCode::identity_mismatch,
          ERROR_INVALID_DATA,
          L"VSS差分領域Review証拠検証",
          L"表示した最新の差分領域証拠番号と一致しません"));
    }
    if (!impl_->last_observed_elapsed_ms.has_value() ||
        elapsed_ms < *impl_->last_observed_elapsed_ms) {
      return impl_->fail_closed(
          elapsed_ms,
          monitor_error(
              clonecore::ErrorCode::verification_failed,
              ERROR_INVALID_TIME,
              L"VSS差分領域Review時刻検証",
              L"Review経過時間が最新観測より前へ戻ったため継続できません"),
          false);
    }
    if (impl_->sequence == (std::numeric_limits<std::uint64_t>::max)()) {
      return impl_->fail_closed(
          elapsed_ms,
          monitor_error(
              clonecore::ErrorCode::internal_error,
              ERROR_ARITHMETIC_OVERFLOW,
              L"VSS差分領域Review証拠番号",
              L"Review証拠番号を安全に更新できません"),
          false);
    }

    if (action == VssDiffAreaReviewAction::safe_cancel) {
      ++impl_->sequence;
      VssDiffAreaPollEvidence reviewed = *impl_->latest;
      reviewed.sequence = impl_->sequence;
      reviewed.observed_elapsed_ms = elapsed_ms;
      reviewed.reviewed_evidence_sequence = evidence_sequence;
      reviewed.review_action = action;
      reviewed.failure.reset();
      reviewed.directive =
          VssDiffAreaMonitorDirective::safe_cancel_required;
      impl_->current_directive = reviewed.directive;
      return impl_->publish(std::move(reviewed), false);
    }

    // A resume decision is valid only against a new observation made at the
    // decision boundary. The evidence displayed to the operator is retained
    // solely as the no-worse baseline; it never advances the poll clock.
    const auto& displayed_samples = impl_->latest->samples;
    observation_attempted = true;
    auto observed = impl_->observer->observe(impl_->binding);
    impl_->last_observed_elapsed_ms = elapsed_ms;
    if (!observed) {
      return impl_->fail_closed(elapsed_ms, observed.error(), true);
    }

    auto derived = validate_and_derive_samples(
        impl_->policy, impl_->binding, observed.value());
    if (!derived) {
      return impl_->fail_closed(elapsed_ms, derived.error(), true);
    }
    auto fresh_samples = derived.take_value();
    const auto diff_area_status =
        impl_->validate_or_latch_diff_area_volumes(fresh_samples);
    if (!diff_area_status) {
      return impl_->fail_closed(
          elapsed_ms, diff_area_status.error(), true);
    }

    const bool no_worse =
        observations_are_no_worse(displayed_samples, fresh_samples);
    ++impl_->sequence;
    impl_->current_directive = no_worse
        ? VssDiffAreaMonitorDirective::continue_operation
        : VssDiffAreaMonitorDirective::review_required;
    return impl_->publish(
        VssDiffAreaPollEvidence{
            .sequence = impl_->sequence,
            .observed_elapsed_ms = elapsed_ms,
            .policy = impl_->policy,
            .directive = impl_->current_directive,
            .samples = std::move(fresh_samples),
            .reviewed_evidence_sequence = evidence_sequence,
            .review_action = action,
        },
        true);
  } catch (...) {
    return impl_->fail_closed_for_unexpected_exception(
        elapsed_ms, observation_attempted);
  }
}

VssDiffAreaMonitorDirective VssDiffAreaMonitor::directive() const noexcept {
  return impl_->current_directive;
}

const VssDiffAreaMonitorPolicy& VssDiffAreaMonitor::policy() const noexcept {
  return impl_->policy;
}

const VssDiffAreaMonitorBinding& VssDiffAreaMonitor::binding() const noexcept {
  return impl_->binding;
}

const std::optional<VssDiffAreaPollEvidence>&
VssDiffAreaMonitor::latest_evidence() const noexcept {
  return impl_->latest;
}

class VssDiffAreaOperationMonitor::Impl final {
 public:
  Impl(
      VssDiffAreaMonitorPolicy supplied_policy,
      VssDiffAreaMonitorBinding supplied_binding,
      std::unique_ptr<IVssDiffAreaObserver> supplied_observer,
      VssDiffAreaReviewCallback supplied_review,
      VssDiffAreaElapsedMillisecondsProvider supplied_elapsed)
      : observer(std::move(supplied_observer)),
        review_callback(std::move(supplied_review)),
        elapsed_ms(std::move(supplied_elapsed)) {
    evidence.policy = std::move(supplied_policy);
    evidence.binding = std::move(supplied_binding);
    evidence.only_polled_at_disk_operation_safe_boundaries = true;
    evidence.same_process_only = true;
  }

  [[nodiscard]] clonecore::Result<std::uint64_t> now() {
    try {
      return clonecore::Result<std::uint64_t>::success(
          elapsed_ms ? elapsed_ms() : GetTickCount64());
    } catch (...) {
      return clonecore::Result<std::uint64_t>::failure(monitor_error(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"VSS差分領域経過時間取得",
          L"経過時間providerが例外を送出したため安全に継続できません"));
    }
  }

  [[nodiscard]] clonecore::Status save_poll(
      const VssDiffAreaPollEvidence& poll) {
    try {
      evidence.polls.push_back(VssDiffAreaOperationPollEvidence{
          .reason = pending_reason,
          .safe_boundary = pending_boundary,
          .monitor = poll,
      });
      return clonecore::success_status();
    } catch (...) {
      return clonecore::Status::failure(monitor_error(
          clonecore::ErrorCode::internal_error,
          ERROR_UNHANDLED_EXCEPTION,
          L"VSS差分領域製品証拠保存",
          L"製品経路の差分領域証拠を保持できませんでした"));
    }
  }

  [[nodiscard]] clonecore::Status terminal(clonecore::Error error) {
    evidence.final_directive =
        VssDiffAreaMonitorDirective::safe_cancel_required;
    evidence.terminal_failure = error;
    return clonecore::Status::failure(std::move(error));
  }

  [[nodiscard]] clonecore::Status directive_status(
      const VssDiffAreaPollResult& result) {
    evidence.final_directive = result.directive;
    if (result.directive ==
        VssDiffAreaMonitorDirective::continue_operation) {
      return clonecore::success_status();
    }
    if (result.directive == VssDiffAreaMonitorDirective::review_required) {
      return resolve_review_loop();
    }
    if (!evidence.polls.empty() &&
        evidence.polls.back().monitor.failure.has_value()) {
      return terminal(*evidence.polls.back().monitor.failure);
    }
    return terminal(monitor_error(
        clonecore::ErrorCode::cancelled,
        ERROR_CANCELLED,
        L"VSS差分領域安全取消",
        L"VSS差分領域の監視結果により安全に中止しました"));
  }

  [[nodiscard]] clonecore::Status resolve_review_loop() {
    while (monitor->directive() ==
           VssDiffAreaMonitorDirective::review_required) {
      if (!monitor->latest_evidence().has_value()) {
        return terminal(monitor_error(
            clonecore::ErrorCode::internal_error,
            ERROR_INVALID_STATE,
            L"VSS差分領域Review証拠",
            L"Review待ちに対応する表示証拠がありません"));
      }
      const auto displayed = *monitor->latest_evidence();
      VssDiffAreaReviewDecision decision{
          .displayed_evidence_sequence = displayed.sequence,
          .action = VssDiffAreaReviewAction::safe_cancel,
      };
      std::optional<clonecore::Error> decision_failure;
      if (review_callback) {
        try {
          auto selected = review_callback(displayed);
          if (selected) {
            decision = selected.take_value();
          } else {
            decision_failure = selected.error();
          }
        } catch (...) {
          decision_failure = monitor_error(
              clonecore::ErrorCode::internal_error,
              ERROR_UNHANDLED_EXCEPTION,
              L"VSS差分領域Review UI",
              L"Review UI callbackが例外を送出したため安全に中止します");
        }
      }

      const bool valid_action =
          decision.action == VssDiffAreaReviewAction::resume_once ||
          decision.action == VssDiffAreaReviewAction::safe_cancel;
      const bool accepted = !decision_failure.has_value() && valid_action &&
          decision.displayed_evidence_sequence == displayed.sequence;
      try {
        evidence.reviews.push_back(VssDiffAreaOperationReviewEvidence{
            .displayed_evidence_sequence = displayed.sequence,
            .submitted_evidence_sequence =
                decision.displayed_evidence_sequence,
            .action = decision.action,
            .accepted = accepted,
        });
      } catch (...) {
        return terminal(monitor_error(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"VSS差分領域Review証拠保存",
            L"表示したReviewと利用者操作の対応を保持できませんでした"));
      }

      if (!accepted) {
        if (!decision_failure.has_value()) {
          decision_failure = monitor_error(
              clonecore::ErrorCode::identity_mismatch,
              ERROR_INVALID_DATA,
              L"VSS差分領域Review sequence",
              L"操作が表示中の最新証拠番号に束縛されていません");
        }
        decision.action = VssDiffAreaReviewAction::safe_cancel;
        decision.displayed_evidence_sequence = displayed.sequence;
      }

      pending_reason = VssDiffAreaOperationPollReason::review_resume;
      pending_boundary.reset();
      auto elapsed = now();
      if (!elapsed) {
        return terminal(elapsed.error());
      }
      auto resolved = monitor->resolve_review(
          displayed.sequence, elapsed.value(), decision.action);
      if (!resolved) {
        return terminal(resolved.error());
      }
      evidence.final_directive = resolved.value().directive;
      if (!accepted && decision_failure.has_value()) {
        evidence.terminal_failure = *decision_failure;
      }
      if (resolved.value().directive ==
          VssDiffAreaMonitorDirective::safe_cancel_required) {
        return terminal(
            decision_failure.value_or(monitor_error(
                clonecore::ErrorCode::cancelled,
                ERROR_CANCELLED,
                L"VSS差分領域Review安全取消",
                L"表示した差分領域証拠を確認して安全に中止しました")));
      }
    }
    return clonecore::success_status();
  }

  [[nodiscard]] clonecore::Status perform_poll(
      const VssDiffAreaOperationPollReason reason,
      std::optional<clonecore::DiskOperationSafeBoundary> boundary,
      const bool force) {
    if (!monitor) {
      return terminal(monitor_error(
          clonecore::ErrorCode::internal_error,
          ERROR_INVALID_HANDLE,
          L"VSS差分領域製品monitor",
          L"製品経路のmonitorが初期化されていません"));
    }
    pending_reason = reason;
    pending_boundary = std::move(boundary);
    auto elapsed = now();
    if (!elapsed) {
      return terminal(elapsed.error());
    }
    auto polled = force ? monitor->poll_now(elapsed.value())
                        : monitor->poll(elapsed.value());
    if (!polled) {
      return terminal(polled.error());
    }
    return directive_status(polled.value());
  }

  std::unique_ptr<IVssDiffAreaObserver> observer;
  VssDiffAreaReviewCallback review_callback;
  VssDiffAreaElapsedMillisecondsProvider elapsed_ms;
  std::unique_ptr<VssDiffAreaMonitor> monitor;
  VssDiffAreaOperationEvidence evidence;
  VssDiffAreaOperationPollReason pending_reason{
      VssDiffAreaOperationPollReason::initial_before_output};
  std::optional<clonecore::DiskOperationSafeBoundary> pending_boundary;
};

VssDiffAreaOperationMonitor::VssDiffAreaOperationMonitor(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

VssDiffAreaOperationMonitor::~VssDiffAreaOperationMonitor() = default;

clonecore::Result<std::unique_ptr<VssDiffAreaOperationMonitor>>
VssDiffAreaOperationMonitor::create(
    VssDiffAreaMonitorPolicy policy,
    VssDiffAreaMonitorBinding binding,
    std::unique_ptr<IVssDiffAreaObserver> observer,
    VssDiffAreaReviewCallback review_callback,
    VssDiffAreaElapsedMillisecondsProvider elapsed_ms) {
  try {
    if (!observer) {
      return clonecore::Result<
          std::unique_ptr<VssDiffAreaOperationMonitor>>::failure(
          monitor_error(
              clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"VSS差分領域製品observer",
              L"製品経路には所有されたread-only observerが必要です"));
    }
    auto impl = std::make_unique<Impl>(
        std::move(policy),
        std::move(binding),
        std::move(observer),
        std::move(review_callback),
        std::move(elapsed_ms));
    Impl* const state = impl.get();
    auto monitor = VssDiffAreaMonitor::create(
        impl->evidence.policy,
        impl->evidence.binding,
        impl->observer.get(),
        [state](const VssDiffAreaPollEvidence& value) {
          return state->save_poll(value);
        });
    if (!monitor) {
      return clonecore::Result<
          std::unique_ptr<VssDiffAreaOperationMonitor>>::failure(
          monitor.error());
    }
    impl->monitor = monitor.take_value();
    return clonecore::Result<
        std::unique_ptr<VssDiffAreaOperationMonitor>>::success(
        std::unique_ptr<VssDiffAreaOperationMonitor>(
            new VssDiffAreaOperationMonitor(std::move(impl))));
  } catch (...) {
    return clonecore::Result<
        std::unique_ptr<VssDiffAreaOperationMonitor>>::failure(
        monitor_error(
            clonecore::ErrorCode::internal_error,
            ERROR_UNHANDLED_EXCEPTION,
            L"VSS差分領域製品monitor生成",
            L"製品monitor生成時の例外を安全側に拒否しました"));
  }
}

clonecore::Status VssDiffAreaOperationMonitor::initial_poll() {
  if (impl_->evidence.initial_poll_completed_before_output ||
      !impl_->evidence.polls.empty()) {
    return impl_->terminal(monitor_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_STATE,
        L"VSS差分領域初回poll順序",
        L"初回pollは最初のoutput変更前に1回だけ実行できます"));
  }
  auto status = impl_->perform_poll(
      VssDiffAreaOperationPollReason::initial_before_output,
      std::nullopt,
      true);
  if (status) {
    impl_->evidence.initial_poll_completed_before_output = true;
  }
  return status;
}

clonecore::Status VssDiffAreaOperationMonitor::completion_poll() {
  if (!impl_->evidence.initial_poll_completed_before_output ||
      impl_->evidence
          .completion_poll_completed_before_snapshot_callback_return) {
    return impl_->terminal(monitor_error(
        clonecore::ErrorCode::invalid_data,
        ERROR_INVALID_STATE,
        L"VSS差分領域完了poll順序",
        L"完了pollには成功した初回pollが必要で、1回だけ実行できます"));
  }
  auto status = impl_->perform_poll(
      VssDiffAreaOperationPollReason::
          completion_before_snapshot_callback_return,
      std::nullopt,
      true);
  if (status) {
    impl_->evidence
        .completion_poll_completed_before_snapshot_callback_return = true;
  }
  return status;
}

clonecore::DiskOperationCallbacks VssDiffAreaOperationMonitor::callbacks(
    clonecore::DiskOperationCallbacks existing) {
  const auto existing_cancellation = existing.cancellation_requested;
  const auto existing_boundary = std::move(existing.safe_boundary);
  existing.safe_boundary =
      [state = impl_.get(), existing_cancellation, existing_boundary](
          const clonecore::DiskOperationSafeBoundary& boundary) {
        if (existing_cancellation) {
          try {
            if (existing_cancellation()) {
              return clonecore::DiskOperationControlDecision::
                  cancel_operation;
            }
          } catch (...) {
            static_cast<void>(state->terminal(monitor_error(
                clonecore::ErrorCode::internal_error,
                ERROR_UNHANDLED_EXCEPTION,
                L"VSS差分領域既存取消確認",
                L"既存の取消callbackが例外を送出しました")));
            return clonecore::DiskOperationControlDecision::
                cancel_operation;
          }
        }
        const auto monitored = state->perform_poll(
            VssDiffAreaOperationPollReason::safe_boundary,
            boundary,
            false);
        if (!monitored) {
          return clonecore::DiskOperationControlDecision::cancel_operation;
        }
        if (!existing_boundary) {
          return clonecore::DiskOperationControlDecision::continue_operation;
        }
        try {
          return existing_boundary(boundary);
        } catch (...) {
          static_cast<void>(state->terminal(monitor_error(
              clonecore::ErrorCode::internal_error,
              ERROR_UNHANDLED_EXCEPTION,
              L"VSS差分領域既存安全境界",
              L"既存の手動一時停止／取消callbackが例外を送出しました")));
          return clonecore::DiskOperationControlDecision::cancel_operation;
        }
      };
  return existing;
}

const VssDiffAreaOperationEvidence&
VssDiffAreaOperationMonitor::evidence() const noexcept {
  return impl_->evidence;
}

clonecore::Status validate_completed_vss_diff_area_operation_evidence(
    const VssDiffAreaOperationEvidence& evidence) {
  const auto policy = validate_policy(evidence.policy);
  if (!policy) {
    return policy;
  }
  const auto binding = validate_binding(evidence.binding);
  if (!binding) {
    return binding;
  }
  if (!evidence.same_process_only ||
      !evidence.initial_poll_completed_before_output ||
      !evidence.only_polled_at_disk_operation_safe_boundaries ||
      !evidence
           .completion_poll_completed_before_snapshot_callback_return ||
      evidence.final_directive !=
          VssDiffAreaMonitorDirective::continue_operation ||
      evidence.terminal_failure.has_value() ||
      evidence.polls.size() < 2U ||
      evidence.polls.front().reason !=
          VssDiffAreaOperationPollReason::initial_before_output ||
      evidence.polls.back().reason !=
          VssDiffAreaOperationPollReason::
              completion_before_snapshot_callback_return) {
    return clonecore::Status::failure(monitor_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        L"VSS差分領域製品完了証拠",
        L"same-process初回／安全境界／完了pollまたは最終継続directiveが揃っていません"));
  }

  std::uint64_t previous_sequence{};
  std::vector<std::wstring> diff_area_volumes(
      evidence.binding.snapshots.size());
  std::vector<std::uint32_t> backing_volume_serials(
      evidence.binding.snapshots.size());
  std::vector<std::uint64_t> backing_volume_totals(
      evidence.binding.snapshots.size());
  for (std::size_t poll_index = 0U;
       poll_index < evidence.polls.size(); ++poll_index) {
    const auto& route_poll = evidence.polls[poll_index];
    const auto& poll = route_poll.monitor;
    if (poll.sequence == 0U ||
        (previous_sequence != 0U && poll.sequence != previous_sequence + 1U) ||
        poll.policy.poll_interval_ms != evidence.policy.poll_interval_ms ||
        poll.policy.danger_used_basis_points !=
            evidence.policy.danger_used_basis_points ||
        poll.policy.minimum_remaining_bytes !=
            evidence.policy.minimum_remaining_bytes ||
        poll.failure.has_value() ||
        poll.samples.size() != evidence.binding.snapshots.size()) {
      return clonecore::Status::failure(monitor_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"VSS差分領域poll証拠再検証",
          L"poll sequence、policy、sample件数、または成功状態が不正です"));
    }
    previous_sequence = poll.sequence;
    if ((route_poll.reason ==
             VssDiffAreaOperationPollReason::safe_boundary) !=
        route_poll.safe_boundary.has_value()) {
      return clonecore::Status::failure(monitor_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"VSS差分領域安全境界証拠",
          L"定期pollだけがDiskOperationSafeBoundaryへ束縛されている必要があります"));
    }
    for (std::size_t sample_index = 0U;
         sample_index < poll.samples.size(); ++sample_index) {
      const auto& expected = evidence.binding.snapshots[sample_index];
      const auto& sample = poll.samples[sample_index];
      const auto& observed = sample.observation;
      if (!equals_case_insensitive(
              observed.snapshot_set_id, evidence.binding.snapshot_set_id) ||
          !equals_case_insensitive(observed.snapshot_id, expected.snapshot_id) ||
          !equals_case_insensitive(
              observed.original_volume_guid_path,
              expected.original_volume_guid_path) ||
          !equals_case_insensitive(
              observed.snapshot_device_path,
              expected.snapshot_device_path) ||
          !equals_case_insensitive(observed.provider_id, expected.provider_id) ||
          observed.creation_timestamp != expected.creation_timestamp ||
          observed.observed_source_identity_token !=
              expected.expected_source_identity_token ||
          !is_volume_guid_path(observed.diff_area_volume_guid_path) ||
          !is_volume_guid_path(observed.backing_volume_guid_path) ||
          !equals_case_insensitive(
              observed.backing_volume_guid_path,
              observed.diff_area_volume_guid_path) ||
          observed.backing_volume_total_bytes == 0U ||
          observed.backing_volume_free_bytes >
              observed.backing_volume_total_bytes ||
          observed.backing_volume_available_bytes >
              observed.backing_volume_total_bytes ||
          observed.maximum_kind != VssDiffAreaMaximumKind::bounded ||
          observed.maximum_bytes == 0U ||
          observed.used_bytes > observed.allocated_bytes ||
          observed.allocated_bytes > observed.maximum_bytes) {
        return clonecore::Status::failure(monitor_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"VSS差分領域binding完了再検証",
            L"Set、Snapshot、Volume、device、provider、source epoch、または有限容量が固定証拠と一致しません"));
      }
      const std::uint64_t expected_threshold = threshold_bytes(
          observed.maximum_bytes,
          evidence.policy.danger_used_basis_points);
      const std::uint64_t expected_remaining =
          observed.maximum_bytes - observed.used_bytes;
      const std::uint64_t expected_effective_backing_free = (std::min)(
          observed.backing_volume_free_bytes,
          observed.backing_volume_available_bytes);
      if (sample.danger_threshold_bytes != expected_threshold ||
          sample.remaining_bytes != expected_remaining ||
          sample.effective_backing_volume_free_bytes !=
              expected_effective_backing_free ||
          sample.used_threshold_reached !=
              (observed.used_bytes >= expected_threshold) ||
          sample.remaining_reserve_reached !=
              (evidence.policy.minimum_remaining_bytes != 0U &&
               expected_remaining <=
                   evidence.policy.minimum_remaining_bytes) ||
          sample.backing_volume_reserve_reached !=
              (expected_effective_backing_free == 0U ||
               (evidence.policy.minimum_remaining_bytes != 0U &&
                expected_effective_backing_free <=
                    evidence.policy.minimum_remaining_bytes))) {
        return clonecore::Status::failure(monitor_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_DATA,
            L"VSS差分領域容量証拠再計算",
            L"logical remainingまたはbacking Volume空き容量の派生証拠が一致しません"));
      }
      if (poll_index == 0U) {
        diff_area_volumes[sample_index] =
            observed.backing_volume_guid_path;
        backing_volume_serials[sample_index] =
            observed.backing_volume_serial_number;
        backing_volume_totals[sample_index] =
            observed.backing_volume_total_bytes;
      } else if (!equals_case_insensitive(
                     diff_area_volumes[sample_index],
                     observed.backing_volume_guid_path) ||
                 backing_volume_serials[sample_index] !=
                     observed.backing_volume_serial_number ||
                 backing_volume_totals[sample_index] !=
                     observed.backing_volume_total_bytes) {
        return clonecore::Status::failure(monitor_error(
            clonecore::ErrorCode::identity_mismatch,
            ERROR_INVALID_DATA,
            L"VSS差分領域Volume完了再検証",
            L"backing Volume GUID、serial、またはtotal容量が初回pollから変化しています"));
      }
    }
  }
  if (evidence.polls.back().monitor.directive !=
      VssDiffAreaMonitorDirective::continue_operation) {
    return clonecore::Status::failure(monitor_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_STATE,
        L"VSS差分領域最終directive再検証",
        L"completion pollが継続可能状態ではありません"));
  }
  std::size_t review_index{};
  for (const auto& route_poll : evidence.polls) {
    const auto& poll = route_poll.monitor;
    const bool has_review_binding =
        poll.reviewed_evidence_sequence.has_value() ||
        poll.review_action.has_value();
    if (!has_review_binding) {
      if (route_poll.reason ==
          VssDiffAreaOperationPollReason::review_resume) {
        return clonecore::Status::failure(monitor_error(
            clonecore::ErrorCode::verification_failed,
            ERROR_INVALID_DATA,
            L"VSS差分領域Review poll対応",
            L"review_resume pollに表示sequenceと操作の束縛がありません"));
      }
      continue;
    }
    if (!poll.reviewed_evidence_sequence.has_value() ||
        !poll.review_action.has_value() ||
        route_poll.reason !=
            VssDiffAreaOperationPollReason::review_resume ||
        review_index >= evidence.reviews.size()) {
      return clonecore::Status::failure(monitor_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"VSS差分領域Review poll対応",
          L"review証拠とfresh pollの件数または種別が一致しません"));
    }
    const auto& review = evidence.reviews[review_index++];
    if (!review.accepted || review.displayed_evidence_sequence == 0U ||
        review.displayed_evidence_sequence !=
            review.submitted_evidence_sequence ||
        review.displayed_evidence_sequence !=
            poll.reviewed_evidence_sequence.value() ||
        review.action != VssDiffAreaReviewAction::resume_once ||
        poll.review_action.value() != review.action) {
      return clonecore::Status::failure(monitor_error(
          clonecore::ErrorCode::verification_failed,
          ERROR_INVALID_DATA,
          L"VSS差分領域Review完了再検証",
          L"成功結果には同じ表示sequenceへ束縛された今回だけ再開とfresh pollだけを保持できます"));
    }
  }
  if (review_index != evidence.reviews.size()) {
    return clonecore::Status::failure(monitor_error(
        clonecore::ErrorCode::verification_failed,
        ERROR_INVALID_DATA,
        L"VSS差分領域Review件数再検証",
        L"fresh pollに対応しないreview証拠が残っています"));
  }
  return clonecore::success_status();
}

clonecore::Result<std::wstring> encode_vss_diff_area_source_epoch_token(
    const std::span<const std::byte> source_epoch_bytes) {
  if (source_epoch_bytes.empty() ||
      source_epoch_bytes.size() > kMaximumSourceEpochBytes) {
    return clonecore::Result<std::wstring>::failure(monitor_error(
        clonecore::ErrorCode::invalid_argument,
        ERROR_INVALID_PARAMETER,
        L"VSS差分領域source epoch token",
        L"source epoch bytesは1以上256以下である必要があります"));
  }
  static constexpr wchar_t hex[] = L"0123456789abcdef";
  try {
    std::wstring token;
    token.reserve(source_epoch_bytes.size() * 2U);
    for (const auto value : source_epoch_bytes) {
      const auto byte = static_cast<unsigned char>(value);
      token.push_back(hex[(byte >> 4U) & 0x0fU]);
      token.push_back(hex[byte & 0x0fU]);
    }
    return clonecore::Result<std::wstring>::success(std::move(token));
  } catch (...) {
    return clonecore::Result<std::wstring>::failure(monitor_error(
        clonecore::ErrorCode::internal_error,
        ERROR_UNHANDLED_EXCEPTION,
        L"VSS差分領域source epoch token",
        L"source epoch tokenを安全に生成できませんでした"));
  }
}

}  // namespace ytec::vssrequester

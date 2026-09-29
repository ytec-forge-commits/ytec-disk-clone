#include "ytec/clonecore/rescue_copy.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <span>
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

ytec::clonecore::Error test_error(
    const ytec::clonecore::ErrorCode code,
    const DWORD native_code,
    const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = native_code,
      .operation = operation,
      .message = L"失敗注入",
  };
}

using ReadKey = std::pair<std::uint64_t, std::size_t>;

class FailingMemorySource final
    : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit FailingMemorySource(
      const std::size_t size,
      const std::uint32_t sector_size = 512U)
      : bytes_(size), sector_size_(sector_size) {
    for (std::size_t index = 0U; index < bytes_.size(); ++index) {
      bytes_[index] = std::byte{static_cast<unsigned char>((index % 251U) + 1U)};
    }
  }

  std::uint64_t size_bytes() const noexcept override { return bytes_.size(); }

  std::uint32_t logical_sector_size() const noexcept override {
    return sector_size_;
  }

  ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    const ReadKey key{offset, length};
    read_order.push_back(key);
    ++call_counts[key];
    if (const auto fatal = fatal_errors.find(key);
        fatal != fatal_errors.end()) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          fatal->second);
    }
    if (auto injected = recoverable_failure_counts.find(key);
        injected != recoverable_failure_counts.end() &&
        injected->second != 0U) {
      --injected->second;
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(
              ytec::clonecore::ErrorCode::io_failed,
              ERROR_CRC,
              L"モック媒体読取り"));
    }
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(
              ytec::clonecore::ErrorCode::invalid_argument,
              ERROR_INVALID_PARAMETER,
              L"モック範囲外読取り"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length)));
  }

  const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

  mutable std::map<ReadKey, std::size_t> recoverable_failure_counts;
  mutable std::map<ReadKey, ytec::clonecore::Error> fatal_errors;
  mutable std::map<ReadKey, std::size_t> call_counts;
  mutable std::vector<ReadKey> read_order;

 private:
  std::vector<std::byte> bytes_;
  std::uint32_t sector_size_{};
};

class VerifyingMemoryTarget final
    : public ytec::clonecore::ITargetDiskWriter {
 public:
  explicit VerifyingMemoryTarget(
      const std::size_t size,
      const std::uint32_t sector_size = 512U)
      : bytes_(size, std::byte{0x7F}), sector_size_(sector_size) {}

  std::uint64_t size_bytes() const noexcept override { return bytes_.size(); }

  std::uint32_t logical_sector_size() const noexcept override {
    return sector_size_;
  }

  ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    write_order.emplace_back(offset, bytes.size());
    if (offset > bytes_.size() || bytes.size() > bytes_.size() - offset) {
      return ytec::clonecore::Status::failure(test_error(
          ytec::clonecore::ErrorCode::io_failed,
          ERROR_INVALID_PARAMETER,
          L"モック範囲外書込み"));
    }
    std::copy(
        bytes.begin(),
        bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++read_back_count;
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(
              ytec::clonecore::ErrorCode::io_failed,
              ERROR_INVALID_PARAMETER,
              L"モック範囲外読戻し"));
    }
    std::vector<std::byte> result(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length));
    if (corrupt_read_back && !result.empty()) {
      result[0] ^= std::byte{1};
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }

  ytec::clonecore::Status flush_target() override {
    ++flush_count;
    if (fail_flush) {
      return ytec::clonecore::Status::failure(test_error(
          ytec::clonecore::ErrorCode::io_failed,
          ERROR_WRITE_FAULT,
          L"モックflush"));
    }
    return ytec::clonecore::success_status();
  }

  const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

  std::vector<ReadKey> write_order;
  mutable std::size_t read_back_count{};
  std::size_t flush_count{};
  bool corrupt_read_back{};
  bool fail_flush{};

 private:
  std::vector<std::byte> bytes_;
  std::uint32_t sector_size_{};
};

ytec::clonecore::RescueRawCopyRequest valid_request() {
  return ytec::clonecore::RescueRawCopyRequest{
      .environment = ytec::clonecore::RescueExecutionEnvironment::windows,
      .source_kind = ytec::clonecore::RescueSourceKind::data_disk,
      .rescue_mode_explicitly_confirmed = true,
      .large_block_bytes = 1024U,
      .maximum_failed_block_count = 32U,
      .maximum_missing_range_count = 32U,
  };
}

void test_retry_order_and_full_recovery() {
  FailingMemorySource source(4096U);
  VerifyingMemoryTarget target(4096U);
  source.recoverable_failure_counts[{1024U, 1024U}] = 2U;
  source.recoverable_failure_counts[{3072U, 1024U}] = 1U;
  std::vector<ytec::clonecore::DiskOperationSafeBoundary> boundaries;
  std::vector<ytec::clonecore::RescueCopyProgress> progress;
  auto request = valid_request();
  request.callbacks.progress = [&](const auto& value) {
    progress.push_back(value);
  };
  request.callbacks.safe_boundary = [&](const auto& boundary) {
    boundaries.push_back(boundary);
    return ytec::clonecore::DiskOperationControlDecision::continue_operation;
  };

  const auto result = ytec::clonecore::execute_rescue_raw_copy(
      request, source, target);
  check(result.has_value(), "Finite retry sequence should recover the fixture");
  const auto& report = result.value();
  check(
      source.read_order ==
          std::vector<ReadKey>{
              {0U, 1024U},
              {1024U, 1024U},
              {2048U, 1024U},
              {3072U, 1024U},
              {3072U, 1024U},
              {1024U, 1024U},
              {1024U, 512U},
              {1536U, 512U}},
      "Reads must be forward, then failed blocks in reverse order, then sectors");
  check(
      report.forward_failed_block_count == 2U &&
          report.reverse_recovered_block_count == 1U &&
          report.reverse_failed_block_count == 1U &&
          report.sector_recovered_count == 2U &&
          report.recovered_bytes == 2048U,
      "Report should distinguish reverse and sector recovery");
  check(
      !report.partial_data_loss && report.missing_ranges.empty() &&
          report.zero_filled_bytes == 0U,
      "Fully recovered media must not invent a loss range");
  check(
      report.layout_preserved_without_conversion && report.byte_exact_copy &&
          report.target_flushed &&
          report.all_writes_read_back_verified &&
          report.written_and_read_back_verified_bytes == 4096U &&
          target.read_back_count == target.write_order.size() &&
          target.bytes() == source.bytes(),
      "Every target write must be read back and the raw result must match");
  check(
      boundaries.size() == target.write_order.size() &&
          !boundaries.empty() &&
          boundaries.back().completed_bytes == source.size_bytes() &&
          boundaries.back().kind ==
              ytec::clonecore::DiskOperationSafeBoundaryKind::verified_chunk,
      "Every rescue target write must expose one read-back-verified boundary");
  check(
      std::all_of(
          progress.begin(),
          progress.end(),
          [](const ytec::clonecore::RescueCopyProgress& value) {
            return !value.pause_allowed ||
                value.phase == ytec::clonecore::RescueCopyPhase::forward_read ||
                value.phase == ytec::clonecore::RescueCopyPhase::reverse_retry ||
                value.phase == ytec::clonecore::RescueCopyPhase::sector_retry;
          }),
      "Rescue validation and flush phases must never advertise pause");
}

void test_zero_fill_and_exact_loss_map() {
  FailingMemorySource source(2048U);
  VerifyingMemoryTarget target(4096U);
  source.recoverable_failure_counts[{0U, 1024U}] = 2U;
  source.recoverable_failure_counts[{1024U, 1024U}] = 2U;
  source.recoverable_failure_counts[{0U, 512U}] = 1U;
  source.recoverable_failure_counts[{1024U, 512U}] = 1U;
  source.recoverable_failure_counts[{1536U, 512U}] = 1U;

  const auto result = ytec::clonecore::execute_rescue_raw_copy(
      valid_request(), source, target);
  check(result.has_value(), "Exhausted sectors should produce a rescue report");
  const auto& report = result.value();
  check(
      report.partial_data_loss && report.zero_filled_bytes == 1536U &&
          report.exhausted_sector_count == 3U &&
          report.copied_source_bytes == 512U &&
          report.missing_ranges.size() == 2U &&
          report.layout_preserved_without_conversion &&
          !report.byte_exact_copy,
      "Any exhausted sector must force partial_data_loss and exact accounting");
  check(
      report.missing_ranges[0].bytes.offset == 0U &&
          report.missing_ranges[0].bytes.length == 512U &&
          report.missing_ranges[0].first_lba == 0U &&
          report.missing_ranges[0].sector_count == 1U &&
          report.missing_ranges[1].bytes.offset == 1024U &&
          report.missing_ranges[1].bytes.length == 1024U &&
          report.missing_ranges[1].first_lba == 2U &&
          report.missing_ranges[1].sector_count == 2U,
      "Loss map should be sorted, sector-precise, and coalesce adjacent loss");
  for (const auto& missing : report.missing_ranges) {
    check(
        missing.forward_attempts == 1U && missing.reverse_attempts == 1U &&
            missing.sector_attempts == 1U &&
            missing.forward_native_error == ERROR_CRC &&
            missing.reverse_native_error == ERROR_CRC &&
            missing.sector_native_error == ERROR_CRC &&
            missing.zero_fill_read_back_verified,
        "Each loss range must preserve every finite attempt result");
    for (std::uint64_t offset = missing.bytes.offset;
         offset < missing.bytes.offset + missing.bytes.length;
         ++offset) {
      check(target.bytes()[static_cast<std::size_t>(offset)] == std::byte{0},
            "Only mapped unreadable sectors may be zero-filled");
    }
  }
  check(
      std::equal(
          source.bytes().begin() + 512,
          source.bytes().begin() + 1024,
          target.bytes().begin() + 512),
      "A readable sector inside a failed large block must be recovered");
  check(
      std::all_of(
          target.bytes().begin() + 2048,
          target.bytes().end(),
          [](const std::byte value) { return value == std::byte{0x7F}; }),
      "A larger target tail must remain untouched; rescue performs no expansion");
}

void test_cancellation_stops_on_verified_boundary() {
  FailingMemorySource source(2048U);
  VerifyingMemoryTarget target(2048U);
  auto request = valid_request();
  std::size_t safe_boundary_count = 0U;
  request.callbacks.safe_boundary = [&safe_boundary_count](const auto&) {
    ++safe_boundary_count;
    return ytec::clonecore::DiskOperationControlDecision::cancel_operation;
  };

  const auto result =
      ytec::clonecore::execute_rescue_raw_copy(request, source, target);
  check(!result.has_value(), "Cancellation should not return a completion report");
  check(
      result.error().code == ytec::clonecore::ErrorCode::cancelled,
      "Cancellation should use the dedicated error code");
  check(
      target.write_order == std::vector<ReadKey>{{0U, 1024U}} &&
          target.read_back_count == 1U && target.flush_count == 1U &&
          safe_boundary_count == 1U,
      "Cancellation must flush only after the preceding write was read back");
}

void test_fatal_read_and_read_back_corruption_fail_closed() {
  {
    FailingMemorySource source(1024U);
    VerifyingMemoryTarget target(1024U);
    source.fatal_errors[{0U, 1024U}] = test_error(
        ytec::clonecore::ErrorCode::access_denied,
        ERROR_ACCESS_DENIED,
        L"モックReader境界違反");
    const auto result = ytec::clonecore::execute_rescue_raw_copy(
        valid_request(), source, target);
    check(
        !result.has_value() &&
            result.error().code == ytec::clonecore::ErrorCode::access_denied &&
            target.write_order.empty(),
        "Non-media failures must never be converted into zero-filled success");
  }
  {
    FailingMemorySource source(1024U);
    VerifyingMemoryTarget target(1024U);
    target.corrupt_read_back = true;
    const auto result = ytec::clonecore::execute_rescue_raw_copy(
        valid_request(), source, target);
    check(
        !result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::verification_failed &&
            target.flush_count == 1U,
        "A target read-back mismatch must fail and flush the mutation boundary");
  }
}

void test_preflight_gates_reject_without_io() {
  const auto expect_rejected_without_io = [](
                                              ytec::clonecore::RescueRawCopyRequest request,
                                              FailingMemorySource& source,
                                              VerifyingMemoryTarget& target,
                                              const ytec::clonecore::ErrorCode code) {
    const auto result = ytec::clonecore::execute_rescue_raw_copy(
        request, source, target);
    check(
        !result.has_value() && result.error().code == code &&
            source.read_order.empty() && target.write_order.empty() &&
            target.flush_count == 0U,
        "Rejected rescue preflight must not touch either data path");
  };

  {
    FailingMemorySource source(1024U);
    VerifyingMemoryTarget target(1024U);
    auto request = valid_request();
    request.rescue_mode_explicitly_confirmed = false;
    expect_rejected_without_io(
        request,
        source,
        target,
        ytec::clonecore::ErrorCode::confirmation_required);
  }
  {
    FailingMemorySource source(1024U);
    VerifyingMemoryTarget target(1024U);
    auto request = valid_request();
    request.source_kind = ytec::clonecore::RescueSourceKind::system_disk;
    expect_rejected_without_io(
        request,
        source,
        target,
        ytec::clonecore::ErrorCode::unsupported_platform);
  }
  {
    FailingMemorySource source(2048U);
    VerifyingMemoryTarget target(1024U);
    expect_rejected_without_io(
        valid_request(),
        source,
        target,
        ytec::clonecore::ErrorCode::unsupported_layout);
  }
  {
    FailingMemorySource source(4096U, 4096U);
    VerifyingMemoryTarget target(4096U, 512U);
    auto request = valid_request();
    request.large_block_bytes = 4096U;
    expect_rejected_without_io(
        request,
        source,
        target,
        ytec::clonecore::ErrorCode::unsupported_layout);
  }
  {
    FailingMemorySource source(1024U);
    VerifyingMemoryTarget target(1024U);
    auto request = valid_request();
    request.large_block_bytes = 513U;
    expect_rejected_without_io(
        request,
        source,
        target,
        ytec::clonecore::ErrorCode::invalid_argument);
  }
}

void test_winpe_gate_allows_system_rescue() {
  FailingMemorySource source(1024U);
  VerifyingMemoryTarget target(1024U);
  auto request = valid_request();
  request.source_kind = ytec::clonecore::RescueSourceKind::system_disk;
  request.environment = ytec::clonecore::RescueExecutionEnvironment::winpe;
  const auto result = ytec::clonecore::execute_rescue_raw_copy(
      request, source, target);
  check(
      result.has_value() &&
          result.value().layout_preserved_without_conversion &&
          result.value().byte_exact_copy &&
          !result.value().partial_data_loss,
      "The explicit WinPE environment gate should allow system-disk rescue");
}

void test_loss_map_capacity_fails_before_untracked_zero_write() {
  FailingMemorySource source(2048U);
  VerifyingMemoryTarget target(2048U);
  source.recoverable_failure_counts[{0U, 1024U}] = 2U;
  source.recoverable_failure_counts[{1024U, 1024U}] = 2U;
  source.recoverable_failure_counts[{0U, 512U}] = 1U;
  source.recoverable_failure_counts[{1024U, 512U}] = 1U;

  auto request = valid_request();
  request.maximum_missing_range_count = 1U;
  const auto result = ytec::clonecore::execute_rescue_raw_copy(
      request, source, target);
  check(
      !result.has_value() &&
          result.error().code == ytec::clonecore::ErrorCode::invalid_data,
      "An incomplete loss map must fail closed");
  check(
      std::find(
          target.write_order.begin(),
          target.write_order.end(),
          ReadKey{0U, 512U}) == target.write_order.end() &&
          target.flush_count == 1U,
      "Capacity must be checked before an otherwise untracked zero write");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests{
      {"retry_order_and_full_recovery", test_retry_order_and_full_recovery},
      {"zero_fill_and_exact_loss_map", test_zero_fill_and_exact_loss_map},
      {"cancellation_stops_on_verified_boundary",
       test_cancellation_stops_on_verified_boundary},
      {"fatal_read_and_read_back_corruption_fail_closed",
       test_fatal_read_and_read_back_corruption_fail_closed},
      {"preflight_gates_reject_without_io",
       test_preflight_gates_reject_without_io},
      {"winpe_gate_allows_system_rescue",
       test_winpe_gate_allows_system_rescue},
      {"loss_map_capacity_fails_before_untracked_zero_write",
       test_loss_map_capacity_fails_before_untracked_zero_write},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS: " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL: " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL: " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

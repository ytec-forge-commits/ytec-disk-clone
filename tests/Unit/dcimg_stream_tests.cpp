#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/sha256.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
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
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

class MemorySource final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit MemorySource(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++read_count;
    if (fail_reads) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"モックSnapshot読取り", L"注入した読取り失敗です"));
    }
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"モックSnapshot範囲", L"読取り範囲外です"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes_.begin() +
                static_cast<std::ptrdiff_t>(offset + length)));
  }

  mutable std::size_t read_count{};
  bool fail_reads{};

 private:
  std::vector<std::byte> bytes_;
};

class MemoryStagingTarget final
    : public ytec::imageformat::IDcimgStagingTarget {
 public:
  [[nodiscard]] ytec::clonecore::Status begin(
      const std::uint64_t expected_length) override {
    ++begin_count;
    if (fail_begin || expected_length > 64ULL * 1024ULL * 1024ULL) {
      return ytec::clonecore::Status::failure(
          test_error(L"モック段階出力開始", L"開始失敗です"));
    }
    bytes.assign(
        static_cast<std::size_t>(expected_length), std::byte{0});
    begun = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> data) override {
    if (!begun || offset > bytes.size() ||
        data.size() > bytes.size() - offset) {
      return ytec::clonecore::Status::failure(
          test_error(L"モック段階書込み", L"書込み範囲外です"));
    }
    std::copy(
        data.begin(),
        data.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]]
  ytec::clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (!begun || offset > bytes.size() ||
        length > bytes.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          test_error(L"モック段階読戻し", L"読戻し範囲外です"));
    }
    std::vector<std::byte> result(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(offset + length));
    if (corrupt_readback && flushed && !result.empty()) {
      result.front() ^= std::byte{0x01};
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }

  [[nodiscard]] ytec::clonecore::Status resize_before_verification(
      const std::uint64_t final_length) override {
    if (!begun || final_length == 0U || final_length > bytes.size()) {
      return ytec::clonecore::Status::failure(
          test_error(L"モック段階最終長", L"最終長が範囲外です"));
    }
    bytes.resize(static_cast<std::size_t>(final_length));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status flush() override {
    flushed = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status commit_verified() override {
    if (fail_commit) {
      return ytec::clonecore::Status::failure(
          test_error(L"モック段階確定", L"確定失敗です"));
    }
    committed = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status abort_incomplete() override {
    ++abort_count;
    if (fail_abort) {
      return ytec::clonecore::Status::failure(
          test_error(L"モック段階破棄", L"破棄失敗です"));
    }
    aborted = true;
    begun = false;
    bytes.clear();
    return ytec::clonecore::success_status();
  }

  std::vector<std::byte> bytes;
  std::size_t begin_count{};
  std::size_t abort_count{};
  bool begun{};
  bool flushed{};
  bool committed{};
  bool aborted{};
  bool fail_begin{};
  bool fail_commit{};
  bool fail_abort{};
  bool corrupt_readback{};
};

ytec::imageformat::DcimgBuildRequest memory_request(
    const std::vector<std::byte>& source) {
  ytec::imageformat::DcimgBuildRequest request;
  request.source_disk_size = 64ULL * 1024ULL * 1024ULL;
  request.logical_sector_size = 512;
  request.physical_sector_size = 4096;
  request.manifest = {
      std::byte{'M'}, std::byte{'A'}, std::byte{'N'}, std::byte{'1'}};
  request.partition_table_snapshot.assign(512, std::byte{0});
  request.chunks.push_back(ytec::imageformat::DcimgBuildChunk{
      .logical_offset = 0,
      .logical_length = 4096,
      .zero_filled = false,
      .data = std::vector<std::byte>(source.begin(), source.begin() + 4096),
  });
  request.chunks.push_back(ytec::imageformat::DcimgBuildChunk{
      .logical_offset = 16ULL * 1024ULL * 1024ULL,
      .logical_length = 8192,
      .zero_filled = true,
      .data = {},
  });
  request.chunks.push_back(ytec::imageformat::DcimgBuildChunk{
      .logical_offset = 32ULL * 1024ULL * 1024ULL,
      .logical_length = 4096,
      .zero_filled = false,
      .data = std::vector<std::byte>(
          source.begin() + 4096, source.begin() + 8192),
  });
  return request;
}

ytec::imageformat::DcimgStreamBuildRequest stream_request(
    const MemorySource& source) {
  ytec::imageformat::DcimgStreamBuildRequest request;
  request.source_disk_size = 64ULL * 1024ULL * 1024ULL;
  request.logical_sector_size = 512;
  request.physical_sector_size = 4096;
  request.verification_block_bytes = 1024;
  request.manifest = {
      std::byte{'M'}, std::byte{'A'}, std::byte{'N'}, std::byte{'1'}};
  request.partition_table_snapshot.assign(512, std::byte{0});
  request.chunks.push_back(ytec::imageformat::DcimgStreamChunk{
      .logical_offset = 0,
      .logical_length = 4096,
      .source_offset = 0,
      .zero_filled = false,
      .source = &source,
  });
  request.chunks.push_back(ytec::imageformat::DcimgStreamChunk{
      .logical_offset = 16ULL * 1024ULL * 1024ULL,
      .logical_length = 8192,
      .source_offset = 0,
      .zero_filled = true,
      .source = nullptr,
  });
  request.chunks.push_back(ytec::imageformat::DcimgStreamChunk{
      .logical_offset = 32ULL * 1024ULL * 1024ULL,
      .logical_length = 4096,
      .source_offset = 4096,
      .zero_filled = false,
      .source = &source,
  });
  return request;
}

std::vector<std::byte> source_bytes() {
  std::vector<std::byte> bytes(8192);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(index & 0xFFU);
  }
  return bytes;
}

std::vector<std::byte> incompressible_source_bytes() {
  std::vector<std::byte> bytes(8192);
  std::uint32_t state = 0x6D2B79F5U;
  for (auto& byte : bytes) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    byte = static_cast<std::byte>(state & 0xFFU);
  }
  return bytes;
}

void test_stream_matches_in_memory_v1_and_commits() {
  const auto bytes = source_bytes();
  MemorySource source(bytes);
  MemoryStagingTarget target;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          stream_request(source), target);
  check(result.has_value(), "A valid stream build should pass");
  check(
      result.value().committed &&
          result.value().all_chunks_read_back_verified &&
          result.value().global_hash_read_back_verified,
      "A successful stream must report all verification gates");
  check(target.committed && !target.aborted,
        "Only a verified stream should commit");
  check(source.read_count == 2,
        "Each non-zero source chunk should be read exactly once");

  const auto in_memory =
      ytec::imageformat::build_uncompressed_dcimg_v1(
          memory_request(bytes));
  check(in_memory.has_value(), "The reference in-memory image should build");
  check(target.bytes == in_memory.value(),
        "Streaming and in-memory v1 bytes must be identical");
  check(
      ytec::imageformat::inspect_dcimg_v1(target.bytes).has_value(),
      "The committed stream should pass the existing untrusted parser");
}

void test_zstandard_stream_reads_once_and_matches_in_memory_image() {
  const auto bytes = source_bytes();
  MemorySource source(bytes);
  auto request = stream_request(source);
  request.compression = ytec::imageformat::DcimgCompression::zstandard;
  MemoryStagingTarget target;
  const auto result =
      ytec::imageformat::write_verified_dcimg_v1(request, target);
  check(result.has_value(), "A valid Zstandard stream should pass");
  check(
      result.value().committed &&
          result.value().all_chunks_read_back_verified &&
          result.value().global_hash_read_back_verified,
      "A Zstandard stream must pass every verification gate before commit");
  check(source.read_count == 2,
        "Zstandard must read each non-zero source chunk exactly once");
  check(result.value().stored_data_bytes < 8192,
        "Compressible source chunks should reduce stored data bytes");

  const auto inspection =
      ytec::imageformat::inspect_dcimg_v1(target.bytes);
  check(inspection.has_value(),
        "The committed Zstandard stream must pass the untrusted parser");
  check(
      inspection.value().header.compression ==
              ytec::imageformat::DcimgCompression::zstandard &&
          inspection.value().header.compression_version == 1U &&
          inspection.value().chunks[0].compression ==
              ytec::imageformat::DcimgCompression::zstandard &&
          inspection.value().chunks[1].compression ==
              ytec::imageformat::DcimgCompression::none &&
          inspection.value().chunks[2].compression ==
              ytec::imageformat::DcimgCompression::zstandard,
      "The container profile and per-chunk compression should be canonical");

  auto in_memory_request = memory_request(bytes);
  in_memory_request.compression =
      ytec::imageformat::DcimgCompression::zstandard;
  const auto in_memory =
      ytec::imageformat::build_dcimg_v1(in_memory_request);
  check(in_memory.has_value(),
        "The reference in-memory Zstandard image should build");
  check(target.bytes == in_memory.value(),
        "Streaming and in-memory Zstandard v1 bytes must be identical");
}

void test_zstandard_stream_falls_back_for_incompressible_chunks() {
  const auto bytes = incompressible_source_bytes();
  MemorySource source(bytes);
  auto request = stream_request(source);
  request.compression = ytec::imageformat::DcimgCompression::zstandard;
  MemoryStagingTarget target;
  const auto result =
      ytec::imageformat::write_verified_dcimg_v1(request, target);
  check(result.has_value(),
        "An incompressible Zstandard stream should still pass");
  const auto inspection =
      ytec::imageformat::inspect_dcimg_v1(target.bytes);
  check(
      inspection.has_value() &&
          inspection.value().chunks[0].compression ==
              ytec::imageformat::DcimgCompression::none &&
          inspection.value().chunks[2].compression ==
              ytec::imageformat::DcimgCompression::none,
      "Chunks that do not shrink must be stored without compression");
  check(result.value().stored_data_bytes == 8192,
        "Fallback chunks should preserve their exact logical byte length");
}

void test_zstandard_stream_readback_corruption_aborts() {
  MemorySource source(source_bytes());
  auto request = stream_request(source);
  request.compression = ytec::imageformat::DcimgCompression::zstandard;
  MemoryStagingTarget target;
  target.corrupt_readback = true;
  const auto result =
      ytec::imageformat::write_verified_dcimg_v1(request, target);
  check(!result.has_value(),
        "Corrupt Zstandard staged bytes must fail readback verification");
  check(target.aborted && !target.committed && target.bytes.empty(),
        "Corrupt Zstandard output must be discarded and never committed");
}

void test_source_failure_aborts_without_commit() {
  MemorySource source(source_bytes());
  source.fail_reads = true;
  MemoryStagingTarget target;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          stream_request(source), target);
  check(!result.has_value(), "A source read failure must stop the stream");
  check(target.aborted && !target.committed && target.bytes.empty(),
        "A source failure must discard the incomplete output");
}

void test_readback_corruption_aborts_without_commit() {
  MemorySource source(source_bytes());
  MemoryStagingTarget target;
  target.corrupt_readback = true;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          stream_request(source), target);
  check(!result.has_value(), "Readback corruption must fail");
  check(target.aborted && !target.committed,
        "Corrupt staged bytes must never commit");
}

void test_commit_failure_still_aborts() {
  MemorySource source(source_bytes());
  MemoryStagingTarget target;
  target.fail_commit = true;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          stream_request(source), target);
  check(!result.has_value(), "Commit failure must be reported");
  check(target.abort_count == 1 && target.aborted && !target.committed,
        "Commit failure must still discard the incomplete output");
}

void test_invalid_source_range_stops_before_begin() {
  MemorySource source(source_bytes());
  auto request = stream_request(source);
  request.chunks[0].source_offset = source.size_bytes();
  MemoryStagingTarget target;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          request, target);
  check(!result.has_value(), "An out-of-range source must fail");
  check(target.begin_count == 0 && target.abort_count == 0,
        "Invalid input must not create a staged output");
}

void test_begin_failure_attempts_abort() {
  MemorySource source(source_bytes());
  MemoryStagingTarget target;
  target.fail_begin = true;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          stream_request(source), target);
  check(!result.has_value(), "Staging begin failure must be reported");
  check(target.begin_count == 1 && target.abort_count == 1,
        "A partially failed begin must still attempt cleanup");
}

void test_abort_failure_is_not_hidden() {
  MemorySource source(source_bytes());
  source.fail_reads = true;
  MemoryStagingTarget target;
  target.fail_abort = true;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          stream_request(source), target);
  check(!result.has_value(), "Cleanup failure must remain a failure");
  check(result.error().operation == L"dcimg未完了出力破棄",
        "Cleanup failure should be the reported safety operation");
  check(!target.committed && target.abort_count == 1,
        "Failed cleanup must never be reported as committed");
}

void test_progress_reports_distinct_work_and_completion() {
  MemorySource source(source_bytes());
  MemoryStagingTarget target;
  std::vector<ytec::clonecore::DiskOperationProgress> observations;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          stream_request(source),
          target,
          ytec::clonecore::DiskOperationCallbacks{
              .progress =
                  [&observations](
                      const ytec::clonecore::DiskOperationProgress&
                          progress) {
                    observations.push_back(progress);
                  },
          });
  check(result.has_value(), "Progress observation must not change success");
  check(!observations.empty(), "Stream creation should publish progress");
  check(
      observations.front().stage ==
              ytec::clonecore::DiskOperationStage::planning &&
          observations.front().total_read_bytes == 8192 &&
          observations.front().total_write_bytes == 8192 &&
          observations.front().total_verify_bytes > 16'384,
      "Planning progress should expose distinct bounded work totals");
  const auto& completed = observations.back();
  check(
      completed.stage ==
              ytec::clonecore::DiskOperationStage::completed &&
          completed.read_bytes == completed.total_read_bytes &&
          completed.written_bytes == completed.total_write_bytes &&
          completed.verified_bytes == completed.total_verify_bytes,
      "Final progress should show all read, write, and verification work complete");
}

void test_cancellation_aborts_incomplete_output() {
  MemorySource source(source_bytes());
  MemoryStagingTarget target;
  bool cancellation_requested = false;
  const auto result =
      ytec::imageformat::write_verified_uncompressed_dcimg_v1(
          stream_request(source),
          target,
          ytec::clonecore::DiskOperationCallbacks{
              .progress =
                  [&cancellation_requested](
                      const ytec::clonecore::DiskOperationProgress&
                          progress) {
                    if (progress.read_bytes >= 4096) {
                      cancellation_requested = true;
                    }
                  },
              .cancellation_requested =
                  [&cancellation_requested]() {
                    return cancellation_requested;
                  },
          });
  check(!result.has_value(), "Cancellation should stop stream creation");
  check(
      result.error().code ==
          ytec::clonecore::ErrorCode::cancelled,
      "Cancellation should use the dedicated error code");
  check(
      target.aborted && !target.committed && target.bytes.empty(),
      "Cancelled image output must be aborted and never committed");
}

void test_incremental_sha_rejects_short_reader() {
  const auto result = ytec::imageformat::sha256_from_reader(
      4096,
      1024,
      [](const std::uint64_t, const std::size_t length) {
        return ytec::clonecore::Result<std::vector<std::byte>>::success(
            std::vector<std::byte>(length - 1, std::byte{0}));
      });
  check(!result.has_value(),
      "Incremental SHA must reject a short read");
}

void test_four_mib_verification_blocks_reduce_io_calls() {
  constexpr std::uint64_t kLength = 16ULL * 1024ULL * 1024ULL;
  std::size_t one_mib_calls = 0;
  const auto one_mib = ytec::imageformat::sha256_from_reader(
      kLength,
      1024U * 1024U,
      [&one_mib_calls](
          const std::uint64_t,
          const std::size_t length) {
        ++one_mib_calls;
        return ytec::clonecore::Result<std::vector<std::byte>>::success(
            std::vector<std::byte>(length, std::byte{0x5A}));
      });
  std::size_t four_mib_calls = 0;
  const auto four_mib = ytec::imageformat::sha256_from_reader(
      kLength,
      4U * 1024U * 1024U,
      [&four_mib_calls](
          const std::uint64_t,
          const std::size_t length) {
        ++four_mib_calls;
        return ytec::clonecore::Result<std::vector<std::byte>>::success(
            std::vector<std::byte>(length, std::byte{0x5A}));
      });
  check(
      one_mib.has_value() && four_mib.has_value() &&
          one_mib.value() == four_mib.value(),
      "Verification block size must not change the SHA-256 result");
  check(
      one_mib_calls == 16 && four_mib_calls == 4,
      "Four-MiB verification should reduce read callbacks by 75 percent");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"stream_matches_in_memory_v1_and_commits",
       test_stream_matches_in_memory_v1_and_commits},
      {"zstandard_stream_reads_once_and_matches_in_memory_image",
       test_zstandard_stream_reads_once_and_matches_in_memory_image},
      {"zstandard_stream_falls_back_for_incompressible_chunks",
       test_zstandard_stream_falls_back_for_incompressible_chunks},
      {"zstandard_stream_readback_corruption_aborts",
       test_zstandard_stream_readback_corruption_aborts},
      {"source_failure_aborts_without_commit",
       test_source_failure_aborts_without_commit},
      {"readback_corruption_aborts_without_commit",
       test_readback_corruption_aborts_without_commit},
      {"commit_failure_still_aborts",
       test_commit_failure_still_aborts},
      {"invalid_source_range_stops_before_begin",
       test_invalid_source_range_stops_before_begin},
      {"begin_failure_attempts_abort",
       test_begin_failure_attempts_abort},
      {"abort_failure_is_not_hidden",
       test_abort_failure_is_not_hidden},
      {"progress_reports_distinct_work_and_completion",
       test_progress_reports_distinct_work_and_completion},
      {"cancellation_aborts_incomplete_output",
       test_cancellation_aborts_incomplete_output},
      {"incremental_sha_rejects_short_reader",
       test_incremental_sha_rejects_short_reader},
      {"four_mib_verification_blocks_reduce_io_calls",
       test_four_mib_verification_blocks_reduce_io_calls},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

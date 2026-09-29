#include "ytec/imageformat/tsumugi_restore_transaction.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error injected_error(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

ytec::imageformat::TsumugiRestoreDiskIdentity identity() {
  ytec::imageformat::TsumugiRestoreDiskIdentity result{
      .disk_size = 64ULL * 1024ULL,
      .logical_sector_size = 512U,
  };
  result.stable_identity_hash[0] = std::byte{0x71};
  return result;
}

class MemoryTargetSession final
    : public ytec::imageformat::ITsumugiRestoreTargetSession {
 public:
  explicit MemoryTargetSession(
      ytec::imageformat::TsumugiRestoreDiskIdentity observed)
      : observed_(std::move(observed)),
        bytes_(static_cast<std::size_t>(observed_.disk_size), std::byte{0}) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return observed_.logical_sector_size;
  }

  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    order.push_back("write");
    if (fail_write || offset > bytes_.size() ||
        bytes.size() > bytes_.size() - static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Status::failure(injected_error(
          L"合成対象書込み", L"注入した書込み失敗です"));
    }
    std::copy(bytes.begin(), bytes.end(), bytes_.begin() +
        static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    order.push_back("readback");
    if (offset > bytes_.size() ||
        length > bytes_.size() - static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"合成対象読戻し", L"読戻し範囲が不正です"));
    }
    auto result = std::vector<std::byte>(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length));
    if (corrupt_readback && !result.empty()) {
      result[0] ^= std::byte{1};
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }

  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    order.push_back("flush");
    if (fail_flush) {
      return ytec::clonecore::Status::failure(injected_error(
          L"合成対象flush", L"注入したflush失敗です"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::imageformat::TsumugiRestoreDiskIdentity>
  reidentify_locked_target() const override {
    order.push_back("reidentify");
    return ytec::clonecore::Result<
        ytec::imageformat::TsumugiRestoreDiskIdentity>::success(observed_);
  }

  [[nodiscard]] ytec::clonecore::Status prepare_layout(
      const ytec::imageformat::TsumugiVerifiedImage&,
      const ytec::imageformat::TsumugiRestoreTarget&,
      const ytec::imageformat::TsumugiRestoreHost) override {
    order.push_back("prepare-layout");
    if (fail_prepare) {
      return ytec::clonecore::Status::failure(injected_error(
          L"合成layout準備", L"注入したlayout準備失敗です"));
    }
    prepared = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status commit_layout() override {
    order.push_back("commit-layout");
    if (fail_commit) {
      return ytec::clonecore::Status::failure(injected_error(
          L"合成layout commit", L"注入したlayout commit失敗です"));
    }
    layout_committed = true;
    return ytec::clonecore::success_status();
  }

  void abort_layout() noexcept override {
    order.push_back("abort-layout");
    ++abort_count;
  }

  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
    return bytes_;
  }

  bool fail_prepare{};
  bool fail_write{};
  bool fail_flush{};
  bool fail_commit{};
  mutable bool corrupt_readback{};
  bool prepared{};
  bool layout_committed{};
  std::size_t abort_count{};
  mutable std::vector<std::string> order;

 private:
  ytec::imageformat::TsumugiRestoreDiskIdentity observed_;
  std::vector<std::byte> bytes_;
};

ytec::imageformat::TsumugiRestoreTarget whole_target(
    const ytec::imageformat::TsumugiRestoreDiskIdentity& disk) {
  return ytec::imageformat::TsumugiWholeDiskRestoreTarget{
      .disk = disk,
  };
}

void test_bounded_flush_readback_and_commit_order() {
  using namespace ytec::imageformat;
  const auto expected_identity = identity();
  MemoryTargetSession session(expected_identity);
  TsumugiBlockRestoreTransaction transaction(session, 1024U);
  TsumugiVerifiedImage image;
  auto begun = transaction.begin(
      image, whole_target(expected_identity), TsumugiRestoreHost::winpe);
  check(begun.has_value() && session.prepared,
        "matching locked target should prepare its layout");

  std::vector<std::byte> payload(2048U);
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    payload[index] = static_cast<std::byte>((index * 13U) & 0xFFU);
  }
  check(transaction.write_and_verify(
      TsumugiRestoreWrite{
          .stable_target_identity_hash =
              expected_identity.stable_identity_hash,
          .target_offset = 4096U,
          .length = payload.size(),
      }, payload).has_value(),
      "normal payload should write in bounded verified blocks");
  check(transaction.write_and_verify(
      TsumugiRestoreWrite{
          .stable_target_identity_hash =
              expected_identity.stable_identity_hash,
          .target_offset = 8192U,
          .length = 2048U,
          .zero_fill = true,
      }, {}).has_value(),
      "zero-fill payload should be materialized and verified");
  check(transaction.commit().has_value() && session.layout_committed,
        "final layout should commit after every payload verifies");

  check(std::equal(
            payload.begin(), payload.end(), session.bytes().begin() + 4096),
        "normal payload must match target bytes");
  check(std::all_of(
            session.bytes().begin() + 8192,
            session.bytes().begin() + 10240,
            [](const std::byte byte) { return byte == std::byte{0}; }),
        "zero-fill range must remain zero");
  const auto commit = std::find(
      session.order.begin(), session.order.end(), "commit-layout");
  const auto last_read = std::find(
      session.order.rbegin(), session.order.rend(), "readback");
  check(commit != session.order.end() && last_read != session.order.rend() &&
            std::distance(session.order.begin(), commit) >
                std::distance(last_read, session.order.rend()) - 1,
        "primary layout commit must occur after the last payload read-back");
}

void test_identity_drift_stops_before_layout_prepare() {
  using namespace ytec::imageformat;
  const auto expected = identity();
  auto observed = expected;
  ++observed.disk_size;
  MemoryTargetSession session(observed);
  TsumugiBlockRestoreTransaction transaction(session, 1024U);
  TsumugiVerifiedImage image;
  const auto begun = transaction.begin(
      image, whole_target(expected), TsumugiRestoreHost::winpe);
  check(!begun.has_value() && !session.prepared && session.abort_count == 0U,
        "identity drift must stop before destructive layout preparation");
}

void test_readback_and_commit_failures_remain_uncommitted() {
  using namespace ytec::imageformat;
  const auto expected = identity();
  TsumugiVerifiedImage image;

  MemoryTargetSession readback(expected);
  readback.corrupt_readback = true;
  TsumugiBlockRestoreTransaction readback_transaction(readback, 1024U);
  check(readback_transaction.begin(
      image, whole_target(expected), TsumugiRestoreHost::winpe).has_value(),
      "readback fixture should begin");
  std::vector<std::byte> payload(1024U, std::byte{0x4A});
  check(!readback_transaction.write_and_verify(
      TsumugiRestoreWrite{
          .stable_target_identity_hash = expected.stable_identity_hash,
          .target_offset = 4096U,
          .length = payload.size(),
      }, payload).has_value(),
      "readback mismatch must fail");
  readback_transaction.abort();
  check(readback.abort_count == 1U && !readback.layout_committed,
        "readback mismatch must leave the target incomplete");

  MemoryTargetSession commit(expected);
  commit.fail_commit = true;
  TsumugiBlockRestoreTransaction commit_transaction(commit, 1024U);
  check(commit_transaction.begin(
      image, whole_target(expected), TsumugiRestoreHost::winpe).has_value(),
      "commit fixture should begin");
  check(commit_transaction.write_and_verify(
      TsumugiRestoreWrite{
          .stable_target_identity_hash = expected.stable_identity_hash,
          .target_offset = 4096U,
          .length = payload.size(),
      }, payload).has_value(),
      "commit fixture payload should verify");
  check(!commit_transaction.commit().has_value(),
        "layout commit failure must propagate");
  commit_transaction.abort();
  check(commit.abort_count == 1U && !commit.layout_committed,
        "commit failure must remain recoverably incomplete");
}

void test_prepare_failure_aborts_platform_layout() {
  using namespace ytec::imageformat;
  const auto expected = identity();
  MemoryTargetSession session(expected);
  session.fail_prepare = true;
  TsumugiBlockRestoreTransaction transaction(session, 1024U);
  TsumugiVerifiedImage image;
  check(!transaction.begin(
      image, whole_target(expected), TsumugiRestoreHost::winpe).has_value(),
      "layout preparation failure must propagate");
  check(session.abort_count == 1U && !session.layout_committed,
        "partially prepared layout must be aborted inside begin");
}

}  // namespace

int main() {
  try {
    test_bounded_flush_readback_and_commit_order();
    test_identity_drift_stops_before_layout_prepare();
    test_readback_and_commit_failures_remain_uncommitted();
    test_prepare_failure_aborts_platform_layout();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All Tsumugi restore transaction tests passed\n";
  return 0;
}

#include "ytec/imageformat/windows_tsumugi_rescue_staging.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure(message);
  }
}

ytec::clonecore::Error mock_error(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

ytec::clonecore::StableDiskIdentity disk_identity(
    const std::uint32_t number,
    std::wstring instance,
    std::string serial) {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = number,
      .model = L"MOCK DISK " + std::to_wstring(number),
      .size_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .serial_suffix = std::move(serial),
      .device_instance_id = std::move(instance),
      .is_system_disk = false,
  };
}

ytec::imageformat::WindowsImagePathIdentity path_identity(
    const std::byte seed,
    const std::uint64_t size = 0U) {
  ytec::imageformat::WindowsImagePathIdentity value{
      .volume_serial_number = 0x12345678ULL,
      .file_size = size,
  };
  value.file_id.fill(seed);
  return value;
}

ytec::imageformat::WindowsImageDestinationObservation observation() {
  const auto source =
      disk_identity(1U, L"MOCK\\SOURCE\\ONE", "SOURCE01");
  const auto destination =
      disk_identity(2U, L"MOCK\\DESTINATION\\TWO", "DEST0002");
  return ytec::imageformat::WindowsImageDestinationObservation{
      .canonical_final_path = L"D:\\backup\\rescue.tsumugi",
      .partial_path = L"D:\\backup\\rescue.tsumugi.partial",
      .destination_disk = destination,
      .connected_disks = {source, destination},
      .available_bytes = 64ULL * 1024ULL * 1024ULL,
      .physical_disk_extent_count = 1U,
      .file_system =
          ytec::imageformat::WindowsImageDestinationFileSystem::ntfs,
      .parent_is_reparse = false,
      .final_exists = false,
      .partial_exists = false,
      .parent_identity = path_identity(std::byte{0x11}),
      .final_identity = std::nullopt,
      .partial_identity = std::nullopt,
  };
}

ytec::imageformat::WindowsTsumugiRescueStagingRequest request() {
  ytec::imageformat::WindowsTsumugiRescueStagingRequest value{
      .final_path = L"D:\\backup\\rescue.tsumugi",
      .expected_source_disk =
          disk_identity(1U, L"MOCK\\SOURCE\\ONE", "SOURCE01"),
      .source_disk_size = 4096U,
      .logical_sector_size = 512U,
      .required_available_bytes = 16384U,
      .replace_existing = false,
  };
  value.source_model_hash.fill(std::byte{0x21});
  value.source_serial_hash.fill(std::byte{0x22});
  value.source_state_hash.fill(std::byte{0x23});
  return value;
}

struct MockState final {
  ytec::imageformat::WindowsImageDestinationObservation destination{
      observation()};
  std::vector<std::byte> bytes;
  std::wstring staging_path;
  std::vector<std::string> events;
  std::size_t observe_count{};
  std::size_t create_count{};
  std::size_t write_count{};
  std::size_t read_count{};
  std::size_t flush_count{};
  std::size_t seal_count{};
  std::size_t discard_count{};
  std::size_t change_parent_at_observation{
      (std::numeric_limits<std::size_t>::max)()};
  std::size_t remove_source_at_observation{
      (std::numeric_limits<std::size_t>::max)()};
  bool fail_create_before_ownership{};
  bool fail_create_after_ownership{};
  bool fail_seal{};
  bool fail_discard{};
  bool owns{};
  bool sealed{};
  bool flushed{};
};

class MockBackend final
    : public ytec::imageformat::IWindowsTsumugiRescueStagingBackend {
 public:
  explicit MockBackend(std::shared_ptr<MockState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] ytec::clonecore::Result<
      ytec::imageformat::WindowsImageDestinationObservation>
  observe_destination(const std::wstring&) override {
    ++state_->observe_count;
    state_->events.push_back("observe");
    auto current = state_->destination;
    if (state_->observe_count == state_->change_parent_at_observation) {
      current.parent_identity = path_identity(std::byte{0x7F});
    }
    if (state_->observe_count == state_->remove_source_at_observation) {
      current.connected_disks.erase(
          current.connected_disks.begin());
    }
    return ytec::clonecore::Result<
        ytec::imageformat::WindowsImageDestinationObservation>::success(
        std::move(current));
  }

  [[nodiscard]] ytec::clonecore::Status create_new_owned_staging(
      const std::wstring& staging_path,
      const std::uint64_t expected_length) override {
    ++state_->create_count;
    state_->events.push_back("create");
    if (state_->fail_create_before_ownership) {
      return ytec::clonecore::Status::failure(
          mock_error(L"mock create", L"before ownership"));
    }
    state_->owns = true;
    state_->staging_path = staging_path;
    state_->bytes.assign(
        static_cast<std::size_t>(expected_length), std::byte{0});
    if (state_->fail_create_after_ownership) {
      return ytec::clonecore::Status::failure(
          mock_error(L"mock create", L"after ownership"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] bool owns_staging() const noexcept override {
    return state_->owns;
  }

  [[nodiscard]] bool sealed_for_read() const noexcept override {
    return state_->sealed;
  }

  [[nodiscard]] ytec::clonecore::Status write_at(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    ++state_->write_count;
    state_->events.push_back("write");
    if (!state_->owns || state_->sealed ||
        offset > state_->bytes.size() ||
        bytes.size() > state_->bytes.size() -
                           static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Status::failure(
          mock_error(L"mock write", L"invalid state or range"));
    }
    std::copy(
        bytes.begin(),
        bytes.end(),
        state_->bytes.begin() + static_cast<std::size_t>(offset));
    state_->flushed = false;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_at(
      const std::uint64_t offset,
      const std::size_t length) const override {
    ++state_->read_count;
    state_->events.push_back("read");
    if (!state_->owns || offset > state_->bytes.size() ||
        length > state_->bytes.size() - static_cast<std::size_t>(offset)) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          mock_error(L"mock read", L"invalid state or range"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            state_->bytes.begin() + static_cast<std::size_t>(offset),
            state_->bytes.begin() + static_cast<std::size_t>(offset) +
                length));
  }

  [[nodiscard]] ytec::clonecore::Status flush() override {
    ++state_->flush_count;
    state_->events.push_back("flush");
    if (!state_->owns || state_->sealed) {
      return ytec::clonecore::Status::failure(
          mock_error(L"mock flush", L"invalid state"));
    }
    state_->flushed = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status seal_read_only(
      const std::wstring& staging_path,
      const std::uint64_t expected_length) override {
    ++state_->seal_count;
    state_->events.push_back("seal");
    if (state_->fail_seal || !state_->owns || !state_->flushed ||
        state_->sealed || staging_path != state_->staging_path ||
        expected_length != state_->bytes.size()) {
      return ytec::clonecore::Status::failure(
          mock_error(L"mock seal", L"seal failure"));
    }
    state_->sealed = true;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status
  discard_exact_owned_staging() override {
    ++state_->discard_count;
    state_->events.push_back("discard");
    if (state_->fail_discard) {
      return ytec::clonecore::Status::failure(
          mock_error(L"mock discard", L"discard failure"));
    }
    state_->owns = false;
    state_->sealed = false;
    state_->flushed = false;
    state_->bytes.clear();
    return ytec::clonecore::success_status();
  }

 private:
  std::shared_ptr<MockState> state_;
};

struct SessionAndState final {
  std::unique_ptr<ytec::imageformat::ITsumugiRescueStagingSession> session;
  std::shared_ptr<MockState> state;
};

SessionAndState make_session(
    ytec::imageformat::WindowsTsumugiRescueStagingRequest value = request(),
    std::shared_ptr<MockState> state = std::make_shared<MockState>()) {
  auto result = ytec::imageformat::
      make_windows_tsumugi_rescue_staging_session_with_backend(
          value, std::make_unique<MockBackend>(state));
  check(result.has_value(), "A valid rescue staging request should pass");
  return SessionAndState{
      .session = result.take_value(),
      .state = std::move(state),
  };
}

void test_owned_file_lifecycle_is_write_verify_seal_read_discard() {
  auto pair = make_session();
  check(
      pair.state->staging_path ==
              L"D:\\backup\\rescue.tsumugi.rescue-stage.partial" &&
          pair.state->create_count == 1U && pair.state->observe_count == 2U,
      "Factory must derive one adjacent CREATE_NEW staging path");
  check(!pair.session->read(0U, 1U).has_value(),
        "Image-source read must fail before sealing");
  const std::vector<std::byte> bytes{
      std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
  check(pair.session->write_target(512U, bytes).has_value(),
        "Bounded staging write should pass");
  const auto read_back = pair.session->read_back(512U, bytes.size());
  check(read_back.has_value() && read_back.value() == bytes,
        "Raw rescue read-back must match the write");
  check(pair.session->flush_target().has_value(),
        "Raw staging flush should pass");
  check(pair.session->seal_for_image_read().has_value() &&
            pair.session->sealed_for_image_read() &&
            pair.state->observe_count == 4U &&
            pair.state->seal_count == 1U,
        "Seal must bracket read-only reopen with destination observations");
  check(!pair.session->write_target(512U, bytes).has_value() &&
            !pair.session->read_back(512U, bytes.size()).has_value(),
        "Target write/read-back interface must close after sealing");
  const auto image_read = pair.session->read(512U, bytes.size());
  check(image_read.has_value() && image_read.value() == bytes,
        "Sealed image-source read should use the staging file");
  check(pair.session->source_model_hash() == request().source_model_hash &&
            pair.session->source_serial_hash() == request().source_serial_hash &&
            pair.session->source_state_hash() == request().source_state_hash,
        "Sealed source identity hashes must remain request-bound");
  check(pair.session->discard_owned_staging().has_value() &&
            pair.session->discard_owned_staging().has_value() &&
            pair.state->discard_count == 1U && !pair.state->owns,
        "Exact owned cleanup must be successful and idempotent");
  pair.state->destination.partial_exists = true;
  pair.state->destination.partial_identity =
      path_identity(std::byte{0x31}, 2048U);
  check(pair.session->validate_image_destination_before_commit(2048U)
            .has_value() &&
            pair.state->observe_count == 5U,
        "Post-discard validation must accept only the expected image partial on the retained destination");
}

void test_source_destination_and_capacity_fail_before_create() {
  auto same_disk = std::make_shared<MockState>();
  same_disk->destination.destination_disk = request().expected_source_disk;
  auto result = ytec::imageformat::
      make_windows_tsumugi_rescue_staging_session_with_backend(
          request(), std::make_unique<MockBackend>(same_disk));
  check(!result.has_value() && same_disk->create_count == 0U,
        "Source-disk staging must fail before file creation");

  auto no_space = std::make_shared<MockState>();
  no_space->destination.available_bytes =
      request().required_available_bytes - 1U;
  result = ytec::imageformat::
      make_windows_tsumugi_rescue_staging_session_with_backend(
          request(), std::make_unique<MockBackend>(no_space));
  check(!result.has_value() && no_space->create_count == 0U,
        "Combined staging plus image capacity must fail before creation");
}

void test_unknown_or_partially_created_file_cleanup_is_bounded() {
  auto collision = std::make_shared<MockState>();
  collision->fail_create_before_ownership = true;
  auto result = ytec::imageformat::
      make_windows_tsumugi_rescue_staging_session_with_backend(
          request(), std::make_unique<MockBackend>(collision));
  check(!result.has_value() && collision->create_count == 1U &&
            collision->discard_count == 0U,
        "A CREATE_NEW collision must never trigger unknown-path cleanup");

  auto partial = std::make_shared<MockState>();
  partial->fail_create_after_ownership = true;
  result = ytec::imageformat::
      make_windows_tsumugi_rescue_staging_session_with_backend(
          request(), std::make_unique<MockBackend>(partial));
  check(!result.has_value() && partial->create_count == 1U &&
            partial->discard_count == 1U && !partial->owns,
        "A post-ownership creation failure must discard only the owned object");
}

void test_destination_change_after_create_discards_owned_file() {
  auto state = std::make_shared<MockState>();
  state->change_parent_at_observation = 2U;
  const auto result = ytec::imageformat::
      make_windows_tsumugi_rescue_staging_session_with_backend(
          request(), std::make_unique<MockBackend>(state));
  check(!result.has_value() && state->create_count == 1U &&
            state->discard_count == 1U && !state->owns,
        "Parent replacement after CREATE_NEW must fail and clean exact ownership");
}

void test_seal_requires_flush_and_cleans_after_identity_change() {
  auto pair = make_session();
  check(!pair.session->seal_for_image_read().has_value() &&
            pair.state->seal_count == 0U,
        "Unflushed staging must not reach backend sealing");
  check(pair.session->flush_target().has_value(), "Flush should pass");
  pair.state->change_parent_at_observation = 4U;
  check(!pair.session->seal_for_image_read().has_value() &&
            !pair.session->sealed_for_image_read() &&
            pair.state->seal_count == 1U && pair.state->sealed,
        "Post-reopen parent change must withhold the sealed source contract");
  check(pair.session->discard_owned_staging().has_value() &&
            !pair.state->owns,
        "A failed post-seal observation must retain exact cleanup ownership");
}

void test_source_disappearance_after_rescue_does_not_lose_staged_data() {
  auto state = std::make_shared<MockState>();
  auto pair = make_session(request(), state);
  const std::vector<std::byte> bytes(512U, std::byte{0x5A});
  check(pair.session->write_target(0U, bytes).has_value() &&
            pair.session->flush_target().has_value(),
        "Synthetic rescue write should pass");
  state->remove_source_at_observation = 3U;
  check(pair.session->seal_for_image_read().has_value() &&
            pair.session->read(0U, bytes.size()).has_value(),
        "A failed source may disappear after complete rescue without invalidating exact staging");
  check(pair.session->discard_owned_staging().has_value(),
        "Sealed staging should remain exactly discardable");
}

void test_seal_and_discard_failures_never_publish_or_drop_ownership() {
  auto seal_state = std::make_shared<MockState>();
  seal_state->fail_seal = true;
  auto seal_pair = make_session(request(), seal_state);
  check(seal_pair.session->flush_target().has_value() &&
            !seal_pair.session->seal_for_image_read().has_value() &&
            !seal_pair.session->sealed_for_image_read() && seal_state->owns,
        "Injected seal failure must retain owned writable staging for cleanup");
  check(seal_pair.session->discard_owned_staging().has_value(),
        "Failed seal staging should remain cleanup-capable");

  auto discard_state = std::make_shared<MockState>();
  discard_state->fail_discard = true;
  auto discard_pair = make_session(request(), discard_state);
  check(!discard_pair.session->discard_owned_staging().has_value() &&
            discard_state->owns && discard_state->discard_count == 1U,
        "Injected exact cleanup failure must be reported without dropping ownership");
  discard_state->fail_discard = false;
  check(discard_pair.session->discard_owned_staging().has_value() &&
            !discard_state->owns && discard_state->discard_count == 2U,
        "Cleanup may be retried only against the retained owned object");
}

void test_invalid_ranges_and_hashes_fail_closed() {
  auto pair = make_session();
  const std::vector<std::byte> one{std::byte{0x01}};
  check(!pair.session->write_target(4096U, one).has_value() &&
            !pair.session->read_back(0U, 0U).has_value(),
        "Out-of-bounds and empty I/O must fail before backend access");
  check(pair.session->discard_owned_staging().has_value(),
        "Range failure should retain cleanup ownership");

  auto invalid = request();
  invalid.source_state_hash.fill(std::byte{0});
  auto state = std::make_shared<MockState>();
  const auto result = ytec::imageformat::
      make_windows_tsumugi_rescue_staging_session_with_backend(
          invalid, std::make_unique<MockBackend>(state));
  check(!result.has_value() && state->observe_count == 0U &&
            state->create_count == 0U,
        "Zero source identity hash must fail before destination observation");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"owned_file_lifecycle_is_write_verify_seal_read_discard",
       test_owned_file_lifecycle_is_write_verify_seal_read_discard},
      {"source_destination_and_capacity_fail_before_create",
       test_source_destination_and_capacity_fail_before_create},
      {"unknown_or_partially_created_file_cleanup_is_bounded",
       test_unknown_or_partially_created_file_cleanup_is_bounded},
      {"destination_change_after_create_discards_owned_file",
       test_destination_change_after_create_discards_owned_file},
      {"seal_requires_flush_and_cleans_after_identity_change",
       test_seal_requires_flush_and_cleans_after_identity_change},
      {"source_disappearance_after_rescue_does_not_lose_staged_data",
       test_source_disappearance_after_rescue_does_not_lose_staged_data},
      {"seal_and_discard_failures_never_publish_or_drop_ownership",
       test_seal_and_discard_failures_never_publish_or_drop_ownership},
      {"invalid_ranges_and_hashes_fail_closed",
       test_invalid_ranges_and_hashes_fail_closed},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.what() << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name
                << ": unexpected exception: " << exception.what() << '\n';
    }
  }
  std::cout << (tests.size() - static_cast<std::size_t>(failures)) << "/"
            << tests.size() << " tests passed\n";
  return failures == 0 ? 0 : 1;
}

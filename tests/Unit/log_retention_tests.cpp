#include "ytec/windowsapp/log_retention.h"

#include "ytec/clonecore/error.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kDay = 24U * 60U * 60U * 10'000'000U;
constexpr std::uint64_t kMiB = 1024U * 1024U;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::windowsapp::ProductLogObservation observation(
    std::wstring name,
    const std::uint64_t size,
    const std::uint64_t written,
    const std::byte id) {
  ytec::windowsapp::ProductLogObservation result{
      .file_name = std::move(name),
      .size_bytes = size,
      .last_write_utc_100ns = written,
      .volume_serial = 7U,
      .regular_file = true,
      .reparse_point = false,
      .has_utf8_bom = true,
  };
  result.file_id[0] = id;
  return result;
}

std::wstring normal_name(
    const wchar_t suffix = L'1',
    const bool legacy = false) {
  return std::wstring(
             legacy ? L"TsumugiDrive-" : L"TsumugiDrive-normal-") +
         L"20260808-010203-004-123" + suffix + L".log";
}

std::wstring failure_name(const wchar_t suffix = L'1') {
  return L"TsumugiDrive-failed-20260808-010203-004-123" +
         std::wstring(1U, suffix) + L".log";
}

void strict_file_name_classification() {
  using ytec::windowsapp::ProductLogClass;
  using ytec::windowsapp::classify_product_log_file_name;

  check(classify_product_log_file_name(normal_name()) ==
            ProductLogClass::normal,
        "Tagged normal product logs should classify");
  check(classify_product_log_file_name(normal_name(L'2', true)) ==
            ProductLogClass::normal,
        "Existing untagged product logs should remain classified as normal");
  check(classify_product_log_file_name(failure_name()) ==
            ProductLogClass::failure,
        "Tagged failure product logs should classify");
  check(classify_product_log_file_name(
            L"TsumugiDrive-normal-20260808-010203-004-1234-2.log") ==
            ProductLogClass::normal,
        "The bounded collision suffix should classify");

  const std::vector<std::wstring> rejected{
      L"notes.log",
      L"TsumugiDrive-normal-20260808-010203-004-0.log",
      L"TsumugiDrive-normal-20261308-010203-004-1234.log",
      L"TsumugiDrive-normal-20260230-010203-004-1234.log",
      L"TsumugiDrive-normal-20260808-250203-004-1234.log",
      L"TsumugiDrive-normal-20260808-010203-004-1234-0.log",
      L"TsumugiDrive-normal-20260808-010203-004-1234-22.log",
      L"TsumugiDrive-normal-20260808-010203-004-1234.txt",
      L"subdir\\TsumugiDrive-normal-20260808-010203-004-1234.log",
      L"TSUMUGIDRIVE-normal-20260808-010203-004-1234.log",
  };
  for (const auto& name : rejected) {
    check(!classify_product_log_file_name(name).has_value(),
          "Near-match or path-like names must never classify as owned logs");
  }
}

void age_limits_are_inclusive_and_failure_logs_are_separate() {
  using ytec::windowsapp::ProductLogDeletionReason;
  const std::uint64_t now = 200U * kDay;
  auto normal_expired = observation(
      normal_name(L'1'), 10U, now - 30U * kDay, std::byte{1});
  auto normal_kept = observation(
      normal_name(L'2'), 20U, now - 30U * kDay + 1U, std::byte{2});
  auto failure_expired = observation(
      failure_name(L'1'), 30U, now - 90U * kDay, std::byte{3});
  auto failure_kept = observation(
      failure_name(L'2'), 40U, now - 90U * kDay + 1U, std::byte{4});
  auto future = observation(
      normal_name(L'3'), 50U, now + kDay, std::byte{5});

  auto unknown = observation(L"user.log", 99U, 0U, std::byte{6});
  auto reparse = observation(
      normal_name(L'4'), 99U, 0U, std::byte{7});
  reparse.reparse_point = true;
  reparse.regular_file = false;
  auto no_bom = observation(
      normal_name(L'5'), 99U, 0U, std::byte{8});
  no_bom.has_utf8_bom = false;

  const auto plan = ytec::windowsapp::plan_product_log_retention(
      {normal_expired,
       normal_kept,
       failure_expired,
       failure_kept,
       future,
       unknown,
       reparse,
       no_bom},
      now);
  check(plan.has_value(), "A bounded age plan should build");
  check(plan.value().deletions.size() == 2U,
        "Only exact 30-day normal and 90-day failure logs should expire");
  check(std::any_of(
            plan.value().deletions.begin(),
            plan.value().deletions.end(),
            [&](const auto& item) {
              return item.observed.file_name == normal_expired.file_name &&
                     item.reason == ProductLogDeletionReason::normal_age;
            }),
        "The normal 30-day boundary should be inclusive");
  check(std::any_of(
            plan.value().deletions.begin(),
            plan.value().deletions.end(),
            [&](const auto& item) {
              return item.observed.file_name == failure_expired.file_name &&
                     item.reason == ProductLogDeletionReason::failure_age;
            }),
        "The failure 90-day boundary should be inclusive");
  check(plan.value().retained_normal_count == 2U &&
            plan.value().retained_failure_count == 1U &&
            plan.value().ignored_count == 3U,
        "Future, unowned, reparse, and non-product contents must be bounded");
}

void normal_budget_deletes_oldest_and_excludes_failure_logs() {
  using ytec::windowsapp::ProductLogDeletionReason;
  const std::uint64_t now = 50U * kDay;
  auto oldest = observation(
      normal_name(L'3'), 100U * kMiB, now - 20U * kDay, std::byte{1});
  auto middle = observation(
      normal_name(L'2'), 100U * kMiB, now - 10U * kDay, std::byte{2});
  auto newest = observation(
      normal_name(L'1'), 100U * kMiB, now - kDay, std::byte{3});
  auto failure = observation(
      failure_name(), 800U * kMiB, now - 40U * kDay, std::byte{4});

  const auto plan = ytec::windowsapp::plan_product_log_retention(
      {newest, failure, middle, oldest}, now);
  check(plan.has_value(), "A budget plan should build");
  check(plan.value().deletions.size() == 1U &&
            plan.value().deletions.front().observed.file_name ==
                oldest.file_name &&
            plan.value().deletions.front().reason ==
                ProductLogDeletionReason::normal_budget,
        "The oldest normal log must be deleted first to meet 200 MiB");
  check(plan.value().retained_normal_bytes == 200U * kMiB &&
            plan.value().retained_normal_count == 2U &&
            plan.value().retained_failure_count == 1U,
        "Failure logs must not consume the normal-log 200 MiB budget");
}

void oversized_totals_never_wrap_the_budget() {
  const std::uint64_t now = 50U * kDay;
  auto oldest = observation(
      normal_name(L'1'),
      (std::numeric_limits<std::uint64_t>::max)(),
      now - 2U * kDay,
      std::byte{1});
  auto newest = observation(
      normal_name(L'2'), 1U, now - kDay, std::byte{2});
  const auto plan = ytec::windowsapp::plan_product_log_retention(
      {newest, oldest}, now);
  check(plan.has_value() && plan.value().deletions.size() == 1U &&
            plan.value().deletions.front().observed.file_name ==
                oldest.file_name &&
            plan.value().retained_normal_bytes == 1U &&
            plan.value().retained_normal_count == 1U,
        "An overflowing total must delete oldest logs without wrapping");
}

void duplicate_owned_names_fail_closed() {
  const std::uint64_t now = 100U * kDay;
  auto first = observation(
      normal_name(), 1U, now - 31U * kDay, std::byte{1});
  auto second = first;
  second.file_id[0] = std::byte{2};
  const auto plan = ytec::windowsapp::plan_product_log_retention(
      {first, second}, now);
  check(!plan.has_value() &&
            plan.error().code == ytec::clonecore::ErrorCode::invalid_data,
        "Duplicate owned names must stop before deletion");
}

class SyntheticRetentionPlatform final
    : public ytec::windowsapp::IProductLogRetentionPlatform {
 public:
  std::vector<ytec::windowsapp::ProductLogObservation> observations;
  std::vector<std::wstring> deleted;
  std::optional<std::size_t> fail_delete_call;

  ytec::clonecore::Result<
      std::vector<ytec::windowsapp::ProductLogObservation>>
  enumerate_owned_candidates(const std::wstring& directory) override {
    check(directory == L"C:\\Portable\\data\\logs",
          "The executor must retain the exact supplied log directory");
    return ytec::clonecore::Result<
        std::vector<ytec::windowsapp::ProductLogObservation>>::success(
        observations);
  }

  ytec::clonecore::Status delete_if_unchanged(
      const std::wstring& directory,
      const ytec::windowsapp::ProductLogObservation& expected) override {
    check(directory == L"C:\\Portable\\data\\logs",
          "Deletion must stay inside the exact log directory");
    if (fail_delete_call.has_value() &&
        deleted.size() == fail_delete_call.value()) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::identity_mismatch,
          .native_code = ERROR_FILE_INVALID,
          .operation = L"synthetic re-identification",
          .message = L"synthetic failure",
      });
    }
    deleted.push_back(expected.file_name);
    return ytec::clonecore::success_status();
  }
};

class SyntheticCompletionPlatform final
    : public ytec::windowsapp::IProductLogCompletionPlatform {
 public:
  bool succeed{true};
  std::size_t calls{};
  std::wstring directory;
  std::wstring failed;
  std::wstring normal;

  ytec::clonecore::Status promote_failure_no_replace(
      const std::wstring& requested_directory,
      const std::wstring& failed_file_name,
      const std::wstring& normal_file_name) override {
    ++calls;
    directory = requested_directory;
    failed = failed_file_name;
    normal = normal_file_name;
    if (!succeed) {
      return ytec::clonecore::Status::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_FILE_EXISTS,
          .operation = L"synthetic non-overwrite rename",
          .message = L"synthetic failure",
      });
    }
    return ytec::clonecore::success_status();
  }
};

void failed_first_completion_requires_every_clean_condition() {
  using ytec::windowsapp::ProductLogCompletionDisposition;
  const std::wstring failed = failure_name(L'7');
  const std::wstring expected_normal = normal_name(L'7');
  const auto clean = ytec::windowsapp::plan_product_log_completion(
      failed, true, false, false);
  check(clean.has_value() &&
            clean.value().disposition ==
                ProductLogCompletionDisposition::promote_to_normal &&
            clean.value().normal_file_name == expected_normal,
        "Only an error-free clean close should plan normal promotion");

  for (const auto& conditions :
       std::vector<std::array<bool, 3U>>{
           {false, false, false},
           {true, true, false},
           {true, false, true},
           {false, true, true}}) {
    const auto kept = ytec::windowsapp::plan_product_log_completion(
        failed, conditions[0], conditions[1], conditions[2]);
    check(kept.has_value() &&
              kept.value().disposition ==
                  ProductLogCompletionDisposition::keep_failure &&
              kept.value().normal_file_name.empty(),
          "Crash-like, error, or active-operation close must remain failed");
  }
  check(!ytec::windowsapp::plan_product_log_completion(
             normal_name(), true, false, false)
             .has_value() &&
            !ytec::windowsapp::plan_product_log_completion(
                 L"user-failed.log", true, false, false)
                 .has_value(),
        "Completion must accept only a strict failed-first owned name");
}

void completion_executor_never_touches_kept_failures() {
  const std::wstring failed = failure_name(L'8');
  for (const auto& conditions :
       std::vector<std::array<bool, 3U>>{
           {false, false, false},
           {true, true, false},
           {true, false, true}}) {
    SyntheticCompletionPlatform platform;
    const auto result = ytec::windowsapp::complete_product_log_session(
        platform,
        L"C:\\Portable\\data\\logs",
        failed,
        conditions[0],
        conditions[1],
        conditions[2]);
    check(result.has_value() && !result.value().promoted &&
              platform.calls == 0U,
          "A failure-class decision must perform no rename attempt");
  }

  SyntheticCompletionPlatform success;
  const auto promoted = ytec::windowsapp::complete_product_log_session(
      success,
      L"C:\\Portable\\data\\logs",
      failed,
      true,
      false,
      false);
  check(promoted.has_value() && promoted.value().promoted &&
            success.calls == 1U && success.failed == failed &&
            success.normal == normal_name(L'8'),
        "A clean session should request exactly one non-overwriting promotion");

  SyntheticCompletionPlatform collision;
  collision.succeed = false;
  const auto stopped = ytec::windowsapp::complete_product_log_session(
      collision,
      L"C:\\Portable\\data\\logs",
      failed,
      true,
      false,
      false);
  check(!stopped.has_value() && collision.calls == 1U,
        "A rename failure must propagate instead of claiming normal");
}

class ExecutableAdjacentProductLogTree final {
 public:
  ExecutableAdjacentProductLogTree() {
    std::vector<wchar_t> executable(32U * 1024U, L'\0');
    const DWORD copied = GetModuleFileNameW(
        nullptr,
        executable.data(),
        static_cast<DWORD>(executable.size()));
    check(copied != 0U && copied < executable.size(),
          "The complete test executable path is required");
    const std::wstring executable_path(executable.data(), copied);
    const std::size_t separator = executable_path.find_last_of(L"\\/");
    check(separator != std::wstring::npos,
          "The test executable must have an adjacent directory");
    root_ = executable_path.substr(0U, separator);
    data_ = root_ + L"\\data";
    logs_ = data_ + L"\\logs";
    created_data_ = CreateDirectoryW(data_.c_str(), nullptr) != FALSE;
    check(created_data_ || GetLastError() == ERROR_ALREADY_EXISTS,
          "The EXE-adjacent data directory must be available");
    created_logs_ = CreateDirectoryW(logs_.c_str(), nullptr) != FALSE;
    check(created_logs_ || GetLastError() == ERROR_ALREADY_EXISTS,
          "The EXE-adjacent data/logs directory must be available");
  }

  ~ExecutableAdjacentProductLogTree() {
    for (auto file = files_.rbegin(); file != files_.rend(); ++file) {
      static_cast<void>(DeleteFileW(file->c_str()));
    }
    if (created_logs_) {
      static_cast<void>(RemoveDirectoryW(logs_.c_str()));
    }
    if (created_data_) {
      static_cast<void>(RemoveDirectoryW(data_.c_str()));
    }
  }

  ExecutableAdjacentProductLogTree(
      const ExecutableAdjacentProductLogTree&) = delete;
  ExecutableAdjacentProductLogTree& operator=(
      const ExecutableAdjacentProductLogTree&) = delete;

  [[nodiscard]] const std::wstring& data() const noexcept { return data_; }

  [[nodiscard]] std::wstring path(const std::wstring& name) {
    std::wstring result = logs_ + L"\\" + name;
    files_.push_back(result);
    return result;
  }

  void write_owned_log(
      const std::wstring& path,
      const std::byte marker = std::byte{0x31}) const {
    const std::array<std::byte, 4U> bytes{
        std::byte{0xEF}, std::byte{0xBB}, std::byte{0xBF}, marker};
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    check(file != INVALID_HANDLE_VALUE,
          "A new private synthetic log must be created");
    DWORD written = 0U;
    const BOOL succeeded = WriteFile(
        file,
        bytes.data(),
        static_cast<DWORD>(bytes.size()),
        &written,
        nullptr);
    const BOOL closed = CloseHandle(file);
    check(succeeded != FALSE && written == bytes.size() && closed != FALSE,
          "The complete synthetic UTF-8 log must be written and closed");
  }

 private:
  std::wstring root_;
  std::wstring data_;
  std::wstring logs_;
  std::vector<std::wstring> files_;
  bool created_data_{};
  bool created_logs_{};
};

std::wstring runtime_log_name(
    const bool failure,
    const wchar_t collision_suffix) {
  return std::wstring(
             failure ? L"TsumugiDrive-failed-"
                     : L"TsumugiDrive-normal-") +
      L"20260808-010203-004-" +
      std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::wstring(1U, collision_suffix) + L".log";
}

bool file_exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void windows_completion_renames_by_handle_without_overwrite() {
  ExecutableAdjacentProductLogTree temporary;
  const std::wstring failed_name = runtime_log_name(true, L'4');
  const std::wstring normal =
      temporary.path(runtime_log_name(false, L'4'));
  const std::wstring failed = temporary.path(failed_name);
  temporary.write_owned_log(failed);

  const auto completed =
      ytec::windowsapp::complete_windows_product_log_session(
          temporary.data(), failed, true, false, false);
  check(completed.has_value() && completed.value().promoted &&
            !file_exists(failed) && file_exists(normal),
        "A clean single-link UTF-8 failed log must become normal by handle");

  const std::wstring collision_failed =
      temporary.path(runtime_log_name(true, L'5'));
  const std::wstring collision_normal =
      temporary.path(runtime_log_name(false, L'5'));
  temporary.write_owned_log(collision_failed, std::byte{0x41});
  temporary.write_owned_log(collision_normal, std::byte{0x42});
  const auto collision =
      ytec::windowsapp::complete_windows_product_log_session(
          temporary.data(), collision_failed, true, false, false);
  check(!collision.has_value() && file_exists(collision_failed) &&
            file_exists(collision_normal),
        "A normal-name collision must preserve both files without overwrite");
}

void windows_completion_rejects_hard_links_and_open_writers() {
  ExecutableAdjacentProductLogTree temporary;
  const std::wstring linked_failed =
      temporary.path(runtime_log_name(true, L'6'));
  const std::wstring linked_normal =
      temporary.path(runtime_log_name(false, L'6'));
  const std::wstring hard_link = temporary.path(L"synthetic-hard-link.tmp");
  temporary.write_owned_log(linked_failed);
  check(CreateHardLinkW(
            hard_link.c_str(), linked_failed.c_str(), nullptr) != FALSE,
        "The synthetic hard link must be created");
  const auto linked =
      ytec::windowsapp::complete_windows_product_log_session(
          temporary.data(), linked_failed, true, false, false);
  check(!linked.has_value() && file_exists(linked_failed) &&
            !file_exists(linked_normal),
        "A multiply-linked failed log must remain failed");

  const std::wstring open_failed =
      temporary.path(runtime_log_name(true, L'9'));
  const std::wstring open_normal =
      temporary.path(runtime_log_name(false, L'9'));
  temporary.write_owned_log(open_failed);
  const HANDLE writer = CreateFileW(
      open_failed.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  check(writer != INVALID_HANDLE_VALUE,
        "The synthetic conflicting handle must be opened");
  const auto open =
      ytec::windowsapp::complete_windows_product_log_session(
          temporary.data(), open_failed, true, false, false);
  static_cast<void>(CloseHandle(writer));
  check(!open.has_value() && file_exists(open_failed) &&
            !file_exists(open_normal),
        "An open handle that prevents DELETE access must remain failed");
}

void windows_completion_rejects_non_executable_adjacent_data() {
  const std::wstring failed = runtime_log_name(true, L'3');
  const auto unrelated =
      ytec::windowsapp::complete_windows_product_log_session(
          L"C:\\UnrelatedPortable\\data",
          L"C:\\UnrelatedPortable\\data\\logs\\" + failed,
          true,
          false,
          false);
  check(!unrelated.has_value(),
        "The production entry must reject arbitrary folders named data");
}

void executor_uses_deterministic_order_and_stops_on_change() {
  const std::uint64_t now = 200U * kDay;
  SyntheticRetentionPlatform platform;
  const auto lexical_second = observation(
      normal_name(L'2'), 10U, now - 40U * kDay, std::byte{2});
  const auto lexical_first = observation(
      normal_name(L'1'), 20U, now - 40U * kDay, std::byte{1});
  platform.observations = {lexical_second, lexical_first};

  const auto report = ytec::windowsapp::enforce_product_log_retention(
      platform, L"C:\\Portable\\data\\logs", now);
  check(report.has_value() && report.value().deleted_count == 2U &&
            report.value().deleted_bytes == 30U &&
            platform.deleted ==
                std::vector<std::wstring>{
                    lexical_first.file_name, lexical_second.file_name},
        "Deletion must follow deterministic oldest/name/identity order");

  SyntheticRetentionPlatform changed;
  changed.observations = {lexical_second, lexical_first};
  changed.fail_delete_call = 1U;
  const auto stopped = ytec::windowsapp::enforce_product_log_retention(
      changed, L"C:\\Portable\\data\\logs", now);
  check(!stopped.has_value() && changed.deleted.size() == 1U,
        "An identity change must stop all later deletion attempts");
}

}  // namespace

int main() {
  try {
    strict_file_name_classification();
    age_limits_are_inclusive_and_failure_logs_are_separate();
    normal_budget_deletes_oldest_and_excludes_failure_logs();
    oversized_totals_never_wrap_the_budget();
    duplicate_owned_names_fail_closed();
    executor_uses_deterministic_order_and_stops_on_change();
    failed_first_completion_requires_every_clean_condition();
    completion_executor_never_touches_kept_failures();
    windows_completion_renames_by_handle_without_overwrite();
    windows_completion_rejects_hard_links_and_open_writers();
    windows_completion_rejects_non_executable_adjacent_data();
    std::cout << "log retention tests: PASS\n";
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << "log retention tests: FAIL: " << exception.what() << '\n';
    return 1;
  }
}

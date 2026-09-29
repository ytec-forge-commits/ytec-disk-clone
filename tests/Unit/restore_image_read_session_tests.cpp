#include "ytec/imageformat/restore_image_inspection.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
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

class TemporaryDcimg final {
 public:
  TemporaryDcimg() {
    const DWORD required = GetTempPathW(0, nullptr);
    check(required != 0, "GetTempPathW size query should pass");
    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD written =
        GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    check(
        written != 0 &&
            static_cast<std::size_t>(written) < buffer.size(),
        "GetTempPathW should return a bounded path");

    directory_ = std::wstring(buffer.data(), written) +
        L"ytec-tsumugi-restore-session-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    check(
        CreateDirectoryW(directory_.c_str(), nullptr) != FALSE,
        "Temporary directory creation should pass");

    file_ = directory_ + L"\\source.dcimg";
    directory_candidate_ = directory_ + L"\\folder.dcimg";
    check(
        CreateDirectoryW(directory_candidate_.c_str(), nullptr) != FALSE,
        "Directory candidate creation should pass");

    HANDLE handle = CreateFileW(
        file_.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    check(handle != INVALID_HANDLE_VALUE, "Temporary dcimg creation should pass");

    bytes_.resize(8192);
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
      bytes_[index] =
          static_cast<std::byte>(index & static_cast<std::size_t>(0xFF));
    }
    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(
        handle,
        bytes_.data(),
        static_cast<DWORD>(bytes_.size()),
        &bytes_written,
        nullptr);
    CloseHandle(handle);
    check(
        write_ok != FALSE && bytes_written == bytes_.size(),
        "Temporary dcimg contents should be written exactly");
  }

  ~TemporaryDcimg() {
    DeleteFileW(file_.c_str());
    RemoveDirectoryW(directory_candidate_.c_str());
    RemoveDirectoryW(directory_.c_str());
  }

  TemporaryDcimg(const TemporaryDcimg&) = delete;
  TemporaryDcimg& operator=(const TemporaryDcimg&) = delete;

  [[nodiscard]] const std::wstring& file() const noexcept {
    return file_;
  }

  [[nodiscard]] const std::wstring& directory_candidate()
      const noexcept {
    return directory_candidate_;
  }

  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
    return bytes_;
  }

 private:
  std::wstring directory_;
  std::wstring file_;
  std::wstring directory_candidate_;
  std::vector<std::byte> bytes_;
};

void test_session_reads_exact_ranges_and_rejects_bounds() {
  TemporaryDcimg temporary;
  auto opened =
      ytec::imageformat::open_restore_image_file_read_session_v1(
          temporary.file());
  check(opened.has_value(), "A regular local .dcimg should open");
  check(
      opened.value()->image_length() == temporary.bytes().size(),
      "Session should expose the handle-derived file length");
  check(
      !opened.value()->canonical_path().empty(),
      "Session should retain a canonical path");

  const auto read = opened.value()->read_at(123, 1024);
  check(read.has_value(), "A bounded session read should pass");
  check(
      std::equal(
          read.value().begin(),
          read.value().end(),
          temporary.bytes().begin() + 123),
      "Session read bytes should match the file");

  const auto end_read = opened.value()->read_at(
      temporary.bytes().size(), 0);
  check(end_read.has_value() && end_read.value().empty(),
        "A zero-length EOF read should pass");
  check(
      !opened.value()
           ->read_at(temporary.bytes().size(), 1)
           .has_value(),
      "A read beyond EOF must fail closed");
}

void test_session_denies_write_and_delete_sharing_until_closed() {
  TemporaryDcimg temporary;
  auto opened =
      ytec::imageformat::open_restore_image_file_read_session_v1(
          temporary.file());
  check(opened.has_value(), "A regular local .dcimg should open");

  HANDLE writer = CreateFileW(
      temporary.file().c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  const DWORD sharing_error = GetLastError();
  check(
      writer == INVALID_HANDLE_VALUE &&
          sharing_error == ERROR_SHARING_VIOLATION,
      "The live read session must deny a writer");

  opened.take_value().reset();
  writer = CreateFileW(
      temporary.file().c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  check(
      writer != INVALID_HANDLE_VALUE,
      "Closing the session should release the sharing lock");
  if (writer != INVALID_HANDLE_VALUE) {
    CloseHandle(writer);
  }
}

void test_session_rejects_non_dcimg_and_directory() {
  TemporaryDcimg temporary;
  const auto wrong_extension =
      ytec::imageformat::open_restore_image_file_read_session_v1(
          temporary.file() + L".bin");
  check(
      !wrong_extension.has_value(),
      "A non-.dcimg path must fail before opening");

  const auto directory =
      ytec::imageformat::open_restore_image_file_read_session_v1(
          temporary.directory_candidate());
  check(
      !directory.has_value(),
      "A directory named .dcimg must not be accepted as a file");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"session_reads_exact_ranges_and_rejects_bounds",
       test_session_reads_exact_ranges_and_rejects_bounds},
      {"session_denies_write_and_delete_sharing_until_closed",
       test_session_denies_write_and_delete_sharing_until_closed},
      {"session_rejects_non_dcimg_and_directory",
       test_session_rejects_non_dcimg_and_directory},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

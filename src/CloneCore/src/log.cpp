#include "ytec/clonecore/log.h"

#include "ytec/clonecore/log_privacy.h"

#include "ytec/clonecore/unique_handle.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ytec::clonecore {
namespace {

constexpr std::size_t kMaximumLogMessageCharacters = 8192U;

struct FileLogState final {
  UniqueHandle handle;
  bool debug_enabled{};
  std::mutex mutex;
};

std::wstring sanitize_message(
    const std::wstring_view message,
    const std::size_t maximum_characters =
        kMaximumLogMessageCharacters) {
  const std::size_t length =
      (std::min)(message.size(), maximum_characters);
  std::wstring sanitized;
  sanitized.reserve(length);
  bool previous_space = false;
  for (std::size_t index = 0; index < length; ++index) {
    wchar_t character = message[index];
    if (character == L'\r' || character == L'\n' ||
        character == L'\0' ||
        (character < L' ' && character != L'\t')) {
      character = L' ';
    }
    if (character == L'\t') {
      character = L' ';
    }
    if (character == L' ' && previous_space) {
      continue;
    }
    previous_space = character == L' ';
    sanitized.push_back(character);
  }
  if (message.size() > length && maximum_characters >= 3U) {
    if (sanitized.size() > maximum_characters - 3U) {
      sanitized.resize(maximum_characters - 3U);
    }
    sanitized += L"...";
  }
  return sanitized;
}

std::string to_utf8(const std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  if (text.size() >
      static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return {};
  }
  const int required = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      text.data(),
      static_cast<int>(text.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
  if (required <= 0) {
    return {};
  }
  std::string encoded(static_cast<std::size_t>(required), '\0');
  const int written = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      text.data(),
      static_cast<int>(text.size()),
      encoded.data(),
      required,
      nullptr,
      nullptr);
  return written == required ? encoded : std::string{};
}

std::string make_log_line(const LogRecord& record) {
  SYSTEMTIME now{};
  GetLocalTime(&now);
  std::array<wchar_t, 64U> prefix{};
  const int prefix_length = swprintf_s(
      prefix.data(),
      prefix.size(),
      L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%ls] ",
      static_cast<unsigned int>(now.wYear),
      static_cast<unsigned int>(now.wMonth),
      static_cast<unsigned int>(now.wDay),
      static_cast<unsigned int>(now.wHour),
      static_cast<unsigned int>(now.wMinute),
      static_cast<unsigned int>(now.wSecond),
      static_cast<unsigned int>(now.wMilliseconds),
      log_level_name(record.level).data());
  if (prefix_length <= 0) {
    return {};
  }
  const std::wstring message = sanitize_message(record.message);
  std::wstring line(prefix.data(), static_cast<std::size_t>(prefix_length));
  line += message;
  line += L"\r\n";
  return to_utf8(line);
}

bool write_all(const HANDLE handle, const std::string_view bytes) noexcept {
  std::size_t cursor = 0;
  while (cursor < bytes.size()) {
    const std::size_t remaining = bytes.size() - cursor;
    const DWORD requested = static_cast<DWORD>(
        (std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written = 0;
    if (WriteFile(
            handle,
            bytes.data() + cursor,
            requested,
            &written,
            nullptr) == FALSE ||
        written == 0U) {
      return false;
    }
    cursor += written;
  }
  return true;
}

}  // namespace

struct BoundedRamLogRouter::State final {
  explicit State(BoundedRamLogLimits requested_limits)
      : limits(std::move(requested_limits)) {
    if (limits.maximum_total_message_characters != 0U) {
      limits.maximum_message_characters =
          (std::min)(
              limits.maximum_message_characters,
              limits.maximum_total_message_characters);
    }
  }

  void write(const LogRecord& record) noexcept {
    try {
      LogRecord sanitized{
          .level = record.level,
          .message = sanitize_message(
              record.message,
              limits.maximum_message_characters),
      };
      const std::lock_guard lock(mutex);
      if (limits.maximum_records == 0U ||
          limits.maximum_total_message_characters == 0U) {
        ++dropped_record_count;
      } else {
        while (!records.empty() &&
               (records.size() >= limits.maximum_records ||
                stored_message_characters + sanitized.message.size() >
                    limits.maximum_total_message_characters)) {
          stored_message_characters -= records.front().message.size();
          records.pop_front();
          ++dropped_record_count;
        }
        if (sanitized.message.size() <=
            limits.maximum_total_message_characters) {
          records.push_back(sanitized);
          stored_message_characters += sanitized.message.size();
        } else {
          ++dropped_record_count;
        }
      }
      if (persistent_sink.has_value()) {
        persistent_sink->write(sanitized.level, sanitized.message);
      }
    } catch (...) {
      // RAM logging must never terminate or unwind an application operation.
    }
  }

  BoundedRamLogLimits limits;
  mutable std::mutex mutex;
  std::deque<LogRecord> records;
  std::size_t dropped_record_count{};
  std::size_t stored_message_characters{};
  std::optional<Logger> persistent_sink;
  bool permanently_ram_only{};
};

Logger::Logger(Sink sink) : sink_(std::move(sink)) {}

void Logger::write(
    const LogLevel level,
    const std::wstring_view message) const noexcept {
  try {
    if (sink_) {
      const std::wstring sanitized = sanitize_main_log_message(message);
      sink_(LogRecord{
          .level = level,
          .message = sanitized,
      });
    }
  } catch (...) {
    // Logging must never terminate or unwind an application operation.
  }
}

void Logger::info(const std::wstring_view message) const noexcept {
  write(LogLevel::info, message);
}

void Logger::warning(const std::wstring_view message) const noexcept {
  write(LogLevel::warning, message);
}

void Logger::error(const std::wstring_view message) const noexcept {
  write(LogLevel::error, message);
}

void Logger::debug(const std::wstring_view message) const noexcept {
  write(LogLevel::debug, message);
}

BoundedRamLogRouter::BoundedRamLogRouter(BoundedRamLogLimits limits)
    : state_(std::make_shared<State>(std::move(limits))) {}

Logger BoundedRamLogRouter::logger() const {
  return Logger([state = state_](const LogRecord& record) noexcept {
    state->write(record);
  });
}

bool BoundedRamLogRouter::attach_persistent_sink(Logger sink) noexcept {
  try {
    const std::lock_guard lock(state_->mutex);
    if (state_->permanently_ram_only ||
        state_->persistent_sink.has_value()) {
      return false;
    }
    for (const auto& record : state_->records) {
      sink.write(record.level, record.message);
    }
    state_->persistent_sink.emplace(std::move(sink));
    return true;
  } catch (...) {
    return false;
  }
}

void BoundedRamLogRouter::isolate_to_ram_permanently() noexcept {
  try {
    const std::lock_guard lock(state_->mutex);
    state_->persistent_sink.reset();
    state_->permanently_ram_only = true;
  } catch (...) {
    // The method is an irreversible best-effort safety transition.
  }
}

bool BoundedRamLogRouter::persistent_sink_attached() const noexcept {
  try {
    const std::lock_guard lock(state_->mutex);
    return state_->persistent_sink.has_value();
  } catch (...) {
    return false;
  }
}

bool BoundedRamLogRouter::permanently_ram_only() const noexcept {
  try {
    const std::lock_guard lock(state_->mutex);
    return state_->permanently_ram_only;
  } catch (...) {
    return true;
  }
}

BoundedRamLogSnapshot BoundedRamLogRouter::snapshot() const {
  const std::lock_guard lock(state_->mutex);
  return BoundedRamLogSnapshot{
      .records = std::vector<LogRecord>(
          state_->records.begin(), state_->records.end()),
      .dropped_record_count = state_->dropped_record_count,
      .stored_message_characters = state_->stored_message_characters,
      .persistent_sink_attached = state_->persistent_sink.has_value(),
      .permanently_ram_only = state_->permanently_ram_only,
  };
}

std::wstring_view log_level_name(const LogLevel level) noexcept {
  switch (level) {
    case LogLevel::info:
      return L"INFO";
    case LogLevel::warning:
      return L"WARNING";
    case LogLevel::error:
      return L"ERROR";
    case LogLevel::debug:
      return L"DEBUG";
  }
  return L"ERROR";
}

Logger make_stderr_logger() {
  return Logger([](const LogRecord& record) {
    static std::mutex sink_mutex;
    const std::lock_guard lock(sink_mutex);

    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wcerr << std::setfill(L'0') << L'[' << std::setw(4) << now.wYear << L'-'
               << std::setw(2) << now.wMonth << L'-' << std::setw(2) << now.wDay
               << L' ' << std::setw(2) << now.wHour << L':' << std::setw(2)
               << now.wMinute << L':' << std::setw(2) << now.wSecond << L"] ["
               << log_level_name(record.level) << L"] " << record.message << L'\n';
  });
}

Result<Logger> make_utf8_file_logger(
    const std::wstring& path,
    const bool debug_enabled) {
  if (path.empty()) {
    return Result<Logger>::failure(Error{
        .code = ErrorCode::invalid_argument,
        .native_code = ERROR_INVALID_NAME,
        .operation = L"UTF-8ログファイル作成",
        .message = L"ログファイルのパスが空です",
    });
  }
  UniqueHandle handle(CreateFileW(
      path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL,
      nullptr));
  if (!handle) {
    return Result<Logger>::failure(make_win32_error(
        ErrorCode::io_failed,
        L"UTF-8ログファイル作成",
        GetLastError()));
  }
  constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF", 3U};
  if (!write_all(handle.get(), kUtf8Bom)) {
    return Result<Logger>::failure(make_win32_error(
        ErrorCode::io_failed,
        L"UTF-8ログBOM書込み",
        GetLastError()));
  }
  auto state = std::make_shared<FileLogState>();
  state->handle = std::move(handle);
  state->debug_enabled = debug_enabled;
  return Result<Logger>::success(Logger(
      [state = std::move(state)](const LogRecord& record) noexcept {
        if (record.level == LogLevel::debug &&
            !state->debug_enabled) {
          return;
        }
        try {
          const std::string line = make_log_line(record);
          if (line.empty()) {
            return;
          }
          const std::lock_guard lock(state->mutex);
          static_cast<void>(write_all(state->handle.get(), line));
          if (record.level == LogLevel::warning ||
              record.level == LogLevel::error) {
            static_cast<void>(FlushFileBuffers(state->handle.get()));
          }
        } catch (...) {
          // File logging is best-effort after successful file creation.
        }
      }));
}

}  // namespace ytec::clonecore

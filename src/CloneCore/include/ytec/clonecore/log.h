#pragma once

#include "ytec/clonecore/result.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::clonecore {

enum class LogLevel {
  info,
  warning,
  error,
  debug,
};

struct LogRecord final {
  LogLevel level{LogLevel::info};
  std::wstring message;
};

class Logger final {
 public:
  using Sink = std::function<void(const LogRecord&)>;

  explicit Logger(Sink sink);

  void write(LogLevel level, std::wstring_view message) const noexcept;
  void info(std::wstring_view message) const noexcept;
  void warning(std::wstring_view message) const noexcept;
  void error(std::wstring_view message) const noexcept;
  void debug(std::wstring_view message) const noexcept;

 private:
  Sink sink_;
};

struct BoundedRamLogLimits final {
  std::size_t maximum_records{256U};
  std::size_t maximum_total_message_characters{256U * 1024U};
  std::size_t maximum_message_characters{8192U};
};

struct BoundedRamLogSnapshot final {
  std::vector<LogRecord> records;
  std::size_t dropped_record_count{};
  std::size_t stored_message_characters{};
  bool persistent_sink_attached{};
  bool permanently_ram_only{};
};

// Every Logger copy returned by logger() routes through one shared, bounded RAM
// ring. The optional persistent sink is owned only by this shared state, so
// isolate_to_ram_permanently() closes it even while worker threads retain route
// logger copies. Once isolated, this router cannot attach another persistent
// sink during the same process session.
class BoundedRamLogRouter final {
 public:
  explicit BoundedRamLogRouter(BoundedRamLogLimits limits = {});

  [[nodiscard]] Logger logger() const;
  [[nodiscard]] bool attach_persistent_sink(Logger sink) noexcept;
  void isolate_to_ram_permanently() noexcept;

  [[nodiscard]] bool persistent_sink_attached() const noexcept;
  [[nodiscard]] bool permanently_ram_only() const noexcept;
  [[nodiscard]] BoundedRamLogSnapshot snapshot() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

[[nodiscard]] Logger make_stderr_logger();
// Creates a new UTF-8 BOM log file without overwriting an existing file.
// Records are single-line sanitized and readable while the application runs.
[[nodiscard]] Result<Logger> make_utf8_file_logger(
    const std::wstring& path,
    bool debug_enabled);
[[nodiscard]] std::wstring_view log_level_name(LogLevel level) noexcept;

}  // namespace ytec::clonecore

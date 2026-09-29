#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ytec::windowsapp {

enum class StartupDataIssue : std::uint8_t {
  none,
  executable_path_unavailable,
  executable_path_invalid,
  application_directory_unsafe,
  data_directory_unavailable,
  data_directory_unsafe,
  write_probe_failed,
  unexpected_failure,
};

enum class StartupDataStorage : std::uint8_t {
  bounded_ram,
  persistent_data,
  unavailable,
};

struct StartupDataPolicy final {
  StartupDataStorage storage{StartupDataStorage::unavailable};
  StartupDataIssue issue{StartupDataIssue::unexpected_failure};
  std::wstring data_directory;
  std::wstring diagnostic;
  DWORD native_code{ERROR_SUCCESS};

  [[nodiscard]] bool write_operations_permitted() const noexcept {
    return storage != StartupDataStorage::unavailable;
  }

  [[nodiscard]] bool persistent_logging_permitted() const noexcept {
    return storage == StartupDataStorage::persistent_data;
  }

  [[nodiscard]] bool intentionally_ram_isolated() const noexcept {
    return storage == StartupDataStorage::bounded_ram;
  }

  [[nodiscard]] bool diagnostic_only() const noexcept {
    return !write_operations_permitted();
  }
};

struct StartupDataBackingObservation final {
  std::wstring application_directory;
  std::wstring data_directory;
  std::uint32_t disk_number{};
  bool data_directory_exists{};
};

struct TsumugiPortableDataPathProof final {
  std::wstring canonical_data_directory;
  std::wstring canonical_final_path;
  std::wstring canonical_partial_path;
};

// Pure lexical gate for the product-owned EXE-adjacent data tree. Both the
// requested .tsumugi name and its adjacent .partial name must be absolute,
// unambiguous local Windows paths outside data and every child of data. The
// later WindowsTsumugiDestination gate still performs the filesystem-backed
// all-ancestor reparse and stable destination checks.
[[nodiscard]] clonecore::Result<TsumugiPortableDataPathProof>
evaluate_tsumugi_portable_data_path_gate(
    std::wstring_view executable_path,
    std::wstring_view final_path) noexcept;

// Production wrapper. It reads only the current executable path and never
// falls back to AppData or another storage location.
[[nodiscard]] clonecore::Status
require_windows_tsumugi_destination_outside_portable_data(
    const std::wstring& final_path) noexcept;

enum class StartupDataBackingRelationship : std::uint8_t {
  protected_disk,
  disjoint,
  unknown,
};

// The policy is separated from Win32 so every failure branch can be tested
// without touching a real disk, USB drive, or VM.
class IStartupDataPlatform {
 public:
  virtual ~IStartupDataPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<std::wstring>
  executable_path() = 0;

  [[nodiscard]] virtual clonecore::Status
  require_regular_non_reparse_directory(
      const std::wstring& path) = 0;

  [[nodiscard]] virtual clonecore::Status
  create_directory_if_missing(const std::wstring& path) = 0;

  // Must create, write, flush, read back, and remove a private probe file.
  [[nodiscard]] virtual clonecore::Status
  verify_directory_write_access(const std::wstring& path) = 0;
};

// Returns diagnostic-only for every unknown or failed observation. AppData or
// any other fallback directory is intentionally not considered.
[[nodiscard]] StartupDataPolicy evaluate_startup_data_policy(
    IStartupDataPlatform& platform) noexcept;

// The startup path is read-only: no directory creation, probe, retention, or
// file logger is allowed before an operation identifies its source/target.
[[nodiscard]] StartupDataPolicy
make_read_only_bootstrap_data_policy() noexcept;

// Classifies only already-stabilized observations. An empty protected set or
// missing backing observation is unknown and must remain in bounded RAM.
[[nodiscard]] StartupDataBackingRelationship classify_startup_data_backing(
    std::optional<std::uint32_t> backing_disk_number,
    std::span<const std::uint32_t> protected_disk_numbers) noexcept;

// Production read-only adapter for the current EXE and its adjacent "data"
// path. If data does not exist, its existing application parent is resolved.
// UNC/device paths, reparse ancestors, multi-disk volumes, and disconnects fail
// closed without creating anything.
[[nodiscard]] clonecore::Result<StartupDataBackingObservation>
inspect_windows_startup_data_backing() noexcept;

// On-demand full write/readback/delete probe. This must not be called at
// process startup; it is allowed only after the backing disk is proven disjoint
// from all operation source/target disks.
[[nodiscard]] StartupDataPolicy inspect_windows_startup_data_policy() noexcept;

}  // namespace ytec::windowsapp

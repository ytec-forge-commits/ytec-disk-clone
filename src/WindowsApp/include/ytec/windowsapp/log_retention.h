#pragma once

#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::windowsapp {

constexpr std::uint64_t kProductLogNormalRetentionDays = 30U;
constexpr std::uint64_t kProductLogFailureRetentionDays = 90U;
constexpr std::uint64_t kProductLogNormalBudgetBytes =
    200U * 1024U * 1024U;

enum class ProductLogClass : std::uint8_t {
  normal,
  failure,
};

enum class ProductLogDeletionReason : std::uint8_t {
  normal_age,
  normal_budget,
  failure_age,
};

struct ProductLogObservation final {
  std::wstring file_name;
  std::uint64_t size_bytes{};
  std::uint64_t last_write_utc_100ns{};
  std::uint64_t volume_serial{};
  std::array<std::byte, 16U> file_id{};
  bool regular_file{};
  bool reparse_point{};
  bool has_utf8_bom{};
};

struct ProductLogDeletion final {
  ProductLogObservation observed;
  ProductLogClass log_class{ProductLogClass::normal};
  ProductLogDeletionReason reason{ProductLogDeletionReason::normal_age};
};

struct ProductLogRetentionPlan final {
  std::vector<ProductLogDeletion> deletions;
  std::uint64_t retained_normal_bytes{};
  std::size_t retained_normal_count{};
  std::size_t retained_failure_count{};
  std::size_t ignored_count{};
};

struct ProductLogRetentionReport final {
  ProductLogRetentionPlan plan;
  std::size_t deleted_count{};
  std::uint64_t deleted_bytes{};
};

enum class ProductLogCompletionDisposition : std::uint8_t {
  keep_failure,
  promote_to_normal,
};

struct ProductLogCompletionPlan final {
  ProductLogCompletionDisposition disposition{
      ProductLogCompletionDisposition::keep_failure};
  std::wstring failed_file_name;
  std::wstring normal_file_name;
};

struct ProductLogCompletionReport final {
  ProductLogCompletionPlan plan;
  bool promoted{};
};

// A session is created as failed first. Only an explicit clean close with no
// logged error and no active write operation may be promoted to normal.
[[nodiscard]] clonecore::Result<ProductLogCompletionPlan>
plan_product_log_completion(
    const std::wstring& failed_file_name,
    bool clean_close_requested,
    bool error_detected,
    bool active_write_operation) noexcept;

class IProductLogCompletionPlatform {
 public:
  virtual ~IProductLogCompletionPlatform() = default;

  // Must keep the source handle pinned, reject reparse/hard-link inputs, and
  // perform a handle-scoped non-overwriting rename. Failure must leave the
  // failed name in place.
  [[nodiscard]] virtual clonecore::Status promote_failure_no_replace(
      const std::wstring& log_directory,
      const std::wstring& failed_file_name,
      const std::wstring& normal_file_name) = 0;
};

[[nodiscard]] clonecore::Result<ProductLogCompletionReport>
complete_product_log_session(
    IProductLogCompletionPlatform& platform,
    const std::wstring& log_directory,
    const std::wstring& failed_file_name,
    bool clean_close_requested,
    bool error_detected,
    bool active_write_operation);

// Production entry point. The persistent sink must already be detached so the
// failed file has no writer. data_directory is the verified EXE-adjacent data
// directory and failed_log_path must be its direct logs child. No fallback is
// attempted; a failed rename remains a failure-class log.
[[nodiscard]] clonecore::Result<ProductLogCompletionReport>
complete_windows_product_log_session(
    const std::wstring& data_directory,
    const std::wstring& failed_log_path,
    bool clean_close_requested,
    bool error_detected,
    bool active_write_operation);

// Only exact product-generated names are classified. Unknown names, paths,
// malformed timestamps, and unrelated extensions are never product logs.
[[nodiscard]] std::optional<ProductLogClass>
classify_product_log_file_name(const std::wstring& file_name) noexcept;

// Pure deterministic policy. The timestamp unit is the Windows FILETIME unit
// (100 ns since 1601 UTC), but no Win32 API is used by the planner.
[[nodiscard]] clonecore::Result<ProductLogRetentionPlan>
plan_product_log_retention(
    const std::vector<ProductLogObservation>& observations,
    std::uint64_t now_utc_100ns);

class IProductLogRetentionPlatform {
 public:
  virtual ~IProductLogRetentionPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<
      std::vector<ProductLogObservation>>
  enumerate_owned_candidates(const std::wstring& log_directory) = 0;

  // The implementation must reopen without following a reparse point and
  // compare identity, size, time, regular-file state, and BOM before deletion.
  [[nodiscard]] virtual clonecore::Status delete_if_unchanged(
      const std::wstring& log_directory,
      const ProductLogObservation& expected) = 0;
};

[[nodiscard]] clonecore::Result<ProductLogRetentionReport>
enforce_product_log_retention(
    IProductLogRetentionPlatform& platform,
    const std::wstring& log_directory,
    std::uint64_t now_utc_100ns);

// Production entry point. data_directory must be the already-verified
// EXE-adjacent data directory; this function never tries AppData or another
// fallback. It operates only on its direct "logs" child.
[[nodiscard]] clonecore::Result<ProductLogRetentionReport>
enforce_windows_product_log_retention(
    const std::wstring& data_directory);

}  // namespace ytec::windowsapp

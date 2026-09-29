#pragma once

#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

inline constexpr std::size_t kSupportZipMaximumEntryCount = 64U;
inline constexpr std::size_t kSupportZipMaximumCandidateCount = 4096U;
inline constexpr std::uint64_t kSupportZipMaximumSourceFileBytes =
    200U * 1024U * 1024U;
inline constexpr std::uint64_t kSupportZipPreferredSubsetBytes =
    64U * 1024U * 1024U;
inline constexpr std::uint64_t kSupportZipMaximumSourceTotalBytes =
    200U * 1024U * 1024U;
inline constexpr std::uint64_t kSupportZipMaximumArchiveBytes =
    256U * 1024U * 1024U;

struct SupportZipSelectionCandidate final {
  std::wstring file_name;
  std::uint64_t size_bytes{};
  std::uint64_t last_write_utc_100ns{};
};

struct SupportZipCandidateSelection final {
  std::vector<std::size_t> selected_indices;
  std::size_t excluded_count{};
  std::uint64_t selected_bytes{};
};

// Selects newest-first with an ordinal basename tie-break. Normally the
// reviewed subset is at most 64 MiB/64 files. If no file fits that preferred
// budget, the newest valid product log is still selected up to the complete
// 200 MiB retention ceiling, so a valid retained state remains supportable.
[[nodiscard]] clonecore::Result<SupportZipCandidateSelection>
select_support_zip_candidates(
    const std::vector<SupportZipSelectionCandidate>& candidates) noexcept;

// The UI displays this basename-only list before allowing local creation.
// No source path or private file-object identity is exposed through it.
struct SupportZipDisplayEntry final {
  std::wstring archive_entry_name;
  std::uint64_t source_size_bytes{};
  std::uint64_t masked_size_bytes{};
};

struct SupportZipCreationReport;
class SupportZipPlanBuilder;

class SupportZipPlan final {
 public:
  SupportZipPlan(const SupportZipPlan&) = default;
  SupportZipPlan& operator=(const SupportZipPlan&) = default;
  SupportZipPlan(SupportZipPlan&&) noexcept = default;
  SupportZipPlan& operator=(SupportZipPlan&&) noexcept = default;

  [[nodiscard]] const std::wstring& final_path() const noexcept {
    return final_path_;
  }
  [[nodiscard]] const std::wstring& partial_path() const noexcept {
    return partial_path_;
  }
  [[nodiscard]] std::span<const SupportZipDisplayEntry> entries()
      const noexcept {
    return display_entries_;
  }
  [[nodiscard]] std::uint64_t source_total_bytes() const noexcept {
    return source_total_bytes_;
  }
  [[nodiscard]] std::uint64_t masked_total_bytes() const noexcept {
    return masked_total_bytes_;
  }
  [[nodiscard]] std::size_t excluded_log_count() const noexcept {
    return excluded_log_count_;
  }
  [[nodiscard]] std::size_t candidate_log_count() const noexcept {
    return candidate_log_count_;
  }

 private:
  struct ObjectIdentity final {
    std::uint64_t volume_serial{};
    std::array<std::byte, 16U> file_id{};
  };

  struct SourceIdentity final {
    ObjectIdentity object{};
    std::uint64_t size_bytes{};
    std::uint64_t allocation_size{};
    std::uint64_t last_write_utc_100ns{};
    std::uint64_t change_time_utc_100ns{};
    std::uint32_t link_count{};
    std::uint32_t masked_crc32{};
  };

  SupportZipPlan() = default;

  std::wstring executable_path_;
  std::wstring application_directory_;
  std::wstring data_directory_;
  std::wstring log_directory_;
  std::wstring final_path_;
  std::wstring partial_path_;
  ObjectIdentity executable_identity_{};
  ObjectIdentity application_directory_identity_{};
  ObjectIdentity data_directory_identity_{};
  ObjectIdentity log_directory_identity_{};
  ObjectIdentity output_directory_identity_{};
  std::vector<SupportZipDisplayEntry> display_entries_;
  std::vector<SourceIdentity> source_identities_;
  std::uint64_t source_total_bytes_{};
  std::uint64_t masked_total_bytes_{};
  std::size_t candidate_log_count_{};
  std::size_t excluded_log_count_{};

  friend clonecore::Result<SupportZipPlan> plan_windows_support_zip(
      const std::wstring&,
      const std::wstring&);
  friend clonecore::Result<SupportZipPlan>
  plan_current_executable_support_zip(const std::wstring&);
  friend clonecore::Result<SupportZipCreationReport>
  create_windows_support_zip(const SupportZipPlan&);
  friend class SupportZipPlanBuilder;
};

struct SupportZipCreationReport final {
  std::wstring final_path;
  std::vector<SupportZipDisplayEntry> entries;
  std::uint64_t archive_size_bytes{};
  std::size_t excluded_log_count{};
  bool local_only{true};
};

// Plans only strict product-owned UTF-8 BOM logs directly below the supplied
// executable's adjacent data\logs directory. The final path must be a new
// canonical local .zip path outside data and outside AppData.
[[nodiscard]] clonecore::Result<SupportZipPlan> plan_windows_support_zip(
    const std::wstring& executable_path,
    const std::wstring& final_zip_path);

// Convenience production planner. No network operation or automatic send is
// performed; the returned entry list is intended for explicit UI review.
[[nodiscard]] clonecore::Result<SupportZipPlan>
plan_current_executable_support_zip(const std::wstring& final_zip_path);

// Re-identifies every planned input and directory, rebuilds the masked stored
// ZIP, verifies the complete archive on the same CREATE_NEW partial handle,
// and then publishes without replacing an existing final path.
[[nodiscard]] clonecore::Result<SupportZipCreationReport>
create_windows_support_zip(const SupportZipPlan& plan);

}  // namespace ytec::windowsapp

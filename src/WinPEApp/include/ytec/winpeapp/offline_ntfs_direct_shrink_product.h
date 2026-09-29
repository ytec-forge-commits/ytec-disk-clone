#pragma once

#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/windowsshrink/source_analysis.h"
#include "ytec/winpeapp/offline_ntfs_direct_shrink_clone.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::winpeapp {

struct WinPeOfflineNtfsProductPlanningRequest final {
  bool administrator{};
  bool winpe_environment_verified{};
  diskmodel::DiskInfo reviewed_source;
  diskmodel::DiskInfo reviewed_target;
  operationcore::OperationId operation_id{};
  // Empty means the initial all-selected state. Required Windows/Boot/WinRE
  // entries remain selected even if omitted from a later UI choice.
  std::vector<std::uint32_t> selected_source_table_indexes;
  migrationcore::ShrinkSurplusAllocation surplus_allocation{
      migrationcore::ShrinkSurplusAllocation::automatic_proportional};
  std::optional<std::uint32_t> surplus_target_source_table_index;
  std::string analysis_created_utc;
  std::string app_version;
};

struct WinPeOfflineNtfsPartitionCandidate final {
  std::uint32_t source_table_index{};
  migrationcore::MigrationPartitionRole role{
      migrationcore::MigrationPartitionRole::data};
  migrationcore::MigrationFileSystem file_system{
      migrationcore::MigrationFileSystem::ntfs};
  std::uint64_t source_size_bytes{};
  std::uint64_t used_bytes{};
  std::uint64_t cluster_size{};
  std::wstring label;
  bool selected_by_default{true};
  bool required{};
};

struct WinPeOfflineNtfsPartitionReviewBinding final {
  clonecore::StableDiskIdentity source;
  clonecore::StableDiskIdentity target;
  imageformat::Sha256Digest source_layout_hash{};
  imageformat::Sha256Digest target_layout_hash{};
  imageformat::Sha256Digest source_partition_snapshot_hash{};
  imageformat::Sha256Digest source_analysis_hash{};
  imageformat::Sha256Digest source_epoch_hash{};
  imageformat::Sha256Digest binding_hash{};
  bool source_contains_windows{};
};

struct WinPeOfflineNtfsPartitionInspection final {
  WinPeOfflineNtfsPartitionReviewBinding binding;
  std::vector<WinPeOfflineNtfsPartitionCandidate> candidates;
};

// Pure, no-I/O product-boundary gate. This must pass before the WinPE adapter
// changes the source's non-persistent OS read-only attribute.
[[nodiscard]] clonecore::Status
validate_winpe_offline_ntfs_direct_shrink_no_io_preflight(
    const WinPeOfflineNtfsProductPlanningRequest& request);

// Canonical content hash for WinPE offline analysis. It excludes display-only
// creation time/app version, but includes stable identity, exact raw partition
// snapshot, every partition role/extent/filesystem/measurement, Volume GUID
// binding, BitLocker state and offline Windows version.
[[nodiscard]] clonecore::Result<imageformat::Sha256Digest>
hash_winpe_offline_ntfs_source_analysis_v1(
    const windowsshrink::ShrinkSourceAnalysis& analysis);

// Pure review seam used by focused tests. The source DiskInfo and analysis
// must already describe the OS-level read-only source observation.
[[nodiscard]] clonecore::Result<WinPeOfflineNtfsPartitionInspection>
inspect_winpe_offline_ntfs_direct_shrink_from_analysis(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis);

// Pure plan seam. It requires exact equality with the completed review
// binding and builds a new immutable MigrationCore decision plus VSS-free
// target transaction plan from the same authenticated analysis object.
[[nodiscard]] clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>
build_winpe_offline_ntfs_direct_shrink_after_review_from_analysis(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const windowsshrink::ShrinkSourceAnalysis& analysis,
    const WinPeOfflineNtfsPartitionReviewBinding& completed_review);

// Product read-only inspection. It sets the exact source's non-persistent OS
// disk attribute read-only before opening or analyzing it and deliberately
// leaves that attribute set. No writable target is opened.
[[nodiscard]] clonecore::Result<WinPeOfflineNtfsPartitionInspection>
inspect_winpe_offline_ntfs_direct_shrink_with_windows_apis(
    const WinPeOfflineNtfsProductPlanningRequest& request);

// Product plan after UI review. It repeats stable source/target inventory,
// source read-only latch, read-only GPT/NTFS analysis and review-binding
// comparison before returning the plan. No target write occurs.
[[nodiscard]] clonecore::Result<WinPeOfflineNtfsDirectShrinkPlan>
plan_winpe_offline_ntfs_direct_shrink_after_review_with_windows_apis(
    const WinPeOfflineNtfsProductPlanningRequest& request,
    const WinPeOfflineNtfsPartitionReviewBinding& completed_review);

// Production dependencies for execute_winpe_offline_ntfs_direct_shrink_clone.
// Construction performs no disk I/O. The target factory is supplied by the
// VSS-free DirectShrinkTarget static library after extraction.
[[nodiscard]] WinPeOfflineNtfsDirectShrinkDependencies
make_winpe_offline_ntfs_direct_shrink_dependencies_with_windows_apis(
    const WinPeOfflineNtfsDirectShrinkExecutionOptions& options);

}  // namespace ytec::winpeapp

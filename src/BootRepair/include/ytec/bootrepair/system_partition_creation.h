#pragma once

#include "ytec/bootrepair/automatic_repair_plan.h"
#include "ytec/clonecore/result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ytec::bootrepair {

// BCD-003 deliberately uses a separate destructive transaction.  Automatic
// discovery remains read-only and may only report that a system partition is
// missing.  The product must then obtain an additional exact uppercase-OK
// confirmation before this transaction is allowed to shrink an NTFS volume.
struct SystemPartitionCreationObservation final {
  AutomaticBootRepairPlan plan;
  std::uint64_t max_reclaimable_bytes{};
  bool exact_windows_volume_found{};
  bool simple_ntfs_volume{};
  bool volume_online{};
  bool volume_transition_stable{};
  bool volume_health_acceptable{};
  bool forbidden_volume_role_or_encryption{};
};

class ReviewedSystemPartitionCreation final {
 public:
  ReviewedSystemPartitionCreation(
      const ReviewedSystemPartitionCreation&) = default;
  ReviewedSystemPartitionCreation(
      ReviewedSystemPartitionCreation&&) noexcept = default;
  ReviewedSystemPartitionCreation& operator=(
      const ReviewedSystemPartitionCreation&) = default;
  ReviewedSystemPartitionCreation& operator=(
      ReviewedSystemPartitionCreation&&) noexcept = default;

  [[nodiscard]] const AutomaticBootRepairPlan& discovery() const noexcept;
  [[nodiscard]] const clonecore::StableDiskIdentity& selected_identity()
      const noexcept;
  [[nodiscard]] const diskmodel::PartitionInfo& windows_partition()
      const noexcept;
  [[nodiscard]] const std::wstring& windows_volume_name() const noexcept;
  [[nodiscard]] BootSystemPartitionRole system_role() const noexcept;
  [[nodiscard]] std::uint64_t system_partition_size_bytes() const noexcept;
  [[nodiscard]] std::uint64_t reclaim_bytes() const noexcept;
  [[nodiscard]] std::uint64_t system_partition_offset_bytes() const noexcept;
  [[nodiscard]] std::uint64_t shrunken_windows_size_bytes() const noexcept;

 private:
  ReviewedSystemPartitionCreation(
      AutomaticBootRepairPlan discovery,
      DiscoveredWindowsInstallation windows,
      std::uint64_t system_partition_size_bytes,
      std::uint64_t reclaim_bytes,
      std::uint64_t system_partition_offset_bytes,
      std::uint64_t shrunken_windows_size_bytes);

  AutomaticBootRepairPlan discovery_;
  DiscoveredWindowsInstallation windows_;
  std::uint64_t system_partition_size_bytes_{};
  std::uint64_t reclaim_bytes_{};
  std::uint64_t system_partition_offset_bytes_{};
  std::uint64_t shrunken_windows_size_bytes_{};

  friend clonecore::Result<ReviewedSystemPartitionCreation>
  review_system_partition_creation(
      const AutomaticBootRepairPlan&,
      std::uint32_t,
      const SystemPartitionCreationObservation&);
};

// Pure review.  The observation must be a fresh full discovery of the same
// disk plus an exact Microsoft VDS volume observation.  Only one basic NTFS
// Windows partition is eligible.  GPT creates a 260 MiB FAT32 ESP; MBR creates
// a 100 MiB NTFS Active primary partition.  Both require 1 MiB aligned exact
// end-shrink geometry and retain a reclaimability safety margin.
[[nodiscard]] clonecore::Result<ReviewedSystemPartitionCreation>
review_system_partition_creation(
    const AutomaticBootRepairPlan& discovery,
    std::uint32_t selected_windows_partition_number,
    const SystemPartitionCreationObservation& observation);

// Rebinds a reviewed plan to a fresh read-only pre-mutation observation.  Disk
// number churn is allowed only when the stable identity matches; the complete
// partition layout, Windows/WinRE evidence and volume binding must be exact.
[[nodiscard]] clonecore::Status
revalidate_system_partition_creation_review(
    const ReviewedSystemPartitionCreation& reviewed,
    const SystemPartitionCreationObservation& fresh);

// Pure post-mutation bindings shared by the transaction and the Windows VDS
// adapter.  They compare the stable identity, every original partition and
// every Windows/WinRE observation; a matching Windows extent alone is never
// sufficient authority for a later create, delete, or extend call.
[[nodiscard]] clonecore::Status
validate_system_partition_creation_shrunken_plan(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& observed);

[[nodiscard]] clonecore::Status
validate_system_partition_creation_completed_plan(
    const ReviewedSystemPartitionCreation& reviewed,
    const AutomaticBootRepairPlan& observed);

// Raw inventory variants exist for the narrow failure window where VDS has
// created the exact reviewed partition but formatting has not made it a valid
// automatic-repair candidate yet.  They still require stable reidentification
// and a complete exact partition layout before cleanup can mutate anything.
[[nodiscard]] clonecore::Status
validate_system_partition_creation_shrunken_disk(
    const ReviewedSystemPartitionCreation& reviewed,
    const diskmodel::DiskInfo& observed);

[[nodiscard]] clonecore::Status
validate_system_partition_creation_created_disk_for_rollback(
    const ReviewedSystemPartitionCreation& reviewed,
    const diskmodel::DiskInfo& observed);

class ISystemPartitionCreationPlatform {
 public:
  virtual ~ISystemPartitionCreationPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<
      SystemPartitionCreationObservation>
  observe_read_only(
      const clonecore::StableDiskIdentity& expected_disk,
      std::uint32_t windows_partition_number) = 0;

  // Each mutating call must independently reidentify the stable disk and bind
  // the exact reviewed Windows Volume GUID/extent.  No implementation may use
  // the remembered disk number as its sole target identity.
  [[nodiscard]] virtual clonecore::Status shrink_windows_exact(
      const ReviewedSystemPartitionCreation& reviewed) = 0;
  [[nodiscard]] virtual clonecore::Status create_and_format_system_exact(
      const ReviewedSystemPartitionCreation& reviewed) = 0;
  [[nodiscard]] virtual clonecore::Status delete_created_system_exact(
      const ReviewedSystemPartitionCreation& reviewed) = 0;
  [[nodiscard]] virtual clonecore::Status extend_windows_exact(
      const ReviewedSystemPartitionCreation& reviewed) = 0;
};

enum class SystemPartitionCreationOutcome : std::uint8_t {
  committed,
  failed_before_mutation,
  rolled_back_exact,
  rollback_incomplete,
};

struct SystemPartitionCreationReport final {
  SystemPartitionCreationOutcome outcome{
      SystemPartitionCreationOutcome::failed_before_mutation};
  bool confirmation_verified{};
  bool pre_mutation_revalidated{};
  bool windows_shrunk{};
  bool shrunken_layout_verified{};
  bool system_partition_created_and_formatted{};
  bool completed_plan_verified{};
  bool rollback_attempted{};
  bool created_partition_removed{};
  bool windows_extent_restored{};
  bool original_layout_restored{};
  std::optional<AutomaticBootRepairPlan> completed_plan;
  std::optional<clonecore::Error> primary_error;
  std::optional<clonecore::Error> rollback_error;
};

// Executes only the system-partition preparation transaction.  BCD/WinRE/NVRAM
// remain a later reviewed transaction against completed_plan.  Any failure
// after the NTFS shrink attempts exact rollback before returning.
[[nodiscard]] SystemPartitionCreationReport
execute_system_partition_creation(
    const ReviewedSystemPartitionCreation& reviewed,
    const clonecore::TargetConfirmation& confirmation,
    ISystemPartitionCreationPlatform& platform);

// Production Microsoft VDS adapter.  Construction itself is non-mutating.
// The returned object performs no operation until one of the methods above is
// called by the reviewed transaction.
[[nodiscard]] std::unique_ptr<ISystemPartitionCreationPlatform>
make_windows_system_partition_creation_platform();

}  // namespace ytec::bootrepair

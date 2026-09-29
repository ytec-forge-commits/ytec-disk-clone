#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/migrationcore/direct_clone_plan.h"
#include "ytec/operationcore/operation.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ytec::directshrink {

inline constexpr std::uint64_t kStagingAlignmentBytes = 1024ULL * 1024ULL;
inline constexpr std::uint64_t kCheckpointOffsetBytes = 64ULL * 1024ULL;
inline constexpr std::uint64_t kCheckpointRecordBytes = 4ULL * 1024ULL;
inline constexpr std::uint64_t kStagingControlReserveBytes =
    64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kStagingFileSystemReserveBytes =
    1024ULL * 1024ULL * 1024ULL;

struct MbrPlanBinding final {
  imageformat::Sha256Digest source_sector0_hash{};
  std::array<std::byte, 440U> source_bootstrap{};
  std::uint32_t source_disk_signature{};
  std::uint32_t target_disk_signature{};
  imageformat::Sha256Digest planning_signature_inventory_hash{};
};

enum class PartitionTaskKind : std::uint8_t {
  recreate_efi_system,
  recreate_microsoft_reserved,
  create_empty_ntfs,
  apply_ntfs_wim,
  copy_exact_raw,
};

struct PartitionTask final {
  PartitionTaskKind kind{PartitionTaskKind::apply_ntfs_wim};
  std::uint32_t target_number{};
  std::optional<std::uint32_t> source_table_index;
  migrationcore::MigrationPartitionRole role{
      migrationcore::MigrationPartitionRole::data};
  bool active{};
  std::uint64_t source_offset_bytes{};
  std::uint64_t target_offset_bytes{};
  std::uint64_t construction_size_bytes{};
  std::uint64_t target_size_bytes{};
  std::uint64_t source_size_bytes{};
  std::uint64_t source_used_bytes{};
  // Required only for copy_exact_raw. GPT stores the exact 16-byte source
  // type GUID; MBR stores the source type byte in element 0 and zeroes the
  // remaining bytes. This is partition metadata, never executable content.
  std::array<std::byte, 16U> source_partition_type{};
  std::wstring original_volume_guid_path;
  std::uint64_t archive_upper_bound_bytes{};
};

struct TargetOwnedStagingPlan final {
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  std::uint64_t control_reserve_bytes{};
  std::uint64_t archive_offset_bytes{};
  std::uint64_t archive_capacity_bytes{};
  std::optional<std::uint32_t> final_growth_owner_target_number;
};

enum class SourcePartitionDisposition : std::uint8_t {
  transferred_to_target,
  recreated_as_generated_system_partition,
  replaced_by_generated_uefi_boot,
  omitted_unselected,
};

struct SourcePartitionMapping final {
  std::uint32_t source_table_index{};
  migrationcore::MigrationPartitionRole role{
      migrationcore::MigrationPartitionRole::data};
  SourcePartitionDisposition disposition{
      SourcePartitionDisposition::transferred_to_target};
  std::optional<std::uint32_t> target_number;
  bool requested{};
  bool selected{};
  bool required{};
};

// VSS-free by-value construction input.  Environment-specific source
// preflight is deliberately outside this contract; Windows and WinPE each
// authenticate their own source epoch before passing a normalized target plan.
struct TargetPlanData final {
  operationcore::OperationPlan operation_plan;
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  imageformat::Sha256Digest expected_source_layout_hash{};
  imageformat::Sha256Digest expected_target_layout_hash{};
  imageformat::Sha256Digest source_partition_snapshot_hash{};
  std::optional<MbrPlanBinding> mbr_preserve_binding;
  migrationcore::MigrationPartitionStyle source_partition_style{
      migrationcore::MigrationPartitionStyle::gpt};
  migrationcore::MigrationPartitionStyle partition_style{
      migrationcore::MigrationPartitionStyle::gpt};
  migrationcore::DirectClonePartitionStyleChoice partition_style_choice{
      migrationcore::DirectClonePartitionStyleChoice::preserve};
  migrationcore::ShrinkSurplusAllocation surplus_allocation{
      migrationcore::ShrinkSurplusAllocation::leave_unallocated};
  std::optional<std::uint32_t> surplus_target_source_table_index;
  std::uint64_t checkpoint_offset_bytes{kCheckpointOffsetBytes};
  TargetOwnedStagingPlan staging;
  std::vector<PartitionTask> tasks;
  std::vector<SourcePartitionMapping> source_partition_mappings;
  imageformat::Sha256Digest final_layout_hash{};
  bool boot_finalization_required{};
  bool target_is_active_rescue_media{};
  std::uint64_t archive_task_count{};
  std::uint64_t maximum_archive_upper_bound_bytes{};
  std::uint64_t ntfs_extension_task_count{};
};

class TargetPlan final {
 public:
  TargetPlan(const TargetPlan&) = default;
  TargetPlan(TargetPlan&&) noexcept = default;
  TargetPlan& operator=(const TargetPlan&) = delete;
  TargetPlan& operator=(TargetPlan&&) = delete;

  [[nodiscard]] const operationcore::OperationPlan& operation_plan()
      const noexcept {
    return data_.operation_plan;
  }
  [[nodiscard]] const clonecore::StableDiskIdentity& expected_source()
      const noexcept {
    return data_.expected_source;
  }
  [[nodiscard]] const clonecore::StableDiskIdentity& expected_target()
      const noexcept {
    return data_.expected_target;
  }
  [[nodiscard]] const imageformat::Sha256Digest& expected_source_layout_hash()
      const noexcept {
    return data_.expected_source_layout_hash;
  }
  [[nodiscard]] const imageformat::Sha256Digest& expected_target_layout_hash()
      const noexcept {
    return data_.expected_target_layout_hash;
  }
  [[nodiscard]] const imageformat::Sha256Digest&
  source_partition_snapshot_hash() const noexcept {
    return data_.source_partition_snapshot_hash;
  }
  [[nodiscard]] const std::optional<MbrPlanBinding>& mbr_preserve_binding()
      const noexcept {
    return data_.mbr_preserve_binding;
  }
  [[nodiscard]] migrationcore::MigrationPartitionStyle source_partition_style()
      const noexcept {
    return data_.source_partition_style;
  }
  [[nodiscard]] migrationcore::MigrationPartitionStyle partition_style()
      const noexcept {
    return data_.partition_style;
  }
  [[nodiscard]] migrationcore::DirectClonePartitionStyleChoice
  partition_style_choice() const noexcept {
    return data_.partition_style_choice;
  }
  [[nodiscard]] migrationcore::ShrinkSurplusAllocation surplus_allocation()
      const noexcept {
    return data_.surplus_allocation;
  }
  [[nodiscard]] std::optional<std::uint32_t>
  surplus_target_source_table_index() const noexcept {
    return data_.surplus_target_source_table_index;
  }
  [[nodiscard]] std::uint64_t checkpoint_offset_bytes() const noexcept {
    return data_.checkpoint_offset_bytes;
  }
  [[nodiscard]] const TargetOwnedStagingPlan& staging() const noexcept {
    return data_.staging;
  }
  [[nodiscard]] std::span<const PartitionTask> tasks() const noexcept {
    return data_.tasks;
  }
  [[nodiscard]] std::span<const SourcePartitionMapping>
  source_partition_mappings() const noexcept {
    return data_.source_partition_mappings;
  }
  [[nodiscard]] const imageformat::Sha256Digest& final_layout_hash()
      const noexcept {
    return data_.final_layout_hash;
  }
  [[nodiscard]] bool boot_finalization_required() const noexcept {
    return data_.boot_finalization_required;
  }
  [[nodiscard]] bool target_is_active_rescue_media() const noexcept {
    return data_.target_is_active_rescue_media;
  }
  [[nodiscard]] std::uint64_t archive_task_count() const noexcept {
    return data_.archive_task_count;
  }
  [[nodiscard]] std::uint64_t maximum_archive_upper_bound_bytes()
      const noexcept {
    return data_.maximum_archive_upper_bound_bytes;
  }
  [[nodiscard]] std::uint64_t ntfs_extension_task_count() const noexcept {
    return data_.ntfs_extension_task_count;
  }

 private:
  explicit TargetPlan(TargetPlanData data) : data_(std::move(data)) {}
  TargetPlanData data_;

  friend clonecore::Result<TargetPlan> make_target_plan(TargetPlanData);
};

// Pure structural validator and immutable constructor.  It validates the
// target transaction contract, not Windows-online or WinPE source preflight.
[[nodiscard]] clonecore::Result<TargetPlan> make_target_plan(
    TargetPlanData data);

enum class CheckpointPhase : std::uint8_t {
  prepared,
  applying,
  commit_ready,
};

struct CheckpointEvidence final {
  CheckpointPhase phase{CheckpointPhase::prepared};
  std::uint64_t revision{};
  operationcore::Sha256Digest plan_hash{};
  imageformat::Sha256Digest staging_identity_hash{};
  imageformat::Sha256Digest record_hash{};
  imageformat::Sha256Digest aggregate_write_digest{};
  clonecore::StableDiskIdentity observed_target;
  std::uint64_t completed_task_count{};
  std::uint64_t verified_target_bytes{};
  bool durable{};
  bool flushed{};
  bool read_back_verified{};
  bool target_offline{};
  bool final_layout_committed{};
};

struct TargetPreparationEvidence final {
  std::uint64_t prepared_task_count{};
  std::uint64_t verified_target_bytes{};
  imageformat::Sha256Digest write_digest{};
  bool every_write_flushed{};
  bool every_write_read_back{};
  bool target_offline{};
  bool final_layout_committed{};
};

enum class CaptureSourceKind : std::uint8_t {
  windows_vss_snapshot,
  winpe_read_only_volume,
};

// A target WIM store receives only a read path plus authenticated binding
// facts.  Windows requires a ShadowCopy path; WinPE requires a canonical
// Volume GUID root while both read-only handles and the disk read-only latch
// remain held.  The implementation validates the selected kind separately.
struct CaptureSource final {
  CaptureSourceKind kind{CaptureSourceKind::winpe_read_only_volume};
  std::uint32_t source_table_index{};
  std::uint64_t source_offset_bytes{};
  std::uint64_t source_size_bytes{};
  std::wstring original_volume_guid_path;
  std::wstring capture_identity;
  std::wstring read_device_path;
  bool source_os_read_only{};
  bool source_disk_handle_held_read_only{};
  bool source_volume_handle_held_read_only{};
  bool source_volume_extent_reverified{};
};

struct StagedArchiveEvidence final {
  std::uint32_t source_table_index{};
  std::uint32_t target_number{};
  std::wstring capture_identity;
  std::wstring read_device_path;
  std::uint64_t archive_length{};
  imageformat::Sha256Digest archive_hash{};
  bool sealed_no_write_delete_sharing{};
  bool flushed{};
  bool complete_read_back_hash_verified{};
  bool target_offline{};
};

struct AppliedPartitionEvidence final {
  std::uint32_t source_table_index{};
  std::uint32_t target_number{};
  std::uint64_t verified_target_bytes{};
  imageformat::Sha256Digest archive_hash{};
  imageformat::Sha256Digest target_write_digest{};
  bool every_write_flushed{};
  bool every_write_read_back{};
  bool file_system_metadata_verified{};
  bool target_offline{};
};

struct ExactRawPartitionEvidence final {
  std::uint32_t source_table_index{};
  std::uint32_t target_number{};
  std::uint64_t verified_target_bytes{};
  std::uint64_t verified_chunk_count{};
  imageformat::Sha256Digest source_sha256{};
  imageformat::Sha256Digest target_sha256{};
  imageformat::Sha256Digest target_write_digest{};
  bool source_reader_read_only{};
  bool source_extent_exact{};
  bool every_write_flushed{};
  bool every_chunk_read_back{};
  bool complete_target_hash_verified{};
  bool target_offline{};
};

struct BootEvidence final {
  bool required{};
  bool completed{};
  bool boot_files_read_back_verified{};
  bool recovery_configuration_verified{};
  bool target_offline{};
  bool target_only_reconstruction{};
  bool exact_target_volume_extents{};
  bool legacy_bios{};
  bool real_boot_not_claimed{};
};

struct FinalCommitEvidence final {
  imageformat::Sha256Digest committed_layout_hash{};
  imageformat::Sha256Digest aggregate_write_digest{};
  bool source_reidentified{};
  bool source_layout_unchanged{};
  bool target_reidentified{};
  bool staging_identity_reverified{};
  bool checkpoint_reverified{};
  bool staging_removed{};
  bool checkpoint_retired{};
  bool checkpoint_retirement_pending{};
  bool construction_layout_non_bootable{};
  bool checkpoint_retained_through_extensions_and_boot{};
  bool boot_completed_before_final_layout_publication{};
  bool final_layout_published_before_checkpoint_retirement{};
  bool hidden_final_layout_published_and_read_back{};
  std::uint64_t extended_ntfs_partition_count{};
  bool every_required_ntfs_extension_verified{};
  std::optional<std::uint32_t> targeted_surplus_source_table_index;
  std::optional<std::uint32_t> targeted_surplus_target_number;
  std::uint64_t targeted_surplus_previous_file_system_bytes{};
  std::uint64_t targeted_surplus_final_file_system_bytes{};
  bool targeted_surplus_owner_verified{};
  bool targeted_surplus_exact_size_verified{};
  bool targeted_surplus_readback_verified{};
  bool every_write_flushed{};
  bool every_write_read_back{};
  bool primary_layout_committed_last{};
  bool target_offline{};
  migrationcore::MigrationPartitionStyle final_partition_style{
      migrationcore::MigrationPartitionStyle::gpt};
  bool source_mbr_sector0_unchanged{};
  bool source_mbr_bootstrap_unchanged{};
  bool target_mbr_signature_collision_free{};
  bool final_mbr_sector0_read_back_verified{};
  std::uint32_t final_mbr_disk_signature{};
  std::uint32_t final_mbr_active_partition_count{};
};

// Destructive target-only boundary.  No method receives a writable source.
class ITargetPlatform {
 public:
  virtual ~ITargetPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<CheckpointEvidence>
  begin_target_owned_staging(
      const TargetPlan& plan,
      const operationcore::Sha256Digest& operation_plan_hash) = 0;
  [[nodiscard]] virtual clonecore::Result<TargetPreparationEvidence>
  prepare_non_archive_partitions_and_verify(
      std::span<const PartitionTask> tasks) = 0;
  [[nodiscard]] virtual clonecore::Result<StagedArchiveEvidence>
  capture_ntfs_wim_to_owned_staging(
      const PartitionTask& task,
      const CaptureSource& source) = 0;
  [[nodiscard]] virtual clonecore::Result<AppliedPartitionEvidence>
  apply_staged_ntfs_wim_and_verify(
      const PartitionTask& task,
      const StagedArchiveEvidence& archive) = 0;
  [[nodiscard]] virtual clonecore::Result<ExactRawPartitionEvidence>
  copy_exact_raw_and_verify(
      const PartitionTask& task,
      const clonecore::ISourceDiskReader& read_only_source) = 0;
  [[nodiscard]] virtual clonecore::Status discard_exact_staged_archive(
      const StagedArchiveEvidence& archive) = 0;
  [[nodiscard]] virtual clonecore::Result<CheckpointEvidence>
  persist_prepared_partitions_checkpoint(
      const CheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;
  [[nodiscard]] virtual clonecore::Result<CheckpointEvidence>
  persist_progress_checkpoint(
      const CheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;
  [[nodiscard]] virtual clonecore::Result<BootEvidence>
  finalize_boot_from_staged_layout_and_verify(const TargetPlan& plan) = 0;
  [[nodiscard]] virtual clonecore::Result<CheckpointEvidence>
  seal_commit_ready_checkpoint(
      const CheckpointEvidence& previous,
      std::uint64_t completed_task_count,
      std::uint64_t verified_target_bytes,
      const imageformat::Sha256Digest& aggregate_write_digest) = 0;
  [[nodiscard]] virtual clonecore::Result<CheckpointEvidence>
  prepare_final_extents_keep_incomplete_and_verify(
      const TargetPlan& plan,
      const CheckpointEvidence& expected) = 0;
  [[nodiscard]] virtual clonecore::Result<CheckpointEvidence>
  revalidate_before_final_commit(
      const TargetPlan& plan,
      const CheckpointEvidence& expected) = 0;
  [[nodiscard]] virtual clonecore::Result<FinalCommitEvidence>
  commit_final_layout_last(
      const TargetPlan& plan,
      const CheckpointEvidence& commit_ready) = 0;
  virtual void abort_keep_offline_incomplete() noexcept = 0;
};

}  // namespace ytec::directshrink

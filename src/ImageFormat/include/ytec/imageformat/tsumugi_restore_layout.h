#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_manifest.h"
#include "ytec/migrationcore/shrink_layout.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace ytec::imageformat {

enum class TsumugiRestoreLayoutWriteKind : std::uint8_t {
  gpt_primary_entries,
  gpt_backup_entries,
  gpt_backup_header,
  gpt_protective_mbr,
  gpt_primary_header,
  mbr_sector,
};

struct TsumugiRestoreLayoutWrite final {
  TsumugiRestoreLayoutWriteKind kind{
      TsumugiRestoreLayoutWriteKind::gpt_primary_entries};
  std::uint64_t offset{};
  std::vector<std::byte> bytes;
};

using TsumugiRestoreTargetLayout =
    std::variant<clonecore::GptDisk, clonecore::MbrDisk>;

struct TsumugiWholeDiskRestoreLayoutPlan final {
  PartitionTableStyle style{PartitionTableStyle::gpt};
  std::uint64_t target_size_bytes{};
  std::uint32_t logical_sector_size{};
  // These ranges are cleared and read back before payload restoration. They
  // cover both possible table locations without touching the source disk.
  std::vector<clonecore::ByteRange> invalidation_ranges;
  // GPT entry arrays are staged after payload verification. No valid GPT
  // header is exposed by these writes.
  std::vector<TsumugiRestoreLayoutWrite> staged_writes;
  // This sequence is non-cancellable. GPT exposes the backup header, then the
  // protective MBR, and finally the primary header. MBR has one final sector.
  std::vector<TsumugiRestoreLayoutWrite> commit_writes;
  TsumugiRestoreTargetLayout target_layout;
};

// Adds one partition to reviewed unallocated space without changing the disk
// identity or any existing partition entry. published_writes use a preserving
// order; rollback_writes contain the exact bytes read from the same locked
// target handle before payload restoration.
struct TsumugiPreservingPartitionLayoutPlanV1 final {
  PartitionTableStyle style{PartitionTableStyle::gpt};
  std::uint64_t target_size_bytes{};
  std::uint32_t logical_sector_size{};
  std::uint32_t new_partition_number{};
  std::uint64_t new_partition_offset{};
  std::uint64_t new_partition_size{};
  std::vector<TsumugiRestoreLayoutWrite> published_writes;
  std::vector<TsumugiRestoreLayoutWrite> rollback_writes;
  TsumugiRestoreTargetLayout original_layout;
  TsumugiRestoreTargetLayout target_layout;
};

struct TsumugiShrinkWholeDiskRestoreLayoutPlanV1 final {
  // Concrete content/filesystem actions and source-to-target partition
  // mapping. Generated ESP/MSR entries intentionally have no source index.
  migrationcore::ShrinkMigrationPlan migration;
  // Target-only MBR/GPT metadata. It stays invalid until staged payloads have
  // passed readback and commit_writes are issued in their fixed order.
  TsumugiWholeDiskRestoreLayoutPlan metadata;
};

enum class TsumugiShrinkConstructionPurposeV1 : std::uint8_t {
  apply_file_image,
  recreate_empty_file_system,
  prepare_efi_system,
};

// A temporary, deliberately non-bootable GPT view used to format and populate
// exactly one final partition while the final GPT/MBR remains withheld.
// The temporary partition is always Microsoft Basic Data with the GPT
// no-default-drive-letter attribute. It has fresh construction-only GUIDs and
// never carries an ESP type, an Active flag, or final partition identifiers.
// retirement_ranges cover only the temporary GPT metadata sectors; unlike the
// initial whole-disk invalidation ranges they never overlap restored payload.
struct TsumugiShrinkConstructionLayoutPlanV1 final {
  TsumugiShrinkConstructionPurposeV1 purpose{
      TsumugiShrinkConstructionPurposeV1::apply_file_image};
  // Generated ESP construction has no source partition. final_target_number
  // is therefore the stable, unique transaction key for every purpose.
  std::optional<std::uint32_t> source_table_index;
  std::uint32_t final_target_number{};
  std::uint64_t target_offset{};
  std::uint64_t target_size{};
  TsumugiWholeDiskRestoreLayoutPlan temporary_metadata;
  std::vector<clonecore::ByteRange> retirement_ranges;
};

inline constexpr std::uint64_t
    kTsumugiConstructionNoDefaultDriveLetterAttribute =
        0x8000000000000000ULL;

// Builds target-specific whole-disk metadata from the authenticated snapshot.
// Exact/rescue offsets are preserved, unselected partitions are omitted, and
// disk/partition identifiers are regenerated. This function performs no I/O.
[[nodiscard]] clonecore::Result<TsumugiWholeDiskRestoreLayoutPlan>
make_tsumugi_whole_disk_restore_layout_plan_v1(
    const TsumugiManifest& manifest,
    std::uint64_t target_size_bytes,
    std::uint32_t target_sector_size,
    clonecore::IGuidGenerator& guid_generator,
    clonecore::IMbrSignatureGenerator& signature_generator,
    std::span<const std::uint32_t> disallowed_mbr_signatures = {});

// Builds a smaller-target reconstruction plan from the authenticated shrink
// manifest. It performs no I/O, never changes source offsets, and supports
// MBR preservation, GPT preservation, or MBR-to-GPT. GPT-to-MBR and an exact
// RAW payload combined with a logical-sector or partition-style conversion
// fail closed. windows_is_amd64 is explicit because Tsumugi v1 does not infer
// an architecture from filesystem contents during restore planning.
[[nodiscard]] clonecore::Result<
    TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
    const TsumugiManifest& manifest,
    std::uint64_t target_size_bytes,
    std::uint32_t target_sector_size,
    TsumugiManifestPartitionStyle target_style,
    bool windows_is_amd64,
    clonecore::IGuidGenerator& guid_generator,
    clonecore::IMbrSignatureGenerator& signature_generator,
    std::span<const std::uint32_t> disallowed_mbr_signatures = {});

// Builds one temporary GPT view per file-archive application, empty supported
// filesystem recreation, or generated ESP preparation target. The final
// target may itself be GPT or MBR; construction is always Basic Data GPT so
// BIOS/MBR Active and UEFI/ESP boot paths remain absent. Exact RAW and MSR
// targets deliberately receive no temporary construction layout. The returned
// metadata performs no I/O.
// A platform adapter must publish at most one plan at a time, keep the disk
// online only for the verified volume operation, then retire every listed
// metadata range before another plan or the final layout is exposed.
[[nodiscard]] clonecore::Result<
    std::vector<TsumugiShrinkConstructionLayoutPlanV1>>
make_tsumugi_shrink_construction_layout_plans_v1(
    const TsumugiShrinkWholeDiskRestoreLayoutPlanV1& final_plan,
    clonecore::IGuidGenerator& guid_generator);

}  // namespace ytec::imageformat

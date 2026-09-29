#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/clonecore/windows_volume_bitmap.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ytec::diskmodel {

struct ReidentifiedPhysicalClone final {
  DiskInfo source;
  DiskInfo target;
  clonecore::StableDiskIdentity source_identity;
  clonecore::StableDiskIdentity target_identity;
};

struct PhysicalCloneHandles final {
  ReidentifiedPhysicalClone observed;
  std::unique_ptr<clonecore::ISourceDiskReader> source;
  std::unique_ptr<clonecore::ITargetDiskWriter> target;
};

struct ReidentifiedPhysicalTarget final {
  DiskInfo target;
  clonecore::StableDiskIdentity target_identity;
};

struct PhysicalTargetHandle final {
  ReidentifiedPhysicalTarget observed;
  std::unique_ptr<clonecore::ITargetDiskWriter> target;
};

struct ReidentifiedReadOnlyDisk final {
  DiskInfo observed;
  clonecore::StableDiskIdentity identity;
};

struct ReadOnlyPhysicalDiskHandle final {
  ReidentifiedReadOnlyDisk observed;
  std::unique_ptr<clonecore::ISourceDiskReader> reader;
};

struct VolumePartitionLocation final {
  std::uint32_t table_index{};
  std::uint64_t offset_bytes{};
};

// Resolves an existing local parent path to one physical disk without opening
// it for write. Drive-letter paths only; UNC/device paths, reparse ancestors,
// and multi-disk volumes fail closed. This is also the shared local image-path
// trust boundary used before reading or writing a bundle.
[[nodiscard]] clonecore::Result<std::uint32_t>
query_single_disk_number_for_local_path(const std::wstring& candidate_path);

// The concrete implementation of this backend is intentionally not exposed.
// Destructive access must go through the verified functions below.
class IWindowsPhysicalDiskBackend {
 public:
  virtual ~IWindowsPhysicalDiskBackend() = default;

  [[nodiscard]] virtual clonecore::Result<
      std::unique_ptr<clonecore::ISourceDiskReader>>
  open_source(const DiskInfo& disk) = 0;

  [[nodiscard]] virtual clonecore::Result<
      std::unique_ptr<clonecore::ITargetDiskWriter>>
  open_offline_target(const DiskInfo& disk) = 0;

  [[nodiscard]] virtual clonecore::Status set_target_offline(
      const DiskInfo& disk,
      bool offline) = 0;

  // Changes only the non-persistent Windows disk attribute. The implementation
  // must set Persist=FALSE and must not modify partition or filesystem bytes.
  [[nodiscard]] virtual clonecore::Status set_source_read_only(
      const DiskInfo& disk,
      bool read_only) = 0;
};

// Maps the exact requested single-disk partition offsets to Volume GUID paths.
// It performs no mount or write. Every requested partition must map exactly
// once, and multi-disk volumes fail closed.
[[nodiscard]] clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>
query_windows_volume_bindings_by_offset(
    const DiskInfo& source_disk,
    std::span<const VolumePartitionLocation> expected_partitions);

[[nodiscard]] clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>
query_windows_volume_bitmap_bindings(
    const DiskInfo& source_disk,
    const clonecore::GptDisk& source_gpt);

[[nodiscard]] clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>
query_windows_volume_bitmap_bindings(
    const DiskInfo& source_disk,
    const clonecore::MbrDisk& source_mbr);

[[nodiscard]] clonecore::Result<ReidentifiedReadOnlyDisk>
reidentify_read_only_physical_disk(
    const clonecore::StableDiskIdentity& expected,
    IDiskInventoryProvider& inventory);

[[nodiscard]] clonecore::Result<ReadOnlyPhysicalDiskHandle>
open_verified_read_only_physical_disk(
    const clonecore::StableDiskIdentity& expected,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Result<ReadOnlyPhysicalDiskHandle>
open_verified_read_only_physical_disk_with_windows_apis(
    const clonecore::StableDiskIdentity& expected);

// Reidentifies the source before and after changing the non-persistent OS disk
// attribute. This is the WinPE source-isolation boundary; it performs no
// partition/filesystem write and does not trust a remembered disk number.
[[nodiscard]] clonecore::Status set_verified_source_read_only(
    const clonecore::StableDiskIdentity& expected_source,
    bool read_only,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Status
set_verified_source_read_only_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_source,
    bool read_only);

[[nodiscard]] clonecore::Result<ReidentifiedPhysicalClone>
reidentify_physical_clone_selection(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    IDiskInventoryProvider& inventory,
    bool require_target_same_or_larger = true);

// Destructive-boundary variant. The read-only selection function above is
// used by OperationCore before confirmation; this function additionally
// requires the exact target confirmation token.
[[nodiscard]] clonecore::Result<ReidentifiedPhysicalClone>
reidentify_physical_clone(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory,
    bool require_target_same_or_larger = true);

[[nodiscard]] clonecore::Status set_verified_target_offline(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    bool offline,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Result<PhysicalCloneHandles>
open_verified_physical_clone(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

// Target-only destructive boundary used by verified image restore. Every
// entry point performs a fresh full inventory scan, stable-identity check,
// system/removable/read-only/sector gate, and exact two-step confirmation.
[[nodiscard]] clonecore::Result<ReidentifiedPhysicalTarget>
reidentify_physical_target(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory);

[[nodiscard]] clonecore::Status
set_verified_physical_target_offline(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    bool offline,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Result<PhysicalTargetHandle>
open_verified_physical_target(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    IDiskInventoryProvider& inventory,
    IWindowsPhysicalDiskBackend& backend);

[[nodiscard]] clonecore::Status set_verified_target_offline_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    bool offline);

[[nodiscard]] clonecore::Result<PhysicalCloneHandles>
open_verified_physical_clone_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_source,
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation);

[[nodiscard]] clonecore::Status
set_verified_physical_target_offline_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation,
    bool offline);

[[nodiscard]] clonecore::Result<PhysicalTargetHandle>
open_verified_physical_target_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_target,
    const clonecore::TargetConfirmation& confirmation);

}  // namespace ytec::diskmodel

#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/gpt.h"
#include "ytec/clonecore/mbr.h"
#include "ytec/windowsapp/online_direct_shrink_clone.h"
#include "ytec/windowsapp/windows_shrink_restore_platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ytec::windowsapp {

struct WindowsDirectShrinkOwnedWimEvidence final {
  std::uint32_t source_table_index{};
  std::uint64_t length{};
  imageformat::Sha256Digest hash{};
  bool sealed_without_write_or_delete_sharing{};
  bool flushed{};
  bool complete_read_back_hash_verified{};
};

struct WindowsDirectShrinkBootFinalizationEvidence final {
  bool microsoft_signed_bcdboot{};
  bool fresh_bcd_store_read_back_verified{};
  bool construction_gpt_non_bootable_verified{};
  bool efi_ownership_safe_before_mount{};
  bool efi_ownership_revalidated_before_mutation{};
  bool microsoft_boot_namespace_read_back_verified{};
  bool temporary_mounts_released{};
  bool final_target_reidentified{};
  bool partition_layout_unchanged{};
  bool nvram_unchanged{};
  bool legacy_bios{};
  bool exact_target_volume_extents{};
  bool target_only_reconstruction{};
};

// Direct-shrink boot reconstruction never discovers its system target by
// partition type. The GPT construction uses BasicData + NO_DRIVE_LETTER; the
// MBR construction uses inactive 0x07/0x27 primaries and a zero bootstrap.
// The caller supplies exact, freshly rebound Volume GUID roots and immutable
// target extents for Windows and the construction system volume. Production
// mounts only those volumes, invokes signed BCDBoot with explicit /s and
// firmware, releases every mount, and leaves source/NVRAM unchanged.
struct WindowsDirectShrinkBootFinalizationRequest final {
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  clonecore::TargetConfirmation confirmation;
  std::uint32_t expected_target_disk_number{};
  std::uint32_t expected_windows_partition_number{};
  std::uint64_t expected_windows_partition_offset{};
  std::uint64_t expected_windows_partition_size{};
  std::uint32_t expected_system_partition_number{};
  std::uint64_t expected_system_partition_offset{};
  std::uint64_t expected_system_partition_size{};
  std::uint32_t expected_mbr_disk_signature{};
  std::wstring windows_volume_root;
  std::wstring system_volume_root;
  bootrepair::BcdBootFirmware firmware{bootrepair::BcdBootFirmware::uefi};
};

struct WindowsDirectShrinkWinReFinalizationRequest final {
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  clonecore::TargetConfirmation confirmation;
  std::uint32_t expected_target_disk_number{};
  std::uint32_t expected_windows_partition_number{};
  std::uint64_t expected_windows_partition_offset{};
  std::uint64_t expected_windows_partition_size{};
  std::uint32_t expected_recovery_partition_number{};
  std::uint64_t expected_recovery_partition_offset{};
  std::uint64_t expected_recovery_partition_size{};
  migrationcore::MigrationPartitionStyle expected_partition_style{
      migrationcore::MigrationPartitionStyle::gpt};
  std::uint32_t expected_mbr_disk_signature{};
  std::wstring windows_volume_root;
  std::wstring recovery_volume_root;
};

struct WindowsDirectShrinkWinReFinalizationEvidence final {
  std::uint32_t registered_partition_number{};
  std::uint64_t registered_image_size_bytes{};
  bool microsoft_signed_reagentc{};
  bool cloned_source_registration_disabled{};
  bool candidate_identity_locked{};
  bool fixed_setreimage_arguments{};
  bool fixed_enable_arguments{};
  bool target_revalidated_before_each_mutation_and_diagnostic{};
  bool read_only_reinspection_completed{};
  bool registered_location_matches_expected_target{};
  bool registered_image_present{};
  bool temporary_mounts_released{};
};

// Target-owned NTFS file boundary used by signed System32 DISM.  The
// production implementation creates one owned, non-reparse directory on the
// temporary staging volume, fixes the WIM by file identity, hashes it fully,
// closes every handle before the target is offlined, and reopens/re-hashes it
// before apply or exact deletion.
class IWindowsDirectShrinkOwnedWimStore {
 public:
  virtual ~IWindowsDirectShrinkOwnedWimStore() = default;

  [[nodiscard]] virtual clonecore::Result<
      WindowsDirectShrinkOwnedWimEvidence>
  capture_and_seal(
      std::uint32_t source_table_index,
      const std::wstring& snapshot_device_path,
      std::uint64_t archive_upper_bound_bytes) = 0;

  [[nodiscard]] virtual clonecore::Status apply_locked_and_reverify(
      std::uint32_t source_table_index,
      const imageformat::Sha256Digest& expected_hash,
      const std::wstring& target_volume_root) = 0;

  [[nodiscard]] virtual clonecore::Status discard_exact(
      std::uint32_t source_table_index,
      const imageformat::Sha256Digest& expected_hash) = 0;
};

using WindowsDirectShrinkOwnedWimStoreFactory = std::function<
    clonecore::Result<std::unique_ptr<
        IWindowsDirectShrinkOwnedWimStore>>(
        const std::wstring& staging_volume_root,
        std::uint64_t staging_capacity_bytes,
        std::uint64_t maximum_archive_upper_bound_bytes,
        const clonecore::DiskOperationCallbacks& callbacks)>;

using WindowsDirectShrinkPlatformReidentifier = std::function<
    clonecore::Result<diskmodel::ReidentifiedPhysicalClone>(
        const clonecore::StableDiskIdentity&,
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&)>;

using WindowsDirectShrinkBootFinalizer = std::function<clonecore::Result<
    WindowsDirectShrinkBootFinalizationEvidence>(
    const WindowsDirectShrinkBootFinalizationRequest&)>;

using WindowsDirectShrinkWinReFinalizer = std::function<clonecore::Result<
    WindowsDirectShrinkWinReFinalizationEvidence>(
    const WindowsDirectShrinkWinReFinalizationRequest&)>;

struct WindowsDirectShrinkClonePlatformRequest final {
  clonecore::TargetConfirmation confirmation;
  clonecore::DiskOperationCallbacks callbacks;
};

// Test seam for the production state machine.  All values are supplied at
// construction, which must remain free of disk I/O.  begin_target_owned_staging()
// performs the first read-only observation and only then may invalidate the
// target.
struct WindowsDirectShrinkClonePlatformDependencies final {
  std::unique_ptr<IWindowsTsumugiShrinkRestorePlatformIo> target_io;
  std::unique_ptr<clonecore::IGuidGenerator> guid_generator;
  WindowsDirectShrinkOwnedWimStoreFactory make_wim_store;
  imageformat::Sha256Digest connection_instance_hash{};
  WindowsDirectShrinkPlatformReidentifier reidentify_confirmed;
  WindowsDirectShrinkBootFinalizer finalize_boot;
  WindowsDirectShrinkWinReFinalizer finalize_winre;
  WindowsDirectShrinkMbrSafetyObserver observe_mbr_safety;
};

// Production-safe slice: preserve-style GPT/MBR or reviewed target-only
// MBR-to-GPT,
// 512-byte sectors, NTFS content, generated ESP/MSR, optional single recovery,
// target-owned non-overlapping staging and verified NTFS extension for
// automatic or reviewed single-data surplus. Temporary and hidden-final GPTs
// reuse the final target's
// freshly generated disk/partition GUIDs but expose all tasks as non-bootable
// BasicData + NO_DRIVE_LETTER; only final publication changes type/name/attrs.
// Non-NTFS payloads and every unproved role/layout fail during factory
// construction before the Win32 I/O seam is called.
[[nodiscard]] clonecore::Result<std::unique_ptr<
    IWindowsDirectShrinkClonePlatform>>
make_windows_direct_shrink_clone_platform(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const WindowsDirectShrinkClonePlatformRequest& request);

[[nodiscard]] clonecore::Result<std::unique_ptr<
    IWindowsDirectShrinkClonePlatform>>
make_windows_direct_shrink_clone_platform_with_dependencies(
    const WindowsDirectShrinkClonePlan& plan,
    const diskmodel::ReidentifiedPhysicalClone& observed,
    const WindowsDirectShrinkClonePlatformRequest& request,
    WindowsDirectShrinkClonePlatformDependencies dependencies);

}  // namespace ytec::windowsapp

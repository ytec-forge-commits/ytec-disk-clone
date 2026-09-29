#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/windowsapp/rescue_media_storage.h"
#include "ytec/windowsapp/usb_volume_mapping.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

enum class RescueUsbInspectionState : std::uint8_t {
  not_started,
  scanning,
  unknown_media,
  verified_owned,
  blocked,
};

struct RescueUsbOwnershipManifest final {
  std::uint32_t schema_version{};
  std::wstring purpose;
  std::wstring media_id;
  std::wstring boot_file_system;
  RescueUsbDataFileSystem data_file_system{
      RescueUsbDataFileSystem::ntfs};
  std::uint64_t boot_partition_bytes{};
  RescueUsbCanonicalLayout canonical_layout;
  RescueMediaTreeIdentity owned_boot_tree_identity;
  std::vector<RescueMediaFileFingerprint> owned_boot_files;
  std::vector<std::wstring> owned_boot_directories;
};

struct RescueUsbInspectionCacheKey final {
  clonecore::StableDiskIdentity target;
  RescueUsbCanonicalLayout canonical_layout;
  RescueUsbProvisioningMode mode{
      RescueUsbProvisioningMode::preserve_data_refresh};
  RescueUsbDataFileSystem data_file_system{
      RescueUsbDataFileSystem::ntfs};
};

// The evidence intentionally retains no private data filename. Only the
// bounded aggregate identity is allowed to cross the read-only inspector.
struct RescueUsbOwnedMediaEvidence final {
  RescueUsbInspectionCacheKey cache_key;
  RescueUsbOwnedMediaInspection owned_media;
  bool physical_write_started{};
};

struct RescueUsbOwnedVolumeRoots final {
  clonecore::StableDiskIdentity target_identity;
  RescueUsbCanonicalLayout canonical_layout;
  wchar_t boot_drive_letter{};
  std::wstring boot_root;
  wchar_t data_drive_letter{};
  std::wstring data_root;
  RescueUsbDataFileSystem data_file_system{
      RescueUsbDataFileSystem::ntfs};
  bool physical_write_started{};
};

struct RescueUsbInspectionResult final {
  RescueUsbInspectionState state{RescueUsbInspectionState::not_started};
  std::optional<RescueUsbOwnedMediaEvidence> evidence;
  std::wstring message;
  bool physical_write_started{};
};

// Strict schema-v2 parser. Unknown/duplicate fields, invalid UTF-8, JSON
// escapes, integer overflow and all configured tree bounds fail closed.
[[nodiscard]] clonecore::Result<RescueUsbOwnershipManifest>
parse_rescue_usb_ownership_manifest(
    std::span<const std::byte> utf8_json);

// Pure composition seam used by the Windows adapter and synthetic tests.
// marker_bytes must be the exact 36-byte ASCII media id. The returned cache
// key binds stable identity, complete canonical layout, refresh mode and FS.
[[nodiscard]] clonecore::Result<RescueUsbOwnedMediaEvidence>
build_rescue_usb_owned_media_evidence(
    const diskmodel::DiskInfo& target,
    const RescueUsbOwnershipManifest& manifest,
    std::span<const std::byte> marker_bytes,
    std::vector<RescueMediaFileFingerprint> observed_boot_files,
    std::vector<std::wstring> observed_boot_directories,
    const RescueMediaTreeIdentity& observed_data_tree_identity);

// Revalidates a cached inspection against the currently selected disk and UI
// selection. Any identity, full-layout, mode or filesystem drift fails.
[[nodiscard]] clonecore::Status
validate_rescue_usb_inspection_target_binding(
    const RescueUsbOwnedMediaEvidence& evidence,
    const diskmodel::DiskInfo& selected_target);

[[nodiscard]] clonecore::Status validate_rescue_usb_inspection_evidence(
    const RescueUsbOwnedMediaEvidence& evidence,
    const diskmodel::DiskInfo& selected_target,
    RescueUsbProvisioningMode selected_mode,
    RescueUsbDataFileSystem selected_file_system);

// Binds a fresh read-only ownership scan to the immutable plan reviewed by
// the user.  Private paths never cross this seam; only the complete owned boot
// evidence and the private data-tree aggregate are compared by value.
[[nodiscard]] clonecore::Status
validate_rescue_usb_fresh_inspection_for_plan(
    const RescueUsbOwnedMediaEvidence& fresh_evidence,
    const RescueUsbStoragePlan& reviewed_plan);

// Resolves both partitions from injected read-only volume extents. It accepts
// only one boot and one data volume of the completed 4 GiB + remaining layout.
[[nodiscard]] clonecore::Result<RescueUsbOwnedVolumeRoots>
resolve_rescue_usb_owned_volume_roots(
    const diskmodel::DiskInfo& target,
    std::span<const DriveLetterVolume> volumes,
    std::wstring boot_file_system,
    std::wstring data_file_system);

// Production read-only adapter. It never locks, dismounts, formats or opens a
// physical disk for write. Missing marker+manifest means unknown media;
// partial/invalid ownership, filesystem errors and observed drift are blocked.
[[nodiscard]] RescueUsbInspectionResult
inspect_rescue_usb_owned_media_with_windows_apis(
    const diskmodel::DiskInfo& target,
    const std::atomic_bool* cancellation_requested = nullptr);

// Re-enumerates the reviewed physical target and repeats the complete
// read-only ownership scan.  This is intended for the background worker
// immediately before the local MediaBuilder is allowed to write.
[[nodiscard]] clonecore::Status
reinspect_rescue_usb_storage_plan_with_windows_apis(
    const RescueUsbStoragePlan& reviewed_plan,
    const std::atomic_bool* cancellation_requested = nullptr);

}  // namespace ytec::windowsapp

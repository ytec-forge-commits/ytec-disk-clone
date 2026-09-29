#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/windowsapp/rescue_media_storage.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct DriveLetterDiskExtent final {
  std::uint32_t disk_number{};
  std::uint64_t starting_offset{};
  std::uint64_t length{};
};

struct DriveLetterVolume final {
  wchar_t drive_letter{};
  std::vector<DriveLetterDiskExtent> extents;
};

struct RescueUsbDriveLetterResolution final {
  clonecore::StableDiskIdentity target_identity;
  wchar_t drive_letter{};
  std::wstring root_path;
  std::uint32_t partition_number{};
  std::uint64_t extent_start{};
  std::uint64_t extent_length{};
  bool drive_letter_was_unassigned{};
  bool physical_write_started{};
};

enum class RescueUsbDestinationVerificationPoint : std::uint8_t {
  before_write,
  after_write,
};

// Mode-aware resolver used by the immutable reviewed storage plan. Before an
// initialization it proposes an unused letter regardless of the current basic
// partition count. Before refresh and after either mode it requires the exact
// 4 GiB boot partition mapping. It performs no physical write.
[[nodiscard]] clonecore::Result<RescueUsbDriveLetterResolution>
resolve_rescue_usb_drive_letter_for_plan(
    const diskmodel::DiskInfo& target,
    std::span<const DriveLetterVolume> volumes,
    const RescueUsbStoragePlan& reviewed_plan,
    RescueUsbDestinationVerificationPoint verification_point);

// Resolves a previously inventoried single-partition USB, or the exact 4 GiB
// boot partition of a specification-10.3 two-partition Y-TEC layout, to its
// local drive letter. For a zero-partition RAW/GPT/MBR USB, it instead
// reserves one currently unused letter for later target-bound initialization.
// The supplied volume list is read-only evidence and is injectable for tests.
// Spanned, ambiguous, out-of-range, system, non-USB, read-only, offline and
// unstable targets fail closed. This function never opens a device.
[[nodiscard]] clonecore::Result<RescueUsbDriveLetterResolution>
resolve_rescue_usb_drive_letter(
    const diskmodel::DiskInfo& target,
    std::span<const DriveLetterVolume> volumes);

// Enumerates every occupied local drive letter and obtains physical extents
// for fixed/removable volumes with zero desired access. Other occupied letters
// are retained without extents so an unpartitioned USB never reserves them.
// No lock, dismount, format or write IOCTL is sent.
[[nodiscard]] clonecore::Result<std::vector<DriveLetterVolume>>
enumerate_windows_drive_letter_volumes_read_only();

// Windows adapter for the pure resolver. It performs only the read-only
// enumeration above and returns physical_write_started=false.
[[nodiscard]] clonecore::Result<RescueUsbDriveLetterResolution>
resolve_windows_rescue_usb_drive_letter_read_only(
    const diskmodel::DiskInfo& target);

[[nodiscard]] clonecore::Result<RescueUsbDriveLetterResolution>
resolve_windows_rescue_usb_drive_letter_for_plan_read_only(
    const diskmodel::DiskInfo& target,
    const RescueUsbStoragePlan& reviewed_plan,
    RescueUsbDestinationVerificationPoint verification_point);

}  // namespace ytec::windowsapp

#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/sha256.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

inline constexpr std::uint64_t kRescueUsbMinimumBytes =
    8ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kRescueUsbBootPartitionBytes =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kRescueUsbOwnershipSchemaVersion = 2U;
inline constexpr std::wstring_view kRescueUsbOwnershipPurpose =
    L"Y-TEC Tsumugi Drive rescue USB ownership";
inline constexpr std::wstring_view kRescueUsbMarkerRelativePath =
    L"YtecDiskClone\\rescue-media-id.txt";
inline constexpr std::wstring_view kRescueUsbManifestRelativePath =
    L"YtecDiskClone\\rescue-media-manifest.json";
inline constexpr std::wstring_view kRescueUsbTransactionRelativePath =
    L".ytec-rescue-transaction";
inline constexpr std::uint64_t kRescueUsbMaximumOwnershipManifestBytes =
    64ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kRescueUsbMaximumBootTreeEntryCount = 65'536U;
inline constexpr std::uint64_t kRescueUsbMaximumBootPathCharacters =
    8ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kRescueUsbMaximumBootLogicalBytes =
    kRescueUsbBootPartitionBytes;
inline constexpr std::size_t kRescueUsbMaximumDataTreeEntryCount = 262'144U;
inline constexpr std::uint64_t kRescueUsbMaximumDataPathCharacters =
    64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kRescueUsbMaximumDataLogicalBytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;

enum class RescueUsbProvisioningMode : std::uint8_t {
  initialize_all,
  preserve_data_refresh,
};

enum class RescueUsbDataFileSystem : std::uint8_t {
  ntfs,
  exfat,
};

struct RescueUsbPartitionLayoutEntry final {
  std::uint32_t number{};
  diskmodel::PartitionStyle style{diskmodel::PartitionStyle::unknown};
  std::wstring type;
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
  bool bootable{};

  bool operator==(const RescueUsbPartitionLayoutEntry&) const = default;
};

// A complete, order-normalized partition-table observation. Disk number is
// deliberately absent: stable identity is bound separately and both values
// must match at every later write boundary.
struct RescueUsbCanonicalLayout final {
  diskmodel::PartitionStyle disk_style{diskmodel::PartitionStyle::unknown};
  std::vector<RescueUsbPartitionLayoutEntry> partitions;

  bool operator==(const RescueUsbCanonicalLayout&) const = default;
};

struct RescueMediaFileFingerprint final {
  std::wstring relative_path;
  std::uint64_t length{};
  imageformat::Sha256Digest sha256{};
  bool reparse_point{};
  std::uint32_t hard_link_count{1U};

  bool operator==(const RescueMediaFileFingerprint&) const = default;
};

// Privacy-preserving identity of one bounded canonical filesystem tree.  The
// digest binds normalized relative paths, directory entries, file lengths and
// file SHA-256 values, but no relative path escapes this value object.  It is
// therefore safe to retain in the immutable reviewed plan and to summarize in
// diagnostics without disclosing user filenames.
struct RescueMediaTreeIdentity final {
  std::uint64_t entry_count{};
  std::uint64_t file_count{};
  std::uint64_t total_path_characters{};
  std::uint64_t total_logical_bytes{};
  imageformat::Sha256Digest root_digest{};

  bool operator==(const RescueMediaTreeIdentity&) const = default;
};

// The filesystem adapter excludes the ownership manifest itself from
// observed_boot_files because a JSON document cannot contain its own digest.
// Every other boot-volume file, including rescue-media-id.txt, must be listed
// by the manifest and match observed_boot_files exactly.
struct RescueUsbOwnedMediaInspection final {
  std::uint32_t schema_version{};
  std::wstring purpose;
  std::wstring media_id;
  std::wstring boot_file_system;
  RescueUsbDataFileSystem data_file_system{
      RescueUsbDataFileSystem::ntfs};
  std::uint64_t boot_partition_bytes{};
  RescueUsbCanonicalLayout manifest_layout;
  RescueMediaTreeIdentity manifest_owned_boot_tree_identity;
  std::vector<RescueMediaFileFingerprint> manifest_owned_boot_files;
  std::vector<std::wstring> manifest_owned_boot_directories;
  RescueMediaTreeIdentity observed_boot_tree_identity;
  std::vector<RescueMediaFileFingerprint> observed_boot_files;
  std::vector<std::wstring> observed_boot_directories;
  RescueMediaTreeIdentity observed_data_tree_identity;

  bool operator==(const RescueUsbOwnedMediaInspection&) const = default;
};

struct RescueUsbStoragePlanInput final {
  const diskmodel::DiskInfo* target{};
  RescueUsbProvisioningMode mode{
      RescueUsbProvisioningMode::initialize_all};
  RescueUsbDataFileSystem data_file_system{
      RescueUsbDataFileSystem::ntfs};
  const RescueUsbOwnedMediaInspection* owned_media{};
};

struct RescueUsbStoragePlan final {
  RescueUsbProvisioningMode mode{
      RescueUsbProvisioningMode::initialize_all};
  RescueUsbDataFileSystem data_file_system{
      RescueUsbDataFileSystem::ntfs};
  clonecore::StableDiskIdentity expected_target;
  RescueUsbCanonicalLayout reviewed_layout;
  std::uint64_t planned_boot_partition_bytes{
      kRescueUsbBootPartitionBytes};
  bool planned_data_partition_uses_remaining_space{true};
  std::optional<RescueUsbOwnedMediaInspection> reviewed_owned_media;
  imageformat::Sha256Digest review_binding_digest{};
  bool physical_write_started{};
};

[[nodiscard]] RescueUsbCanonicalLayout
make_rescue_usb_canonical_layout(const diskmodel::DiskInfo& disk);

// Cross-process binding used by the audited PowerShell adapter. The digest is
// computed from the same order-normalized number/style/type/offset/size/
// bootable vector held by the immutable reviewed plan.
[[nodiscard]] clonecore::Result<imageformat::Sha256Digest>
make_rescue_usb_canonical_layout_digest(
    const RescueUsbCanonicalLayout& layout);

// Pure read-only planner for specification 10.3. Initialization accepts an
// 8 GiB-or-larger basic RAW/MBR/GPT removable USB and replaces its complete
// layout only after uppercase OK. Refresh accepts exactly the owned
// two-partition Y-TEC layout and binds the complete boot tree plus the private
// data-tree identity by value.
[[nodiscard]] clonecore::Result<RescueUsbStoragePlan>
plan_rescue_usb_storage(const RescueUsbStoragePlanInput& input);

// Recomputes the immutable plan binding and requires exact stable identity,
// full canonical layout and (for refresh) ownership/file-tree evidence.
// This function is pure and performs no device or filesystem access.
[[nodiscard]] clonecore::Status validate_rescue_usb_storage_plan(
    const RescueUsbStoragePlan& plan,
    const diskmodel::DiskInfo& observed_target,
    const RescueUsbOwnedMediaInspection* observed_owned_media);

// Verifies only the immutable in-memory review binding and fixed 4 GiB plus
// remaining-space recipe. Device/layout re-observation is still mandatory at
// each destructive seam and is performed by validate_rescue_usb_storage_plan
// or the audited PowerShell adapter.
[[nodiscard]] clonecore::Status validate_rescue_usb_storage_plan_binding(
    const RescueUsbStoragePlan& plan);

// Validates the completed specification-10.3 MBR recipe without consulting a
// marker or manifest. Ownership is a separate mandatory gate for refresh.
[[nodiscard]] clonecore::Status validate_rescue_usb_completed_layout(
    const RescueUsbCanonicalLayout& layout,
    std::uint64_t disk_size);

// Both snapshots are canonicalized and compared by path, length and SHA-256.
// Reparse points, hard links, invalid relative paths and duplicates fail
// closed even when the two untrusted vectors happen to be equal.
[[nodiscard]] clonecore::Status validate_rescue_usb_data_unchanged(
    const std::vector<RescueMediaFileFingerprint>& before,
    const std::vector<RescueMediaFileFingerprint>& after);

// Canonical tree builders used by the production read-only inspector.  Input
// paths remain caller-owned and are never copied into the returned identity.
// Boot entries permit the 4 GiB owned area limits; private data entries use
// the larger, still bounded, preservation limits.
[[nodiscard]] clonecore::Result<RescueMediaTreeIdentity>
make_rescue_usb_boot_tree_identity(
    const std::vector<RescueMediaFileFingerprint>& files,
    const std::vector<std::wstring>& directories);

[[nodiscard]] clonecore::Result<RescueMediaTreeIdentity>
make_rescue_usb_private_data_tree_identity(
    const std::vector<RescueMediaFileFingerprint>& files,
    const std::vector<std::wstring>& directories);

[[nodiscard]] clonecore::Status validate_rescue_usb_data_unchanged(
    const RescueMediaTreeIdentity& before,
    const RescueMediaTreeIdentity& after);

[[nodiscard]] std::wstring_view rescue_usb_provisioning_mode_name(
    RescueUsbProvisioningMode mode) noexcept;

[[nodiscard]] std::wstring_view rescue_usb_data_file_system_name(
    RescueUsbDataFileSystem file_system) noexcept;

}  // namespace ytec::windowsapp

#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/tsumugi_image_service.h"
#include "ytec/imageformat/windows_tsumugi_destination.h"
#include "ytec/imageformat/windows_tsumugi_rescue_staging.h"
#include "ytec/vssrequester/online_tsumugi_backup.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

struct TsumugiSourceIdentityHashes final {
  imageformat::Sha256Digest model{};
  imageformat::Sha256Digest serial{};
  imageformat::Sha256Digest locked_state{};
};

// These two values are independent from the partition snapshot and can be
// recomputed by restore preflight from a newly inventoried disk.
[[nodiscard]] clonecore::Result<imageformat::Sha256Digest>
hash_tsumugi_source_model(std::wstring_view model);

[[nodiscard]] clonecore::Result<imageformat::Sha256Digest>
hash_tsumugi_source_serial(
    std::string_view serial_suffix,
    std::wstring_view device_instance_id);

// Canonical privacy-preserving source identifiers shared by image creation
// and restore preflight. Disk number is deliberately excluded because it is
// not stable across reconnect/reboot. All strings are length-delimited and
// domain-separated before hashing; no clear model/serial is stored here.
[[nodiscard]] clonecore::Result<TsumugiSourceIdentityHashes>
make_tsumugi_source_identity_hashes(
    const clonecore::StableDiskIdentity& source,
    std::uint32_t physical_sector_size,
    std::span<const std::byte> canonical_partition_snapshot);

struct OnlineImageCreateRequest final {
  diskmodel::DiskInfo selected_source;
  // Empty preserves the legacy whole-disk path. Non-empty values are
  // one-based PartitionNumber values from selected_source and are normalized
  // again after stable reidentification. Required Windows regions are forced
  // by DiskModel and cannot be omitted by a caller.
  std::vector<std::uint32_t> selected_partition_numbers;
  std::wstring final_path;
  bool administrator{};
  std::uint32_t windows_major{};
  std::uint32_t windows_minor{};
  std::uint32_t windows_build{};
  std::string windows_architecture;
  std::string created_utc;
  std::string app_version;
  std::optional<std::string_view> encryption_password;
  imageformat::TsumugiCreateVerificationMode verification_mode{
      imageformat::TsumugiCreateVerificationMode::complete};
  bool replace_existing{};
  vssrequester::AsyncWaitOptions async_wait;
  clonecore::DiskOperationCallbacks callbacks;
  vssrequester::VssDiffAreaReviewCallback diff_area_review_callback;
  const clonecore::Logger* logger{};
};

using OnlineImageReadOnlyDiskOpener = std::function<clonecore::Result<
    diskmodel::ReadOnlyPhysicalDiskHandle>(
    const clonecore::StableDiskIdentity&)>;

using OnlineImageGptVolumeBindingQuery = std::function<clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>(
    const diskmodel::DiskInfo&,
    const clonecore::GptDisk&,
    std::span<const std::uint32_t>)>;

using OnlineImageMbrVolumeBindingQuery = std::function<clonecore::Result<
    std::vector<clonecore::VolumeBitmapBinding>>(
    const diskmodel::DiskInfo&,
    const clonecore::MbrDisk&,
    std::span<const std::uint32_t>)>;

using OnlineImageDestinationFileSystemQuery = std::function<clonecore::Result<
    imageformat::TsumugiImageStorageFileSystem>(const std::wstring&)>;

using OnlineImageDestinationValidator = std::function<clonecore::Status(
    const imageformat::WindowsTsumugiDestinationGuardRequest&)>;

using OnlineImageExecutor = std::function<clonecore::Result<
    vssrequester::OnlineTsumugiBackupReport>(
    const vssrequester::WindowsOnlineTsumugiBackupRequest&)>;

struct OnlineImageCreateDependencies final {
  OnlineImageReadOnlyDiskOpener open_read_only_disk;
  OnlineImageGptVolumeBindingQuery query_gpt_bindings;
  OnlineImageMbrVolumeBindingQuery query_mbr_bindings;
  OnlineImageDestinationFileSystemQuery query_destination_file_system;
  OnlineImageDestinationValidator validate_destination;
  OnlineImageExecutor execute_backup;
};

using WindowsDataRescueImageStagingFactory = std::function<clonecore::Result<
    std::unique_ptr<imageformat::ITsumugiRescueStagingSession>>(
    const imageformat::WindowsTsumugiRescueStagingRequest&)>;

struct WindowsDataRescueImageCreateDependencies final {
  OnlineImageReadOnlyDiskOpener open_read_only_disk;
  OnlineImageDestinationFileSystemQuery query_destination_file_system;
  OnlineImageDestinationValidator validate_destination;
  WindowsDataRescueImageStagingFactory make_rescue_staging;
};

struct WindowsDataRescueImageCreateReport final {
  clonecore::StableDiskIdentity source_identity;
  diskmodel::PartitionStyle source_partition_style{
      diskmodel::PartitionStyle::unknown};
  std::uint32_t imaged_partition_count{};
  std::uint64_t logical_payload_bytes{};
  bool source_was_read_only_or_offline{};
  imageformat::TsumugiRescueImageCreateReport rescue;
};

// Creates one exact-mode .tsumugi image directly from the running Windows
// host. NTFS payloads are read only from one VSS Snapshot Set. The physical
// source handle may supply only static ESP/Recovery regions; live FAT32/exFAT
// data partitions fail closed and must be imaged from WinPE.
[[nodiscard]] clonecore::Result<vssrequester::OnlineTsumugiBackupReport>
execute_online_image_create(
    const OnlineImageCreateRequest& request,
    const OnlineImageCreateDependencies& dependencies);

// Product adapter. It performs no UAC request and uses only the audited
// Windows disk, volume, destination and VSS implementations.
[[nodiscard]] clonecore::Result<vssrequester::OnlineTsumugiBackupReport>
execute_online_image_create_with_windows_apis(
    const OnlineImageCreateRequest& request);

// Creates a rescue-classified .tsumugi from a non-system data disk whose
// read-only or offline attribute was already established before this call.
// Windows never changes the source attribute. The failing source is rescued
// once into an owned local staging session and is not reread by the container
// pass. System disks must use the WinPE product path.
[[nodiscard]] clonecore::Result<WindowsDataRescueImageCreateReport>
execute_windows_data_rescue_image_create(
    const OnlineImageCreateRequest& request,
    const WindowsDataRescueImageCreateDependencies& dependencies);

[[nodiscard]] clonecore::Result<WindowsDataRescueImageCreateReport>
execute_windows_data_rescue_image_create_with_windows_apis(
    const OnlineImageCreateRequest& request);

}  // namespace ytec::windowsapp

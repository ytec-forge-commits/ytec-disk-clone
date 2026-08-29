#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/tsumugi_image_service.h"
#include "ytec/imageformat/windows_tsumugi_destination.h"
#include "ytec/imageformat/windows_tsumugi_rescue_staging.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::winpeapp {

struct DirectImageCreateRequest final {
  // The immutable selection reviewed in the current PE session. The
  // controller converts it to StableDiskIdentity before any source-state
  // transition and never rebuilds the expected identity from a disk number.
  diskmodel::DiskInfo selected_source;
  // Empty means whole disk. Non-empty values are one-based PartitionNumber
  // values from selected_source. Exact mode binds the normalized selection to
  // the manifest and (for persistent creation) the durable resume plan.
  std::vector<std::uint32_t> selected_partition_numbers;
  std::wstring final_path;
  std::string created_utc;
  std::string app_version;
  std::optional<std::string_view> encryption_password;
  imageformat::TsumugiCreateVerificationMode verification_mode{
      imageformat::TsumugiCreateVerificationMode::complete};
  bool replace_existing{};
  bool rescue_mode{};
  clonecore::DiskOperationCallbacks callbacks;
};

struct DirectImageCreateReport final {
  clonecore::StableDiskIdentity source_identity;
  diskmodel::PartitionStyle source_partition_style{
      diskmodel::PartitionStyle::unknown};
  std::uint32_t imaged_partition_count{};
  std::uint64_t logical_payload_bytes{};
  bool source_read_only_verified{};
  // WinPE deliberately keeps the source protected after success. A later
  // operation must explicitly reselect and reidentify it before changing the
  // attribute; this controller never silently releases source protection.
  bool source_left_read_only{};
  bool layout_revalidated_before_commit{};
  bool rescue_mode{};
  std::optional<clonecore::RescueRawCopyReport> rescue;
  imageformat::TsumugiImageCreateReport image;
};

using DirectImageSourceReadOnlySetter = std::function<clonecore::Status(
    const clonecore::StableDiskIdentity&,
    bool)>;

using DirectImageReadOnlyDiskOpener = std::function<clonecore::Result<
    diskmodel::ReadOnlyPhysicalDiskHandle>(
    const clonecore::StableDiskIdentity&)>;

using DirectImageDestinationFileSystemQuery = std::function<
    clonecore::Result<imageformat::TsumugiImageStorageFileSystem>(
        const std::wstring&)>;

using DirectImageDestinationValidator = std::function<clonecore::Status(
    const imageformat::WindowsTsumugiDestinationGuardRequest&)>;

using DirectImageRescueStagingFactory = std::function<clonecore::Result<
    std::unique_ptr<imageformat::ITsumugiRescueStagingSession>>(
    const imageformat::WindowsTsumugiRescueStagingRequest&)>;

struct DirectImageCreateDependencies final {
  DirectImageSourceReadOnlySetter set_source_read_only;
  DirectImageReadOnlyDiskOpener open_read_only_source;
  DirectImageDestinationFileSystemQuery query_destination_file_system;
  DirectImageDestinationValidator validate_destination;
  DirectImageRescueStagingFactory make_rescue_staging;
};

// Creates one exact- or explicitly selected rescue-mode .tsumugi directly in
// the current WinPE session.
// There is no reservation file or deferred job. The source disk is made
// read-only and reidentified, all selected partitions are read through one
// immutable source session, and the verified adjacent .partial is promoted
// only after a final layout and destination recheck.
[[nodiscard]] clonecore::Result<DirectImageCreateReport>
execute_direct_image_create(
    const DirectImageCreateRequest& request,
    const DirectImageCreateDependencies& dependencies);

// Product adapter using only the audited DiskModel, local-file and ImageFormat
// implementations. It never opens a physical write target.
[[nodiscard]] clonecore::Result<DirectImageCreateReport>
execute_direct_image_create_with_windows_apis(
    const DirectImageCreateRequest& request);

}  // namespace ytec::winpeapp

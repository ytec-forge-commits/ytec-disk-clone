#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/result.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ytec::vssrequester {

struct SnapshotVolumeOpenRequest final {
  std::wstring snapshot_device_path;
  std::uint64_t expected_size_bytes{};
  std::uint32_t logical_sector_size{};
};

class ISnapshotVolumeBackend {
 public:
  virtual ~ISnapshotVolumeBackend() = default;

  [[nodiscard]] virtual clonecore::Result<
      std::unique_ptr<clonecore::ISourceDiskReader>>
  open_read_only(const SnapshotVolumeOpenRequest& request) = 0;
};

[[nodiscard]] clonecore::Status validate_snapshot_volume_open_request(
    const SnapshotVolumeOpenRequest& request);

[[nodiscard]] clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>
open_snapshot_volume_reader(
    const SnapshotVolumeOpenRequest& request,
    ISnapshotVolumeBackend& backend);

[[nodiscard]] clonecore::Result<
    std::unique_ptr<clonecore::ISourceDiskReader>>
open_snapshot_volume_reader_with_windows_apis(
    const SnapshotVolumeOpenRequest& request);

}  // namespace ytec::vssrequester

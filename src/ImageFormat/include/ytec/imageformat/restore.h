#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/sha256.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace ytec::imageformat {

struct DcimgRestoreRequest final {
  clonecore::StableDiskIdentity expected_target;
  clonecore::StableDiskIdentity observed_target;
  clonecore::TargetConfirmation confirmation;
  std::uint64_t expected_image_length{};
  std::optional<Sha256Digest> expected_global_hash;
  std::size_t maximum_chunk_bytes{1024U * 1024U};
  clonecore::DiskOperationCallbacks callbacks;
};

struct DcimgRestoreReport final {
  std::uint64_t restored_data_bytes{};
  std::uint64_t committed_partition_table_bytes{};
  std::uint32_t restored_chunk_count{};
  bool complete_image_verified_before_write{};
  bool backup_manifest_verified_before_write{};
  bool read_back_verified{};
  bool partition_table_committed{};
  BackupPartitionStyle partition_style{BackupPartitionStyle::gpt};
  BackupBootMode boot_mode{BackupBootMode::uefi};
  bool contains_windows{};
  std::uint64_t windows_partition_offset{};
};

class DcimgRestorePreparedSource final {
 public:
  DcimgRestorePreparedSource(DcimgRestorePreparedSource&&) noexcept;
  DcimgRestorePreparedSource& operator=(
      DcimgRestorePreparedSource&&) noexcept;
  ~DcimgRestorePreparedSource();

  DcimgRestorePreparedSource(
      const DcimgRestorePreparedSource&) = delete;
  DcimgRestorePreparedSource& operator=(
      const DcimgRestorePreparedSource&) = delete;

 private:
  struct Impl;
  explicit DcimgRestorePreparedSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;

  friend clonecore::Result<DcimgRestorePreparedSource>
  prepare_verified_dcimg_v1_from_reader(
      std::uint64_t,
      const Sha256ReadCallback&,
      const DcimgRestoreRequest&);
  friend clonecore::Result<DcimgRestoreReport>
  restore_prepared_dcimg_v1(
      DcimgRestorePreparedSource&&,
      const DcimgRestoreRequest&,
      clonecore::ITargetDiskWriter&);
};

// Completes the full read-only verification and retains its result together
// with the exact reader. Resources captured by the reader must remain alive.
// This function never opens or changes the target disk.
[[nodiscard]] clonecore::Result<DcimgRestorePreparedSource>
prepare_verified_dcimg_v1_from_reader(
    std::uint64_t image_length,
    const Sha256ReadCallback& reader,
    const DcimgRestoreRequest& request);

// Restores only a source created by the preparation function. This preserves
// one locked source handle from full verification through final target commit
// without performing a second complete image scan.
[[nodiscard]] clonecore::Result<DcimgRestoreReport>
restore_prepared_dcimg_v1(
    DcimgRestorePreparedSource&& source,
    const DcimgRestoreRequest& request,
    clonecore::ITargetDiskWriter& target);

// Phase 2 synthetic restore boundary. The complete image and target identity
// are verified before the first target write. This function is not connected
// to physical-disk discovery or a product execution service.
[[nodiscard]] clonecore::Result<DcimgRestoreReport>
restore_verified_dcimg_v1(
    std::span<const std::byte> image,
    const DcimgRestoreRequest& request,
    clonecore::ITargetDiskWriter& target);

// Bounded-memory restore path for large v1 images. The complete container,
// metadata, and restore layout are verified before the first target write.
// Every non-zero data chunk is read again, checked against its recorded
// SHA-256, then written and read back before the partition table is committed.
[[nodiscard]] clonecore::Result<DcimgRestoreReport>
restore_verified_dcimg_v1_from_reader(
    std::uint64_t image_length,
    const Sha256ReadCallback& reader,
    const DcimgRestoreRequest& request,
    clonecore::ITargetDiskWriter& target);

}  // namespace ytec::imageformat

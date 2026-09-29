#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/shrink_image_manifest.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::imageformat {

struct RestoreImageInspectionReport final {
  std::wstring canonical_path;
  std::uint64_t image_length{};
  Sha256Digest global_hash{};
  DcimgHeader header;
  BackupImageManifest manifest;
  PartitionSnapshot partition_snapshot;
  bool complete_container_verified{};
  bool metadata_verified{};
  bool restore_layout_verified{};
  bool restore_execution_enabled{};
  // Present only for a fully verified manifest.dcmig bundle preflight.
  std::optional<ShrinkImageManifest> shrink_manifest;
};

struct RestoreImageInspectionOptions final {
  std::function<bool()> cancellation_requested;
  std::function<void(
      const DcimgReadInspectionProgress&)> progress;
};

class IRestoreImageReadSession {
 public:
  virtual ~IRestoreImageReadSession() = default;

  [[nodiscard]] virtual const std::wstring& canonical_path()
      const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t image_length()
      const noexcept = 0;
  [[nodiscard]] virtual clonecore::Result<std::vector<std::byte>>
  read_at(std::uint64_t offset, std::size_t length) const = 0;
};

// Opens one local, non-reparse .dcimg file read-only and keeps a single
// handle alive. FILE_SHARE_WRITE and FILE_SHARE_DELETE are deliberately not
// granted, so the same session can span full verification and restoration.
// No elevation or target-disk access is requested.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<IRestoreImageReadSession>>
open_restore_image_file_read_session_v1(
    const std::wstring& requested_path);

// Performs complete container, metadata, chunk, and restore-layout
// verification through a bounded read callback. No target disk is opened.
[[nodiscard]] clonecore::Result<RestoreImageInspectionReport>
inspect_restore_image_reader_v1(
    std::wstring canonical_path,
    std::uint64_t image_length,
    const Sha256ReadCallback& reader,
    const RestoreImageInspectionOptions& options = {});

// Opens only a local drive-letter .dcimg regular file read-only, rejects
// reparse points, and delegates to the same complete reader verification.
[[nodiscard]] clonecore::Result<RestoreImageInspectionReport>
inspect_restore_image_file_v1(
    const std::wstring& requested_path,
    const RestoreImageInspectionOptions& options = {});

}  // namespace ytec::imageformat

#pragma once

#include "ytec/imageformat/tsumugi_image_service.h"

#include <cstddef>
#include <span>

namespace ytec::imageformat {

// A platform adapter backed by one already locked physical-target handle.
// reidentify_locked_target() must derive the identity from that exact handle,
// not from a disk number looked up separately.
class ITsumugiRestoreTargetSession
    : public clonecore::ITargetDiskWriter {
 public:
  ~ITsumugiRestoreTargetSession() override = default;

  [[nodiscard]] virtual clonecore::Result<TsumugiRestoreDiskIdentity>
  reidentify_locked_target() const = 0;

  // For whole-disk restore this backs up and invalidates the primary layout;
  // for unallocated restore this prepares an uncommitted partition; for an
  // existing partition it locks and records the original state.
  [[nodiscard]] virtual clonecore::Status prepare_layout(
      const TsumugiVerifiedImage& image,
      const TsumugiRestoreTarget& target,
      TsumugiRestoreHost host) = 0;

  // Must read back all layout metadata and expose the primary GPT/MBR table
  // last. It is called only after every payload write passed read-back.
  [[nodiscard]] virtual clonecore::Status commit_layout() = 0;

  // Keeps a partial destination incomplete/offline and restores only metadata
  // whose recovery is proven safe by the platform adapter.
  virtual void abort_layout() noexcept = 0;
};

// Shared bounded writer used by Windows and PE platform adapters. It rejects
// range/identity drift, flushes every bounded write before read-back, and does
// not report success until the layout session commits last.
class TsumugiBlockRestoreTransaction final
    : public ITsumugiRestoreTransaction {
 public:
  explicit TsumugiBlockRestoreTransaction(
      ITsumugiRestoreTargetSession& session,
      std::size_t verification_block_bytes = 4U * 1024U * 1024U) noexcept;

  [[nodiscard]] clonecore::Result<TsumugiRestoreDiskIdentity> begin(
      const TsumugiVerifiedImage& image,
      const TsumugiRestoreTarget& target,
      TsumugiRestoreHost host) override;

  [[nodiscard]] clonecore::Status write_and_verify(
      const TsumugiRestoreWrite& write,
      std::span<const std::byte> plaintext) override;

  [[nodiscard]] clonecore::Status commit() override;
  void abort() noexcept override;

 private:
  ITsumugiRestoreTargetSession* session_{};
  std::size_t verification_block_bytes_{};
  TsumugiRestoreDiskIdentity current_identity_{};
  bool begun_{};
  bool committed_{};
};

}  // namespace ytec::imageformat

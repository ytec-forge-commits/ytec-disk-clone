#pragma once

#include "ytec/imageformat/tsumugi_image_service.h"
#include "ytec/imageformat/tsumugi_restore_layout.h"
#include "ytec/windowsapp/shrink_work_placement.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace ytec::windowsapp {

// Destructive Windows platform boundary used by the stateful product
// transaction below. begin_offline_incomplete() must re-identify and offline
// the target, invalidate its completion state, and verify that every supplied
// work path is disjoint from the target. It must not format a target volume.
class IWindowsTsumugiShrinkRestorePlatform {
 public:
  virtual ~IWindowsTsumugiShrinkRestorePlatform() = default;

  [[nodiscard]] virtual clonecore::Result<
      imageformat::TsumugiRestoreDiskIdentity>
  begin_offline_incomplete(
      const imageformat::TsumugiVerifiedImage& image,
      const imageformat::TsumugiWholeDiskRestoreTarget& target,
      const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1&
          reviewed_layout,
      const WindowsShrinkWorkPaths& work_paths) = 0;

  [[nodiscard]] virtual clonecore::Status create_target_file_system(
      const imageformat::TsumugiShrinkArchiveTarget& target) = 0;

  [[nodiscard]] virtual clonecore::Status begin_staged_wim(
      const imageformat::TsumugiShrinkArchiveTarget& target,
      const std::wstring& scratch_directory) = 0;

  // For an authenticated zero-fill chunk plaintext is empty and the platform
  // must append chunk.length zero bytes. Other chunks carry exactly length
  // bytes. Both forms are part of the verified WIM byte stream.
  [[nodiscard]] virtual clonecore::Status append_staged_wim(
      const imageformat::TsumugiShrinkArchiveChunk& chunk,
      std::span<const std::byte> plaintext) = 0;

  [[nodiscard]] virtual clonecore::Status verify_staged_single_image_wim(
      std::uint32_t source_table_index) = 0;

  [[nodiscard]] virtual clonecore::Status apply_staged_wim(
      std::uint32_t source_table_index) = 0;

  // Must reopen the resulting target filesystem, verify real metadata,
  // enumerate its complete visible namespace, and read every non-reparse
  // ordinary file's unnamed stream through stable EOF. Reparse targets and
  // alternate data streams are outside this v1 evidence. Merely trusting the
  // DISM exit code is not sufficient.
  [[nodiscard]] virtual clonecore::Status
  verify_applied_file_system_readback(
      const imageformat::TsumugiShrinkArchiveTarget& target) = 0;

  // The same zero-fill convention applies to exact RAW writes. Every written
  // sector still requires an immediate real-target readback.
  [[nodiscard]] virtual clonecore::Status write_exact_raw_and_verify(
      const imageformat::TsumugiRestoreWrite& write,
      std::span<const std::byte> plaintext) = 0;

  // Flushes all target writes and exposes the final primary partition table
  // last. The completed target remains offline for explicit user review.
  [[nodiscard]] virtual clonecore::Status commit_final_layout_last() = 0;

  // Idempotent and noexcept. The target must remain offline and visibly
  // incomplete; it must never restore an unverified primary layout. It is a
  // no-op if begin_offline_incomplete() failed before touching the target.
  virtual void abort_keep_offline_incomplete() noexcept = 0;
};

class WindowsTsumugiShrinkRestoreTransaction final
    : public imageformat::ITsumugiShrinkRestoreTransaction {
 public:
  ~WindowsTsumugiShrinkRestoreTransaction() override;

  WindowsTsumugiShrinkRestoreTransaction(
      WindowsTsumugiShrinkRestoreTransaction&&) = delete;
  WindowsTsumugiShrinkRestoreTransaction& operator=(
      WindowsTsumugiShrinkRestoreTransaction&&) = delete;
  WindowsTsumugiShrinkRestoreTransaction(
      const WindowsTsumugiShrinkRestoreTransaction&) = delete;
  WindowsTsumugiShrinkRestoreTransaction& operator=(
      const WindowsTsumugiShrinkRestoreTransaction&) = delete;

  [[nodiscard]] clonecore::Result<imageformat::TsumugiRestoreDiskIdentity>
  begin(
      const imageformat::TsumugiVerifiedImage& image,
      const imageformat::TsumugiRestoreTarget& target,
      imageformat::TsumugiRestoreHost host) override;

  [[nodiscard]] clonecore::Status write_exact_raw_and_verify(
      const imageformat::TsumugiRestoreWrite& write,
      std::span<const std::byte> plaintext) override;

  [[nodiscard]] clonecore::Status recreate_empty_file_system_and_verify(
      const imageformat::TsumugiShrinkArchiveTarget& target) override;

  [[nodiscard]] clonecore::Status begin_wim_archive(
      const imageformat::TsumugiShrinkArchiveTarget& target) override;

  [[nodiscard]] clonecore::Status append_wim_archive(
      const imageformat::TsumugiShrinkArchiveChunk& chunk,
      std::span<const std::byte> plaintext) override;

  [[nodiscard]] clonecore::Status complete_wim_archive_and_verify(
      std::uint32_t source_table_index) override;

  [[nodiscard]] clonecore::Status commit() override;
  void abort() noexcept override;

 private:
  class Impl;
  explicit WindowsTsumugiShrinkRestoreTransaction(
      std::unique_ptr<Impl> impl) noexcept;

  friend clonecore::Result<std::unique_ptr<
      WindowsTsumugiShrinkRestoreTransaction>>
  make_windows_tsumugi_shrink_restore_transaction(
      const clonecore::StableDiskIdentity&,
      const WindowsShrinkWorkPaths&,
      const WindowsShrinkWorkPlacementObservation&,
      const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1&,
      std::unique_ptr<IWindowsTsumugiShrinkRestorePlatform>);

  std::unique_ptr<Impl> impl_;
};

// protected_source identifies the disk containing the source image and/or
// executable-adjacent state. The same placement guard used by online capture
// prevents scratch/checkpoint/log from depending on that disk. The platform
// independently repeats this separation against the destructive target.
[[nodiscard]] clonecore::Result<std::unique_ptr<
    WindowsTsumugiShrinkRestoreTransaction>>
make_windows_tsumugi_shrink_restore_transaction(
    const clonecore::StableDiskIdentity& protected_source,
    const WindowsShrinkWorkPaths& work_paths,
    const WindowsShrinkWorkPlacementObservation& observed_work,
    const imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1&
        reviewed_layout,
    std::unique_ptr<IWindowsTsumugiShrinkRestorePlatform> platform);

}  // namespace ytec::windowsapp

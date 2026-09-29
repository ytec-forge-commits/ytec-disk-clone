#pragma once

#include "ytec/winpeapp/direct_image_restore_resume.h"

#include <memory>

namespace ytec::winpeapp {

// Owns every dependency whose address is referenced by the pure resume
// controller.  The object is deliberately non-movable so its published
// dependency view remains stable for the whole controller call.
class DirectImageRestoreResumeBackendV1 final {
 public:
  ~DirectImageRestoreResumeBackendV1();

  DirectImageRestoreResumeBackendV1(
      const DirectImageRestoreResumeBackendV1&) = delete;
  DirectImageRestoreResumeBackendV1& operator=(
      const DirectImageRestoreResumeBackendV1&) = delete;
  DirectImageRestoreResumeBackendV1(
      DirectImageRestoreResumeBackendV1&&) = delete;
  DirectImageRestoreResumeBackendV1& operator=(
      DirectImageRestoreResumeBackendV1&&) = delete;

  [[nodiscard]] const DirectImageRestoreResumeDependencies&
  dependencies() const noexcept;

 private:
  class Impl;
  explicit DirectImageRestoreResumeBackendV1(
      std::unique_ptr<Impl> impl) noexcept;

  friend clonecore::Result<std::unique_ptr<
      DirectImageRestoreResumeBackendV1>>
  make_direct_image_restore_resume_backend_v1(
      operationcore::IResumeSlotPlatform&,
      DirectImageRestoreDependencies,
      DirectImageRestoreResumeStorageProbe);

  std::unique_ptr<Impl> impl_;
};

// Dependency-injected composition used by synthetic tests and by the
// production factory below.  The supplied physical adapter must still be a
// full fail-closed physical restore adapter; this function never supplies a
// callback-only success fallback.
[[nodiscard]] clonecore::Result<std::unique_ptr<
    DirectImageRestoreResumeBackendV1>>
make_direct_image_restore_resume_backend_v1(
    operationcore::IResumeSlotPlatform& slot_platform,
    DirectImageRestoreDependencies physical_dependencies,
    DirectImageRestoreResumeStorageProbe prove_storage_separation);

// Production Win32/WinPE composition.  The caller supplies the boot-media
// stable-identity resolver and the already-opened-storage separation proof;
// the factory supplies real image verification, target reidentification,
// offline/open handling and the low-level persistent resume engine.  No
// main/gui wiring is performed here.
[[nodiscard]] clonecore::Result<std::unique_ptr<
    DirectImageRestoreResumeBackendV1>>
make_direct_image_restore_windows_resume_backend_v1(
    operationcore::IResumeSlotPlatform& slot_platform,
    ActiveRescueMediaTargetQuery active_rescue_media_query,
    DirectImageRestoreResumeStorageProbe prove_storage_separation);

}  // namespace ytec::winpeapp

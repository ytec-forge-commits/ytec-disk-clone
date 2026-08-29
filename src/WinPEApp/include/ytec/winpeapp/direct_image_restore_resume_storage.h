#pragma once

#include "ytec/operationcore/windows_resume_slot_platform.h"
#include "ytec/winpeapp/direct_image_restore_resume.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ytec::winpeapp {

enum class ResumeStoragePathRole : std::uint8_t {
  checkpoint_data,
  image,
  active_rescue,
  image_output_parent,
};

// One normalized storage domain obtained while the relevant file/directory,
// volume, and (for physical storage) disk handles are all open.  A physical
// disk produces the same digest for every partition on that disk.  CD/DVD and
// the WinPE X: RAM volume use a volume-domain digest and can never be treated
// as a physical restore target.
struct OpenedResumeStorageDomainV1 final {
  operationcore::Sha256Digest storage_identity_hash{};
  std::optional<operationcore::Sha256Digest>
      file_object_identity_hash;
  bool identity_from_open_handles{};
  // False for CD/DVD and the WinPE X: RAM volume. They may be inspected as
  // opened domains, but X: must never authorize a supposedly persistent slot
  // mutation that would disappear on restart.
  bool persistent_checkpoint_backing{true};

  [[nodiscard]] bool operator==(
      const OpenedResumeStorageDomainV1&) const noexcept = default;
};

using ResumeStoragePathObserverV1 = std::function<clonecore::Result<
    OpenedResumeStorageDomainV1>(
        const std::wstring&,
        ResumeStoragePathRole)>;

using ResumeStorageTargetObserverV1 = std::function<clonecore::Result<
    OpenedResumeStorageDomainV1>(
        const clonecore::StableDiskIdentity&)>;

using ResumeStorageActiveObserverV1 = std::function<clonecore::Result<
    OpenedResumeStorageDomainV1>()>;

struct DirectImageRestoreResumeStorageDependenciesV1 final {
  ResumeStoragePathObserverV1 observe_path_storage;
  ResumeStorageTargetObserverV1 observe_locked_target_storage;
  ResumeStorageActiveObserverV1 observe_active_rescue_storage;
};

// The two callbacks share one small in-process binding.  The restore probe
// first opens and proves data/image/target/active storage.  A subsequent slot
// create/replace callback then reopens the data backing and permits mutation
// only if it is the same domain and remains disjoint from image and target.
// Startup inspect and guarded discard do not consume that selected binding.
struct DirectImageRestoreResumeStoragePlatformV1 final {
  operationcore::WindowsResumeDataBackingProbe prove_data_backing;
  DirectImageRestoreResumeStorageProbe prove_restore_storage;
};

[[nodiscard]] clonecore::Result<
    DirectImageRestoreResumeStoragePlatformV1>
make_direct_image_restore_resume_storage_platform_v1(
    DirectImageRestoreResumeStorageDependenciesV1 dependencies);

// Production Win32 path/target observers.  They perform read-only opens only.
// Fixed/removable file paths must map to exactly one physical disk.  The only
// nonphysical fallbacks are a verified CD/DVD volume and X: for the fixed
// EXE-adjacent checkpoint data directory.
[[nodiscard]] clonecore::Result<OpenedResumeStorageDomainV1>
observe_windows_resume_path_storage_domain_v1(
    const std::wstring& path,
    ResumeStoragePathRole role);

[[nodiscard]] clonecore::Result<OpenedResumeStorageDomainV1>
observe_windows_resume_physical_storage_domain_v1(
    const clonecore::StableDiskIdentity& identity);

// Full production composition, including the active rescue marker resolver.
[[nodiscard]] clonecore::Result<
    DirectImageRestoreResumeStoragePlatformV1>
make_direct_image_restore_windows_storage_platform_v1();

}  // namespace ytec::winpeapp

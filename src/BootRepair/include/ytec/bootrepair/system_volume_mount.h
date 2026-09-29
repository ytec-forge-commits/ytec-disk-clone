#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::bootrepair {

struct BootVolumeObservation final {
  std::wstring volume_name;
  BootRepairVolumeLocation location;
  std::vector<std::wstring> mount_points;
};

struct TemporarySystemVolumeMountPlan final {
  BcdBootFirmware firmware{BcdBootFirmware::uefi};
  std::uint32_t disk_number{};
  std::uint32_t partition_number{};
  std::wstring volume_name;
  std::wstring temporary_root;
  BootRepairVolumeLocation expected_location;
};

// Selects exactly one unassigned ESP or BIOS Active NTFS volume and an unused
// drive letter. This function performs no mount or disk write.
[[nodiscard]] clonecore::Result<TemporarySystemVolumeMountPlan>
plan_temporary_system_volume_mount(
    const diskmodel::DiskInfo& disk,
    BcdBootFirmware firmware,
    const std::vector<BootVolumeObservation>& volumes,
    std::wstring_view unavailable_drive_letters);

class ISystemVolumeMountApi {
 public:
  virtual ~ISystemVolumeMountApi() = default;

  [[nodiscard]] virtual clonecore::Status attach(
      const std::wstring& temporary_root,
      const std::wstring& volume_name) = 0;

  [[nodiscard]] virtual clonecore::Result<BootVolumeObservation> inspect(
      const std::wstring& temporary_root,
      const std::wstring& expected_volume_name,
      const BootRepairVolumeLocation& expected_location) = 0;

  [[nodiscard]] virtual clonecore::Status detach(
      const std::wstring& temporary_root,
      const std::wstring& expected_volume_name) = 0;
};

class TemporarySystemVolumeMount final {
 public:
  ~TemporarySystemVolumeMount() noexcept;

  TemporarySystemVolumeMount(const TemporarySystemVolumeMount&) = delete;
  TemporarySystemVolumeMount& operator=(
      const TemporarySystemVolumeMount&) = delete;
  TemporarySystemVolumeMount(TemporarySystemVolumeMount&& other) noexcept;
  TemporarySystemVolumeMount& operator=(TemporarySystemVolumeMount&&) = delete;

  [[nodiscard]] static clonecore::Result<TemporarySystemVolumeMount> acquire(
      const TemporarySystemVolumeMountPlan& plan,
      ISystemVolumeMountApi& api);

  [[nodiscard]] const std::wstring& root() const noexcept { return root_; }
  [[nodiscard]] bool mounted() const noexcept { return mounted_; }

  // Successful callers must release explicitly so cleanup failures can fail
  // the overall boot-repair operation. The destructor is a best-effort guard.
  [[nodiscard]] clonecore::Status release();

 private:
  TemporarySystemVolumeMount(
      std::wstring root,
      std::wstring volume_name,
      ISystemVolumeMountApi& api) noexcept;

  std::wstring root_;
  std::wstring volume_name_;
  ISystemVolumeMountApi* api_{};
  bool mounted_{};
};

// Read-only Volume GUID enumeration used by WinPE preflight.
[[nodiscard]] clonecore::Result<std::vector<BootVolumeObservation>>
enumerate_windows_boot_volumes_read_only();

[[nodiscard]] std::unique_ptr<ISystemVolumeMountApi>
make_windows_system_volume_mount_api();

}  // namespace ytec::bootrepair

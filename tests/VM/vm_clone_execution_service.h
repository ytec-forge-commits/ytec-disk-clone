#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/winpeapp/app_runner.h"

#include <cstdint>
#include <string_view>

namespace ytec::vmtest {

enum class VmCloneProfile : std::uint8_t {
  synthetic,
  boot,
  legacy_bios,
  legacy_bios_x64,
};

struct VmCloneSelection final {
  diskmodel::DiskInfo source_disk;
  diskmodel::DiskInfo target_disk;
  clonecore::StableDiskIdentity source_identity;
  clonecore::StableDiskIdentity target_identity;
};

[[nodiscard]] std::wstring_view vm_clone_authorization(
    VmCloneProfile profile);

[[nodiscard]] bool is_virtualbox_guest();
[[nodiscard]] bool is_administrator();

[[nodiscard]] clonecore::Result<VmCloneSelection> select_vm_clone_disks(
    std::uint32_t source_number,
    std::uint32_t target_number,
    VmCloneProfile profile);

[[nodiscard]] clonecore::Result<VmCloneSelection>
select_unique_vm_clone_target(
    std::uint32_t source_number,
    VmCloneProfile profile);

class VmCloneExecutionService final
    : public winpeapp::ICloneExecutionService {
 public:
  [[nodiscard]] clonecore::Result<winpeapp::CloneExecutionReport> execute(
      const winpeapp::CloneExecutionRequest& request) override;
};

}  // namespace ytec::vmtest

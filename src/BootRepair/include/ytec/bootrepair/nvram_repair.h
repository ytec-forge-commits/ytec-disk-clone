#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::bootrepair {

// One opened firmware-variable observation. BootOrder and Boot#### variables
// are accepted only with the ordinary NV|BS|RT attribute set; authenticated,
// append-write, volatile, or otherwise unfamiliar variables fail closed.
struct FirmwareVariableValue final {
  std::vector<std::byte> bytes;
  std::uint32_t attributes{};
};

struct CurrentPcNvramRepairRequest final {
  clonecore::StableDiskIdentity expected_disk;
  diskmodel::PartitionInfo expected_esp;
  std::uint32_t logical_sector_size{};
  bool explicitly_for_current_pc{};
  clonecore::TargetConfirmation confirmation;
};

struct CurrentPcNvramRepairReport final {
  std::uint16_t boot_option_number{};
  bool already_valid{};
  bool boot_option_written{};
  bool boot_order_written{};
  bool prior_boot_order_preserved{};
  bool exact_target_verified{};
  bool rollback_attempted{};
  bool rollback_succeeded{};
};

class ICurrentPcNvramRepairPlatform {
 public:
  virtual ~ICurrentPcNvramRepairPlatform() = default;

  // Must re-enumerate and compare the complete reviewed disk/ESP immediately
  // before every firmware mutation. A disk number by itself is insufficient.
  [[nodiscard]] virtual clonecore::Status revalidate_target(
      const clonecore::StableDiskIdentity& expected_disk,
      const diskmodel::PartitionInfo& expected_esp) = 0;

  [[nodiscard]] virtual clonecore::Result<
      std::optional<FirmwareVariableValue>>
  read_efi_global_variable(const std::wstring& name) = 0;

  // Replace/delete only while the immediately re-read current value is still
  // byte-for-byte identical to expected. A null replacement means deletion.
  // This prevents rollback from overwriting a variable changed after our
  // mutation. Production performs the comparison and mutation under one
  // process-local serialization gate because Win32 exposes no firmware CAS.
  [[nodiscard]] virtual clonecore::Status
  replace_efi_global_variable_if_exact(
      const std::wstring& name,
      const std::optional<FirmwareVariableValue>& expected,
      const std::optional<FirmwareVariableValue>& replacement) = 0;
};

using CurrentPcNvramTargetRevalidator = std::function<clonecore::Status(
    const clonecore::StableDiskIdentity&,
    const diskmodel::PartitionInfo&)>;

// Pure helpers are public so synthetic tests can prove exact GPT ESP binding
// without touching host firmware variables.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
build_windows_boot_manager_load_option(
    const diskmodel::PartitionInfo& expected_esp,
    std::uint32_t logical_sector_size);

[[nodiscard]] clonecore::Result<bool>
windows_boot_manager_load_option_matches(
    std::span<const std::byte> load_option,
    const diskmodel::PartitionInfo& expected_esp,
    std::uint32_t logical_sector_size);

[[nodiscard]] clonecore::Result<CurrentPcNvramRepairReport>
execute_current_pc_nvram_repair(
    const CurrentPcNvramRepairRequest& request,
    ICurrentPcNvramRepairPlatform& platform);

// Production adapter uses only the documented UEFI global-variable APIs. The
// caller supplies the product's fresh disk/layout revalidation callback.
[[nodiscard]] std::unique_ptr<ICurrentPcNvramRepairPlatform>
make_windows_current_pc_nvram_repair_platform(
    CurrentPcNvramTargetRevalidator target_revalidator);

}  // namespace ytec::bootrepair

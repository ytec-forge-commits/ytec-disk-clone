#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/efi_delete_transaction.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::bootrepair {

// Immutable routing material used to obtain the ESP attributes that are not
// present in DiskModel::PartitionInfo. The production observer reidentifies
// the disk and requires the Volume GUID to map to this exact GPT extent.
struct WindowsEfiDeleteEspRequest final {
  clonecore::StableDiskIdentity expected_disk;
  std::uint32_t expected_partition_number{};
  std::uint64_t expected_offset_bytes{};
  std::uint64_t expected_length_bytes{};
  std::wstring expected_partition_identifier;
  std::wstring expected_partition_type_identifier;
  std::wstring expected_volume_guid_root;
};

// Read-only production observation. It opens the Volume GUID and underlying
// partition for metadata queries only; it does not mount or mutate the ESP.
[[nodiscard]] clonecore::Result<EfiDeleteEspIdentity>
inspect_windows_efi_delete_esp_identity_read_only(
    const WindowsEfiDeleteEspRequest& request);

// Review-only production inspector. The returned object performs bounded
// handle-relative traversal and hashing but exposes no mutation methods.
[[nodiscard]] std::unique_ptr<IEfiDeleteReadOnlyInspector>
make_windows_efi_delete_read_only_inspector();

// Explicit injection points used only by focused synthetic tests. The
// product factory below always selects none. An after_* point deliberately
// reports partial_or_unknown so rollback/result classification can be tested
// without claiming an unproven no-mutation state.
enum class WindowsEfiDeleteFailurePoint : std::uint8_t {
  none,
  before_quarantine_create,
  after_quarantine_create,
  before_candidate_move,
  after_candidate_move,
  before_bcd_rebuild,
  after_bcd_backup_move,
  after_bcd_rebuild,
  before_bcd_rollback,
  before_candidate_delete,
  after_candidate_delete,
  before_bcd_commit,
  before_quarantine_cleanup,
};

// Pure failure-readback classifier used by the production outer-BCD
// transaction and focused tests. Both observations must come from successful
// exact-identity queries made after move_file_no_replace() failed. Only the
// original exact object still at source plus an absent destination proves that
// the failed rename made no mutation; every other state remains unknown.
[[nodiscard]] EfiDeleteMutationExtent
classify_windows_efi_delete_failed_bcd_backup_move_readback(
    const BcdStoreFileIdentity& expected_source,
    const std::optional<BcdStoreFileIdentity>& observed_source,
    const std::optional<BcdStoreFileIdentity>& observed_destination) noexcept;

struct WindowsEfiDeleteFailureInjection final {
  WindowsEfiDeleteFailurePoint point{
      WindowsEfiDeleteFailurePoint::none};
  std::optional<std::size_t> candidate_index;
};

[[nodiscard]] EfiDeletePlatformStepResult
make_windows_efi_delete_injected_failure(
    WindowsEfiDeleteFailurePoint point,
    bool mutation_may_have_occurred);

// Production-specific extension used by the WinPE product coordinator to
// retain the verified per-Windows BCDBoot report. The generic transaction
// still owns all ordering and rollback decisions.
class IWindowsEfiDeleteTransactionPlatform
    : public IEfiDeleteTransactionPlatform {
 public:
  ~IWindowsEfiDeleteTransactionPlatform() override = default;

  [[nodiscard]] virtual const std::optional<MultiWindowsBcdBootReport>&
  verified_bcd_report() const noexcept = 0;
};

// Product factory. Every request must be a reviewed UEFI batch targeting one
// exact drive root. The adapter independently proves that drive root maps to
// ReviewedEfiDeletePlan::expected_esp() before touching BCD or quarantine.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<IWindowsEfiDeleteTransactionPlatform>>
make_windows_efi_delete_transaction_platform(
    std::vector<BcdBootRequest> requests_in_boot_priority);

// Focused-test factory. Product sources must not call this overload.
[[nodiscard]] clonecore::Result<
    std::unique_ptr<IWindowsEfiDeleteTransactionPlatform>>
make_windows_efi_delete_transaction_platform_for_failure_injection(
    std::vector<BcdBootRequest> requests_in_boot_priority,
    WindowsEfiDeleteFailureInjection injection);

}  // namespace ytec::bootrepair

#pragma once

#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/bootrepair/mbr2gpt.h"
#include "ytec/bootrepair/winre_diagnostic.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/operation_types.h"
#include "ytec/imageformat/sha256.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::winpeapp {

enum class ClonePartitionStyle : std::uint8_t {
  gpt,
  mbr,
};

struct CloneExecutionRequest final {
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  clonecore::TargetConfirmation confirmation;
  std::wstring authorization;
  imageformat::TransferMode transfer_mode{imageformat::TransferMode::exact};
  clonecore::DiskOperationCallbacks callbacks;
  // Direct product operations leave the completed target offline. Internal
  // chained conversion may temporarily request an online handoff.
  bool leave_target_offline{true};
};

struct CloneExecutionReport final {
  ClonePartitionStyle partition_style{ClonePartitionStyle::gpt};
  std::uint64_t copied_data_bytes{};
  std::uint32_t copied_partition_count{};
  std::uint32_t recreated_partition_count{};
  bool read_back_verified{};
  bool partition_table_committed{};
  bool target_returned_online{};
  bool target_left_offline{};
  bool boot_finalization_required{true};
  bootrepair::StandaloneBootRepairReport boot_repair;
  bool windows_partition_temporarily_mounted{};
  bool system_partition_temporarily_mounted{};
  bool temporary_mounts_released{};
  bool boot_finalization_verified{};
};

class ICloneExecutionService {
 public:
  virtual ~ICloneExecutionService() = default;

  [[nodiscard]] virtual clonecore::Result<CloneExecutionReport> execute(
      const CloneExecutionRequest& request) = 0;
};

// Product WinPE direct-clone implementation. The caller must build the request
// from a selection reviewed during the current PE session. The service repeats
// stable source/target identification before every destructive transition,
// holds the source read-only handle, writes only through the verified target
// boundary, and leaves a failed partial target offline.
[[nodiscard]] std::unique_ptr<ICloneExecutionService>
make_windows_clone_execution_service();

// Immutable selection reviewed in the current WinPE session. The GUI keeps
// this value from the read-only review through confirmation and execution;
// it must never rebuild the expected identities from disk numbers after the
// user has reviewed the target.
struct DirectCloneOperationPlan final {
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  diskmodel::PartitionStyle source_partition_style{
      diskmodel::PartitionStyle::unknown};
  std::wstring source_bus_type;
  std::wstring target_bus_type;
  std::size_t source_partition_count{};
  std::size_t target_partition_count{};
  diskmodel::DiskHealthInfo source_health;
  diskmodel::DiskHealthInfo target_health;
};

// Performs only read-only enumeration and builds the stable identity plan
// which the caller must retain unchanged until execute_direct_clone_operation.
[[nodiscard]] clonecore::Result<DirectCloneOperationPlan>
prepare_direct_clone_operation(
    std::uint32_t source_disk_number,
    std::uint32_t target_disk_number,
    diskmodel::IDiskInventoryProvider& provider);

// Accepts only the exact user-facing token "OK", translates it to the
// target-bound internal confirmation, and invokes the service with the
// identities retained in the reviewed plan. The concrete service performs the
// final physical reidentification before any destructive transition.
[[nodiscard]] clonecore::Result<CloneExecutionReport>
execute_direct_clone_operation(
    const DirectCloneOperationPlan& plan,
    bool target_erasure_acknowledged,
    std::wstring_view typed_confirmation,
    std::wstring authorization,
    ICloneExecutionService& service,
    clonecore::DiskOperationCallbacks callbacks = {});

struct Mbr2GptPartitionLayoutEntry final {
  std::uint32_t number{};
  diskmodel::PartitionStyle style{diskmodel::PartitionStyle::unknown};
  std::wstring type;
  std::uint64_t offset_bytes{};
  std::uint64_t size_bytes{};
  bool bootable{};

  bool operator==(const Mbr2GptPartitionLayoutEntry&) const = default;
};

// Canonical by-value disk layout used to bind the read-only review to the
// destructive seam. Partition order from enumeration is not significant;
// make_mbr2gpt_canonical_disk_layout sorts every entry.
struct Mbr2GptCanonicalDiskLayout final {
  diskmodel::PartitionStyle disk_style{diskmodel::PartitionStyle::unknown};
  std::vector<Mbr2GptPartitionLayoutEntry> partitions;

  bool operator==(const Mbr2GptCanonicalDiskLayout&) const = default;
};

[[nodiscard]] Mbr2GptCanonicalDiskLayout
make_mbr2gpt_canonical_disk_layout(const diskmodel::DiskInfo& disk);

// Pure exact-match gate shared by the product service and synthetic tests.
// A same-count change to number/style/type/offset/size/bootable fails closed.
[[nodiscard]] clonecore::Status validate_mbr2gpt_reviewed_layouts(
    const Mbr2GptCanonicalDiskLayout& expected_source,
    const Mbr2GptCanonicalDiskLayout& expected_target,
    const diskmodel::DiskInfo& observed_source,
    const diskmodel::DiskInfo& observed_target);

struct Mbr2GptDirectExecutionRequest final {
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  Mbr2GptCanonicalDiskLayout expected_source_layout;
  Mbr2GptCanonicalDiskLayout expected_target_layout;
  clonecore::TargetConfirmation confirmation;
  clonecore::DiskOperationCallbacks callbacks;
};

struct Mbr2GptDirectExecutionReport final {
  CloneExecutionReport clone;
  bootrepair::Mbr2GptConversionReport conversion;
  bootrepair::StandaloneBootRepairReport boot_repair;
  bool source_reidentified_unchanged{};
  bool source_left_read_only{};
  bool target_reidentified_as_gpt{};
  bool efi_system_partition_verified{};
  bool microsoft_reserved_partition_verified{};
  bool offline_windows_verified{};
  bool windows_partition_temporarily_mounted{};
  bool temporary_windows_mount_released{};
  bool final_layout_verified{};
  bool final_target_left_offline{};
};

class IMbr2GptDirectExecutionService {
 public:
  virtual ~IMbr2GptDirectExecutionService() = default;

  [[nodiscard]] virtual clonecore::Result<Mbr2GptDirectExecutionReport> execute(
      const Mbr2GptDirectExecutionRequest& request) = 0;
};

// Product WinPE migration implementation. It first performs the verified MBR
// clone to the separate target, then invokes only the Microsoft-signed
// System32 MBR2GPT and BCDBoot boundaries. The source remains read-only.
[[nodiscard]] std::unique_ptr<IMbr2GptDirectExecutionService>
make_windows_mbr2gpt_direct_execution_service();

// Narrow, fail-closed product plan for the currently connected
// MBR-to-GPT-to-a-separate-disk path. The reviewed exact-clone identities are
// retained by value and the extra fields record the MBR structural facts that
// were checked before any destructive service can be reached.
struct Mbr2GptDirectOperationPlan final {
  DirectCloneOperationPlan clone;
  std::size_t primary_partition_count{};
  std::uint32_t active_system_partition_number{};
  Mbr2GptCanonicalDiskLayout source_layout;
  Mbr2GptCanonicalDiskLayout target_layout;
  imageformat::Sha256Digest review_binding_digest{};
};

// Read-only inventory gate for the product GUI. This slice deliberately
// accepts only one-to-three basic MBR primary partitions of type 0x07/0x27,
// exactly one active 0x07 system partition, a healthy source, and a
// same-or-larger supported fixed target. The execution service still verifies
// a unique supported offline Windows installation and Microsoft's signed
// tools before modifying the target.
[[nodiscard]] clonecore::Result<Mbr2GptDirectOperationPlan>
prepare_mbr2gpt_direct_operation(
    std::uint32_t source_disk_number,
    std::uint32_t target_disk_number,
    diskmodel::IDiskInventoryProvider& provider);

// Requires the same two-step destructive confirmation as exact clone, passes
// only the identities and MBR structural facts retained in the reviewed
// immutable value, and accepts a result only after MBR clone, Microsoft
// conversion, UEFI boot rebuild, final layout verification, and the final
// target-offline transition all verify.
[[nodiscard]] clonecore::Result<Mbr2GptDirectExecutionReport>
execute_mbr2gpt_direct_operation(
    const Mbr2GptDirectOperationPlan& plan,
    bool target_erasure_acknowledged,
    std::wstring_view typed_confirmation,
    IMbr2GptDirectExecutionService& service,
    clonecore::DiskOperationCallbacks callbacks = {});

[[nodiscard]] int run_winpe_app(
    const std::vector<std::wstring>& arguments,
    diskmodel::IDiskInventoryProvider& provider,
    std::ostream& output,
    std::ostream& error_output);

}  // namespace ytec::winpeapp

#pragma once

#include "ytec/clonecore/rescue_copy.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/operationcore/operation.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ytec::winpeapp {

inline constexpr std::wstring_view kRescueCloneResultClassification =
    L"救出結果（一部欠損の可能性あり）";

// Immutable read-only review retained by the PE GUI through the destructive
// confirmation. Rescue cloning is a whole-disk RAW operation only: it never
// shrinks a filesystem, converts a partition style, or finalizes boot files.
struct RescueCloneOperationPlan final {
  operationcore::OperationId operation_id{};
  clonecore::StableDiskIdentity expected_source;
  clonecore::StableDiskIdentity expected_target;
  imageformat::Sha256Digest expected_source_layout_hash{};
  imageformat::Sha256Digest expected_target_layout_hash{};
  diskmodel::PartitionStyle source_partition_style{
      diskmodel::PartitionStyle::unknown};
  std::wstring source_bus_type;
  std::wstring target_bus_type;
  std::size_t source_partition_count{};
  std::size_t target_partition_count{};
  diskmodel::DiskHealthInfo source_health;
  diskmodel::DiskHealthInfo target_health;
  std::size_t large_block_bytes{4U * 1024U * 1024U};
  bool active_rescue_media_checked{};
};

using RescueCloneReidentifier = std::function<clonecore::Result<
    diskmodel::ReidentifiedPhysicalClone>(
    const clonecore::StableDiskIdentity&,
    const clonecore::StableDiskIdentity&)>;
using RescueCloneActiveMediaQuery = std::function<clonecore::Result<bool>(
    const clonecore::StableDiskIdentity&)>;
using RescueCloneSourceReadOnlySetter = std::function<clonecore::Status(
    const clonecore::StableDiskIdentity&,
    bool)>;
using RescueCloneSourceOpener = std::function<clonecore::Result<
    diskmodel::ReadOnlyPhysicalDiskHandle>(
    const clonecore::StableDiskIdentity&)>;
using RescueCloneTargetOfflineSetter = std::function<clonecore::Status(
    const clonecore::StableDiskIdentity&,
    const clonecore::StableDiskIdentity&,
    const clonecore::TargetConfirmation&,
    bool)>;
using RescueCloneTargetOpener = std::function<clonecore::Result<
    diskmodel::PhysicalTargetHandle>(
    const clonecore::StableDiskIdentity&,
    const clonecore::TargetConfirmation&)>;

struct RescueCloneDependencies final {
  RescueCloneReidentifier reidentify_selection;
  RescueCloneActiveMediaQuery is_active_rescue_media;
  RescueCloneSourceReadOnlySetter set_source_read_only;
  RescueCloneSourceOpener open_read_only_source;
  RescueCloneTargetOfflineSetter set_target_offline;
  RescueCloneTargetOpener open_offline_target;
};

struct RescueCloneExecutionReport final {
  clonecore::RescueRawCopyReport raw;
  // Rescue writes exactly the reviewed source extent. A larger target's tail
  // is deliberately not presented as erased or validated.
  std::uint64_t untouched_target_tail_bytes{};
  bool source_left_read_only{};
  bool target_left_offline{};
  bool active_rescue_media_excluded_by_stable_identity{};
  // Product presentation is deliberately conservative even when every failed
  // read was later recovered. The exact missing map remains separately
  // available in raw.missing_ranges.
  bool must_display_as_partial_loss{};
  bool shrinking_performed{};
  bool partition_style_conversion_performed{};
  bool boot_finalization_performed{};
};

struct RescueCloneOperationReport final {
  operationcore::OperationPlan plan;
  operationcore::OperationResult lifecycle;
  std::optional<RescueCloneExecutionReport> rescue;
};

// Bounded product text used by the PE UI. The conservative result
// classification and the actual missing-range truth are intentionally shown
// as separate lines.
[[nodiscard]] std::wstring format_rescue_clone_product_result(
    const RescueCloneExecutionReport& report);

// Read-only review. The active rescue USB check is mandatory and is bound to
// the target's stable identity; a missing resolver fails closed.
[[nodiscard]] clonecore::Result<RescueCloneOperationPlan>
prepare_rescue_clone_operation(
    std::uint32_t source_disk_number,
    std::uint32_t target_disk_number,
    diskmodel::IDiskInventoryProvider& provider,
    const RescueCloneActiveMediaQuery& active_rescue_media_query);

// Builds OperationPlan -> reidentifies -> validates exact uppercase OK ->
// makes the PE source read-only -> takes only the target offline -> performs
// finite rescue retries and read-back verification -> verifies final source
// read-only / target offline evidence. A successful Result still requires the
// caller to inspect lifecycle.outcome and rescue.
[[nodiscard]] clonecore::Result<RescueCloneOperationReport>
execute_rescue_clone_operation(
    const RescueCloneOperationPlan& reviewed_plan,
    bool target_erasure_acknowledged,
    std::wstring_view typed_confirmation,
    const RescueCloneDependencies& dependencies,
    clonecore::DiskOperationCallbacks callbacks = {});

// Constructs only function adapters. It performs no disk I/O.
[[nodiscard]] RescueCloneDependencies
make_rescue_clone_windows_dependencies();

}  // namespace ytec::winpeapp

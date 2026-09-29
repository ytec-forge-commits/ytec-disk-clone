#pragma once

#include "ytec/clonecore/rescue_copy.h"
#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/operationcore/operation.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

inline constexpr std::wstring_view
    kWindowsDataRescueCloneResultClassification =
        L"救出結果（一部欠損の可能性あり）";

// Immutable read-only review for the running-Windows data-disk rescue path.
// The source must not be the running Windows system disk. This mode copies
// only the reviewed whole-disk RAW extent and never invokes VSS, shrink,
// partition-style conversion, or boot finalization. The app never changes a
// source-disk attribute in Windows; this slice therefore accepts only a
// source already observed as read-only or offline and opens it read-only.
struct WindowsDataRescueClonePlan final {
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
  bool source_was_read_only{};
  bool source_was_offline{};
  bool protected_rescue_media_checked{};
};

using WindowsDataRescueCloneReidentifier =
    std::function<clonecore::Result<diskmodel::ReidentifiedPhysicalClone>(
        const clonecore::StableDiskIdentity&,
        const clonecore::StableDiskIdentity&)>;
using WindowsDataRescueProtectedTargetQuery =
    std::function<clonecore::Result<bool>(
        const clonecore::StableDiskIdentity&)>;

// Read-only evidence used to keep a marked Y-TEC rescue medium out of the
// destructive Windows rescue target path. A volume spanning more than one
// physical disk is deliberately represented so the resolver can fail closed.
struct WindowsDataRescueMountedVolume final {
  std::wstring root_path;
  std::vector<std::uint32_t> disk_numbers;
};

struct WindowsDataRescueProtectedTargetDependencies final {
  std::function<clonecore::Result<bool>()> running_in_winpe;
  std::function<clonecore::Result<
      std::vector<WindowsDataRescueMountedVolume>>()>
      enumerate_mounted_volumes;
  std::function<clonecore::Result<std::optional<std::string>>(
      const std::wstring&)>
      read_rescue_marker;
  std::function<clonecore::Result<diskmodel::InventoryReport>()>
      enumerate_disks;
};

// Performs only read-only registry, marker, volume-extent and inventory
// queries. It observes the target-volume mapping twice and requires an exact
// stable-identity match before returning false (not protected).
[[nodiscard]] clonecore::Result<bool>
resolve_windows_data_rescue_protected_target(
    const clonecore::StableDiskIdentity& expected_target,
    const WindowsDataRescueProtectedTargetDependencies& dependencies);

[[nodiscard]] WindowsDataRescueProtectedTargetDependencies
make_windows_data_rescue_protected_target_dependencies();

[[nodiscard]] clonecore::Result<bool>
query_windows_data_rescue_protected_target_with_windows_apis(
    const clonecore::StableDiskIdentity& expected_target);
using WindowsDataRescueSourceOpener =
    std::function<clonecore::Result<diskmodel::ReadOnlyPhysicalDiskHandle>(
        const clonecore::StableDiskIdentity&)>;
using WindowsDataRescueTargetOfflineSetter =
    std::function<clonecore::Status(
        const clonecore::StableDiskIdentity&,
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&,
        bool)>;
using WindowsDataRescueTargetOpener =
    std::function<clonecore::Result<diskmodel::PhysicalTargetHandle>(
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&)>;

struct WindowsDataRescueCloneDependencies final {
  WindowsDataRescueCloneReidentifier reidentify_selection;
  WindowsDataRescueProtectedTargetQuery is_protected_rescue_media;
  WindowsDataRescueSourceOpener open_read_only_source;
  WindowsDataRescueTargetOfflineSetter set_target_offline;
  WindowsDataRescueTargetOpener open_offline_target;
};

struct WindowsDataRescueCloneExecutionReport final {
  clonecore::RescueRawCopyReport raw;
  // Bytes after the source extent are deliberately not written, erased, or
  // verified and therefore cannot be described as completed clone output.
  std::uint64_t untouched_target_tail_bytes{};
  bool source_opened_read_only{};
  bool source_was_read_only_or_offline{};
  bool source_attributes_unchanged{};
  bool target_left_offline{};
  bool protected_rescue_media_excluded_by_stable_identity{};
  bool must_display_as_partial_loss{};
  bool shrinking_performed{};
  bool partition_style_conversion_performed{};
  bool boot_finalization_performed{};
};

struct WindowsDataRescueCloneOperationReport final {
  operationcore::OperationPlan plan;
  operationcore::OperationResult lifecycle;
  std::optional<WindowsDataRescueCloneExecutionReport> rescue;
};

// Read-only review. Unrecognized/empty source partition layouts remain
// eligible because rescue is whole-disk RAW, but stable identity, geometry,
// basic-device classification, target health, and protected rescue-media
// exclusion must all be certain.
[[nodiscard]] clonecore::Result<WindowsDataRescueClonePlan>
prepare_windows_data_rescue_clone(
    std::uint32_t source_disk_number,
    std::uint32_t target_disk_number,
    diskmodel::IDiskInventoryProvider& provider,
    const WindowsDataRescueProtectedTargetQuery& protected_target_query);

// A successful wrapper Result means the OperationPlan lifecycle ran. The
// caller must require lifecycle.outcome == completed and rescue.has_value().
[[nodiscard]] clonecore::Result<WindowsDataRescueCloneOperationReport>
execute_windows_data_rescue_clone(
    const WindowsDataRescueClonePlan& reviewed_plan,
    bool target_erasure_acknowledged,
    std::wstring_view typed_confirmation,
    const WindowsDataRescueCloneDependencies& dependencies,
    clonecore::DiskOperationCallbacks callbacks = {});

// Constructs native disk adapters without performing disk I/O. The protected
// rescue-media resolver remains mandatory and is supplied by the Windows UI
// connection after it has implemented the marker-backed product policy.
[[nodiscard]] WindowsDataRescueCloneDependencies
make_windows_data_rescue_clone_dependencies(
    WindowsDataRescueProtectedTargetQuery protected_target_query);

// Bounded UI text: conservative rescue classification and actual missing-map
// truth are always rendered separately.
[[nodiscard]] std::wstring format_windows_data_rescue_clone_result(
    const WindowsDataRescueCloneExecutionReport& report);

}  // namespace ytec::windowsapp

#pragma once

#include "ytec/diskmodel/physical_disk.h"
#include "ytec/imageformat/tsumugi_image_service.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace ytec::imageformat {

struct TsumugiPhysicalExistingPartitionRestoreSelection final {
  // One-based order in the reviewed drive layout plus the Windows partition
  // number and exact geometry.  The controller verifies all four values
  // against fresh inventory before and after opening the offline disk.
  std::uint32_t target_table_index{};
  std::uint32_t target_partition_number{};
  std::uint64_t target_offset{};
  std::uint64_t target_size{};

  [[nodiscard]] bool operator==(
      const TsumugiPhysicalExistingPartitionRestoreSelection&)
      const noexcept = default;
};

struct TsumugiPhysicalUnallocatedRestoreSelection final {
  std::uint64_t target_offset{};
  std::uint64_t target_size{};

  [[nodiscard]] bool operator==(
      const TsumugiPhysicalUnallocatedRestoreSelection&)
      const noexcept = default;
};

using TsumugiPhysicalIndividualPartitionTarget = std::variant<
    TsumugiPhysicalExistingPartitionRestoreSelection,
    TsumugiPhysicalUnallocatedRestoreSelection>;

// Immutable source/target placement reviewed by the user.  It deliberately
// excludes a disk number or handle; the surrounding request binds the stable
// target identity and the complete reviewed-layout hash.
struct TsumugiPhysicalIndividualPartitionRestoreSelection final {
  std::uint32_t source_table_index{};
  TsumugiPhysicalIndividualPartitionTarget target;

  [[nodiscard]] bool operator==(
      const TsumugiPhysicalIndividualPartitionRestoreSelection&)
      const noexcept = default;
};

struct TsumugiPhysicalRestoreRequest final {
  TsumugiImageVerifyRequest image;
  Sha256Digest expected_image_global_hash{};
  Sha256Digest expected_source_state_hash{};
  clonecore::StableDiskIdentity expected_target;
  Sha256Digest expected_target_layout_hash{};
  std::optional<TsumugiPhysicalIndividualPartitionRestoreSelection>
      individual_partition;
  clonecore::TargetConfirmation confirmation;
  bool administrator{};
  // Windows callers pass false. WinPE callers must obtain this value from a
  // trusted read-only resolver for the current boot medium; it is never a UI
  // checkbox or a remembered disk number.
  bool target_is_active_rescue_media{};
  clonecore::DiskOperationCallbacks callbacks;
};

struct TsumugiPhysicalRestoreReport final {
  TsumugiRestoreReport restore;
  bool initial_image_verification_completed{};
  bool target_reidentified_before_offline{};
  bool target_handle_reidentified{};
  bool target_left_offline{};
  bool boot_repair_offer_required{};
  bool partial_loss{};
};

using TsumugiPhysicalRestoreVerifier = std::function<clonecore::Result<
    TsumugiVerifiedImage>(
    const TsumugiImageVerifyRequest&,
    const clonecore::DiskOperationCallbacks&)>;

using TsumugiPhysicalRestoreTargetReidentifier =
    std::function<clonecore::Result<diskmodel::ReidentifiedPhysicalTarget>(
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&)>;

using TsumugiPhysicalRestoreTargetOfflineSetter =
    std::function<clonecore::Status(
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&,
        bool)>;

using TsumugiPhysicalRestoreTargetOpener =
    std::function<clonecore::Result<diskmodel::PhysicalTargetHandle>(
        const clonecore::StableDiskIdentity&,
        const clonecore::TargetConfirmation&)>;

using TsumugiPhysicalRestoreMbrSignatureCollector =
    std::function<clonecore::Result<std::vector<std::uint32_t>>(
        const clonecore::StableDiskIdentity&)>;

using TsumugiPhysicalRestoreConnectionTokenGenerator =
    std::function<clonecore::Result<Sha256Digest>()>;

// Invoked by the real engine after its final complete image verification and
// immediately before layout invalidation. The controller implementation
// performs a fresh inventory, stable-identity/class/layout/offline check.
using TsumugiPhysicalRestoreLockedTargetRevalidator =
    std::function<clonecore::Result<TsumugiRestoreDiskIdentity>()>;

using TsumugiPhysicalRestoreEngine = std::function<clonecore::Result<
    TsumugiRestoreReport>(
    const TsumugiImageVerifyRequest&,
    const TsumugiVerifiedImage&,
    const TsumugiRestoreDiskIdentity&,
    diskmodel::PhysicalTargetHandle,
    std::span<const std::uint32_t>,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&,
    const clonecore::DiskOperationCallbacks&)>;

using TsumugiPhysicalIndividualPartitionRestoreEngine =
    std::function<clonecore::Result<TsumugiRestoreReport>(
        const TsumugiImageVerifyRequest&,
        const TsumugiVerifiedImage&,
        const TsumugiRestoreDiskIdentity&,
        const TsumugiPhysicalIndividualPartitionRestoreSelection&,
        diskmodel::PhysicalTargetHandle,
        const TsumugiPhysicalRestoreLockedTargetRevalidator&,
        const clonecore::DiskOperationCallbacks&)>;

struct TsumugiPhysicalRestoreDependencies final {
  TsumugiPhysicalRestoreVerifier verify_image;
  TsumugiPhysicalRestoreTargetReidentifier reidentify_target;
  TsumugiPhysicalRestoreTargetOfflineSetter set_target_offline;
  TsumugiPhysicalRestoreTargetOpener open_offline_target;
  TsumugiPhysicalRestoreMbrSignatureCollector collect_mbr_signatures;
  TsumugiPhysicalRestoreConnectionTokenGenerator make_connection_token;
  TsumugiPhysicalRestoreEngine execute_engine;
  TsumugiPhysicalIndividualPartitionRestoreEngine
      execute_individual_partition_engine;
};

struct TsumugiPhysicalRestoreTargetClass final {
  bool usb_attached{};
  bool usb_memory{};
  bool dynamic_disk{};
  bool storage_spaces{};
  bool software_raid{};
  bool unresolved_hardware_raid{};
  bool unsupported_virtual{};

  [[nodiscard]] bool operator==(
      const TsumugiPhysicalRestoreTargetClass&) const noexcept = default;
};

// These hashes are shared by image creation, final review and physical
// restore. Keeping one canonical implementation prevents a target/source
// comparison from silently drifting between Windows and WinPE.
[[nodiscard]] clonecore::Result<Sha256Digest>
hash_tsumugi_source_model_v1(std::wstring_view model);

[[nodiscard]] clonecore::Result<Sha256Digest>
hash_tsumugi_source_serial_v1(
    std::string_view serial_suffix,
    std::wstring_view device_instance_id);

// Canonical stable-target digest shared by read-only review and every
// destructive adapter.  Keeping this public prevents a platform-specific
// implementation from drifting from the exact/rescue restore identity domain.
[[nodiscard]] clonecore::Result<Sha256Digest>
hash_tsumugi_physical_restore_target_identity_v1(
    const clonecore::StableDiskIdentity& identity);

[[nodiscard]] clonecore::Result<Sha256Digest>
hash_tsumugi_physical_restore_target_layout_v1(
    const diskmodel::DiskInfo& target);

[[nodiscard]] TsumugiPhysicalRestoreTargetClass
classify_tsumugi_physical_restore_target(
    const diskmodel::DiskInfo& target);

[[nodiscard]] clonecore::Status
validate_tsumugi_physical_restore_target(
    const diskmodel::DiskInfo& target,
    const TsumugiPhysicalRestoreTargetClass& target_class,
    bool target_is_active_rescue_media);

// Read-only validation shared by UI review and the destructive controller.
// Existing-partition selections must identify one exact current entry.
// Unallocated selections must use the same partition-table style as the
// image, identify a sector-aligned gap, and leave room for one new entry.
[[nodiscard]] clonecore::Status
validate_tsumugi_physical_individual_partition_selection_v1(
    const TsumugiManifest& manifest,
    const diskmodel::DiskInfo& target,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection);

// Returns deterministic, fixed-size placements at the first 1 MiB-aligned
// address of each safe gap. It performs no I/O and never mutates inventory.
[[nodiscard]] clonecore::Result<std::vector<
    TsumugiPhysicalIndividualPartitionRestoreSelection>>
find_tsumugi_physical_unallocated_restore_candidates_v1(
    const TsumugiManifest& manifest,
    const diskmodel::DiskInfo& target,
    std::uint32_t source_table_index);

// Shared fail-closed controller used by the Windows and WinPE thin adapters.
// The engine dependency is host-specialized by its factory; this controller
// itself performs no deferred job handoff and never brings a target online.
[[nodiscard]] clonecore::Result<TsumugiPhysicalRestoreReport>
execute_tsumugi_physical_restore_v1(
    const TsumugiPhysicalRestoreRequest& request,
    const TsumugiPhysicalRestoreDependencies& dependencies);

// Shared low-level exact/rescue whole-disk engine. It reopens and completely
// verifies the image before the first target callback, owns the exact opened
// writer, invalidates both table locations, flushes and reads every payload
// write back, and exposes the primary GPT/MBR table last.
[[nodiscard]] clonecore::Result<TsumugiRestoreReport>
execute_tsumugi_physical_whole_disk_restore_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    std::span<const std::uint32_t> disallowed_mbr_signatures,
    TsumugiRestoreHost host,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Individual-partition engine. Existing targets retain unchanged metadata.
// Unallocated targets receive one preserving GPT/MBR entry only after every
// payload block has passed flush/readback; metadata failure restores the exact
// captured table bytes from the same locked handle.
[[nodiscard]] clonecore::Result<TsumugiRestoreReport>
execute_tsumugi_physical_individual_partition_restore_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    TsumugiRestoreHost host,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Compatibility entry point retained for existing callers and tests. It
// accepts only an existing-partition selection.
[[nodiscard]] clonecore::Result<TsumugiRestoreReport>
execute_tsumugi_physical_existing_partition_restore_engine_v1(
    const TsumugiImageVerifyRequest& image_request,
    const TsumugiVerifiedImage& initially_verified,
    const TsumugiRestoreDiskIdentity& target_identity,
    const TsumugiPhysicalIndividualPartitionRestoreSelection& selection,
    std::unique_ptr<clonecore::ITargetDiskWriter> target,
    TsumugiRestoreHost host,
    const TsumugiPhysicalRestoreLockedTargetRevalidator&
        revalidate_locked_target,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Read-only connected-disk signature inventory and same-connection random
// token helpers. They do not open any target for write.
[[nodiscard]] clonecore::Result<std::vector<std::uint32_t>>
collect_tsumugi_mbr_signatures_with_windows_apis(
    const clonecore::StableDiskIdentity& selected_target);

[[nodiscard]] clonecore::Result<Sha256Digest>
make_tsumugi_connection_instance_hash_with_windows_apis();

}  // namespace ytec::imageformat

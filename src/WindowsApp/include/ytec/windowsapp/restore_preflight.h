#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"
#include "ytec/imageformat/tsumugi_image_service.h"
#include "ytec/imageformat/tsumugi_physical_restore.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::windowsapp {

// A successful report is read-only evidence and never authorizes target I/O.
struct TsumugiRestoreImagePreflightReport final {
  std::wstring canonical_path;
  imageformat::TsumugiImageStorageFileSystem storage_file_system{
      imageformat::TsumugiImageStorageFileSystem::unknown};
  std::uint64_t image_length{};
  imageformat::Sha256Digest global_hash{};
  imageformat::TsumugiHeader header;
  imageformat::TsumugiManifest manifest;
  std::vector<clonecore::ByteRange> unreadable_ranges;
  bool encrypted{};
  bool partial_loss{};
  bool complete_container_verified{};
  bool metadata_verified{};
  bool restore_layout_verified{};
  bool restore_execution_enabled{};
};

struct TsumugiRestoreImagePreflightOptions final {
  // The password view is consumed synchronously and is never retained in the
  // returned report.  Omitting it for an encrypted image fails closed.
  std::optional<std::string_view> password;
  std::function<bool()> cancellation_requested;
  std::function<void(const clonecore::DiskOperationProgress&)> progress;
};

enum class TsumugiRestorePasswordPromptDecision : std::uint8_t {
  stop,
  no_password_required,
  prompt_required,
  password_ready,
};

// Pure UI-flow gate.  The fixed-header probe is only a bounded hint for
// deciding whether to show the password dialog; complete image verification
// remains mandatory after this decision.
struct TsumugiRestorePasswordPromptState final {
  bool header_probe_succeeded{};
  bool encrypted{};
  std::optional<bool> prompt_accepted;
  bool password_available{};
};

[[nodiscard]] TsumugiRestorePasswordPromptDecision
evaluate_tsumugi_restore_password_prompt(
    const TsumugiRestorePasswordPromptState& state) noexcept;

// The image container must remain readable for the entire restore.  This
// read-only gate rejects the disk that currently backs the image path and also
// fails when either disk cannot be identified stably.
[[nodiscard]] clonecore::Status
validate_tsumugi_restore_storage_target_separation(
    const clonecore::StableDiskIdentity& image_storage,
    const clonecore::StableDiskIdentity& restore_target);

enum class RestoreTargetSelectionIssue : std::uint8_t {
  image_unavailable,
  inventory_loading,
  inventory_unavailable,
  inventory_has_issues,
  target_not_selected,
  target_is_original_source,
  target_is_system,
  target_state_unknown,
  target_is_read_only,
  target_is_removable,
  target_health_abnormal,
  target_style_unknown,
  target_too_small,
  logical_sector_mismatch,
  individual_selection_invalid,
  unstable_identity,
  ready_for_confirmation,
};

struct RestoreTargetSelectionView final {
  RestoreTargetSelectionIssue issue{
      RestoreTargetSelectionIssue::image_unavailable};
  bool ready_for_confirmation{};
  bool restore_execution_enabled{};
  std::optional<clonecore::StableDiskIdentity> target_identity;
  std::wstring message;
};

// Opens one local NTFS/exFAT .tsumugi file without write/delete sharing and
// completely authenticates the container, every plaintext chunk, and the
// typed manifest.  It never opens a target disk.
[[nodiscard]] clonecore::Result<TsumugiRestoreImagePreflightReport>
inspect_tsumugi_restore_image_file(
    const std::wstring& requested_path,
    const TsumugiRestoreImagePreflightOptions& options = {});

// Evaluates only information already obtained through read-only inventory.
// Passing this gate does not authorize opening or writing a target disk.
// Candidate gate for a completely verified .tsumugi image. It uses only
// read-only inventory and therefore deliberately leaves execution disabled.
[[nodiscard]] RestoreTargetSelectionView
evaluate_tsumugi_restore_target_selection(
    const TsumugiRestoreImagePreflightReport* image,
    const diskmodel::InventoryReport* inventory,
    std::optional<std::size_t> target_index,
    bool inventory_loading,
    std::optional<imageformat::
        TsumugiPhysicalIndividualPartitionRestoreSelection>
        individual_partition = std::nullopt);

}  // namespace ytec::windowsapp

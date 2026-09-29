#pragma once

#include "ytec/bootrepair/automatic_repair_plan.h"
#include "ytec/bootrepair/efi_delete_transaction_windows.h"
#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/bootrepair/winre_registration.h"
#include "ytec/winpeapp/repair_layout.h"

#include <span>
#include <string_view>
#include <optional>
#include <vector>

namespace ytec::winpeapp {

// The executable subset of one read-only automatic discovery result. The
// current bridge accepts an unambiguous supported BIOS/MBR installation or a
// GPT/UEFI installation whose single existing ESP was read-only classified as
// Microsoft-only/empty. Ambiguous EFI content stays fail-closed; an independently
// named third-party top-level namespace is handled by the separate reviewed
// delete transaction. Current-PC NVRAM is a separate
// explicit reviewed transaction and is never delegated to BCDBoot.
struct WinPeAutomaticBootRepairReview final {
  bootrepair::BootRepairTargetRequest request;
  std::uint32_t windows_partition_number{};
  std::uint32_t system_partition_number{};
  bool temporary_system_mount_required{};
};

[[nodiscard]] clonecore::Result<WinPeAutomaticBootRepairReview>
build_executable_automatic_boot_repair_review(
    const bootrepair::AutomaticBootRepairPlan& plan);

struct WinPeAutomaticBootRepairProductChoice final {
  bootrepair::AutomaticWindowsRegistrationPolicy windows_policy{
      bootrepair::AutomaticWindowsRegistrationPolicy::selected_only};
  std::vector<std::uint32_t> windows_partition_priority;
  bootrepair::AutomaticThirdPartyEfiPolicy third_party_efi_policy{
      bootrepair::AutomaticThirdPartyEfiPolicy::preserve};
  bool third_party_efi_delete_explicitly_approved{};
  bootrepair::AutomaticNvramRepairPolicy nvram_policy{
      bootrepair::AutomaticNvramRepairPolicy::leave_unchanged};
  bool current_pc_nvram_explicitly_approved{};
  bool explicitly_approved{};
};

// Product-safe choice request for the currently implemented transaction.
// The caller must obtain an explicit all-in-displayed-order or selected-only
// decision from the user; this function never invents a Windows priority.
// Exactly one existing system partition is used. Third-party EFI defaults to
// preserve and delete requires a separate explicit dangerous choice. Current-PC
// NVRAM repair is accepted only for UEFI after its
// own explicit this-PC choice; WinRE registration is never attempted implicitly.
[[nodiscard]] clonecore::Result<
    bootrepair::AutomaticBootRepairChoiceRequest>
build_product_automatic_boot_repair_choice_request(
    const bootrepair::AutomaticBootRepairPlan& plan,
    const WinPeAutomaticBootRepairProductChoice& choice);

struct WinPeReviewedAutomaticBootRepairExecution final {
  std::vector<bootrepair::BootRepairTargetRequest>
      requests_in_boot_priority;
  std::vector<std::uint32_t> windows_partition_numbers_in_boot_priority;
  std::uint32_t system_partition_number{};
  bool temporary_system_mount_required{};
  bool third_party_efi_preserved{};
  bool third_party_efi_delete_requested{};
  bool repair_current_pc_nvram{};
  bool normal_boot_only_partial{};
  struct WinReAction final {
    std::uint32_t windows_partition_number{};
    bootrepair::AutomaticWinReRepairDisposition disposition{
        bootrepair::AutomaticWinReRepairDisposition::
            normal_boot_only_partial};
    std::wstring offline_windows_directory;
    std::wstring candidate_directory;
    std::uint32_t expected_target_partition_number{};
    bootrepair::WinReRegisteredPathKind expected_registered_path_kind{
        bootrepair::WinReRegisteredPathKind::windows_system32_recovery};
    bootrepair::WinReDiagnosticReport prior_diagnostic;
    std::optional<bootrepair::WinReRegistrationImageIdentity>
        reviewed_candidate;
  };
  std::vector<WinReAction> winre_actions_in_boot_priority;
};

// Maps immutable core-reviewed choices into the production standalone BCD
// transaction, an ordered WinRE action list and an explicitly reviewed NVRAM
// disposition. ESP creation remains separate; deletion is only a routing flag
// for the dedicated immutable-manifest transaction and is never delegated to
// the standalone BCD service.
[[nodiscard]] clonecore::Result<
    WinPeReviewedAutomaticBootRepairExecution>
build_executable_reviewed_automatic_boot_repair(
    const bootrepair::ReviewedAutomaticBootRepairChoices& reviewed);

// True only for one reviewed GPT ESP whose ownership evidence proves that all
// non-Microsoft content consists of independent normal directories directly
// below EFI. Microsoft/Boot, fallback, ambiguous and untrusted layouts never
// expose the dangerous product choice.
[[nodiscard]] bool automatic_boot_repair_allows_third_party_efi_delete(
    const bootrepair::AutomaticBootRepairPlan& plan) noexcept;

// Builds the immutable disk/partition/Volume GUID routing request used by both
// the dedicated review and the execution-time exact reinspection.
[[nodiscard]] clonecore::Result<bootrepair::WindowsEfiDeleteEspRequest>
build_windows_efi_delete_esp_request(
    const bootrepair::ReviewedAutomaticBootRepairChoices& reviewed);

[[nodiscard]] std::wstring format_reviewed_efi_delete_plan(
    const bootrepair::ReviewedEfiDeletePlan& reviewed);

[[nodiscard]] std::wstring format_efi_delete_transaction_report(
    const bootrepair::EfiDeleteTransactionReport& report);

struct WinPeAutomaticBootRepairWinReImageBinding final {
  std::uint32_t windows_partition_number{};
  bootrepair::WinReRegistrationImageIdentity identity;
};

// Binds the exact Winre.wim objects opened and hashed before the uppercase-OK
// confirmation. Every registration action requires exactly one binding;
// existing registrations and partial actions reject stray image evidence.
[[nodiscard]] clonecore::Result<
    WinPeReviewedAutomaticBootRepairExecution>
bind_reviewed_automatic_boot_repair_winre_images(
    WinPeReviewedAutomaticBootRepairExecution execution,
    std::span<const WinPeAutomaticBootRepairWinReImageBinding> bindings);

// Binds every read-only standalone inspection back to the reviewed plan and
// ordered requests before the confirmation UI becomes available.
[[nodiscard]] clonecore::Status
validate_reviewed_automatic_boot_repair_inspections(
    const bootrepair::AutomaticBootRepairPlan& plan,
    const bootrepair::ReviewedAutomaticBootRepairChoices& choices,
    const WinPeReviewedAutomaticBootRepairExecution& execution,
    std::span<const bootrepair::BootRepairTargetSelection> inspected);

// Closes the seam between automatic discovery and the existing standalone
// transaction. The inventory identity, complete DiskInfo, every selected
// partition field, and the executable review mapping must all describe the
// same target before the review can be retained for confirmation.
[[nodiscard]] clonecore::Status validate_automatic_boot_repair_inspection(
    const bootrepair::AutomaticBootRepairPlan& plan,
    const WinPeAutomaticBootRepairReview& review,
    const bootrepair::BootRepairTargetSelection& inspected);

// Exact semantic comparison used between review and execution. Disk numbers
// alone are insufficient; the complete selected layout, candidate extents,
// Volume GUID mappings, version/WinRE evidence and policy flags must remain
// unchanged before the standalone transaction is allowed to run.
[[nodiscard]] bool equivalent_automatic_boot_repair_plan(
    const bootrepair::AutomaticBootRepairPlan& reviewed,
    const bootrepair::AutomaticBootRepairPlan& observed) noexcept;

struct WinPeAutomaticBootRepairLayout final {
  UiRectangle target_disk;
  UiRectangle inspect;
  UiRectangle confirmation_token;
  UiRectangle execute;
  UiRectangle cancel_review;
  UiRectangle output;
};

[[nodiscard]] WinPeAutomaticBootRepairLayout
build_winpe_automatic_boot_repair_layout(
    int client_width,
    int client_height) noexcept;

struct WinPeAutomaticBootRepairUiInput final {
  bool inventory_ready{};
  bool idle{};
  bool target_selected{};
  bool reviewed{};
  bool execution_active{};
  std::wstring_view confirmation_text;
};

struct WinPeAutomaticBootRepairUiView final {
  bool target_enabled{};
  bool inspect_enabled{};
  bool confirmation_visible{};
  bool confirmation_enabled{};
  bool execute_visible{};
  bool execute_enabled{};
  bool cancel_review_visible{};
  bool cancel_review_enabled{};
};

[[nodiscard]] WinPeAutomaticBootRepairUiView
build_winpe_automatic_boot_repair_ui_view(
    const WinPeAutomaticBootRepairUiInput& input) noexcept;

}  // namespace ytec::winpeapp

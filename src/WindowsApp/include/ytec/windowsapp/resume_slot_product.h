#pragma once

#include "ytec/operationcore/resume_slot.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ytec::windowsapp {

// Route-neutral, admission-only binding for a Windows product writer.  It is
// never a persistent resume capability, checkpoint, or execution plan.  The
// output backing identity is included in the immutable hash because
// OperationPlan has no output role for file-producing routes.
struct WindowsResumeSlotAdmissionReview final {
  operationcore::OperationId operation_id{};
  operationcore::OperationKind kind{operationcore::OperationKind::clone};
  std::optional<clonecore::StableDiskIdentity> source;
  std::optional<clonecore::StableDiskIdentity> target;
  std::optional<clonecore::StableDiskIdentity> output_backing;
  std::uint64_t expected_work_bytes{};
  std::span<const std::wstring_view> immutable_review_fields;
  std::span<const operationcore::Sha256Digest> immutable_review_digests;
};

[[nodiscard]] clonecore::Result<operationcore::OperationPlan>
make_windows_resume_slot_admission_plan(
    const WindowsResumeSlotAdmissionReview& review);

[[nodiscard]] clonecore::Result<operationcore::OperationId>
make_windows_resume_slot_admission_operation_id_with_windows_apis();

struct WindowsResumeSlotStartupView final {
  bool active{};
  bool resume_action_available{};
  std::wstring title;
  std::wstring details;
  std::wstring owned_discard_summary;
  std::optional<operationcore::ResumeSlotBinding> binding;
};

// Pure projection for an already validated slot record.  Windows currently
// has no product resume controller, so an active record never enables a fake
// resume action; it remains available only for exact-bound owned discard.
[[nodiscard]] clonecore::Result<WindowsResumeSlotStartupView>
make_windows_resume_slot_startup_view(
    const std::optional<operationcore::ResumeSlotRecord>& record);

[[nodiscard]] clonecore::Result<WindowsResumeSlotStartupView>
inspect_windows_resume_slot(operationcore::IResumeSlotPlatform& platform);

[[nodiscard]] clonecore::Status guard_new_windows_operation_start(
    const operationcore::OperationPlan& admission_plan,
    operationcore::IResumeSlotPlatform& platform);

[[nodiscard]] clonecore::Status discard_bound_windows_resume_slot(
    const operationcore::ResumeSlotBinding& reviewed_binding,
    operationcore::IResumeSlotPlatform& platform);

// Production wrappers use exactly the current EXE-adjacent data slot.  The
// protected identities are used only to report truthful backing separation;
// observe/discard do not mutate storage and OperationCore requires separation
// only for create/replace, which these Windows wrappers never expose.
[[nodiscard]] clonecore::Result<WindowsResumeSlotStartupView>
inspect_current_windows_resume_slot();

[[nodiscard]] clonecore::Status guard_current_windows_operation_start(
    const operationcore::OperationPlan& admission_plan,
    std::span<const clonecore::StableDiskIdentity> protected_identities);

[[nodiscard]] clonecore::Status discard_current_windows_resume_slot(
    const operationcore::ResumeSlotBinding& reviewed_binding);

}  // namespace ytec::windowsapp

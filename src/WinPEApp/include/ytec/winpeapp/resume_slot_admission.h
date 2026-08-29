#pragma once

#include "ytec/operationcore/resume_slot.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ytec::winpeapp {

// Builds a route-neutral OperationPlan exclusively for the global
// SingleResumeSlot admission gate. It is never a resumable capability,
// checkpoint, execution plan, or permission to resume. The product caller
// must bind the final reviewed source/target/output choices through the
// identities, framed text fields, and operation-specific digests.
[[nodiscard]] clonecore::Result<operationcore::OperationPlan>
make_winpe_resume_slot_admission_plan(
    operationcore::OperationId operation_id,
    operationcore::OperationKind kind,
    std::optional<clonecore::StableDiskIdentity> source,
    std::optional<clonecore::StableDiskIdentity> target,
    std::uint64_t expected_work_bytes,
    std::span<const std::wstring_view> immutable_review_fields,
    std::span<const operationcore::Sha256Digest> immutable_review_digests = {});

// Freshly observes the one fixed slot through OperationCore. Active, unknown,
// corrupt, orphaned, relinked, or relocated state refuses every new writer
// without mutating the checkpoint or its owned partial.
[[nodiscard]] clonecore::Status guard_new_winpe_operation_start(
    const operationcore::OperationPlan& admission_plan,
    operationcore::IResumeSlotPlatform& platform);

// Resume is not a new operation. It is admitted only when the complete binding
// still opens the same valid slot. This is a read-only preflight; the owning
// restore controller must repeat the binding and storage proofs before I/O.
[[nodiscard]] clonecore::Status guard_bound_winpe_restore_resume(
    const operationcore::ResumeSlotBinding& reviewed_binding,
    operationcore::IResumeSlotPlatform& platform);

}  // namespace ytec::winpeapp

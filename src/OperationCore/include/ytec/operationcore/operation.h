#pragma once

#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace ytec::operationcore {

inline constexpr std::uint32_t kOperationPlanSchemaVersion = 1U;
inline constexpr std::wstring_view kDestructiveConfirmationToken = L"OK";

using OperationId = std::array<std::byte, 16>;
using Sha256Digest = std::array<std::byte, 32>;

enum class OperationKind : std::uint8_t {
  clone,
  image_create,
  image_restore,
  boot_repair,
  rescue_clone,
  rescue_image,
};

enum class OperationEnvironment : std::uint8_t {
  windows,
  winpe,
};

struct OperationPlan final {
  std::uint32_t schema_version{kOperationPlanSchemaVersion};
  OperationId operation_id{};
  OperationKind kind{OperationKind::clone};
  OperationEnvironment environment{OperationEnvironment::windows};
  std::optional<clonecore::StableDiskIdentity> source;
  std::optional<clonecore::StableDiskIdentity> target;
  // Exact byte count for image/repair operations. Clone/rescue-clone plans
  // use this as the immutable logical upper bound because a filesystem
  // used-range plan is formed only after the source Snapshot exists.
  std::uint64_t expected_work_bytes{};

  // The caller hashes every immutable operation-specific choice (partition
  // selection, target layout, image identity, conversion mode, and so on).
  // OperationCore binds that digest to the common identity and lifecycle data.
  Sha256Digest immutable_payload_hash{};
};

[[nodiscard]] clonecore::Status validate_operation_plan(
    const OperationPlan& plan);

[[nodiscard]] clonecore::Result<Sha256Digest> hash_operation_plan(
    const OperationPlan& plan);

[[nodiscard]] bool operation_requires_confirmation(
    OperationKind kind) noexcept;

// Deliberately performs no trimming, case folding, or width conversion.
[[nodiscard]] clonecore::Status validate_operation_confirmation(
    const OperationPlan& plan,
    std::wstring_view typed_token);

struct ReidentifiedOperation final {
  std::optional<clonecore::StableDiskIdentity> source;
  std::optional<clonecore::StableDiskIdentity> target;
};

[[nodiscard]] clonecore::Status validate_reidentified_operation(
    const OperationPlan& plan,
    const ReidentifiedOperation& observed);

struct ExecutionEvidence final {
  // Actual bytes read, written, and read back. For sparse clone kinds this
  // may be smaller than OperationPlan::expected_work_bytes, but never zero or
  // greater than that reviewed logical upper bound.
  std::uint64_t processed_work_bytes{};
  Sha256Digest output_hash{};
};

struct VerificationEvidence final {
  // Must exactly match the actual execution amount, including sparse clones.
  std::uint64_t verified_work_bytes{};
  Sha256Digest output_hash{};
};

enum class OperationPhase : std::uint8_t {
  planning,
  reidentifying,
  awaiting_confirmation,
  executing,
  verifying,
  completed,
};

enum class OperationOutcome : std::uint8_t {
  completed,
  failed,
  cancelled,
};

struct OperationResult final {
  OperationId operation_id{};
  Sha256Digest plan_hash{};
  OperationPhase phase{OperationPhase::planning};
  OperationOutcome outcome{OperationOutcome::failed};
  std::uint64_t processed_work_bytes{};
  std::uint64_t verified_work_bytes{};
  std::optional<clonecore::Error> error;
};

using ReidentifyCallback = std::function<
    clonecore::Result<ReidentifiedOperation>(const OperationPlan&)>;
using ExecuteCallback = std::function<clonecore::Result<ExecutionEvidence>(
    const OperationPlan&,
    const clonecore::DiskOperationCallbacks&)>;
using VerifyCallback = std::function<clonecore::Result<VerificationEvidence>(
    const OperationPlan&,
    const ExecutionEvidence&,
    const clonecore::DiskOperationCallbacks&)>;

struct OperationCallbacks final {
  ReidentifyCallback reidentify;
  ExecuteCallback execute;
  VerifyCallback verify;
  clonecore::DiskOperationCallbacks disk_operation;
};

// This coordinator owns no disk handle and performs no disk I/O. Concrete
// engines are injected only after the plan, fresh identities, and exact OK
// confirmation have passed. Verification is mandatory and cannot be skipped.
[[nodiscard]] OperationResult run_operation(
    const OperationPlan& plan,
    std::wstring_view typed_confirmation,
    const OperationCallbacks& callbacks);

}  // namespace ytec::operationcore

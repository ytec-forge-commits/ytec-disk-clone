#pragma once

#include "ytec/operationcore/windows_resume_slot_platform.h"
#include "ytec/winpeapp/direct_image_create.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::winpeapp {

enum class DirectImageCreateResumeAction : std::uint8_t {
  start_new,
  resume_existing,
  cancel,
};

struct DirectImageCreateResumeContinuityV1 final {
  std::string created_utc;
  std::string app_version;
  imageformat::TsumugiCreateVerificationMode verification_mode{
      imageformat::TsumugiCreateVerificationMode::complete};
  bool encrypted{};
  // Empty is the legacy whole-disk contract. Partial selections use the
  // canonical 128-bit bitmap representation; no path, password, or key is
  // persisted.
  std::vector<std::uint32_t> selected_partition_numbers;
  std::array<std::byte, 16U> image_id{};
  imageformat::TsumugiArgon2Parameters argon2{};
  std::array<std::byte, imageformat::kTsumugiGcmNonceBytes> base_nonce{};
};

// Canonical, bounded and non-secret.  Passwords, derived keys and plaintext
// are not representable in this token.  Parsing rejects every non-canonical
// spelling rather than migrating or guessing it.
[[nodiscard]] clonecore::Result<std::wstring>
build_direct_image_create_resume_continuity_v1(
    const DirectImageCreateResumeContinuityV1& continuity);

[[nodiscard]] clonecore::Result<DirectImageCreateResumeContinuityV1>
parse_direct_image_create_resume_continuity_v1(
    const std::wstring& token);

struct DirectImageCreateResumeStorageProof final {
  operationcore::Sha256Digest checkpoint_storage_identity_hash{};
  operationcore::Sha256Digest source_storage_identity_hash{};
  operationcore::Sha256Digest destination_storage_identity_hash{};
  bool all_identities_from_open_handles{};
};

using DirectImageCreateResumeStorageProbe = std::function<
    clonecore::Result<DirectImageCreateResumeStorageProof>(
        const DirectImageCreateRequest&,
        const std::optional<operationcore::ResumeSlotRecord>&)>;

using DirectImageCreateResumeSlotPlatformFactory = std::function<
    clonecore::Result<std::unique_ptr<operationcore::IResumeSlotPlatform>>(
        std::vector<operationcore::WindowsResumeOwnedObject>)>;

struct DirectImageCreateResumeDependencies final {
  DirectImageCreateDependencies direct;
  operationcore::IResumeSlotPlatform* slot_platform{};
  DirectImageCreateResumeSlotPlatformFactory make_bound_slot_platform;
  DirectImageCreateResumeStorageProbe prove_storage_separation;
};

struct DirectImageCreateResumeCommand final {
  DirectImageCreateResumeAction action{
      DirectImageCreateResumeAction::start_new};
  operationcore::OperationId new_operation_id{};
  std::optional<operationcore::ResumeSlotBinding> reviewed_existing_slot;
};

struct DirectImageCreateResumeStartupObservation final {
  operationcore::PersistentPeExactImageCreateObjectState object_state{
      operationcore::PersistentPeExactImageCreateObjectState::no_slot};
  std::optional<operationcore::ResumeSlotBinding> binding;
  std::optional<clonecore::StableDiskIdentity> source;
  std::wstring final_path;
  std::uint64_t verified_logical_bytes{};
  std::uint64_t verified_chunk_count{};
  std::uint64_t expected_logical_bytes{};
  std::optional<operationcore::CheckpointPhase> checkpoint_phase;
  std::optional<DirectImageCreateResumeContinuityV1> continuity;
};

// Read-only startup classifier. Unknown/corrupt/orphaned/relinked state is a
// failure and remains untouched. A slot for another capability is reported as
// other_capability so the global startup router can delegate it.
[[nodiscard]] clonecore::Result<DirectImageCreateResumeStartupObservation>
inspect_direct_image_create_resume_v1(
    operationcore::IResumeSlotPlatform& platform);

[[nodiscard]] clonecore::Result<std::wstring>
format_direct_image_create_resume_startup_review_v1(
    const DirectImageCreateResumeStartupObservation& observation);

// Staged state only.  The caller must show its own second confirmation and
// pass the exact displayed binding. Published final files are never deleted
// by this API; their transaction must instead complete full verification and
// retirement.
[[nodiscard]] clonecore::Status discard_direct_image_create_resume_v1(
    const operationcore::ResumeSlotBinding& reviewed_binding,
    operationcore::IResumeSlotPlatform& platform);

// Exact (non-rescue), create-new-only product path.  It creates schema v3
// before the first owned-object byte, reuses only a fully reverified prefix,
// then publishes to an absent final name and retires journal/slot only after
// complete final verification.
[[nodiscard]] clonecore::Result<DirectImageCreateReport>
execute_direct_image_create_resume_v1(
    const DirectImageCreateRequest& request,
    const DirectImageCreateResumeCommand& command,
    const DirectImageCreateResumeDependencies& dependencies);

struct DirectImageCreateResumeStoragePlatformV1 final {
  operationcore::WindowsResumeDataBackingProbe prove_data_backing;
  DirectImageCreateResumeStorageProbe prove_image_create_storage;
  DirectImageCreateResumeSlotPlatformFactory make_bound_slot_platform;
};

[[nodiscard]] clonecore::Result<
    DirectImageCreateResumeStoragePlatformV1>
make_direct_image_create_windows_resume_storage_platform_v1();

[[nodiscard]] clonecore::Result<DirectImageCreateReport>
execute_direct_image_create_resume_with_windows_apis_v1(
    const DirectImageCreateRequest& request,
    const DirectImageCreateResumeCommand& command);

}  // namespace ytec::winpeapp

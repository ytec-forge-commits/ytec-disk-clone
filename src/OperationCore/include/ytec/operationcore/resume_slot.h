#pragma once

#include "ytec/operationcore/checkpoint.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::operationcore {

// The product owns one active resume slot. Callers never pass a path to an
// individual operation; the platform adapter is configured once and reports
// the fixed path on every observation.
inline constexpr std::wstring_view kResumeSlotFileName =
    L"active.checkpoint";

enum class ResumeCapability : std::uint8_t {
  persistent_exact_restore = 0U,
  persistent_rescue_restore = 1U,
  same_process_only_vss_image_create = 2U,
  same_process_only_vss_clone = 3U,
  same_process_only_pe_image_create = 4U,
  same_process_only_pe_clone = 5U,
  unsupported_shrink_migration = 6U,
  unsupported_raw_rescue = 7U,
  persistent_pe_exact_image_create = 8U,
};

enum class ResumeLifetime : std::uint8_t {
  persistent,
  same_process_only,
  unsupported,
};

[[nodiscard]] ResumeLifetime resume_lifetime(
    ResumeCapability capability) noexcept;

[[nodiscard]] clonecore::Status validate_resume_capability(
    ResumeCapability capability,
    OperationKind kind,
    OperationEnvironment environment);

// These are operation-specific identity digests produced by the owning
// engine. They may identify a disk, an authenticated image, or an output file
// object. The slot layer never guesses them from a path or disk number.
struct ResumeIdentityBinding final {
  Sha256Digest source_identity_hash{};
  Sha256Digest target_identity_hash{};
  Sha256Digest output_identity_hash{};
};

// A .partial file is optional (whole-disk restore has none). When declared,
// the platform must obtain file_object_identity_hash from an open regular
// single-link file, not from a pathname alone.
struct ResumeOwnedPartialBinding final {
  OperationId operation_id{};
  ResumeIdentityBinding identities{};
  Sha256Digest file_object_identity_hash{};
};

enum class ResumeOwnedObjectRole : std::uint8_t {
  image_partial = 1U,
  image_resume_journal = 2U,
  rescue_stage = 3U,
};

inline constexpr std::size_t kMaximumResumeOwnedObjects = 3U;

// Schema-v3 operations bind every mutable output object by an explicit role
// and an opened file-object identity. Paths are platform-owned routing data and
// are intentionally absent from this logical record.
struct ResumeOwnedObjectBinding final {
  ResumeOwnedObjectRole role{ResumeOwnedObjectRole::image_partial};
  OperationId operation_id{};
  ResumeIdentityBinding identities{};
  Sha256Digest file_object_identity_hash{};

  [[nodiscard]] bool operator==(
      const ResumeOwnedObjectBinding&) const noexcept = default;
};

struct ResumeOwnedObjectReviewBinding final {
  ResumeOwnedObjectRole role{ResumeOwnedObjectRole::image_partial};
  Sha256Digest file_object_identity_hash{};

  [[nodiscard]] bool operator==(
      const ResumeOwnedObjectReviewBinding&) const noexcept = default;
};

struct ResumeSlotRecord final {
  ResumeCapability capability{ResumeCapability::persistent_exact_restore};
  ParsedCheckpoint checkpoint{};
  ResumeIdentityBinding identities{};
  std::optional<ResumeOwnedPartialBinding> owned_partial;
  std::vector<ResumeOwnedObjectBinding> owned_objects;
};

// A stale UI observation cannot authorize resume, replacement, or discard.
// Every mutating platform call receives this complete immutable binding and
// must re-check it on the open file handles immediately before mutation.
struct ResumeSlotBinding final {
  ResumeCapability capability{ResumeCapability::persistent_exact_restore};
  OperationId operation_id{};
  ResumeIdentityBinding identities{};
  Sha256Digest checkpoint_record_hash{};
  std::optional<Sha256Digest> partial_file_object_identity_hash;
  std::vector<ResumeOwnedObjectReviewBinding> owned_object_file_bindings;
};

struct ResumeFileStorageProof final {
  bool exists{};
  bool is_regular_file{};
  bool is_reparse_free{};
  std::uint32_t hard_link_count{};
};

// This proof is deliberately produced by a platform seam. A concrete Windows
// adapter must use opened handles to prove regular-file/reparse/link state and
// backing-storage placement. A query failure is not represented as false data;
// it must fail observe_fixed_slot().
struct ResumeSlotStorageProof final {
  std::wstring checkpoint_path;
  bool paths_are_canonical_local{};
  bool parent_chain_reparse_free{};
  bool placement_separated_from_source{};
  bool checkpoint_and_partial_paths_distinct{};
  ResumeFileStorageProof checkpoint_file{};
  ResumeFileStorageProof owned_partial_file{};
  std::vector<ResumeFileStorageProof> owned_object_files;
};

struct ResumeSlotObservation final {
  ResumeSlotStorageProof storage{};
  std::optional<ResumeSlotRecord> slot;
  std::optional<ResumeOwnedPartialBinding> observed_owned_partial;
  std::vector<ResumeOwnedObjectBinding> observed_owned_objects;
};

enum class PersistentPeExactImageCreateObjectState : std::uint8_t {
  no_slot,
  other_capability,
  staged,
  published,
  retirement_pending,
};

// Capability-8-only recovery observation.  The final path is derived from
// the authenticated image-partial path in the slot envelope; callers never
// supply or relink it.  A published state is accepted only when the completed
// name opens the exact pre-move image-partial File ID.
struct PersistentPeExactImageCreateObservation final {
  PersistentPeExactImageCreateObjectState state{
      PersistentPeExactImageCreateObjectState::no_slot};
  std::optional<ResumeSlotRecord> slot;
  std::optional<ResumeSlotBinding> binding;
  std::wstring final_path;
  bool final_path_available{};
};

// Product verification evidence produced while the concrete platform keeps
// the published file locked against write/delete replacement.  The platform
// also compares image_length/global_hash with the commit-ready checkpoint;
// booleans alone can never retire the transaction.
struct PersistentPeExactImageCreateVerification final {
  std::uint64_t image_length{};
  Sha256Digest global_hash{};
  bool header_hash_verified{};
  bool metadata_authenticated{};
  bool all_chunks_verified{};
  bool global_hash_verified{};
};

using PersistentPeExactImageCreateReproof =
    std::function<clonecore::Status()>;
using PersistentPeExactImageCreateVerifier = std::function<
    clonecore::Result<PersistentPeExactImageCreateVerification>(
        const std::wstring&)>;

struct PersistentPeExactImageCreateCommitRequest final {
  ResumeSlotBinding reviewed_binding;
  std::wstring reviewed_final_path;
  // Required before the staged object is published.  Post-publish crash
  // recovery may leave it empty because no source byte is read or rewritten.
  PersistentPeExactImageCreateReproof reprove_before_publish;
  PersistentPeExactImageCreateVerifier verify_published_image;
};

struct PersistentPeExactImageCreateCommitReport final {
  bool recovered_after_publish{};
  bool image_published{};
  bool complete_image_verified{};
  bool journal_retired{};
  bool slot_retired{};
};

class IResumeSlotPlatform {
 public:
  virtual ~IResumeSlotPlatform() = default;

  // Observes one configured slot only. Directory scanning and history/list
  // semantics are outside this interface.
  [[nodiscard]] virtual clonecore::Result<ResumeSlotObservation>
  observe_fixed_slot() = 0;

  [[nodiscard]] virtual clonecore::Status create_fixed_slot(
      const ResumeSlotRecord& record) = 0;

  [[nodiscard]] virtual clonecore::Status replace_fixed_slot(
      const Sha256Digest& expected_checkpoint_record_hash,
      const ResumeSlotRecord& next) = 0;

  // If binding declares a partial, both open objects must still match and the
  // platform removes that owned partial and checkpoint as one guarded action.
  // If it does not, only the exactly matched checkpoint may be removed.
  [[nodiscard]] virtual clonecore::Status
  discard_fixed_slot_and_owned_partial(
      const ResumeSlotBinding& binding) = 0;

  // Narrow capability-8 extension.  Default platforms refuse it.  It cannot
  // delete a completed-name file or any object not bound by the exact slot.
  [[nodiscard]] virtual clonecore::Result<
      PersistentPeExactImageCreateObservation>
  inspect_persistent_pe_exact_image_create();

  // Publishes only an exact commit-ready image-partial to an absent final
  // path, or recovers that same File ID after a crash.  The completed image is
  // fully verified before journal then checkpoint retirement.
  [[nodiscard]] virtual clonecore::Result<
      PersistentPeExactImageCreateCommitReport>
  commit_persistent_pe_exact_image_create(
      const PersistentPeExactImageCreateCommitRequest& request);
};

[[nodiscard]] clonecore::Status validate_resume_slot_record(
    const ResumeSlotRecord& record);

[[nodiscard]] clonecore::Result<ResumeSlotBinding>
make_resume_slot_binding(const ResumeSlotRecord& record);

class SingleResumeSlot final {
 public:
  explicit SingleResumeSlot(IResumeSlotPlatform& platform) noexcept;

  SingleResumeSlot(const SingleResumeSlot&) = delete;
  SingleResumeSlot& operator=(const SingleResumeSlot&) = delete;
  SingleResumeSlot(SingleResumeSlot&&) = delete;
  SingleResumeSlot& operator=(SingleResumeSlot&&) = delete;

  // Returns no value only when both the checkpoint and owned partial are
  // absent. Corrupt, unknown, orphaned, relinked, or relocated state fails
  // closed and remains untouched.
  [[nodiscard]] clonecore::Result<std::optional<ResumeSlotRecord>> inspect();

  // Route-neutral admission gate for every new product OperationPlan that may
  // write a target or output. A valid active slot is not treated as permission
  // to restart that plan: callers must use the exact bound resume/discard
  // path instead. Unknown, corrupt, orphaned, or replaced slot state also
  // fails closed. This method never mutates the checkpoint or owned partial.
  [[nodiscard]] clonecore::Status guard_new_operation_start(
      const OperationPlan& plan);

  [[nodiscard]] clonecore::Result<ResumeSlotRecord> open_bound(
      const ResumeSlotBinding& expected);

  [[nodiscard]] clonecore::Status create(const ResumeSlotRecord& record);

  [[nodiscard]] clonecore::Status replace(
      const ResumeSlotBinding& expected,
      const ParsedCheckpoint& next_checkpoint);

  [[nodiscard]] clonecore::Status discard(
      const ResumeSlotBinding& expected);

 private:
  [[nodiscard]] clonecore::Result<ResumeSlotObservation> observe();

  IResumeSlotPlatform* platform_{};
  std::optional<std::wstring> bound_checkpoint_path_;
};

}  // namespace ytec::operationcore

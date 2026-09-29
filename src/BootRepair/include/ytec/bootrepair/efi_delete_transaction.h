#pragma once

#include "ytec/bootrepair/efi_boot_ownership.h"
#include "ytec/clonecore/disk_identity.h"
#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::bootrepair {

using EfiDeleteSha256 = std::array<std::byte, 32>;
using EfiDeleteFileId = std::array<std::byte, 16>;

inline constexpr std::size_t kMaximumEfiDeleteCandidates = 64U;
inline constexpr std::size_t kMaximumEfiDeleteTreeEntries = 4'096U;
inline constexpr std::size_t kMaximumEfiDeleteTreeDepth = 8U;
inline constexpr std::size_t kMaximumEfiDeleteNameCharacters = 128U;
inline constexpr std::size_t kMaximumEfiDeleteRelativePathCharacters = 1'024U;

// The quarantine is a fixed child of the ESP root, outside EFI. A production
// adapter must create it with no-replace semantics and must reject every
// pre-existing object as foreign. It is never a path-based deletion target.
[[nodiscard]] std::wstring_view efi_delete_quarantine_namespace() noexcept;

enum class EfiDeleteEntryKind : std::uint8_t {
  regular_file,
  directory,
  reparse,
  unknown,
};

// Raw read-only evidence for one entry. relative_path is relative to EFI, uses
// the conservative printable-ASCII name subset, and includes the candidate's
// top-level directory name. The candidate root must itself be present as a
// directory entry. A platform adapter must obtain the
// identity, metadata and file SHA-256 from the same non-reparse handle while
// denying write/delete sharing, then re-query that handle after hashing and
// reject any drift before returning. Directory parent/child membership must
// be proven from retained handles instead of following a mutable full path.
// It must also repeat the bounded EFI top-level/ownership census after all
// trees and reject a changed census, so the observation is one stable review.
struct EfiDeleteTreeEntryObservation final {
  EfiDeleteEntryKind kind{EfiDeleteEntryKind::unknown};
  std::wstring relative_path;
  std::uint64_t volume_serial_number{};
  EfiDeleteFileId file_id{};
  bool file_id_valid{};
  std::uint64_t size_bytes{};
  std::uint64_t creation_time{};
  std::uint64_t last_access_time{};
  std::uint64_t last_write_time{};
  std::uint64_t change_time{};
  std::uint32_t hard_link_count{};
  EfiDeleteSha256 file_sha256{};
  bool file_sha256_valid{};
};

// One independently named, normal directory directly below EFI. Microsoft,
// Boot, the fallback namespace, loose files and other top-level objects are
// never representable as deletion candidates.
struct EfiDeleteCandidateObservation final {
  std::wstring relative_name;
  std::vector<EfiDeleteTreeEntryObservation> entries;
};

// Stable ESP binding used in addition to StableDiskIdentity. The Volume GUID
// root, GPT unique/type identifiers, attributes and FAT32 filesystem are
// reviewed values, while geometry and filesystem volume serial prevent a
// different partition from being swapped into the same routing path.
struct EfiDeleteEspIdentity final {
  std::uint32_t partition_number{};
  std::uint64_t offset_bytes{};
  std::uint64_t length_bytes{};
  std::wstring partition_identifier;
  std::wstring partition_type_identifier;
  std::uint64_t partition_attributes{};
  std::wstring volume_guid_root;
  std::wstring filesystem_name;
  std::uint64_t volume_serial_number{};
};

struct EfiDeleteReviewObservation final {
  clonecore::StableDiskIdentity disk;
  EfiDeleteEspIdentity esp;
  EfiBootOwnershipEvidence ownership;
  bool bounded_top_level_enumeration_complete{};
  std::vector<EfiDeleteCandidateObservation> candidates;
};

// Review-time adapter. It must reidentify the requested disk and ESP, then
// perform the bounded handle-based read-only traversal documented above.
class IEfiDeleteReadOnlyInspector {
 public:
  virtual ~IEfiDeleteReadOnlyInspector() = default;

  [[nodiscard]] virtual clonecore::Result<EfiDeleteReviewObservation>
  inspect_candidates_read_only(
      const clonecore::StableDiskIdentity& expected_disk,
      const EfiDeleteEspIdentity& expected_esp) = 0;
};

struct EfiDeleteTreeEntryManifest final {
  EfiDeleteEntryKind kind{EfiDeleteEntryKind::unknown};
  std::wstring relative_path;
  std::uint64_t volume_serial_number{};
  EfiDeleteFileId file_id{};
  std::uint64_t size_bytes{};
  std::uint64_t creation_time{};
  std::uint64_t last_access_time{};
  std::uint64_t last_write_time{};
  std::uint64_t change_time{};
  std::uint32_t hard_link_count{};
  EfiDeleteSha256 file_sha256{};
  bool has_file_sha256{};
};

struct EfiDeleteCandidateManifest final {
  std::wstring relative_name;
  std::vector<EfiDeleteTreeEntryManifest> entries;
};

// Only review_efi_delete_candidates() can construct this object. Candidate and
// entry order is canonical, so enumeration order cannot change the digest.
class ReviewedEfiDeletePlan final {
 public:
  [[nodiscard]] const clonecore::StableDiskIdentity& expected_disk()
      const noexcept;
  [[nodiscard]] const EfiDeleteEspIdentity& expected_esp() const noexcept;
  [[nodiscard]] const EfiBootOwnershipEvidence& expected_ownership()
      const noexcept;
  [[nodiscard]] std::span<const EfiDeleteCandidateManifest> candidates()
      const noexcept;
  [[nodiscard]] const EfiDeleteSha256& manifest_sha256() const noexcept;

 private:
  ReviewedEfiDeletePlan(
      clonecore::StableDiskIdentity disk,
      EfiDeleteEspIdentity esp,
      EfiBootOwnershipEvidence ownership,
      std::vector<EfiDeleteCandidateManifest> candidates,
      EfiDeleteSha256 manifest_sha256);

  clonecore::StableDiskIdentity disk_;
  EfiDeleteEspIdentity esp_;
  EfiBootOwnershipEvidence ownership_;
  std::vector<EfiDeleteCandidateManifest> candidates_;
  EfiDeleteSha256 manifest_sha256_{};

  friend clonecore::Result<ReviewedEfiDeletePlan>
  review_efi_delete_candidates(const EfiDeleteReviewObservation&);
};

// Pure, bounded validation and canonical manifest construction. It performs
// no filesystem or disk I/O. Empty candidate sets are rejected because this
// transaction is only applicable to an explicit BCD-004 delete choice.
[[nodiscard]] clonecore::Result<ReviewedEfiDeletePlan>
review_efi_delete_candidates(const EfiDeleteReviewObservation& observation);

// Review entry point for a host that starts with an already selected stable
// disk/ESP. The inspector performs only read-only I/O; this function refuses
// routing drift before producing the immutable plan shown to the user.
[[nodiscard]] clonecore::Result<ReviewedEfiDeletePlan>
review_efi_delete_candidates_read_only(
    const clonecore::StableDiskIdentity& expected_disk,
    const EfiDeleteEspIdentity& expected_esp,
    IEfiDeleteReadOnlyInspector& inspector);

[[nodiscard]] bool equivalent_efi_delete_esp_identity(
    const EfiDeleteEspIdentity& left,
    const EfiDeleteEspIdentity& right) noexcept;

[[nodiscard]] bool equivalent_efi_delete_manifest(
    const ReviewedEfiDeletePlan& left,
    const ReviewedEfiDeletePlan& right) noexcept;

[[nodiscard]] std::wstring efi_delete_source_relative_path(
    const EfiDeleteCandidateManifest& candidate);

[[nodiscard]] clonecore::Result<std::wstring>
efi_delete_quarantine_slot_relative_path(std::size_t candidate_index);

struct EfiDeleteConfirmation final {
  bool destructive_warning_acknowledged{};
  std::wstring typed_token;
};

enum class EfiDeletePlatformFailureKind : std::uint8_t {
  none,
  foreign_object,
  tamper_detected,
  race_detected,
  io_failure,
  verification_failure,
  platform_contract_violation,
};

enum class EfiDeleteMutationExtent : std::uint8_t {
  // For mutating calls this means the adapter proved the requested mutation
  // did not take effect (or fully restored and read back its own sub-step).
  none,
  // The requested mutation completed and its identity/readback proof passed.
  complete,
  partial_or_unknown,
};

// A failed mutating operation must report whether it proved that no mutation
// occurred. If it cannot prove that, partial_or_unknown is mandatory.
struct EfiDeletePlatformStepResult final {
  bool succeeded{};
  EfiDeletePlatformFailureKind failure_kind{
      EfiDeletePlatformFailureKind::platform_contract_violation};
  EfiDeleteMutationExtent mutation_extent{
      EfiDeleteMutationExtent::partial_or_unknown};
  std::optional<clonecore::Error> error;

  [[nodiscard]] static EfiDeletePlatformStepResult completed();
  [[nodiscard]] static EfiDeletePlatformStepResult failed(
      EfiDeletePlatformFailureKind failure_kind,
      EfiDeleteMutationExtent mutation_extent,
      clonecore::Error error);
};

struct EfiDeleteObjectIdentity final {
  std::uint64_t volume_serial_number{};
  EfiDeleteFileId file_id{};
};

struct EfiDeleteQuarantineCreateResult final {
  EfiDeletePlatformStepResult step;
  EfiDeleteObjectIdentity identity;
};

// Destructive platform contract. The production Win32 implementation is
// accepted only while every operation below remains handle-bound.
//
// Rules for every implementation:
// - inspect_candidates_read_only() performs bounded, non-reparse traversal and
//   stable disk/ESP reidentification without assigning a drive letter. During
//   execution it retains the verified ESP/EFI/candidate handles until commit
//   or rollback; a later method may not reopen a candidate by full path alone.
// - create_owned_quarantine_no_replace() creates only the fixed namespace on
//   the same ESP; any existing object is foreign and must not be reused. Its
//   opened directory handle remains owned by the adapter for the transaction;
//   EfiDeleteObjectIdentity is audit evidence, never path-only routing.
// - move/rollback rename the already-opened expected directory handle with
//   no-replace semantics to/from efi_delete_quarantine_slot_relative_path(),
//   then read back the exact tree identity.
// - rebuild_microsoft_bcd_and_verify_readback() uses signed BCDBoot, verifies
//   the new BCD and retains an exact prior-store rollback boundary. Failure may
//   report mutation_extent::none only after Microsoft/BCD rollback readback.
// - rollback_microsoft_bcd_rebuild_if_identity_matches() is required when
//   final deletion has not mutated any candidate. commit_microsoft_bcd_rebuild()
//   may discard the retained prior store only after every candidate deletion
//   has completed. Both operations remain exact-handle identity operations.
// - recursive deletion reopens every quarantined entry by handle, verifies the
//   complete manifest identity and parent relationship, and marks those
//   handles for deletion. DeleteFile/RemoveDirectory path-only fallbacks are
//   forbidden.
// - none of the move/delete methods may target the ESP root, EFI root,
//   EFI\\Microsoft, EFI\\Boot, or a fallback loader.
class IEfiDeleteTransactionPlatform : public IEfiDeleteReadOnlyInspector {
 public:
  virtual ~IEfiDeleteTransactionPlatform() = default;

  [[nodiscard]] virtual EfiDeleteQuarantineCreateResult
  create_owned_quarantine_no_replace(
      const ReviewedEfiDeletePlan& reviewed) = 0;

  [[nodiscard]] virtual EfiDeletePlatformStepResult
  move_candidate_to_quarantine_handle_bound(
      const ReviewedEfiDeletePlan& reviewed,
      std::size_t candidate_index,
      const EfiDeleteCandidateManifest& candidate,
      const EfiDeleteObjectIdentity& quarantine_identity) = 0;

  [[nodiscard]] virtual EfiDeletePlatformStepResult
  rollback_candidate_from_quarantine_handle_bound(
      const ReviewedEfiDeletePlan& reviewed,
      std::size_t candidate_index,
      const EfiDeleteCandidateManifest& candidate,
      const EfiDeleteObjectIdentity& quarantine_identity) = 0;

  [[nodiscard]] virtual EfiDeletePlatformStepResult
  rebuild_microsoft_bcd_and_verify_readback(
      const ReviewedEfiDeletePlan& reviewed) = 0;

  [[nodiscard]] virtual EfiDeletePlatformStepResult
  rollback_microsoft_bcd_rebuild_if_identity_matches(
      const ReviewedEfiDeletePlan& reviewed) = 0;

  [[nodiscard]] virtual EfiDeletePlatformStepResult
  commit_microsoft_bcd_rebuild(
      const ReviewedEfiDeletePlan& reviewed) = 0;

  [[nodiscard]] virtual EfiDeletePlatformStepResult
  delete_quarantined_candidate_tree_handle_bound(
      const ReviewedEfiDeletePlan& reviewed,
      std::size_t candidate_index,
      const EfiDeleteCandidateManifest& candidate,
      const EfiDeleteObjectIdentity& quarantine_identity) = 0;

  [[nodiscard]] virtual EfiDeletePlatformStepResult
  remove_owned_quarantine_if_empty_handle_bound(
      const ReviewedEfiDeletePlan& reviewed,
      const EfiDeleteObjectIdentity& quarantine_identity) = 0;
};

enum class EfiDeleteTransactionOutcome : std::uint8_t {
  committed,
  stopped_before_mutation,
  rolled_back,
  partial_rollback,
  finalization_incomplete,
  partial_delete,
  committed_bcd_cleanup_incomplete,
  committed_quarantine_cleanup_incomplete,
};

enum class EfiDeleteFailureStage : std::uint8_t {
  none,
  confirmation,
  fresh_inspection,
  target_identity,
  tree_manifest,
  quarantine_prepare,
  quarantine_move,
  microsoft_bcd_rebuild,
  microsoft_bcd_commit,
  rollback,
  final_delete,
  quarantine_cleanup,
  platform_contract,
};

struct EfiDeleteTransactionReport final {
  EfiDeleteTransactionOutcome outcome{
      EfiDeleteTransactionOutcome::stopped_before_mutation};
  EfiDeleteFailureStage failure_stage{EfiDeleteFailureStage::none};
  EfiDeletePlatformFailureKind platform_failure{
      EfiDeletePlatformFailureKind::none};
  EfiDeletePlatformFailureKind rollback_platform_failure{
      EfiDeletePlatformFailureKind::none};
  std::optional<clonecore::Error> primary_error;
  std::optional<clonecore::Error> rollback_error;
  std::size_t quarantined_candidates{};
  std::size_t rolled_back_candidates{};
  std::size_t deleted_candidates{};
  bool stable_target_reidentified{};
  bool fresh_manifest_verified{};
  bool all_candidates_were_quarantined_before_bcd{};
  bool microsoft_bcd_rebuild_readback_verified{};
  bool microsoft_bcd_failure_rollback_verified{};
  bool microsoft_bcd_rollback_boundary_committed{};
  bool microsoft_bcd_rolled_back_after_delete_stop{};
};

// Executes only after exact uppercase OK and a fresh exact manifest match.
// Every candidate is quarantined before BCDBoot is called, and no candidate is
// deleted before BCDBoot plus readback succeeds. If the first final deletion
// stops without mutation, both candidates and the retained BCD transaction
// roll back exactly. After any deletion mutation there is intentionally no
// false claim that full rollback is possible, though every still-intact later
// candidate is restored exactly.
[[nodiscard]] EfiDeleteTransactionReport execute_efi_delete_transaction(
    const ReviewedEfiDeletePlan& reviewed,
    const EfiDeleteConfirmation& confirmation,
    IEfiDeleteTransactionPlatform& platform);

}  // namespace ytec::bootrepair

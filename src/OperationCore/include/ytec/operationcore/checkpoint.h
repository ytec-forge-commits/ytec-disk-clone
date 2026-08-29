#pragma once

#include "ytec/operationcore/operation.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::operationcore {

inline constexpr std::uint32_t kCheckpointSchemaVersionV1 = 1U;
inline constexpr std::uint32_t kCheckpointSchemaVersionV2 = 2U;
inline constexpr std::uint32_t kCheckpointSchemaVersionV3 = 3U;
inline constexpr std::uint32_t kCheckpointSchemaVersion =
    kCheckpointSchemaVersionV2;
inline constexpr std::size_t kMaximumCheckpointBytes = 256U * 1024U;
inline constexpr std::size_t kMaximumCheckpointPreparationSectors = 4096U;

enum class CheckpointPhase : std::uint8_t {
  executing = 0U,
  verifying = 1U,
  preparing = 2U,
  prepared = 3U,
  commit_ready = 4U,
};

struct CheckpointPreparationSectorEvidence final {
  std::uint64_t offset{};
  std::uint64_t length{};
  Sha256Digest original_hash{};

  [[nodiscard]] bool operator==(
      const CheckpointPreparationSectorEvidence&) const noexcept = default;
};

struct CheckpointPreparationEvidence final {
  Sha256Digest initial_layout_hash{};
  std::uint32_t logical_sector_size{};
  std::vector<CheckpointPreparationSectorEvidence> original_sectors;

  [[nodiscard]] bool operator==(
      const CheckpointPreparationEvidence&) const noexcept = default;
};

// Schema v3 binds the durable output prefix to all app-owned objects that
// participate in a resumable image-create transaction. The digest is supplied
// by the owning stream backend and covers its canonical journal state; this
// layer deliberately does not infer it from file lengths alone.
struct CheckpointOutputProgressEvidence final {
  Sha256Digest verified_prefix_hash{};
  std::uint64_t primary_output_length{};
  std::uint64_t journal_length{};
  std::uint64_t auxiliary_output_length{};

  [[nodiscard]] bool operator==(
      const CheckpointOutputProgressEvidence&) const noexcept = default;
};

struct InterruptionCheckpoint final {
  std::uint32_t schema_version{kCheckpointSchemaVersion};
  OperationId operation_id{};
  OperationKind kind{OperationKind::clone};
  OperationEnvironment environment{OperationEnvironment::windows};
  CheckpointPhase phase{CheckpointPhase::executing};
  std::uint64_t revision{1U};
  std::uint64_t expected_work_bytes{};
  std::uint64_t verified_work_bytes{};
  std::uint64_t verified_chunk_count{};
  Sha256Digest plan_hash{};
  Sha256Digest output_identity_hash{};
  std::optional<clonecore::StableDiskIdentity> source;
  std::optional<clonecore::StableDiskIdentity> target;

  // A VSS snapshot id, PE source-state token, or equally strict continuity
  // value supplied by the owning engine. It is never guessed by this layer.
  std::wstring continuity_token;

  // Schema v2 only. Destructive restore controllers bind the reviewed initial
  // layout and the original SHA-256 digest of every sector they invalidate
  // before payload progress begins. Raw metadata bytes are never persisted.
  // Legacy executing/verifying checkpoints omit it.
  std::optional<CheckpointPreparationEvidence> preparation_evidence;

  // Schema v3 only. This records no password, key, plaintext, or raw metadata.
  // A caller must re-open and fully authenticate/hash the declared prefix
  // before it treats these lengths as resumable progress.
  std::optional<CheckpointOutputProgressEvidence> output_progress_evidence;
};

struct ParsedCheckpoint final {
  InterruptionCheckpoint checkpoint;
  Sha256Digest record_hash{};
};

[[nodiscard]] clonecore::Status validate_checkpoint(
    const InterruptionCheckpoint& checkpoint);

[[nodiscard]] clonecore::Status validate_checkpoint_transition(
    const InterruptionCheckpoint& current,
    const InterruptionCheckpoint& next);

[[nodiscard]] clonecore::Status validate_checkpoint_for_resume(
    const InterruptionCheckpoint& checkpoint,
    const OperationPlan& plan,
    const ReidentifiedOperation& observed,
    std::wstring_view continuity_token,
    const Sha256Digest& output_identity_hash);

[[nodiscard]] clonecore::Result<std::vector<std::byte>>
serialize_checkpoint(const InterruptionCheckpoint& checkpoint);

[[nodiscard]] clonecore::Result<ParsedCheckpoint> parse_checkpoint(
    std::span<const std::byte> bytes);

// The store manages exactly the path passed by the caller. It never scans a
// directory or creates a list/history. A fixed "<path>.new" staging
// file is created with CREATE_NEW; any pre-existing stage is left untouched.
[[nodiscard]] clonecore::Result<std::optional<ParsedCheckpoint>>
read_single_checkpoint(const std::wstring& path);

[[nodiscard]] clonecore::Status create_single_checkpoint(
    const std::wstring& path,
    const InterruptionCheckpoint& checkpoint);

[[nodiscard]] clonecore::Status replace_single_checkpoint(
    const std::wstring& path,
    const Sha256Digest& expected_current_record_hash,
    const InterruptionCheckpoint& next);

// Removes only the exact open file whose validated record hash matches.
// Unknown, corrupt, wrong-version, or changed files are never deleted.
[[nodiscard]] clonecore::Status discard_single_checkpoint(
    const std::wstring& path,
    const Sha256Digest& expected_record_hash);

}  // namespace ytec::operationcore

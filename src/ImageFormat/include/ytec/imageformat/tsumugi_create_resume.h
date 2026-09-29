#pragma once

#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/tsumugi_stream.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::size_t kTsumugiCreateResumeJournalMaximumBytes =
    96U * 1024U * 1024U;
inline constexpr std::uint64_t kTsumugiCreateResumeJournalMaximumRecords =
    (kTsumugiCreateResumeJournalMaximumBytes - 512U) / 240U;

using TsumugiCreateResumeOperationIdV1 = std::array<std::byte, 16U>;

// Everything needed to reject a different operation, source generation,
// destination, plan, or output. No password, derived key, or plaintext is
// represented here.
struct TsumugiCreateResumeBindingV1 final {
  TsumugiCreateResumeOperationIdV1 operation_id{};
  Sha256Digest plan_hash{};
  Sha256Digest source_identity_hash{};
  Sha256Digest source_state_hash{};
  Sha256Digest destination_storage_identity_hash{};
  Sha256Digest output_identity_hash{};

  [[nodiscard]] bool operator==(
      const TsumugiCreateResumeBindingV1&) const noexcept = default;
};

enum class TsumugiCreateResumePhaseV1 : std::uint8_t {
  preparing = 1U,
  prepared = 2U,
  commit_ready = 3U,
};

struct TsumugiCreateResumeProgressV1 final {
  TsumugiCreateResumePhaseV1 phase{TsumugiCreateResumePhaseV1::preparing};
  std::uint64_t verified_logical_bytes{};
  std::uint64_t verified_chunk_count{};
  std::uint64_t primary_output_length{};
  std::uint64_t journal_length{};
  Sha256Digest verified_prefix_hash{};

  [[nodiscard]] bool operator==(
      const TsumugiCreateResumeProgressV1&) const noexcept = default;
};

struct TsumugiCreateResumeOwnedPathsV1 final {
  std::wstring image_partial_path;
  std::wstring journal_path;
};

struct TsumugiCreateResumeJournalHeaderV1 final {
  TsumugiCreateResumeBindingV1 binding;
  Sha256Digest final_path_hash{};
  Sha256Digest manifest_hash{};
  Sha256Digest chain_root{};
  TsumugiPayloadKind payload_kind{TsumugiPayloadKind::exact_disk};
  ImageCompression compression{ImageCompression::none};
  TsumugiCreateVerificationMode verification_mode{
      TsumugiCreateVerificationMode::complete};
  std::uint64_t source_disk_size{};
  std::uint64_t expected_logical_bytes{};
  std::uint64_t chunk_count{};
  std::uint64_t metadata_length{};
  std::uint64_t payload_offset{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{};
  std::array<std::byte, 16U> image_id{};
  bool encrypted{};
  TsumugiArgon2Parameters argon2{};
  std::array<std::byte, kTsumugiGcmNonceBytes> base_nonce{};
};

struct TsumugiCreateResumeJournalChunkV1 final {
  std::uint64_t chunk_index{};
  TsumugiChunkRecord record;
  Sha256Digest stored_bytes_hash{};
  Sha256Digest previous_chain_hash{};
  Sha256Digest chain_hash{};
};

struct TsumugiCreateResumeJournalInspectionV1 final {
  TsumugiCreateResumeJournalHeaderV1 header;
  std::vector<TsumugiCreateResumeJournalChunkV1> chunks;
  std::uint64_t journal_length{};
  Sha256Digest header_hash{};
  Sha256Digest verified_prefix_hash{};
};

// Pure bounded parser used by product recovery and deterministic negative
// tests. The input is always treated as untrusted.
[[nodiscard]] clonecore::Result<TsumugiCreateResumeJournalInspectionV1>
inspect_tsumugi_create_resume_journal_v1(
    std::span<const std::byte> journal_bytes);

struct TsumugiCreateResumeCheckpointHooksV1 final {
  // Called after both CREATE_NEW handles exist but before the first byte is
  // written. It must bind both file objects and durably create active.checkpoint.
  // Once this callback is invoked, a failure is persistence-ambiguous: the
  // backend retains both objects because the slot may already be durable.
  std::function<clonecore::Status(
      const TsumugiCreateResumeOwnedPathsV1&,
      const TsumugiCreateResumeBindingV1&,
      const TsumugiCreateResumeProgressV1&)> create_before_first_mutation;

  // Called before reading or truncating an existing journal/partial. It must
  // re-prove the source state, destination and both opened file-object IDs.
  std::function<clonecore::Status(
      const TsumugiCreateResumeOwnedPathsV1&,
      const TsumugiCreateResumeBindingV1&,
      const TsumugiCreateResumeProgressV1&)> prove_existing_before_resume;

  // Must perform one exact-hash/revision checked durable checkpoint replace.
  std::function<clonecore::Status(
      const TsumugiCreateResumeProgressV1&,
      const TsumugiCreateResumeProgressV1&)> replace_after_verified_prefix;
};

struct TsumugiCreateResumeRequestV1 final {
  TsumugiStreamBuildRequest stream;
  TsumugiCreateResumeBindingV1 binding;
  // Empty starts a new pair with CREATE_NEW. A value resumes only the exact
  // already-opened slot state and never auto-migrates another version.
  std::optional<TsumugiCreateResumeProgressV1> expected_progress;
  TsumugiCreateResumeCheckpointHooksV1 checkpoint;
};

struct TsumugiCreateResumePreparedV1 final {
  TsumugiStreamBuildReport stream;
  TsumugiCreateResumeOwnedPathsV1 owned_paths;
  TsumugiCreateResumeProgressV1 progress;
  std::uint64_t reused_verified_chunk_count{};
  std::uint64_t appended_chunk_count{};
  bool resumed{};
  bool complete_partial_verified{};
  bool commit_ready{};
};

// Exact-disk WinPE backend only. It preserves the completed `.tsumugi` v1
// wire format, keeps cancellation/failure state for a bound later resume, and
// stops at a fully verified commit-ready partial. Final-name commit and exact
// slot retirement are intentionally a later controller phase.
[[nodiscard]] clonecore::Result<TsumugiCreateResumePreparedV1>
prepare_resumable_tsumugi_file_v1(
    const TsumugiCreateResumeRequestV1& request,
    const clonecore::DiskOperationCallbacks& callbacks = {});

}  // namespace ytec::imageformat

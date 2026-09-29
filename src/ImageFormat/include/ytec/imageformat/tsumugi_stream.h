#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/tsumugi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::imageformat {

// A logical image chunk whose bytes are obtained on demand. Zero-filled and
// unreadable-zero-filled chunks do not have a source and are represented only
// by their flags and logical length.
struct TsumugiStreamBuildChunk final {
  std::uint64_t logical_offset{};
  std::uint64_t logical_length{};
  std::uint64_t source_offset{};
  TsumugiChunkFlags flags{TsumugiChunkFlags::none};
  const clonecore::ISourceDiskReader* source{};
  std::optional<TsumugiRescueReadEvidence> rescue_read_evidence;
};

// Complete is the safe default and performs an additional full-container and
// plaintext-chunk scan after the image has been written. Fast retains every
// write-time read-back/hash/authentication gate and the final bounded metadata
// verification, but deliberately omits only that additional full scan.
enum class TsumugiCreateVerificationMode : std::uint8_t {
  complete = 0U,
  fast = 1U,
};

[[nodiscard]] bool is_supported_tsumugi_create_verification_mode(
    TsumugiCreateVerificationMode mode) noexcept;

struct TsumugiStreamBuildRequest final {
  // Only an absolute local-drive .tsumugi path on NTFS or exFAT is accepted.
  // The writer creates "<final>.partial" with CREATE_NEW. Existing completed
  // images may be replaced only after the new image passes complete read-back
  // verification from the still-open staging handle.
  std::wstring final_path;
  TsumugiPayloadKind payload_kind{TsumugiPayloadKind::exact_disk};
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{kImageChunkSize16MiB};
  ImageCompression compression{ImageCompression::zstandard};
  std::size_t verification_block_bytes{4U * 1024U * 1024U};
  TsumugiCreateVerificationMode verification_mode{
      TsumugiCreateVerificationMode::complete};
  std::array<std::byte, 16> image_id{};
  std::vector<std::byte> manifest;
  std::vector<TsumugiStreamBuildChunk> chunks;
  std::optional<TsumugiEncryptionSettings> encryption;
  bool replace_existing{};
};

struct TsumugiStreamBuildReport final {
  std::wstring final_path;
  std::wstring retained_recovery_path;
  std::uint64_t image_length{};
  std::uint64_t stored_data_bytes{};
  std::uint64_t zero_filled_bytes{};
  std::uint64_t chunk_count{};
  bool replaced_existing{};
  bool all_chunks_read_back_verified{};
  // Every non-zero chunk was decoded from its immediate read-back bytes. For
  // encrypted images this includes AES-GCM tag authentication; all modes then
  // verify the recovered plaintext SHA-256 before moving to the next chunk.
  bool all_chunks_authenticated_and_hashed{};
  bool global_hash_read_back_verified{};
  bool final_metadata_read_back_verified{};
  bool final_complete_scan_performed{};
  TsumugiCreateVerificationMode verification_mode{
      TsumugiCreateVerificationMode::complete};
  bool committed{};
};

// Owns one completely written neighbouring .partial which has passed the
// selected creation verification mode. The final name remains untouched until
// commit_verified() succeeds. This is used by VSS callers so BackupComplete
// and Snapshot-set deletion can finish before the image becomes visible as a
// completed backup.
class TsumugiStagedFileV1 final {
 public:
  ~TsumugiStagedFileV1();

  TsumugiStagedFileV1(TsumugiStagedFileV1&&) noexcept;
  TsumugiStagedFileV1& operator=(TsumugiStagedFileV1&&) noexcept;
  TsumugiStagedFileV1(const TsumugiStagedFileV1&) = delete;
  TsumugiStagedFileV1& operator=(const TsumugiStagedFileV1&) = delete;

  [[nodiscard]] const TsumugiStreamBuildReport& report() const noexcept;
  [[nodiscard]] bool pending() const noexcept;

  // Re-identifies the owned .partial and the reviewed final path, then performs
  // the recoverable final-name transaction. This method is single-use.
  [[nodiscard]] clonecore::Result<TsumugiStreamBuildReport>
  commit_verified();

  // Deletes only the exact owned .partial. An identity change fails closed and
  // leaves the unknown file untouched. Destruction also attempts this cleanup.
  [[nodiscard]] clonecore::Status abort_incomplete() noexcept;

 private:
  class Impl;
  explicit TsumugiStagedFileV1(std::unique_ptr<Impl> impl) noexcept;

  friend clonecore::Result<TsumugiStagedFileV1>
  prepare_verified_tsumugi_file_v1(
      const TsumugiStreamBuildRequest&,
      const clonecore::DiskOperationCallbacks&);

  std::unique_ptr<Impl> impl_;
};

// Creates and verifies an owned adjacent .partial with the requested mode, but
// deliberately does not expose it under the completed image name.
[[nodiscard]] clonecore::Result<TsumugiStagedFileV1>
prepare_verified_tsumugi_file_v1(
    const TsumugiStreamBuildRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Streams a canonical .tsumugi v1 image without retaining the payload in RAM.
// At most one logical chunk, the bounded metadata area, and one verification
// block are resident at a time. Cancellation and every failure leave the final
// name untouched and never promote the owned .partial file.
[[nodiscard]] clonecore::Result<TsumugiStreamBuildReport>
write_verified_tsumugi_file_v1(
    const TsumugiStreamBuildRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks = {});

struct TsumugiStreamVerifyRequest final {
  std::wstring image_path;
  std::optional<std::string_view> password;
  std::size_t verification_block_bytes{4U * 1024U * 1024U};
};

struct TsumugiFileHeaderProbe final {
  TsumugiHeader header;
  std::uint64_t image_length{};
  bool encrypted{};
};

// Opens one local .tsumugi without write/delete sharing and reads only the
// fixed 512-byte header. The full fixed-header schema, required features,
// header SHA-256, section bounds against the observed file length, and final
// file identity are checked. This probe only determines whether a password
// prompt is required; it is not a substitute for complete image verification.
[[nodiscard]] clonecore::Result<TsumugiFileHeaderProbe>
probe_tsumugi_file_header_v1(const std::wstring& image_path);

struct TsumugiStreamInspection final {
  // Captured from the exact immutable handle used for the complete
  // verification pass.  Higher-level persistent-resume code binds the
  // reviewed pass and the restore pass to this file object, length and
  // last-write observation instead of trusting the path alone.
  struct OpenedFileObservationV1 final {
    std::uint64_t volume_serial{};
    std::array<std::byte, 16U> file_id{};
    std::uint64_t size{};
    std::uint64_t last_write_time{};
    bool identity_from_open_handle{};

    [[nodiscard]] bool operator==(
        const OpenedFileObservationV1&) const noexcept = default;
  } opened_file;
  TsumugiHeader header;
  std::vector<std::byte> manifest;
  std::vector<TsumugiChunkRecord> records;
  Sha256Digest global_hash{};
  bool header_hash_verified{};
  bool metadata_authenticated{};
  bool all_chunks_verified{};
  bool global_hash_verified{};
};

// Opens the image once without FILE_SHARE_WRITE or FILE_SHARE_DELETE and
// treats all bytes as untrusted. The entire container is authenticated and
// hashed with bounded reads.
[[nodiscard]] clonecore::Result<TsumugiStreamInspection>
verify_tsumugi_file_v1(
    const TsumugiStreamVerifyRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// For zero-filled records plaintext is empty and the callback must interpret
// the record flag as an instruction to zero the logical range.
using TsumugiVerifiedChunkCallback = std::function<clonecore::Status(
    const TsumugiChunkRecord& record,
    std::span<const std::byte> plaintext)>;

// Runs on the same immutable handle after the complete first verification
// pass and before the first restore callback. Higher layers use this to bind a
// previously reviewed restore plan to the exact authenticated image.
using TsumugiVerifiedInspectionGate = std::function<clonecore::Status(
    const TsumugiStreamInspection& inspection)>;

struct TsumugiStreamRestoreReport final {
  TsumugiStreamInspection inspection;
  std::uint64_t delivered_logical_bytes{};
  std::uint64_t delivered_chunk_count{};
  bool callbacks_started_after_complete_verification{};
};

// Performs a strict two-pass read on one immutable file handle. No callback is
// invoked until the first pass has validated the header, footer, global hash,
// metadata authentication, every record, and every plaintext chunk hash.
[[nodiscard]] clonecore::Result<TsumugiStreamRestoreReport>
read_verified_tsumugi_file_v1(
    const TsumugiStreamVerifyRequest& request,
    const TsumugiVerifiedChunkCallback& verified_chunk,
    const clonecore::DiskOperationCallbacks& callbacks = {},
    const TsumugiVerifiedInspectionGate& inspection_gate = {});

}  // namespace ytec::imageformat

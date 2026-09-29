#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/image_primitives.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_crypto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ytec::imageformat {

inline constexpr std::uint16_t kTsumugiMajorVersion = 1U;
inline constexpr std::uint16_t kTsumugiMinorVersion = 0U;
inline constexpr std::uint32_t kTsumugiHeaderSize = 512U;
inline constexpr std::uint32_t kTsumugiFooterSize = 64U;
inline constexpr std::uint32_t kTsumugiMetadataHeaderSize = 32U;
inline constexpr std::uint32_t kTsumugiChunkRecordSize = 112U;
inline constexpr std::uint64_t kTsumugiMaximumChunkCount = 1'048'576ULL;
inline constexpr std::size_t kTsumugiMaximumManifestBytes =
    16U * 1024U * 1024U;
inline constexpr std::size_t kTsumugiMaximumMetadataBytes =
    32U * 1024U * 1024U;

enum class TsumugiPayloadKind : std::uint16_t {
  exact_disk = 1U,
  shrink_disk = 2U,
  rescue_disk = 3U,
};

enum class TsumugiRequiredFeature : std::uint32_t {
  none = 0U,
  encrypted = 1U,
  unreadable_range_map = 2U,
  rescue_read_evidence = 4U,
};

enum class TsumugiChunkFlags : std::uint32_t {
  none = 0U,
  zero_filled = 1U,
  unreadable_zero_filled = 3U,
};

// Records the finite read sequence that failed before an unreadable rescue
// range was replaced with zeroes. The three native errors are preserved as
// observed Windows error codes; zero is retained if a source reports it.
struct TsumugiRescueReadEvidence final {
  std::uint8_t forward_attempts{};
  std::uint8_t reverse_attempts{};
  std::uint8_t sector_attempts{};
  bool zero_fill_read_back_verified{};
  std::uint32_t forward_native_error{};
  std::uint32_t reverse_native_error{};
  std::uint32_t sector_native_error{};

  friend bool operator==(
      const TsumugiRescueReadEvidence&,
      const TsumugiRescueReadEvidence&) = default;
};

struct TsumugiBuildChunk final {
  std::uint64_t logical_offset{};
  std::uint64_t logical_length{};
  TsumugiChunkFlags flags{TsumugiChunkFlags::none};
  std::vector<std::byte> data;
  std::optional<TsumugiRescueReadEvidence> rescue_read_evidence;
};

struct TsumugiEncryptionSettings final {
  std::string_view password;
  TsumugiArgon2Parameters argon2;
  std::array<std::byte, kTsumugiGcmNonceBytes> base_nonce{};
};

struct TsumugiBuildRequest final {
  TsumugiPayloadKind payload_kind{TsumugiPayloadKind::exact_disk};
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{kImageChunkSize16MiB};
  ImageCompression compression{ImageCompression::zstandard};
  std::array<std::byte, 16> image_id{};
  std::vector<std::byte> manifest;
  std::vector<TsumugiBuildChunk> chunks;
  std::optional<TsumugiEncryptionSettings> encryption;
};

struct TsumugiHeader final {
  std::uint16_t major_version{};
  std::uint16_t minor_version{};
  std::uint32_t required_features{};
  TsumugiPayloadKind payload_kind{TsumugiPayloadKind::exact_disk};
  ImageCompression compression{ImageCompression::none};
  std::uint64_t source_disk_size{};
  std::uint32_t logical_sector_size{};
  std::uint32_t physical_sector_size{};
  std::uint32_t chunk_size{};
  std::uint64_t chunk_count{};
  ImageSection data;
  ImageSection metadata;
  ImageSection footer;
  TsumugiArgon2Parameters argon2;
  std::array<std::byte, kTsumugiGcmNonceBytes> base_nonce{};
  std::array<std::byte, 16> image_id{};
  std::array<std::byte, kTsumugiGcmTagBytes> metadata_tag{};
  Sha256Digest header_hash{};
};

struct TsumugiChunkRecord final {
  std::uint64_t logical_offset{};
  std::uint64_t logical_length{};
  std::uint64_t stored_offset{};
  std::uint64_t stored_length{};
  TsumugiChunkFlags flags{TsumugiChunkFlags::none};
  ImageCompression compression{ImageCompression::none};
  std::uint64_t nonce_counter{};
  Sha256Digest plaintext_hash{};
  std::array<std::byte, kTsumugiGcmTagBytes> authentication_tag{};
  std::optional<TsumugiRescueReadEvidence> rescue_read_evidence;
};

struct TsumugiVerifiedChunk final {
  TsumugiChunkRecord record;
  std::vector<std::byte> plaintext;
};

struct TsumugiInspection final {
  TsumugiHeader header;
  std::vector<std::byte> manifest;
  std::vector<TsumugiVerifiedChunk> chunks;
  Sha256Digest global_hash{};
  bool header_hash_verified{};
  bool metadata_authenticated{};
  bool all_chunks_verified{};
  bool global_hash_verified{};
};

struct TsumugiInspectRequest final {
  std::optional<std::string_view> password;
};

[[nodiscard]] clonecore::Result<std::vector<std::byte>> build_tsumugi_v1(
    const TsumugiBuildRequest& request);

// Treats every byte as untrusted. No disk write is authorized by a successful
// inspection. Encrypted metadata is not exposed until its GCM tag verifies.
[[nodiscard]] clonecore::Result<TsumugiInspection> inspect_tsumugi_v1(
    std::span<const std::byte> image,
    const TsumugiInspectRequest& request = {});

}  // namespace ytec::imageformat

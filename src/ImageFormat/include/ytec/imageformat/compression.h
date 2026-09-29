#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/imageformat/image_primitives.h"

#include <cstddef>
#include <span>
#include <vector>

namespace ytec::imageformat {

inline constexpr int kImageZstandardCompressionLevel = 3;

// Image-chunk v1 Zstandard profile: one standard frame, no dictionary, exact
// content size, at most 32 MiB uncompressed. The outer image integrity check
// remains mandatory.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
compress_zstandard_image_chunk_v1(
    std::span<const std::byte> uncompressed);

// Rejects unknown content sizes, concatenated/trailing frames, dictionaries,
// expansion beyond the exact declared length, and all Zstandard errors.
[[nodiscard]] clonecore::Result<std::vector<std::byte>>
decompress_zstandard_image_chunk_v1(
    std::span<const std::byte> stored,
    std::size_t expected_uncompressed_length);

// Temporary source-compatibility names for the legacy .dcimg implementation.
inline constexpr int kDcimgZstandardCompressionLevel =
    kImageZstandardCompressionLevel;

[[nodiscard]] clonecore::Result<std::vector<std::byte>>
compress_zstandard_dcimg_v1(std::span<const std::byte> uncompressed);

[[nodiscard]] clonecore::Result<std::vector<std::byte>>
decompress_zstandard_dcimg_v1(
    std::span<const std::byte> stored,
    std::size_t expected_uncompressed_length);

}  // namespace ytec::imageformat

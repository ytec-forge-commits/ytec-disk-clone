#pragma once

#include "ytec/windowsapp/adk_acquisition.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct AdkPatchZipEntry final {
  std::size_t pin_index{};
  std::uint16_t compression_method{};
  std::uint32_t expected_crc32{};
  std::uint64_t compressed_offset{};
  std::uint64_t compressed_byte_count{};
  std::uint64_t uncompressed_byte_count{};
};

struct AdkPatchArchiveInspection final {
  std::string root_directory_name;
  std::vector<AdkPatchZipEntry> entries;
  std::uint64_t total_uncompressed_bytes{};
};

// The parser accepts only a single-disk ZIP32 archive with one safe root
// directory and the exact pinned regular files. It rejects encryption,
// Zip64, traversal, duplicate/case-colliding names, unknown members,
// unsupported methods, non-regular Unix entry types and all size overflows.
[[nodiscard]] clonecore::Result<AdkPatchArchiveInspection>
inspect_adk_patch_archive(
    std::span<const std::byte> archive,
    std::span<const AdkPatchMemberPin> pins);

using AdkPatchChunkWriter =
    std::function<clonecore::Status(std::span<const std::byte>)>;

// Extracts exactly one entry selected by a prior inspection. Output is bounded
// by the pinned uncompressed length and its CRC-32 is checked before success.
[[nodiscard]] clonecore::Status extract_adk_patch_archive_entry(
    std::span<const std::byte> archive,
    const AdkPatchZipEntry& entry,
    const AdkPatchChunkWriter& writer);

}  // namespace ytec::windowsapp

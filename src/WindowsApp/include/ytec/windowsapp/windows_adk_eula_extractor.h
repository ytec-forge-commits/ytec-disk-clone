#pragma once

#include "ytec/windowsapp/adk_consent_review.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ytec::windowsapp {

struct WindowsAdkEmbeddedCabHeader final {
  std::uint32_t cabinet_byte_count{};
  std::uint32_t files_offset{};
  std::uint16_t folder_count{};
  std::uint16_t file_count{};
  std::uint16_t flags{};
  std::uint16_t set_id{};
  std::uint16_t cabinet_index{};
  std::uint8_t version_minor{};
  std::uint8_t version_major{};
};

// Parses the fixed 36-byte MSCF header before the Cabinet API sees the file.
// Multi-cabinet/reserved layouts and unbounded counts are rejected.
[[nodiscard]] clonecore::Result<WindowsAdkEmbeddedCabHeader>
inspect_windows_adk_embedded_cab_header(
    std::span<const std::byte> header,
    std::uint64_t expected_container_bytes);

// Parses the bounded Burn manifest with XmlLite, with DTD processing
// prohibited. Exactly one Payload must bind the pinned SourcePath, FilePath
// and FileSize. Case aliases, duplicate target mappings and namespace tricks
// are rejected rather than guessed.
[[nodiscard]] clonecore::Status
validate_windows_adk_burn_eula_mapping(
    std::span<const std::byte> burn_manifest,
    const AdkEmbeddedEulaPin& pin);

struct WindowsAdkEulaExtractionObservation final {
  bool bootstrap_full_identity_verified{};
  bool attached_container_created_new{};
  bool attached_container_regular_file{};
  bool attached_container_single_link{};
  bool attached_container_reparse_point{};
  bool attached_container_bounds_verified{};
  bool attached_container_readback_equal{};
  bool cabinet_api_structure_verified{};
  bool burn_manifest_bounded{};
  bool burn_ux_mapping_verified{};
  bool member_copy_created_new{};
  bool member_copy_regular_file{};
  bool member_copy_single_link{};
  bool member_copy_reparse_point{};
  std::uint64_t member_byte_count{};
  std::string member_sha256;
  bool member_title_verified{};
  bool bounded_read_complete{};
  bool temporary_files_removed{};
};

// Converts native extraction facts into the consent receipt. Every fact is a
// mandatory gate; this helper is also the fault-injection seam used by tests.
[[nodiscard]] clonecore::Result<AdkEulaDocumentReceipt>
finalize_windows_adk_eula_receipt(
    const AdkEmbeddedEulaPin& pin,
    const WindowsAdkEulaExtractionObservation& observation);

struct WindowsAdkEulaExtractionResult final {
  AdkEulaDocumentReceipt receipt;
  std::vector<std::byte> rtf_document;
};

// Verifies that the supplied receipt came from the exact pinned Deployment
// Tools bootstrap. The concrete Windows platform additionally requires that
// verified_bootstrap.staged_path is still an owned CREATE_NEW stage file.
[[nodiscard]] clonecore::Status
validate_windows_adk_eula_source_identity(
    const AdkPinnedPayload& pinned_bootstrap,
    const AdkVerifiedPayload& verified_bootstrap,
    const AdkEmbeddedEulaPin& pin);

}  // namespace ytec::windowsapp

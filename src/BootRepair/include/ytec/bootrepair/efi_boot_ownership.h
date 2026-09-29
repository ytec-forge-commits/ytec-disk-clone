#pragma once

#include "ytec/clonecore/result.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ytec::bootrepair {

class IExecutableTrustVerifier;

enum class EfiBootOwnershipState : std::uint8_t {
  not_applicable,
  microsoft_only_or_empty,
  non_microsoft_or_untrusted_present,
  ambiguous,
};

// Platform-neutral observations used to classify an existing ESP. The
// Windows adapter obtains every value without mounting, changing, or deleting
// any ESP object. Counters are bounded before this structure is returned.
struct EfiBootOwnershipObservation final {
  bool efi_directory_present{};
  bool microsoft_namespace_present{};
  bool boot_namespace_present{};
  bool fallback_loader_present{};
  bool fallback_loader_microsoft_signed{};
  bool ambiguous_object_detected{};
  // Only normal directories directly below EFI are counted as independently
  // preservable third-party namespaces. Other top-level object kinds make the
  // observation ambiguous instead of becoming actionable.
  std::uint32_t top_level_non_microsoft_namespace_count{};
  std::uint32_t boot_namespace_nonstandard_entries{};
  std::uint32_t microsoft_signed_efi_loader_count{};
  std::uint32_t non_microsoft_or_untrusted_efi_loader_count{};
};

struct EfiBootOwnershipEvidence final {
  EfiBootOwnershipState state{EfiBootOwnershipState::not_applicable};
  bool efi_directory_present{};
  bool microsoft_namespace_present{};
  bool boot_namespace_present{};
  bool fallback_loader_present{};
  bool fallback_loader_microsoft_signed{};
  std::uint32_t microsoft_signed_efi_loader_count{};
  std::uint32_t non_microsoft_or_untrusted_entry_count{};
  std::uint32_t top_level_non_microsoft_namespace_count{};
  std::uint32_t boot_namespace_nonstandard_entry_count{};
  std::uint32_t non_microsoft_or_untrusted_efi_loader_count{};
};

[[nodiscard]] clonecore::Result<EfiBootOwnershipEvidence>
classify_efi_boot_ownership(
    const EfiBootOwnershipObservation& observation);

[[nodiscard]] bool equivalent_efi_boot_ownership(
    const EfiBootOwnershipEvidence& left,
    const EfiBootOwnershipEvidence& right) noexcept;

[[nodiscard]] bool efi_boot_ownership_allows_microsoft_rebuild(
    const EfiBootOwnershipEvidence& evidence) noexcept;

// Allows only independently named, normal top-level EFI directories to remain
// untouched while BCDBoot /s rebuilds Microsoft's namespace. Unknown objects,
// nonstandard EFI\Boot entries, an untrusted fallback loader, and untrusted
// loaders found below the scanned Microsoft/Boot namespaces all fail closed.
[[nodiscard]] bool efi_boot_ownership_allows_third_party_preserve(
    const EfiBootOwnershipEvidence& evidence) noexcept;

class IEfiBootOwnershipInspector {
 public:
  virtual ~IEfiBootOwnershipInspector() = default;

  // volume_root must be one exact Volume GUID root or drive root. The
  // implementation is read-only and must not assign a drive letter.
  [[nodiscard]] virtual clonecore::Result<EfiBootOwnershipEvidence>
  inspect_existing_esp_read_only(const std::wstring& volume_root) = 0;
};

[[nodiscard]] std::unique_ptr<IEfiBootOwnershipInspector>
make_windows_efi_boot_ownership_inspector(
    std::unique_ptr<IExecutableTrustVerifier> trust_verifier);

[[nodiscard]] std::unique_ptr<IEfiBootOwnershipInspector>
make_windows_efi_boot_ownership_inspector();

}  // namespace ytec::bootrepair

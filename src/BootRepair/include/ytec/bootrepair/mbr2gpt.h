#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/clonecore/disk_identity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ytec::bootrepair {

enum class Mbr2GptAction : std::uint8_t {
  validate,
  convert,
};

struct Mbr2GptConversionRequest final {
  std::uint32_t candidate_disk_number{};
  clonecore::StableDiskIdentity expected_target;
  clonecore::TargetConfirmation confirmation;
};

struct Mbr2GptConversionReport final {
  std::wstring executable_path;
  std::uint32_t disk_number{};
  ProcessResult validation;
  ProcessResult conversion;
  bool microsoft_signature_verified{};
  bool target_reidentified_before_conversion{};
};

// The observer must perform fresh read-only disk enumeration on every call.
// Conversion re-observes the same disk number after Microsoft's /validate pass
// and stops if the stable identity changed.
class IMbr2GptTargetObserver {
 public:
  virtual ~IMbr2GptTargetObserver() = default;
  [[nodiscard]] virtual clonecore::Result<clonecore::StableDiskIdentity>
  observe_target(std::uint32_t candidate_disk_number) = 0;
};

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_mbr2gpt_arguments(
    Mbr2GptAction action,
    std::uint32_t disk_number);

// Phase 4 process boundary. It deliberately never emits /allowFullOS or /map.
// Product/physical-disk integration remains disabled until the surrounding
// MBR, BitLocker, dynamic-disk, Windows-installation and UEFI gates exist.
[[nodiscard]] clonecore::Result<Mbr2GptConversionReport>
execute_mbr2gpt_conversion(
    const Mbr2GptConversionRequest& request,
    const std::wstring& trusted_system_directory,
    IMbr2GptTargetObserver& target_observer,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner);

}  // namespace ytec::bootrepair

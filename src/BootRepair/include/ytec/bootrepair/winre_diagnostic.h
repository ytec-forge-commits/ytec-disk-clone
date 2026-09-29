#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/mbr2gpt_layout.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::bootrepair {

enum class WinReRegisteredPathKind : std::uint8_t {
  recovery_windows_re,
  windows_system32_recovery,
};

struct WinReRegisteredLocation final {
  std::uint32_t disk_number{};
  std::uint32_t partition_number{};
  WinReRegisteredPathKind path_kind{
      WinReRegisteredPathKind::recovery_windows_re};
};

struct WinReImageObservation final {
  bool exists{};
  std::uint64_t length{};
};

class IWinReImageProbe {
 public:
  virtual ~IWinReImageProbe() = default;

  [[nodiscard]] virtual clonecore::Result<WinReImageObservation>
  inspect_regular_image(const std::wstring& path) = 0;
};

struct WinReDiagnosticRequest final {
  std::wstring offline_windows_directory;
  std::wstring trusted_system_directory;
  std::uint32_t expected_target_disk_number{};
  // Used only immediately after a reviewed direct clone. If REAgentC reports
  // another disk, the inspector records a stale source-machine registration
  // without opening that foreign path. Generic boot repair leaves this false.
  bool allow_mismatched_registered_location_as_cloned_source_stale{};
};

struct WinReDiagnosticReport final {
  std::wstring executable_path;
  std::wstring inspected_image_path;
  std::uint32_t exit_code{};
  std::string standard_output;
  std::string standard_error;
  WinReSourceState source_state{WinReSourceState::unknown};
  std::uint32_t registered_partition_number{};
  WinReRegisteredPathKind registered_path_kind{
      WinReRegisteredPathKind::recovery_windows_re};
  std::uint64_t winre_image_size_bytes{};
  bool microsoft_signature_verified{};
  bool read_only_command{};
  bool registered_location_reported{};
  bool registered_path_kind_reported{};
  bool registered_location_matches_expected_disk{};
  bool registered_location_mismatch_classified_as_cloned_source_stale{};
  bool registered_image_present{};
  bool fallback_image_present{};
};

class IWinReDiagnosticService {
 public:
  virtual ~IWinReDiagnosticService() = default;

  // Inspects an offline Windows installation with the current trusted
  // Windows/WinPE System32. Implementations must not change WinRE state.
  [[nodiscard]] virtual clonecore::Result<WinReDiagnosticReport> inspect(
      const std::wstring& offline_windows_directory,
      std::uint32_t expected_target_disk_number) = 0;
};

// REAgentC is localized, but the GLOBALROOT device path in its output is an
// ASCII Windows device path. The parser also accepts UTF-16LE capture bytes by
// discarding NUL code-unit bytes. Multiple different locations fail closed.
[[nodiscard]] clonecore::Result<
    std::optional<WinReRegisteredLocation>>
parse_reagentc_registered_location(std::string_view standard_output);

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_reagentc_info_arguments(
    const std::wstring& offline_windows_directory);

// Runs only the documented read-only command:
//   reagentc.exe /info /target <offline Windows directory>
// The executable must be Microsoft-signed and resolved from the supplied
// current WinPE/Windows System32 directory. Registered and fallback Winre.wim
// candidates are opened read-only and must be regular non-reparse files.
[[nodiscard]] clonecore::Result<WinReDiagnosticReport>
inspect_winre_source(
    const WinReDiagnosticRequest& request,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IWinReImageProbe& image_probe);

// Copies only verified diagnostic facts into the pure rebuild request.
// Unknown diagnostics remain a hard failure and never enable recovery
// partition creation.
[[nodiscard]] clonecore::Status
apply_winre_diagnostic_to_rebuild_request(
    const WinReDiagnosticReport& report,
    Mbr2GptRebuildRequest& request);

[[nodiscard]] std::unique_ptr<IWinReImageProbe>
make_windows_winre_image_probe();

[[nodiscard]] clonecore::Result<WinReDiagnosticReport>
inspect_winre_source_with_windows_apis(
    const WinReDiagnosticRequest& request);

[[nodiscard]] std::unique_ptr<IWinReDiagnosticService>
make_windows_winre_diagnostic_service();

}  // namespace ytec::bootrepair

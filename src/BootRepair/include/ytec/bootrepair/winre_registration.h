#pragma once

#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/winre_diagnostic.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ytec::bootrepair {

using WinReImageSha256 = std::array<std::byte, 32>;

// Immutable evidence for the exact Winre.wim reviewed before confirmation.
// The production observer rejects reparse points and hard links, hashes the
// complete file through the same read-only handle, and records the final
// opened path. The transaction opens and holds the file again without write
// or delete sharing before it invokes REAgentC.
struct WinReRegistrationImageIdentity final {
  std::wstring requested_path;
  std::wstring opened_final_path;
  std::uint64_t volume_serial_number{};
  std::array<std::byte, 16> file_id{};
  std::uint64_t length{};
  std::uint64_t last_write_time{};
  std::uint64_t change_time{};
  WinReImageSha256 sha256{};
};

[[nodiscard]] bool equivalent_winre_registration_image_identity(
    const WinReRegistrationImageIdentity& left,
    const WinReRegistrationImageIdentity& right) noexcept;

enum class WinReRegistrationIntent : std::uint8_t {
  register_verified_image,
  normal_boot_only_partial,
};

enum class WinReRegistrationPriorStateOrigin : std::uint8_t {
  // A state observed on the same target before this repair. A verified prior
  // registration is restored if the new registration fails.
  existing_target,
  // A freshly cloned offline Windows may contain a source-machine REAgentC
  // record that cannot be a rollback target. It may be replaced only when the
  // diagnostic proves the record points away from the freshly reidentified
  // target and a verified target-side candidate is supplied. Failure rolls
  // back to a verified disabled/unregistered state, never to the source path.
  cloned_source_stale,
};

enum class WinReRegistrationOutcome : std::uint8_t {
  completed,
  normal_boot_only_partial,
  failed_rolled_back,
  // A cloned source record has no safe target-side prior state to restore.
  // Failure was contained by disabling WinRE and verifying no registration.
  failed_safe_unregistered,
  failed_rollback_incomplete,
};

struct WinReRegistrationRequest final {
  WinReRegistrationIntent intent{
      WinReRegistrationIntent::normal_boot_only_partial};
  WinReRegistrationPriorStateOrigin prior_state_origin{
      WinReRegistrationPriorStateOrigin::existing_target};
  std::wstring offline_windows_directory;
  std::wstring trusted_system_directory;
  std::wstring candidate_directory;
  std::wstring rollback_candidate_directory;
  std::uint32_t expected_target_disk_number{};
  std::uint32_t expected_target_partition_number{};
  WinReRegisteredPathKind expected_registered_path_kind{
      WinReRegisteredPathKind::recovery_windows_re};
  std::optional<WinReRegistrationImageIdentity> reviewed_candidate;

  // A verified prior diagnostic is required before mutation. If it reports a
  // registered WinRE, reviewed_rollback_image must bind that exact prior
  // Winre.wim so rollback never follows a path that changed after review.
  WinReDiagnosticReport prior_diagnostic;
  std::optional<WinReRegistrationImageIdentity> reviewed_rollback_image;
};

struct WinReRegistrationReport final {
  WinReRegistrationOutcome outcome{
      WinReRegistrationOutcome::normal_boot_only_partial};
  bool candidate_locked{};
  bool prior_candidate_locked{};
  bool reagentc_signature_verified{};
  bool cloned_source_registration_disabled{};
  bool set_reimage_completed{};
  bool enable_completed{};
  bool registration_verified{};
  bool rollback_attempted{};
  bool rollback_completed{};
  bool rollback_verified{};
  std::uint32_t target_revalidation_count{};
  std::optional<clonecore::Error> primary_failure;
  std::optional<clonecore::Error> rollback_failure;
  std::optional<WinReDiagnosticReport> final_diagnostic;
};

class IWinReRegistrationImageLock {
 public:
  virtual ~IWinReRegistrationImageLock() = default;

  [[nodiscard]] virtual const WinReRegistrationImageIdentity& identity()
      const noexcept = 0;
};

class IWinReRegistrationImageLocker {
 public:
  virtual ~IWinReRegistrationImageLocker() = default;

  // The returned object must keep the exact file open read-only without write
  // or delete sharing for its full lifetime.
  [[nodiscard]] virtual clonecore::Result<
      std::unique_ptr<IWinReRegistrationImageLock>>
  lock_regular_image(const std::wstring& path) = 0;
};

class IWinReRegistrationTargetGuard {
 public:
  virtual ~IWinReRegistrationTargetGuard() = default;

  // Called before every potentially mutating command and every diagnostic
  // used as a commit/rollback proof. Implementations must re-identify the
  // stable target and the reviewed Windows/Recovery partitions.
  [[nodiscard]] virtual clonecore::Status revalidate_target() = 0;
};

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_reagentc_setreimage_arguments(
    const std::wstring& candidate_directory,
    const std::wstring& offline_windows_directory);

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_reagentc_enable_arguments(
    const std::wstring& offline_windows_directory);

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_reagentc_disable_arguments(
    const std::wstring& offline_windows_directory);

// A partial intent performs no file, process, target or diagnostic I/O. A
// registration intent validates and locks every rollback dependency before
// the first mutation. Once mutation may have started, operational failures are
// returned in a successful report so callers can distinguish verified
// rollback from an incomplete rollback. Validation failures before mutation
// remain Result failures.
[[nodiscard]] clonecore::Result<WinReRegistrationReport>
execute_winre_registration_transaction(
    const WinReRegistrationRequest& request,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IWinReRegistrationImageLocker& image_locker,
    IWinReRegistrationTargetGuard& target_guard,
    IWinReDiagnosticService& diagnostic_service);

// Read-only production helpers. Observation closes its handle after producing
// evidence; callers must use the locker during the confirmed transaction.
[[nodiscard]] clonecore::Result<WinReRegistrationImageIdentity>
observe_winre_registration_image_with_windows_apis(
    const std::wstring& path);

[[nodiscard]] std::unique_ptr<IWinReRegistrationImageLocker>
make_windows_winre_registration_image_locker();

}  // namespace ytec::bootrepair

#pragma once

#include "ytec/clonecore/result.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ytec::windowsapp {

enum class PostMigrationEvidenceState : std::uint8_t {
  verified,
  attention,
  unavailable,
};

struct PostMigrationEvidence final {
  PostMigrationEvidenceState state{
      PostMigrationEvidenceState::unavailable};
  std::wstring summary;
  std::wstring detail;
  std::optional<clonecore::Error> error;
};

enum class PostMigrationWinReState : std::uint8_t {
  registered,
  disabled_or_missing,
  registered_on_another_disk,
  unknown,
};

enum class PostMigrationBitLockerConversionState : std::uint8_t {
  fully_decrypted,
  fully_encrypted,
  encryption_in_progress,
  decryption_in_progress,
  encryption_paused,
  decryption_paused,
  unknown,
  provider_unavailable,
};

enum class PostMigrationBitLockerProtectionState : std::uint8_t {
  off,
  on,
  unknown,
  provider_unavailable,
};

struct PostMigrationBitLockerObservation final {
  PostMigrationBitLockerConversionState conversion{
      PostMigrationBitLockerConversionState::unknown};
  PostMigrationBitLockerProtectionState protection{
      PostMigrationBitLockerProtectionState::unknown};
  std::optional<std::uint32_t> encryption_percentage;
};

struct PostMigrationCheckReport final {
  diskmodel::DiskInfo current_boot_disk;
  std::uint32_t windows_partition_number{};
  std::uint32_t system_partition_number{};
  std::wstring windows_volume_name;
  std::wstring windows_file_system;
  std::wstring system_volume_name;
  std::wstring system_file_system;
  PostMigrationWinReState winre_state{PostMigrationWinReState::unknown};
  PostMigrationBitLockerObservation bitlocker;
  PostMigrationEvidence boot_disk_and_layout;
  PostMigrationEvidence bcd_and_boot_manager;
  PostMigrationEvidence winre;
  PostMigrationEvidence file_system_and_disk_health;
  PostMigrationEvidence bitlocker_status;
  bool read_only_operations_only{};
  // This diagnostic is intentionally post-boot. It must never be presented as
  // proof that the target would have booted before the first successful boot.
  bool preboot_success_guaranteed{};
};

class IPostMigrationReadOnlyPlatform {
 public:
  virtual ~IPostMigrationReadOnlyPlatform() = default;

  [[nodiscard]] virtual clonecore::Result<PostMigrationCheckReport>
  inspect_read_only() = 0;
};

// Validates the evidence contract returned by an injected platform. A
// platform cannot turn an unknown/unsupported observation into VERIFIED, and
// the read-only/non-preboot disclosure is mandatory.
[[nodiscard]] clonecore::Result<PostMigrationCheckReport>
validate_post_migration_check_report(PostMigrationCheckReport report);

[[nodiscard]] clonecore::Result<PostMigrationCheckReport>
run_post_migration_check(IPostMigrationReadOnlyPlatform& platform);

// Construction performs no I/O. inspect_read_only() uses inventory, Volume
// GUID queries, read-only BCD hive validation, signed REAgentC /info, and a
// local read-only WMI SELECT for BitLocker status. It never mounts, unlocks,
// repairs, writes, or changes disk attributes.
[[nodiscard]] std::unique_ptr<IPostMigrationReadOnlyPlatform>
make_windows_post_migration_read_only_platform();

[[nodiscard]] clonecore::Result<PostMigrationCheckReport>
run_post_migration_check_with_windows_apis();

[[nodiscard]] std::wstring format_post_migration_check_report(
    const PostMigrationCheckReport& report);

}  // namespace ytec::windowsapp

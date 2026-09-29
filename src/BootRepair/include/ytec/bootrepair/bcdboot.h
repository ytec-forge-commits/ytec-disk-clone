#pragma once

#include "ytec/clonecore/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ytec::bootrepair {

enum class BcdBootFirmware : std::uint8_t {
  uefi,
  bios,
};

enum class BcdBootStorePolicy : std::uint8_t {
  preserve_existing,
  rebuild_fresh,
};

struct BcdBootRequest final {
  std::wstring target_windows_directory;
  std::wstring target_system_partition_root;
  BcdBootFirmware firmware{BcdBootFirmware::uefi};
  BcdBootStorePolicy store_policy{
      BcdBootStorePolicy::preserve_existing};
};

struct ProcessResult final {
  std::uint32_t exit_code{};
  std::string standard_output;
  std::string standard_error;
};

using ProcessOutputCallback =
    std::function<void(std::string_view)>;

struct BcdBootReport final {
  std::wstring executable_path;
  std::uint32_t exit_code{};
  std::string standard_output;
  std::string standard_error;
  bool microsoft_signature_verified{};
  bool prior_store_replaced{};
  bool fresh_store_verified{};
};

struct MultiWindowsBcdBootReport final {
  std::vector<BcdBootReport> windows_registrations;
  bool prior_store_replaced{};
  bool fresh_store_verified{};
};

// Immutable opened-handle evidence for one BCD file. Rollback of a file that
// BCDBoot created is permitted only while this exact file object, length and
// write/change timestamps still match. This prevents a path replacement from
// turning cleanup into deletion of an unrelated file.
struct BcdStoreFileIdentity final {
  std::uint64_t volume_serial_number{};
  std::array<std::byte, 16> file_id{};
  std::uint64_t length{};
  std::uint64_t last_write_time{};
  std::uint64_t change_time{};
};

[[nodiscard]] bool equivalent_bcd_store_file_identity(
    const BcdStoreFileIdentity& left,
    const BcdStoreFileIdentity& right) noexcept;

class IExecutableTrustVerifier {
 public:
  virtual ~IExecutableTrustVerifier() = default;
  [[nodiscard]] virtual clonecore::Status verify_microsoft_signed(
      const std::wstring& executable_path) = 0;
};

class IProcessRunner {
 public:
  virtual ~IProcessRunner() = default;
  [[nodiscard]] virtual clonecore::Result<ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) = 0;

  // Implementations may override this to deliver stdout while the process is
  // running. The default keeps existing mock/process implementations usable
  // and delivers the captured output once the process has completed.
  [[nodiscard]] virtual clonecore::Result<ProcessResult> run_streamed(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory,
      const ProcessOutputCallback& standard_output_callback);
};

class IBcdStoreFileSystem {
 public:
  virtual ~IBcdStoreFileSystem() = default;

  [[nodiscard]] virtual clonecore::Result<bool>
  is_regular_non_reparse_file(const std::wstring& path) = 0;
  // Returns nullopt only when the path is absent. Existing objects must be a
  // regular, non-reparse, single-link file opened read-only.
  [[nodiscard]] virtual clonecore::Result<
      std::optional<BcdStoreFileIdentity>>
  observe_regular_file_identity(const std::wstring& path) = 0;
  // Opens the BCD hive read-only and verifies the Objects container and the
  // Windows Boot Manager object. File existence alone is not a commit proof.
  [[nodiscard]] virtual clonecore::Status verify_bcd_store_read_only(
      const std::wstring& path) = 0;
  // Moves only the already-observed source identity and reopens the
  // destination to prove that the same object was moved.
  [[nodiscard]] virtual clonecore::Status move_file_no_replace(
      const std::wstring& source,
      const std::wstring& destination,
      const BcdStoreFileIdentity& expected_source) = 0;
  // Opens the path with deletion rights, rechecks the complete identity on
  // the same handle, then marks that handle for deletion. A mismatch never falls
  // back to path-based deletion.
  [[nodiscard]] virtual clonecore::Status
  remove_file_if_identity_matches(
      const std::wstring& path,
      const BcdStoreFileIdentity& expected) = 0;
};

[[nodiscard]] clonecore::Result<std::vector<std::wstring>>
build_bcdboot_arguments(const BcdBootRequest& request);

[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument);

[[nodiscard]] clonecore::Result<BcdBootReport> execute_bcdboot(
    const BcdBootRequest& request,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner);

[[nodiscard]] clonecore::Result<std::wstring> bcd_store_path(
    const BcdBootRequest& request);

[[nodiscard]] clonecore::Result<BcdBootReport>
execute_bcdboot_with_store_transaction(
    const BcdBootRequest& request,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IBcdStoreFileSystem& file_system);

// Rebuilds one BCD store and registers every reviewed Windows in caller order
// under a single backup/rollback boundary. The first request must be
// rebuild_fresh and every later request must preserve_existing. All requests
// must target the same system root and firmware, and Windows roots must be
// unique. Every invocation retains the explicit /s target, so this function
// does not mutate the current PC's NVRAM.
[[nodiscard]] clonecore::Result<MultiWindowsBcdBootReport>
execute_multi_windows_bcdboot_with_store_transaction(
    const std::vector<BcdBootRequest>& requests_in_boot_priority,
    const std::wstring& trusted_system_directory,
    IExecutableTrustVerifier& trust_verifier,
    IProcessRunner& process_runner,
    IBcdStoreFileSystem& file_system);

// Public read-only seam used by synthetic hive acceptance. The production
// transaction uses the same implementation before accepting a new BCD.
[[nodiscard]] clonecore::Status
verify_bcd_store_file_with_windows_apis(const std::wstring& path);

[[nodiscard]] clonecore::Result<MultiWindowsBcdBootReport>
execute_multi_windows_bcdboot_with_windows_apis(
    const std::vector<BcdBootRequest>& requests_in_boot_priority);

[[nodiscard]] clonecore::Result<BcdBootReport>
execute_bcdboot_with_windows_apis(const BcdBootRequest& request);

[[nodiscard]] std::unique_ptr<IExecutableTrustVerifier>
make_windows_authenticode_verifier();

[[nodiscard]] std::unique_ptr<IProcessRunner> make_windows_process_runner(
    std::uint32_t timeout_milliseconds = 10U * 60U * 1000U);

// Production implementation of the exact-identity BCD file seam. It opens
// every target without following reparse points, performs no-replace rename
// through the verified source handle, and marks only an identity-matched
// handle for deletion. Callers remain responsible for their transaction
// ordering and must never substitute path-only cleanup.
[[nodiscard]] std::unique_ptr<IBcdStoreFileSystem>
make_windows_bcd_store_file_system();

}  // namespace ytec::bootrepair

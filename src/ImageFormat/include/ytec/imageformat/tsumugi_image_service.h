#pragma once

#include "ytec/clonecore/block_device.h"
#include "ytec/clonecore/operation_progress.h"
#include "ytec/clonecore/rescue_copy.h"
#include "ytec/clonecore/result.h"
#include "ytec/imageformat/sha256.h"
#include "ytec/imageformat/tsumugi_manifest.h"
#include "ytec/imageformat/tsumugi_restore_layout.h"
#include "ytec/imageformat/tsumugi_stream.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ytec::imageformat {

// The product format is one local file. FAT32 and unknown/network-oriented
// storage are rejected before source reads or restore planning begins. The
// low-level writer independently rechecks the actual destination volume.
enum class TsumugiImageStorageFileSystem : std::uint8_t {
  unknown = 0U,
  ntfs = 1U,
  exfat = 2U,
  fat32 = 3U,
  other = 4U,
};

struct TsumugiImageEncryptionRequest final {
  // The caller owns this view for the duration of create_tsumugi_image_v1().
  // The service generates an image-specific Salt and base Nonce internally.
  std::string_view password;
};

// One immutable source state for the entire image operation. Online callers
// bind this to one VSS Snapshot Set; PE callers bind it to the locked,
// read-only source disk observation. Mixing readers or source generations is
// rejected even when every individual chunk is internally valid.
class ITsumugiImageSourceSession
    : public clonecore::ISourceDiskReader {
 public:
  ~ITsumugiImageSourceSession() override = default;

  [[nodiscard]] virtual Sha256Digest source_model_hash() const noexcept = 0;
  [[nodiscard]] virtual Sha256Digest source_serial_hash() const noexcept = 0;
  [[nodiscard]] virtual Sha256Digest source_state_hash() const noexcept = 0;
};

// One caller-owned transient rescue target. Raw rescue writes and verifies it,
// seal_for_image_read() re-identifies it and removes write access, and the
// image service reads only that sealed view. discard_owned_staging() must
// affect only this exact owned object and be idempotent.
class ITsumugiRescueStagingSession
    : public ITsumugiImageSourceSession,
      public clonecore::ITargetDiskWriter {
 public:
  ~ITsumugiRescueStagingSession() override = default;

  [[nodiscard]] virtual std::uint64_t size_bytes()
      const noexcept override = 0;
  [[nodiscard]] virtual std::uint32_t logical_sector_size()
      const noexcept override = 0;
  [[nodiscard]] virtual clonecore::Status seal_for_image_read() = 0;
  [[nodiscard]] virtual bool sealed_for_image_read() const noexcept = 0;
  [[nodiscard]] virtual clonecore::Status
  discard_owned_staging() noexcept = 0;
  // Called only after exact staging discard and complete image-partial
  // verification. Product adapters must re-identify the original destination
  // and the expected adjacent partial length without requiring the failed
  // source to remain connected.
  [[nodiscard]] virtual clonecore::Status
  validate_image_destination_before_commit(
      std::uint64_t expected_owned_partial_bytes) = 0;
};

// Converts one completed raw-rescue report into canonical rescue-image
// chunks. verified_staging_session must expose the fully flushed,
// write/read-back-verified rescue target in original disk coordinates; it must
// never reopen the failing source for a second payload pass. Every missing
// range must belong to a selected manifest partition or planning fails closed.
[[nodiscard]] clonecore::Result<std::vector<TsumugiStreamBuildChunk>>
make_tsumugi_rescue_chunks_v1(
    const TsumugiManifest& manifest,
    const clonecore::RescueRawCopyReport& rescue_report,
    const ITsumugiImageSourceSession& verified_staging_session,
    std::uint32_t chunk_size = kImageChunkSize16MiB);

struct TsumugiImageCreateRequest final {
  std::wstring final_path;
  TsumugiImageStorageFileSystem storage_file_system{
      TsumugiImageStorageFileSystem::unknown};
  TsumugiManifest manifest;
  std::vector<TsumugiStreamBuildChunk> chunks;
  ImageCompression compression{ImageCompression::zstandard};
  std::uint32_t chunk_size{kImageChunkSize16MiB};
  std::size_t verification_block_bytes{4U * 1024U * 1024U};
  TsumugiCreateVerificationMode verification_mode{
      TsumugiCreateVerificationMode::complete};
  std::optional<TsumugiImageEncryptionRequest> encryption;
  bool replace_existing{};
  const ITsumugiImageSourceSession* source_session{};
};

struct TsumugiImageCreateReport final {
  TsumugiStreamBuildReport stream;
  std::vector<clonecore::ByteRange> unreadable_ranges;
  bool encrypted{};
  bool password_was_weak{};
  bool selected_verification_passed{};
  bool complete_verification_passed{};
};

// Central evidence gate shared by Windows, WinPE, VSS and completion actions.
// It accepts fast mode when every write-time/hash/metadata gate passed; a
// persistent-resume coordinator may safely strengthen that selection with a
// full scan. Complete mode always requires the additional full scan.
[[nodiscard]] bool selected_tsumugi_creation_verification_passed(
    const TsumugiImageCreateReport& report) noexcept;

struct TsumugiRescueImageCreateRequest final {
  // manifest.mode must be rescue. chunks and source_session must remain empty;
  // this service derives both exclusively from the verified rescue result.
  TsumugiImageCreateRequest image;
  clonecore::RescueRawCopyRequest rescue_copy;
  const clonecore::ISourceDiskReader* failing_source{};
  ITsumugiRescueStagingSession* staging{};
};

struct TsumugiRescueImageCreateReport final {
  clonecore::RescueRawCopyReport rescue;
  TsumugiImageCreateReport image;
  bool staging_sealed_for_image_read{};
  bool staging_discarded_before_final_commit{};
  bool staging_destination_revalidated_before_final_commit{};
};

// Service-level staged image. All source-session, manifest coverage,
// encryption and container checks have passed, while the final filename still
// remains untouched. Windows VSS keeps this object until BackupComplete and
// Snapshot-set deletion succeed.
class TsumugiStagedImageV1 final {
 public:
  ~TsumugiStagedImageV1();

  TsumugiStagedImageV1(TsumugiStagedImageV1&&) noexcept;
  TsumugiStagedImageV1& operator=(TsumugiStagedImageV1&&) noexcept;
  TsumugiStagedImageV1(const TsumugiStagedImageV1&) = delete;
  TsumugiStagedImageV1& operator=(const TsumugiStagedImageV1&) = delete;

  [[nodiscard]] const TsumugiImageCreateReport& report() const noexcept;
  [[nodiscard]] bool pending() const noexcept;
  [[nodiscard]] clonecore::Result<TsumugiImageCreateReport>
  commit_verified();
  [[nodiscard]] clonecore::Status abort_incomplete() noexcept;

 private:
  class Impl;
  explicit TsumugiStagedImageV1(std::unique_ptr<Impl> impl) noexcept;

  friend clonecore::Result<TsumugiStagedImageV1>
  prepare_tsumugi_image_v1(
      const TsumugiImageCreateRequest&,
      const clonecore::DiskOperationCallbacks&);

  std::unique_ptr<Impl> impl_;
};

// Performs every creation and selected-verification step but defers the
// recoverable final-name transaction until commit_verified().
[[nodiscard]] clonecore::Result<TsumugiStagedImageV1>
prepare_tsumugi_image_v1(
    const TsumugiImageCreateRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Builds the authenticated typed manifest, generates cryptographic material,
// streams the payload to a neighbouring .partial, applies the requested
// verification mode, and only then promotes it to the final .tsumugi name.
[[nodiscard]] clonecore::Result<TsumugiImageCreateReport>
create_tsumugi_image_v1(
    const TsumugiImageCreateRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Performs one finite raw rescue into an owned transient staging session,
// seals and re-identifies that session, prepares and completely verifies the
// final .tsumugi partial, discards staging, and only then commits the final
// filename. The failing source is never read by the image writer.
[[nodiscard]] clonecore::Result<TsumugiRescueImageCreateReport>
create_tsumugi_rescue_image_v1(
    const TsumugiRescueImageCreateRequest& request,
    const clonecore::DiskOperationCallbacks& image_callbacks = {});

struct TsumugiImageVerifyRequest final {
  std::wstring image_path;
  TsumugiImageStorageFileSystem storage_file_system{
      TsumugiImageStorageFileSystem::unknown};
  std::optional<std::string_view> password;
  std::size_t verification_block_bytes{4U * 1024U * 1024U};
};

struct TsumugiVerifiedImage final {
  TsumugiStreamInspection container;
  TsumugiManifest manifest;
  std::vector<clonecore::ByteRange> unreadable_ranges;
  bool partial_loss{};
};

// Completely authenticates the container and then parses and cross-checks the
// typed manifest. A successful result is inspection only and authorizes no
// target write.
[[nodiscard]] clonecore::Result<TsumugiVerifiedImage>
verify_tsumugi_image_v1(
    const TsumugiImageVerifyRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks = {});

enum class TsumugiRestoreHost : std::uint8_t {
  windows = 1U,
  winpe = 2U,
};

struct TsumugiRestoreDiskIdentity final {
  // Stable composite identity supplied by the disk-inventory layer. A disk
  // number alone is deliberately not represented here.
  Sha256Digest stable_identity_hash{};
  std::uint64_t disk_size{};
  std::uint32_t logical_sector_size{};
  bool is_running_windows_system_disk{};
  bool is_usb_attached{};
  bool is_usb_memory{};
  bool is_active_rescue_media{};
  bool is_dynamic_disk{};
  bool is_storage_spaces{};
  bool is_windows_software_raid{};
  bool has_unresolved_hardware_raid{};
  // Required for USB-attached disks. It changes after disconnect/reconnect,
  // forcing the user to select the target again in the new connection session.
  Sha256Digest connection_instance_hash{};
};

struct TsumugiRestorePartitionPlacement final {
  std::uint32_t source_table_index{};
  std::uint64_t target_offset{};
  std::uint64_t target_size{};
};

struct TsumugiWholeDiskRestoreTarget final {
  TsumugiRestoreDiskIdentity disk;
  // A shrink whole-disk restore is authorized only by this reviewed concrete
  // final layout. The service derives every payload placement and every
  // permitted regenerated-partition omission from it. Exact/rescue restores
  // require this to be empty.
  std::optional<TsumugiShrinkWholeDiskRestoreLayoutPlanV1>
      reviewed_shrink_layout;
  // Compatibility mirror for product adapters while the direct controller is
  // being connected. It may be empty; when supplied it must exactly equal the
  // mapping derived from reviewed_shrink_layout. The service replaces it with
  // the canonical derived mapping before transaction.begin().
  std::vector<TsumugiRestorePartitionPlacement> shrink_placements;
};

enum class TsumugiShrinkPayloadDispositionV1 : std::uint8_t {
  restore_to_reviewed_partition = 1U,
  regenerate_efi_system = 2U,
  regenerate_microsoft_reserved = 3U,
  replace_bios_system_with_uefi = 4U,
  recreate_empty_file_system = 5U,
};

// One authenticated selected payload has exactly one binding. A target number
// of zero is valid only for an explicit regeneration disposition and never
// authorizes an arbitrary skip.
struct TsumugiShrinkPayloadBindingV1 final {
  std::uint32_t source_table_index{};
  std::uint32_t source_partition_number{};
  TsumugiShrinkPayloadDispositionV1 disposition{
      TsumugiShrinkPayloadDispositionV1::restore_to_reviewed_partition};
  std::uint32_t target_number{};
  std::uint64_t target_offset{};
  std::uint64_t target_size{};
};

// Binds every selected shrink payload to the reviewed final layout. ESP/MSR,
// MBR BIOS-system replacement, and an explicitly empty filesystem action are
// the only accepted no-write dispositions. This function performs no I/O.
[[nodiscard]] clonecore::Result<std::vector<TsumugiShrinkPayloadBindingV1>>
make_tsumugi_shrink_payload_bindings_v1(
    const TsumugiManifest& manifest,
    const TsumugiShrinkWholeDiskRestoreLayoutPlanV1& reviewed_layout);

struct TsumugiExistingPartitionRestoreTarget final {
  TsumugiRestoreDiskIdentity disk;
  std::uint32_t target_table_index{};
  std::uint32_t target_partition_number{};
  std::uint64_t target_offset{};
  std::uint64_t target_size{};
};

struct TsumugiUnallocatedRestoreTarget final {
  TsumugiRestoreDiskIdentity disk;
  std::uint64_t target_offset{};
  std::uint64_t target_size{};
};

using TsumugiIndividualPartitionTarget = std::variant<
    TsumugiExistingPartitionRestoreTarget,
    TsumugiUnallocatedRestoreTarget>;

struct TsumugiIndividualPartitionRestoreTarget final {
  std::uint32_t source_table_index{};
  TsumugiIndividualPartitionTarget target;
};

using TsumugiRestoreTarget = std::variant<
    TsumugiWholeDiskRestoreTarget,
    TsumugiIndividualPartitionRestoreTarget>;

struct TsumugiRestorePlanRequest final {
  TsumugiImageVerifyRequest image;
  TsumugiRestoreHost host{TsumugiRestoreHost::windows};
  TsumugiRestoreTarget target;
};

class ITsumugiRestoreTransaction;
class ITsumugiShrinkRestoreTransaction;

class TsumugiRestorePlan final {
 public:
  TsumugiRestorePlan(TsumugiRestorePlan&&) noexcept = default;
  TsumugiRestorePlan& operator=(TsumugiRestorePlan&&) noexcept = default;
  TsumugiRestorePlan(const TsumugiRestorePlan&) = delete;
  TsumugiRestorePlan& operator=(const TsumugiRestorePlan&) = delete;

  [[nodiscard]] const TsumugiVerifiedImage& image() const noexcept;
  [[nodiscard]] const TsumugiRestoreTarget& target() const noexcept;
  [[nodiscard]] TsumugiRestoreHost host() const noexcept;
  [[nodiscard]] bool is_whole_disk_restore() const noexcept;
  [[nodiscard]] bool requires_boot_repair_offer() const noexcept;
  [[nodiscard]] bool has_partial_loss() const noexcept;
  [[nodiscard]] std::uint64_t planned_payload_bytes() const noexcept;
  [[nodiscard]] std::span<const TsumugiShrinkPayloadBindingV1>
  shrink_payload_bindings() const noexcept;

 private:
  friend clonecore::Result<TsumugiRestorePlan>
  prepare_tsumugi_restore_plan_v1(
      const TsumugiRestorePlanRequest&,
      const clonecore::DiskOperationCallbacks&);
  friend clonecore::Result<struct TsumugiRestoreReport>
  execute_tsumugi_restore_plan_v1(
      TsumugiRestorePlan&,
      std::optional<std::string_view>,
      ITsumugiRestoreTransaction&,
      const clonecore::DiskOperationCallbacks&);
  friend clonecore::Result<struct TsumugiShrinkRestoreReport>
  execute_tsumugi_shrink_restore_plan_v1(
      TsumugiRestorePlan&,
      std::optional<std::string_view>,
      ITsumugiShrinkRestoreTransaction&,
      const clonecore::DiskOperationCallbacks&);

  TsumugiRestorePlan() = default;

  std::wstring image_path_;
  TsumugiImageStorageFileSystem storage_file_system_{
      TsumugiImageStorageFileSystem::unknown};
  std::size_t verification_block_bytes_{};
  TsumugiRestoreHost host_{TsumugiRestoreHost::windows};
  TsumugiRestoreTarget target_;
  TsumugiVerifiedImage image_;
  std::vector<TsumugiShrinkPayloadBindingV1> shrink_payload_bindings_;
  bool requires_boot_repair_offer_{};
  std::uint64_t planned_payload_bytes_{};
  std::unique_ptr<std::atomic_bool> consumed_{
      std::make_unique<std::atomic_bool>(false)};
};

// This phase performs complete image verification and target-geometry
// validation only. It never invokes a write callback.
[[nodiscard]] clonecore::Result<TsumugiRestorePlan>
prepare_tsumugi_restore_plan_v1(
    const TsumugiRestorePlanRequest& request,
    const clonecore::DiskOperationCallbacks& callbacks = {});

struct TsumugiRestoreWrite final {
  Sha256Digest stable_target_identity_hash{};
  std::uint32_t source_table_index{};
  std::uint32_t source_partition_number{};
  std::uint64_t source_payload_offset{};
  std::uint64_t target_offset{};
  std::uint64_t length{};
  bool zero_fill{};
  bool unreadable_zero_fill{};
};

// Product restore adapters must own the target handle and the complete layout
// transaction. begin() re-identifies and locks the stable target, records an
// incomplete state, and invalidates/backs up the destination layout as needed.
// write_and_verify() must write, flush as required, and read back the exact
// range. commit() flushes and exposes the final primary partition table last.
// Any path after a successful begin() that does not commit is aborted by the
// service. A callback that merely reports success is therefore insufficient.
class ITsumugiRestoreTransaction {
 public:
  virtual ~ITsumugiRestoreTransaction() = default;

  [[nodiscard]] virtual clonecore::Result<TsumugiRestoreDiskIdentity> begin(
      const TsumugiVerifiedImage& image,
      const TsumugiRestoreTarget& target,
      TsumugiRestoreHost host) = 0;

  [[nodiscard]] virtual clonecore::Status write_and_verify(
      const TsumugiRestoreWrite& write,
      std::span<const std::byte> plaintext) = 0;

  [[nodiscard]] virtual clonecore::Status commit() = 0;
  virtual void abort() noexcept = 0;
};

struct TsumugiShrinkArchiveTarget final {
  Sha256Digest stable_target_identity_hash{};
  std::uint32_t source_table_index{};
  std::uint32_t source_partition_number{};
  TsumugiManifestFileSystem file_system{
      TsumugiManifestFileSystem::unknown};
  std::uint16_t payload_format_version{};
  std::uint64_t cluster_size{};
  std::uint64_t target_offset{};
  std::uint64_t target_size{};
  std::uint64_t archive_length{};
};

struct TsumugiShrinkArchiveChunk final {
  std::uint32_t source_table_index{};
  std::uint64_t source_payload_offset{};
  std::uint64_t archive_offset{};
  std::uint64_t length{};
  bool zero_fill{};
};

// The product adapter owns target layout creation, a temporary WIM staging
// location that is not on the source disk, WIM apply, filesystem verification,
// and the final partition-table commit. This interface deliberately keeps WIM
// bytes separate from sector writes, preventing archive bytes from ever being
// interpreted as a target filesystem image.
class ITsumugiShrinkRestoreTransaction {
 public:
  virtual ~ITsumugiShrinkRestoreTransaction() = default;

  [[nodiscard]] virtual clonecore::Result<TsumugiRestoreDiskIdentity> begin(
      const TsumugiVerifiedImage& image,
      const TsumugiRestoreTarget& target,
      TsumugiRestoreHost host) = 0;

  [[nodiscard]] virtual clonecore::Status write_exact_raw_and_verify(
      const TsumugiRestoreWrite& write,
      std::span<const std::byte> plaintext) = 0;

  // A reviewed create_empty_* action still requires a real target operation;
  // it is never treated as an arbitrary payload skip. The adapter must create
  // the requested empty filesystem and read back its filesystem metadata
  // before reporting success. The service calls this once after complete
  // image verification and target re-identification.
  [[nodiscard]] virtual clonecore::Status
  recreate_empty_file_system_and_verify(
      const TsumugiShrinkArchiveTarget& target) = 0;

  [[nodiscard]] virtual clonecore::Status begin_wim_archive(
      const TsumugiShrinkArchiveTarget& target) = 0;

  [[nodiscard]] virtual clonecore::Status append_wim_archive(
      const TsumugiShrinkArchiveChunk& chunk,
      std::span<const std::byte> plaintext) = 0;

  // Must validate the staged single-image WIM, apply it to the already-created
  // target filesystem, and read back enough filesystem metadata/content to
  // prove that the apply completed before reporting success.
  [[nodiscard]] virtual clonecore::Status
  complete_wim_archive_and_verify(std::uint32_t source_table_index) = 0;

  [[nodiscard]] virtual clonecore::Status commit() = 0;
  virtual void abort() noexcept = 0;
};

struct TsumugiRestoreReport final {
  std::uint64_t written_logical_bytes{};
  std::uint64_t written_chunk_count{};
  std::uint64_t intentionally_omitted_logical_bytes{};
  std::uint64_t intentionally_omitted_chunk_count{};
  std::uint64_t intentionally_regenerated_partitions{};
  bool callbacks_started_after_complete_verification{};
  bool image_matched_prepared_plan{};
  bool target_reidentified_before_write{};
  bool all_writes_read_back_verified{};
  bool final_layout_committed{};
  bool partial_loss{};
};

struct TsumugiShrinkRestoreReport final {
  std::uint64_t archive_logical_bytes{};
  std::uint64_t archive_chunk_count{};
  std::uint64_t exact_raw_logical_bytes{};
  std::uint64_t exact_raw_chunk_count{};
  std::uint64_t completed_archive_partitions{};
  std::uint64_t completed_empty_file_system_partitions{};
  std::uint64_t intentionally_omitted_logical_bytes{};
  std::uint64_t intentionally_omitted_chunk_count{};
  std::uint64_t intentionally_regenerated_partitions{};
  bool callbacks_started_after_complete_verification{};
  bool image_matched_prepared_plan{};
  bool target_reidentified_before_write{};
  bool all_payloads_verified_by_adapter{};
  bool final_layout_committed{};
};

// Reopens the image for execution and uses the stream layer's strict two-pass
// reader. That one handle denies write/delete sharing; the complete header,
// metadata, global hash and every plaintext chunk are verified before this
// service forwards the first target-write callback.
[[nodiscard]] clonecore::Result<TsumugiRestoreReport>
execute_tsumugi_restore_plan_v1(
    TsumugiRestorePlan& plan,
    std::optional<std::string_view> password,
    ITsumugiRestoreTransaction& transaction,
    const clonecore::DiskOperationCallbacks& callbacks = {});

// Dedicated executor for shrink manifests containing single-image WIM
// payloads. The container is fully reverified on one immutable handle before
// begin() or the first archive/RAW callback. There is intentionally no built-in
// Windows/WinPE implementation yet; callers without the dedicated adapter must
// keep this product path disabled.
[[nodiscard]] clonecore::Result<TsumugiShrinkRestoreReport>
execute_tsumugi_shrink_restore_plan_v1(
    TsumugiRestorePlan& plan,
    std::optional<std::string_view> password,
    ITsumugiShrinkRestoreTransaction& transaction,
    const clonecore::DiskOperationCallbacks& callbacks = {});

}  // namespace ytec::imageformat

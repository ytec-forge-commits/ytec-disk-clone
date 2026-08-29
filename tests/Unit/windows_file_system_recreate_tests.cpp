#include "ytec/imageformat/sha256.h"
#include "ytec/migrationengine/windows_file_system_recreate.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::migrationcore::CanonicalFileSystemTree;
using ytec::migrationcore::CanonicalFileSystemTreeEntry;
using ytec::migrationcore::FileSystemRecreateFormatGeometry;
using ytec::migrationcore::FileSystemRecreateSha256;
using ytec::migrationcore::MigrationFileSystem;
using namespace ytec::migrationengine;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint64_t kCanonicalTime =
    119'600'064'000'000'000ULL +
    50ULL *
        ytec::migrationcore::kFileSystemRecreateTimestampQuantum100ns;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

FileSystemRecreateSha256 digest(const std::uint8_t seed) {
  FileSystemRecreateSha256 value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = std::byte{
        static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index))};
  }
  return value;
}

FileSystemRecreateSha256 content_digest(
    const std::span<const std::byte> bytes) {
  const auto hashed = ytec::imageformat::sha256(bytes);
  check(hashed.has_value(), "Synthetic content SHA-256 should succeed");
  return hashed.value();
}

ytec::clonecore::StableDiskIdentity make_source_disk_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 2U,
      .model = L"Synthetic Source",
      .size_bytes = 8ULL * kGiB,
      .logical_sector_size = 512U,
      .serial_suffix = "SRC00001",
      .device_instance_id = L"SYNTHETIC\\SOURCE",
      .is_system_disk = false,
  };
}

ytec::clonecore::StableDiskIdentity make_target_disk_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 7U,
      .model = L"Synthetic Target",
      .size_bytes = 16ULL * kGiB,
      .logical_sector_size = 512U,
      .serial_suffix = "TGT00001",
      .device_instance_id = L"SYNTHETIC\\TARGET",
      .is_system_disk = false,
  };
}

std::vector<std::byte> payload() {
  return {
      std::byte{0x10},
      std::byte{0x20},
      std::byte{0x30},
      std::byte{0x40},
      std::byte{0x50},
  };
}

CanonicalFileSystemTree source_tree(
    const MigrationFileSystem file_system = MigrationFileSystem::fat32) {
  const auto bytes = payload();
  return CanonicalFileSystemTree{
      .file_system = file_system,
      .source_table_index = 4U,
      .enumeration_epoch_sha256 = digest(0x20U),
      .namespace_fully_enumerated = true,
      .opened_handles_only = true,
      .every_regular_file_hashed_to_stable_eof = true,
      .short_name_aliases_collision_free = true,
      .entries = {
          CanonicalFileSystemTreeEntry{
              .relative_path = L"Docs",
              .kind = ytec::migrationcore::
                  FileSystemRecreateEntryKind::directory,
              .portable_attributes =
                  ytec::migrationcore::recreate_attribute_hidden,
              .creation_time_utc_100ns = kCanonicalTime,
              .last_write_time_utc_100ns = kCanonicalTime,
              .hard_link_count = 1U,
              .opened_handle_identity_stable = true,
              .unnamed_stream_hashed_to_stable_eof = false,
              .namespace_supported = true,
          },
          CanonicalFileSystemTreeEntry{
              .relative_path = L"Docs\\payload.bin",
              .kind = ytec::migrationcore::
                  FileSystemRecreateEntryKind::regular_file,
              .size_bytes = bytes.size(),
              .portable_attributes =
                  ytec::migrationcore::recreate_attribute_archive,
              .creation_time_utc_100ns = kCanonicalTime,
              .last_write_time_utc_100ns = kCanonicalTime,
              .content_sha256 = content_digest(bytes),
              .hard_link_count = 1U,
              .opened_handle_identity_stable = true,
              .unnamed_stream_hashed_to_stable_eof = true,
              .namespace_supported = true,
          },
      },
  };
}

FileSystemRecreateFormatGeometry geometry(
    const MigrationFileSystem file_system = MigrationFileSystem::fat32) {
  return FileSystemRecreateFormatGeometry{
      .file_system = file_system,
      .target_volume_bytes = 4ULL * kGiB,
      .logical_sector_size = 512U,
      .cluster_size = 32ULL * 1024ULL,
  };
}

ytec::clonecore::Result<ytec::migrationcore::FileSystemRecreatePlan>
make_core_plan(const CanonicalFileSystemTree& tree) {
  return ytec::migrationcore::plan_file_system_recreation(
      ytec::migrationcore::FileSystemRecreatePlanningRequest{
          .target_partition_number = 3U,
          .target_partition_offset_bytes = 1ULL * kMiB,
          .target_geometry = geometry(tree.file_system),
          .source_tree = tree,
      });
}

ytec::clonecore::Error injected_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = operation,
      .message = L"synthetic injected failure",
  };
}

class FakeSourceFile final : public IFileSystemRecreateSourceFile {
 public:
  explicit FakeSourceFile(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t expected_size_bytes() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_next(
      const std::size_t maximum_bytes) override {
    if (maximum_bytes == 0U || finished_) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"fake source read state"));
    }
    if (offset_ == bytes_.size()) {
      eof_ = true;
      return ytec::clonecore::Result<std::vector<std::byte>>::success({});
    }
    const std::size_t length = (std::min)(
        maximum_bytes, bytes_.size() - offset_);
    std::vector<std::byte> chunk(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
    offset_ += length;
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(chunk));
  }

  [[nodiscard]] ytec::clonecore::Status finish_and_verify() override {
    if (!eof_ || offset_ != bytes_.size() || finished_) {
      return ytec::clonecore::Status::failure(
          injected_error(L"fake source finish"));
    }
    finished_ = true;
    return ytec::clonecore::success_status();
  }

 private:
  std::vector<std::byte> bytes_;
  std::size_t offset_{};
  bool eof_{};
  bool finished_{};
};

class FakeSourceSession final : public IFileSystemRecreateSourceSession {
 public:
  explicit FakeSourceSession(
      const MigrationFileSystem file_system = MigrationFileSystem::fat32,
      const std::uint32_t logical_sector_size = 512U)
      : disk_(make_source_disk_identity()), tree_(source_tree(file_system)) {
    disk_.logical_sector_size = logical_sector_size;
  }

  [[nodiscard]] const ytec::clonecore::StableDiskIdentity& source_disk()
      const noexcept override {
    return disk_;
  }
  [[nodiscard]] std::uint32_t source_table_index() const noexcept override {
    return tree_.source_table_index;
  }
  [[nodiscard]] std::uint64_t source_partition_offset_bytes()
      const noexcept override {
    return 2ULL * kMiB;
  }
  [[nodiscard]] std::uint64_t source_partition_length_bytes()
      const noexcept override {
    return 5ULL * kGiB;
  }
  [[nodiscard]] const CanonicalFileSystemTree& canonical_tree()
      const noexcept override {
    return tree_;
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateSourceEpochEvidence>
  revalidate_source_epoch() override {
    ++revalidation_calls;
    ++sequence_;
    auto epoch = tree_.enumeration_epoch_sha256;
    if (drift_on_revalidation.has_value() &&
        revalidation_calls >= drift_on_revalidation.value()) {
      epoch = digest(0xE0U);
    }
    return ytec::clonecore::Result<
        FileSystemRecreateSourceEpochEvidence>::success(
        FileSystemRecreateSourceEpochEvidence{
            .observed_source_disk = disk_,
            .enumeration_epoch_sha256 = epoch,
            .source_table_index = tree_.source_table_index,
            .source_partition_offset_bytes = 2ULL * kMiB,
            .source_partition_length_bytes = 5ULL * kGiB,
            .freshness_sequence = sequence_,
            .root_file_id_stable = true,
            .exact_single_extent = true,
            .source_token_reidentified = true,
        });
  }

  [[nodiscard]] ytec::clonecore::Result<
      std::unique_ptr<IFileSystemRecreateSourceFile>>
  open_regular_file(
      const std::size_t canonical_entry_index,
      const CanonicalFileSystemTreeEntry& expected_entry) override {
    if (canonical_entry_index != 1U ||
        expected_entry.relative_path != L"Docs\\payload.bin") {
      return ytec::clonecore::Result<
          std::unique_ptr<IFileSystemRecreateSourceFile>>::failure(
          injected_error(L"fake source entry"));
    }
    return ytec::clonecore::Result<
        std::unique_ptr<IFileSystemRecreateSourceFile>>::success(
        std::make_unique<FakeSourceFile>(payload()));
  }

  std::optional<std::uint64_t> drift_on_revalidation;
  std::uint64_t revalidation_calls{};

 private:
  ytec::clonecore::StableDiskIdentity disk_;
  CanonicalFileSystemTree tree_;
  std::uint64_t sequence_{};
};

enum class FailurePoint : std::uint8_t {
  none,
  begin,
  format,
  create_directory,
  create_file,
  write,
  finalize_file,
  directory_metadata,
  namespace_flush,
  readback,
  commit,
};

class FakeTargetPlatform final : public IFileSystemRecreateTargetPlatform {
 public:
  explicit FakeTargetPlatform(
      const MigrationFileSystem file_system = MigrationFileSystem::fat32)
      : disk_(make_target_disk_identity()),
        readback_tree_(source_tree(file_system)),
        file_system_(file_system) {
    readback_tree_.enumeration_epoch_sha256 = digest(0x90U);
    written.reserve(payload().size());
    events.reserve(64U);
  }

  [[nodiscard]] FileSystemRecreateTargetObservation observation() {
    ++sequence_;
    auto observed_disk = disk_;
    if (drift_on_reidentification.has_value() &&
        sequence_ >= drift_on_reidentification.value()) {
      observed_disk.serial_suffix = "DRIFTED";
    }
    return FileSystemRecreateTargetObservation{
        .observed_target_disk = observed_disk,
        .target_partition_number = 3U,
        .target_partition_offset_bytes = 1ULL * kMiB,
        .target_partition_length_bytes = 4ULL * kGiB,
        .freshness_sequence = sequence_,
        .freshly_reidentified = true,
        .reviewed_extent_within_disk = true,
        .exact_partition_extent = !offline,
        .isolation_state = offline
            ? FileSystemRecreateTargetIsolationState::physical_disk_offline
            : FileSystemRecreateTargetIsolationState::
                  construction_volume_online_exclusive,
        .exact_target_handle_retained = started,
        .root_file_id_stable = !offline,
        .root_is_non_reparse = !offline,
        .active_rescue_media = active_rescue_media,
    };
  }

  [[nodiscard]] FileSystemRecreateMutationEvidence receipt(
      const FileSystemRecreateMutationGuard& guard,
      const bool flushed) const {
    auto accepted = guard;
    if (corrupt_receipt_guard) {
      accepted.execution_plan_sha256[0] ^= std::byte{0x01};
    }
    return FileSystemRecreateMutationEvidence{
        .accepted_guard = accepted,
        .guard_revalidated_inside_adapter = true,
        .exact_target_handle_retained = true,
        .isolation_state = offline
            ? FileSystemRecreateTargetIsolationState::physical_disk_offline
            : FileSystemRecreateTargetIsolationState::
                  construction_volume_online_exclusive,
        .completion_incomplete = incomplete,
        .flushed = flushed,
    };
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateTargetObservation>
  reidentify_target_read_only() override {
    events.push_back("reidentify");
    return ytec::clonecore::Result<
        FileSystemRecreateTargetObservation>::success(observation());
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateMutationEvidence>
  begin_incomplete_target(
      const FileSystemRecreateMutationGuard& guard) override {
    events.push_back("begin");
    started = true;
    incomplete = true;
    if (failure == FailurePoint::begin) {
      return ytec::clonecore::Result<
          FileSystemRecreateMutationEvidence>::failure(
          injected_error(L"fake begin"));
    }
    return ytec::clonecore::Result<
        FileSystemRecreateMutationEvidence>::success(receipt(guard, false));
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateMutationEvidence>
  format_target_file_system(
      const FileSystemRecreateMutationGuard& guard,
      const FileSystemRecreateFormatGeometry&) override {
    events.push_back("format");
    formatted = true;
    if (failure == FailurePoint::format) {
      return ytec::clonecore::Result<
          FileSystemRecreateMutationEvidence>::failure(
          injected_error(L"fake format"));
    }
    offline = false;
    return ytec::clonecore::Result<
        FileSystemRecreateMutationEvidence>::success(receipt(guard, false));
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateFormattedTargetObservation>
  inspect_formatted_target_read_only() override {
    events.push_back("inspect_format");
    auto actual = geometry(file_system_);
    if (actual_geometry_mismatch) {
      actual.cluster_size *= 2U;
    }
    return ytec::clonecore::Result<
        FileSystemRecreateFormattedTargetObservation>::success(
        FileSystemRecreateFormattedTargetObservation{
            .target = observation(),
            .actual_geometry = actual,
            .root_reparse_tag = root_reparse ? 1U : 0U,
            .root_is_directory = true,
            .root_opened_handle_identity_stable = true,
        });
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateMutationEvidence>
  create_directory_no_replace(
      const FileSystemRecreateMutationGuard& guard,
      const CanonicalFileSystemTreeEntry&) override {
    events.push_back("create_directory");
    if (failure == FailurePoint::create_directory) {
      return ytec::clonecore::Result<
          FileSystemRecreateMutationEvidence>::failure(
          injected_error(L"fake create directory"));
    }
    return ytec::clonecore::Result<
        FileSystemRecreateMutationEvidence>::success(receipt(guard, false));
  }

  [[nodiscard]] ytec::clonecore::Result<FileSystemRecreateTargetFile>
  create_file_no_replace(
      const FileSystemRecreateMutationGuard& guard,
      const CanonicalFileSystemTreeEntry&) override {
    events.push_back("create_file");
    if (failure == FailurePoint::create_file) {
      return ytec::clonecore::Result<
          FileSystemRecreateTargetFile>::failure(
          injected_error(L"fake create file"));
    }
    return ytec::clonecore::Result<FileSystemRecreateTargetFile>::success(
        FileSystemRecreateTargetFile{
            .opened_handle_token = 1U,
            .mutation = receipt(guard, false),
        });
  }

  [[nodiscard]] ytec::clonecore::Result<FileSystemRecreateTargetWrite>
  write_file_chunk(
      const FileSystemRecreateMutationGuard& guard,
      const FileSystemRecreateTargetFile&,
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    events.push_back("write");
    if (failure == FailurePoint::write) {
      return ytec::clonecore::Result<
          FileSystemRecreateTargetWrite>::failure(
          injected_error(L"fake write"));
    }
    if (offset != written.size()) {
      return ytec::clonecore::Result<
          FileSystemRecreateTargetWrite>::failure(
          injected_error(L"fake write offset"));
    }
    written.insert(written.end(), bytes.begin(), bytes.end());
    const std::size_t reported = short_write && !bytes.empty()
        ? bytes.size() - 1U
        : bytes.size();
    return ytec::clonecore::Result<FileSystemRecreateTargetWrite>::success(
        FileSystemRecreateTargetWrite{
            .bytes_written = reported,
            .mutation = receipt(guard, false),
        });
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateMutationEvidence>
  finalize_file_metadata_flush_and_close(
      const FileSystemRecreateMutationGuard& guard,
      const FileSystemRecreateTargetFile&,
      const CanonicalFileSystemTreeEntry&) override {
    events.push_back("finalize_file");
    if (failure == FailurePoint::finalize_file) {
      return ytec::clonecore::Result<
          FileSystemRecreateMutationEvidence>::failure(
          injected_error(L"fake finalize file"));
    }
    return ytec::clonecore::Result<
        FileSystemRecreateMutationEvidence>::success(receipt(guard, true));
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateMutationEvidence>
  apply_directory_metadata_and_flush(
      const FileSystemRecreateMutationGuard& guard,
      const CanonicalFileSystemTreeEntry&) override {
    events.push_back("directory_metadata");
    if (failure == FailurePoint::directory_metadata) {
      return ytec::clonecore::Result<
          FileSystemRecreateMutationEvidence>::failure(
          injected_error(L"fake directory metadata"));
    }
    return ytec::clonecore::Result<
        FileSystemRecreateMutationEvidence>::success(receipt(guard, true));
  }

  [[nodiscard]] ytec::clonecore::Result<
      FileSystemRecreateMutationEvidence>
  flush_target_namespace(
      const FileSystemRecreateMutationGuard& guard) override {
    events.push_back("namespace_flush");
    if (failure == FailurePoint::namespace_flush) {
      return ytec::clonecore::Result<
          FileSystemRecreateMutationEvidence>::failure(
          injected_error(L"fake namespace flush"));
    }
    return ytec::clonecore::Result<
        FileSystemRecreateMutationEvidence>::success(receipt(guard, true));
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::migrationcore::FileSystemRecreateTargetReadback>
  enumerate_complete_target_readback_read_only(
      const ytec::migrationcore::FileSystemRecreatePlan& plan) override {
    events.push_back("readback");
    if (failure == FailurePoint::readback) {
      return ytec::clonecore::Result<
          ytec::migrationcore::FileSystemRecreateTargetReadback>::failure(
          injected_error(L"fake readback"));
    }
    auto tree = readback_tree_;
    if (readback_mismatch) {
      tree.entries[1].content_sha256[0] ^= std::byte{0x01};
    }
    return ytec::clonecore::Result<
        ytec::migrationcore::FileSystemRecreateTargetReadback>::success(
        ytec::migrationcore::FileSystemRecreateTargetReadback{
            .target_partition_number = plan.target_partition_number(),
            .target_partition_offset_bytes =
                plan.target_partition_offset_bytes(),
            .actual_geometry = geometry(file_system_),
            .target_tree = std::move(tree),
        });
  }

  [[nodiscard]] FileSystemRecreateCommitOutcome
  commit_completion_last(
      const FileSystemRecreateMutationGuard& guard,
      const ytec::migrationcore::FileSystemRecreateVerification& verification)
      noexcept override {
    events.push_back("commit");
    if (failure == FailurePoint::commit) {
      return FileSystemRecreateCommitOutcome{
          .disposition = FileSystemRecreateCommitDisposition::
              prepublication_failure,
          .failure_code = ytec::clonecore::ErrorCode::io_failed,
          .native_failure_code = ERROR_WRITE_FAULT,
      };
    }
    offline = true;
    publication_attempted = true;
    const bool partial = partial_publication || latched_readback_failure;
    publication_latched = !partial_publication;
    committed = publication_latched;
    incomplete = partial || invalid_commit_evidence;
    auto evidence = FileSystemRecreateCompletionEvidence{
        .accepted_guard = guard,
        .verified_manifest_sha256 = verification.observed_manifest_sha256,
        .target_epoch_sha256 = verification.target_epoch_sha256,
        .guard_revalidated_inside_adapter = true,
        .exact_target_reidentified = true,
        .complete_readback_verified = true,
        .completion_committed_last = publication_latched,
        .target_offline = offline,
        .publication_attempted = true,
        .publication_latched = publication_latched,
        .publication_readback_verified = !partial,
        .cleanup_pending = cleanup_pending,
        .incomplete_use_prohibited = partial || invalid_commit_evidence,
    };
    if (invalid_commit_evidence) {
      evidence.publication_readback_verified = false;
    }
    return FileSystemRecreateCommitOutcome{
        .disposition = partial
            ? FileSystemRecreateCommitDisposition::
                  partial_publication_use_prohibited
            : FileSystemRecreateCommitDisposition::completed,
        .evidence = evidence,
        .failure_code = ytec::clonecore::ErrorCode::verification_failed,
        .native_failure_code = static_cast<std::uint32_t>(
            partial ? ERROR_CRC : ERROR_SUCCESS),
    };
  }

  [[nodiscard]] FileSystemRecreateAbortEvidence
  abort_keep_offline_incomplete(
      const FileSystemRecreateMutationGuard& guard) noexcept override {
    abort_called = true;
    if (publication_attempted || publication_latched) {
      abort_after_publication = true;
      return FileSystemRecreateAbortEvidence{
          .accepted_guard = guard,
          .guard_revalidated_inside_adapter = true,
          .exact_target_handle_retained = started,
          .target_offline = offline,
          .completion_incomplete = incomplete,
      };
    }
    offline = true;
    incomplete = true;
    committed = false;
    return FileSystemRecreateAbortEvidence{
        .accepted_guard = guard,
        .guard_revalidated_inside_adapter = true,
        .exact_target_handle_retained = started,
        .target_offline = true,
        .completion_incomplete = true,
    };
  }

  FailurePoint failure{FailurePoint::none};
  std::optional<std::uint64_t> drift_on_reidentification;
  bool active_rescue_media{};
  bool actual_geometry_mismatch{};
  bool root_reparse{};
  bool short_write{};
  bool readback_mismatch{};
  bool invalid_commit_evidence{};
  bool partial_publication{};
  bool latched_readback_failure{};
  bool cleanup_pending{};
  bool corrupt_receipt_guard{};
  bool started{};
  bool formatted{};
  bool abort_called{};
  bool abort_after_publication{};
  bool publication_attempted{};
  bool publication_latched{};
  bool committed{};
  bool offline{true};
  bool incomplete{};
  std::vector<std::byte> written;
  std::vector<std::string> events;

 private:
  ytec::clonecore::StableDiskIdentity disk_;
  CanonicalFileSystemTree readback_tree_;
  MigrationFileSystem file_system_;
  std::uint64_t sequence_{};
};

struct ProductionIoState final {
  bool offline{true};
  bool root_open{};
  bool inject_final_publication_failure{};
  bool fail_next_target_write{};
  bool publication_write_failed{};
  bool fail_observe_after_publication_failure{};
  std::uint32_t observe_calls{};
  std::uint32_t notify_calls{};
  std::uint32_t abort_cleanup_calls{};
  std::uint64_t next_token{1U};
  std::vector<std::byte> file_bytes;
  std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> disk_writes;
  std::vector<std::string> events;
};

class SparseProductionTargetWriter final
    : public ytec::clonecore::ITargetDiskWriter {
 public:
  explicit SparseProductionTargetWriter(
      std::shared_ptr<ProductionIoState> state)
      : state_(std::move(state)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return make_target_disk_identity().size_bytes;
  }
  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return 512U;
  }
  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    state_->events.push_back("disk:write");
    if (offset > size_bytes() || bytes.size() > size_bytes() - offset) {
      return ytec::clonecore::Status::failure(
          injected_error(L"synthetic disk range"));
    }
    if (state_->fail_next_target_write) {
      state_->fail_next_target_write = false;
      state_->publication_write_failed = true;
      state_->events.push_back("disk:injected-final-failure");
      return ytec::clonecore::Status::failure(
          injected_error(L"synthetic final publication"));
    }
    state_->disk_writes.emplace_back(
        offset, std::vector<std::byte>(bytes.begin(), bytes.end()));
    return ytec::clonecore::success_status();
  }
  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    state_->events.push_back("disk:read");
    if (offset > size_bytes() || length > size_bytes() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"synthetic disk read range"));
    }
    std::vector<std::byte> result(length, std::byte{0xCC});
    const std::uint64_t end = offset + length;
    for (const auto& [write_offset, bytes] : state_->disk_writes) {
      const std::uint64_t write_end = write_offset + bytes.size();
      const std::uint64_t overlap_begin = (std::max)(offset, write_offset);
      const std::uint64_t overlap_end = (std::min)(end, write_end);
      if (overlap_begin >= overlap_end) {
        continue;
      }
      std::copy(
          bytes.begin() + static_cast<std::ptrdiff_t>(
              overlap_begin - write_offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(
              overlap_end - write_offset),
          result.begin() + static_cast<std::ptrdiff_t>(
              overlap_begin - offset));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }
  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    state_->events.push_back("disk:flush");
    return ytec::clonecore::success_status();
  }

 private:
  std::shared_ptr<ProductionIoState> state_;
};

ytec::diskmodel::ReidentifiedPhysicalTarget production_physical(
    const bool offline) {
  const auto identity = make_target_disk_identity();
  return ytec::diskmodel::ReidentifiedPhysicalTarget{
      .target = ytec::diskmodel::DiskInfo{
          .disk_number = identity.disk_number,
          .device_path = L"synthetic-target",
          .device_instance_id = identity.device_instance_id,
          .model = identity.model,
          .size_bytes = identity.size_bytes,
          .sector_count = identity.size_bytes / identity.logical_sector_size,
          .logical_sector_size = identity.logical_sector_size,
          .physical_sector_size = 4096U,
          .serial_suffix = identity.serial_suffix,
          .offline = offline,
          .read_only = false,
          .removable = false,
          .is_system_disk = false,
      },
      .target_identity = identity,
  };
}

class FakeProductionTargetIo final
    : public IWindowsFileSystemRecreateTargetIo {
 public:
  explicit FakeProductionTargetIo(
      std::shared_ptr<ProductionIoState> state,
      const MigrationFileSystem file_system = MigrationFileSystem::fat32)
      : state_(std::move(state)), file_system_(file_system) {}

  [[nodiscard]] ytec::clonecore::Result<
      WindowsFileSystemRecreateTargetDiskObservation>
  observe_target_read_only() override {
    ++state_->observe_calls;
    state_->events.push_back("io:observe");
    if (state_->publication_write_failed &&
        state_->fail_observe_after_publication_failure) {
      return ytec::clonecore::Result<
          WindowsFileSystemRecreateTargetDiskObservation>::failure(
          injected_error(L"synthetic post-publication offline proof"));
    }
    return ytec::clonecore::Result<
        WindowsFileSystemRecreateTargetDiskObservation>::success({
        .physical = production_physical(state_->offline),
        .current_layout_sha256 = digest(0x71U),
        .target_class_accepted = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Status set_target_offline(
      const bool offline) override {
    state_->events.push_back(offline ? "io:set-offline" : "io:set-online");
    state_->offline = offline;
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::diskmodel::PhysicalTargetHandle>
  open_offline_target() override {
    state_->events.push_back("io:open-target");
    if (!state_->offline) {
      return ytec::clonecore::Result<
          ytec::diskmodel::PhysicalTargetHandle>::failure(
          injected_error(L"synthetic target not offline"));
    }
    return ytec::clonecore::Result<
        ytec::diskmodel::PhysicalTargetHandle>::success({
        .observed = production_physical(true),
        .target = std::make_unique<SparseProductionTargetWriter>(state_),
    });
  }

  [[nodiscard]] ytec::clonecore::Status notify_layout_changed() override {
    ++state_->notify_calls;
    state_->events.push_back("io:notify-layout");
    if (state_->inject_final_publication_failure &&
        state_->notify_calls == 2U) {
      state_->fail_next_target_write = true;
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      WindowsFileSystemRecreateConstructionVolumeBinding>
  bind_online_construction_volume(
      const std::uint32_t final_target_number,
      const std::uint64_t target_offset,
      const std::uint64_t target_size) override {
    state_->events.push_back("io:bind-volume");
    if (state_->offline) {
      return ytec::clonecore::Result<
          WindowsFileSystemRecreateConstructionVolumeBinding>::failure(
          injected_error(L"synthetic volume still offline"));
    }
    return ytec::clonecore::Result<
        WindowsFileSystemRecreateConstructionVolumeBinding>::success({
        .final_target_number = final_target_number,
        .disk_number = make_target_disk_identity().disk_number,
        .target_offset = target_offset,
        .target_size = target_size,
        .canonical_volume_guid_path =
            L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\",
        .exact_single_disk_extent = true,
    });
  }

  [[nodiscard]] ytec::clonecore::Status format_exact_volume(
      const WindowsFileSystemRecreateConstructionVolumeBinding&,
      const FileSystemRecreateFormatGeometry&) override {
    state_->events.push_back("io:format");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      WindowsFileSystemRecreateFormattedRootObservation>
  open_and_inspect_formatted_root(
      const WindowsFileSystemRecreateConstructionVolumeBinding& volume,
      const FileSystemRecreateFormatGeometry& desired) override {
    state_->events.push_back("io:open-root");
    state_->root_open = true;
    binding_ = volume;
    geometry_ = desired;
    return root_observation();
  }

  [[nodiscard]] ytec::clonecore::Result<
      WindowsFileSystemRecreateFormattedRootObservation>
  revalidate_formatted_root_read_only() override {
    state_->events.push_back("io:revalidate-root");
    if (!state_->root_open || !binding_.has_value()) {
      return ytec::clonecore::Result<
          WindowsFileSystemRecreateFormattedRootObservation>::failure(
          injected_error(L"synthetic root is not retained"));
    }
    return root_observation();
  }

  [[nodiscard]] ytec::clonecore::Status create_directory_no_replace(
      const CanonicalFileSystemTreeEntry&) override {
    state_->events.push_back("io:create-directory");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::uint64_t>
  create_file_no_replace(
      const CanonicalFileSystemTreeEntry&) override {
    state_->events.push_back("io:create-file");
    return ytec::clonecore::Result<std::uint64_t>::success(
        state_->next_token++);
  }

  [[nodiscard]] ytec::clonecore::Result<std::size_t> write_file_chunk(
      const std::uint64_t,
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    state_->events.push_back("io:write-file");
    if (offset != state_->file_bytes.size()) {
      return ytec::clonecore::Result<std::size_t>::failure(
          injected_error(L"synthetic file offset"));
    }
    state_->file_bytes.insert(
        state_->file_bytes.end(), bytes.begin(), bytes.end());
    return ytec::clonecore::Result<std::size_t>::success(bytes.size());
  }

  [[nodiscard]] ytec::clonecore::Status
  finalize_file_metadata_flush_and_hold(
      const std::uint64_t,
      const CanonicalFileSystemTreeEntry&) override {
    state_->events.push_back("io:finalize-file-hold");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status
  apply_directory_metadata_and_flush(
      const CanonicalFileSystemTreeEntry&) override {
    state_->events.push_back("io:directory-metadata");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Status flush_target_namespace() override {
    state_->events.push_back("io:flush-namespace");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<
      ytec::migrationcore::FileSystemRecreateTargetReadback>
  enumerate_complete_target_readback_read_only(
      const ytec::migrationcore::FileSystemRecreatePlan& plan) override {
    state_->events.push_back("io:full-opened-handle-readback");
    auto tree = source_tree(file_system_);
    tree.enumeration_epoch_sha256 = digest(0x90U);
    return ytec::clonecore::Result<
        ytec::migrationcore::FileSystemRecreateTargetReadback>::success({
        .target_partition_number = plan.target_partition_number(),
        .target_partition_offset_bytes =
            plan.target_partition_offset_bytes(),
        .actual_geometry = geometry(file_system_),
        .target_tree = std::move(tree),
    });
  }

  [[nodiscard]] ytec::clonecore::Status
  close_namespace_dismount_and_offline(
      const WindowsFileSystemRecreateConstructionVolumeBinding&) override {
    ++state_->abort_cleanup_calls;
    state_->events.push_back("io:close-dismount-offline");
    state_->root_open = false;
    state_->offline = true;
    return ytec::clonecore::success_status();
  }

 private:
  [[nodiscard]] ytec::clonecore::Result<
      WindowsFileSystemRecreateFormattedRootObservation>
  root_observation() const {
    return ytec::clonecore::Result<
        WindowsFileSystemRecreateFormattedRootObservation>::success({
        .volume = binding_.value(),
        .actual_geometry = geometry_.value(),
        .root_reparse_tag = 0U,
        .root_is_directory = true,
        .root_opened_handle_identity_stable = true,
    });
  }

  std::shared_ptr<ProductionIoState> state_;
  MigrationFileSystem file_system_;
  std::optional<WindowsFileSystemRecreateConstructionVolumeBinding> binding_;
  std::optional<FileSystemRecreateFormatGeometry> geometry_;
};

class ProductionGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  [[nodiscard]] ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    ytec::clonecore::GptGuid value;
    value.bytes.fill(std::byte{seed_++});
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(value);
  }

 private:
  std::uint8_t seed_{0x31U};
};

WindowsFileSystemRecreateProductionTargetRequest production_request(
    const MigrationFileSystem file_system = MigrationFileSystem::fat32) {
  using namespace ytec;
  const auto target = make_target_disk_identity();
  const auto fs_action = migrationcore::MigrationPartitionAction::
      apply_file_image;
  imageformat::TsumugiWholeDiskRestoreLayoutPlan metadata{
      .style = imageformat::PartitionTableStyle::mbr,
      .target_size_bytes = target.size_bytes,
      .logical_sector_size = target.logical_sector_size,
      .invalidation_ranges = {
          {.offset = 0U, .length = kMiB},
          {.offset = target.size_bytes - kMiB, .length = kMiB},
      },
      .commit_writes = {
          {
              .kind = imageformat::TsumugiRestoreLayoutWriteKind::mbr_sector,
              .offset = 0U,
              .bytes = std::vector<std::byte>(512U, std::byte{0x66}),
          },
      },
      .target_layout = clonecore::MbrDisk{
          .logical_sector_size = target.logical_sector_size,
          .sector_count = target.size_bytes / target.logical_sector_size,
          .partitions = {
              clonecore::MbrPartition{
                  .table_index = 0U,
                  .type = file_system == MigrationFileSystem::fat32
                      ? static_cast<std::uint8_t>(0x0CU)
                      : static_cast<std::uint8_t>(0x07U),
                  .first_lba = static_cast<std::uint32_t>(
                      kMiB / target.logical_sector_size),
                  .sector_count = static_cast<std::uint32_t>(
                      (4ULL * kGiB) / target.logical_sector_size),
              },
          },
      },
  };
  imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 final{
      .migration = {
          .target_style = migrationcore::MigrationPartitionStyle::mbr,
          .alignment_bytes = kMiB,
          .minimum_target_size_bytes = 4ULL * kGiB + 2ULL * kMiB,
          .target_size_bytes = target.size_bytes,
          .unallocated_tail_bytes =
              target.size_bytes - kMiB - 4ULL * kGiB,
          .source_remains_unchanged = true,
          .target_partitions = {
              {
                  .target_number = 1U,
                  .source_table_index = 4U,
                  .role = migrationcore::MigrationPartitionRole::data,
                  .file_system = file_system,
                  .action = fs_action,
                  .offset_bytes = kMiB,
                  .size_bytes = 4ULL * kGiB,
              },
          },
      },
      .metadata = std::move(metadata),
  };
  ProductionGuidGenerator guids;
  auto constructions = imageformat::
      make_tsumugi_shrink_construction_layout_plans_v1(final, guids);
  check(constructions.has_value() && constructions.value().size() == 1U,
        "Synthetic production construction plan should build");
  return WindowsFileSystemRecreateProductionTargetRequest{
      .confirmation = clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = clonecore::make_target_confirmation_token(target),
      },
      .expected_original_target_layout_sha256 = digest(0x71U),
      .reviewed_layout = std::move(final),
      .reviewed_construction_layouts = constructions.take_value(),
      .target_is_active_rescue_media = false,
  };
}

ytec::clonecore::Result<WindowsFileSystemRecreateExecutionPlan>
make_production_execution_plan(FakeSourceSession& source) {
  const auto core = ytec::migrationcore::plan_file_system_recreation(
      ytec::migrationcore::FileSystemRecreatePlanningRequest{
          .target_partition_number = 1U,
          .target_partition_offset_bytes = kMiB,
          .target_geometry = geometry(source.canonical_tree().file_system),
          .source_tree = source.canonical_tree(),
      });
  if (!core) {
    return ytec::clonecore::Result<
        WindowsFileSystemRecreateExecutionPlan>::failure(core.error());
  }
  auto target = FileSystemRecreateTargetSelection{
      .expected_target_disk = make_target_disk_identity(),
      .target_partition_number = 1U,
      .target_partition_offset_bytes = kMiB,
      .target_partition_length_bytes = 4ULL * kGiB,
      .reviewed_as_active_rescue_media = false,
  };
  return bind_windows_file_system_recreate_execution_plan(
      core.value(), source, target);
}

FileSystemRecreateTargetSelection selection() {
  return FileSystemRecreateTargetSelection{
      .expected_target_disk = make_target_disk_identity(),
      .target_partition_number = 3U,
      .target_partition_offset_bytes = 1ULL * kMiB,
      .target_partition_length_bytes = 4ULL * kGiB,
      .reviewed_as_active_rescue_media = false,
  };
}

ytec::clonecore::Result<WindowsFileSystemRecreateExecutionPlan>
make_execution_plan(FakeSourceSession& source) {
  const auto core = make_core_plan(source.canonical_tree());
  if (!core) {
    return ytec::clonecore::Result<
        WindowsFileSystemRecreateExecutionPlan>::failure(core.error());
  }
  return bind_windows_file_system_recreate_execution_plan(
      core.value(), source, selection());
}

void require_aborted_failure(
    const ytec::clonecore::Result<FileSystemRecreateTransactionOutcome>& result,
    const FakeTargetPlatform& platform,
    const std::string& context) {
  check(result.has_value(),
        context + " should preserve a typed post-I/O outcome");
  check(result.value().disposition ==
            FileSystemRecreateTransactionDisposition::aborted_incomplete,
        context + " should classify aborted/incomplete");
  check(!result.value().completed_report.has_value() &&
            result.value().abort.has_value() &&
            result.value().error.has_value(),
        context + " should retain abort evidence and structured error");
  check(result.value().abort->guard_revalidated_inside_adapter &&
            result.value().abort->exact_target_handle_retained &&
            result.value().abort->target_offline &&
            result.value().abort->completion_incomplete,
        context + " should retain guarded exact-target abort evidence");
  check(platform.abort_called, context + " should call abort");
  check(platform.offline, context + " should leave target offline");
  check(platform.incomplete, context + " should leave target incomplete");
  check(!platform.committed, context + " should not remain committed");
  check(!platform.abort_after_publication,
        context + " must not abort after publication");
}

void require_post_publication_failure_without_abort(
    const ytec::clonecore::Result<FileSystemRecreateTransactionOutcome>& result,
    const FakeTargetPlatform& platform,
    const bool expected_latched,
    const std::string& context) {
  check(result.has_value(),
        context + " should preserve a typed post-publication outcome");
  check(result.value().disposition == FileSystemRecreateTransactionDisposition::
            partial_publication_use_prohibited,
        context + " should classify partial/use-prohibited");
  check(!result.value().completed_report.has_value() &&
            result.value().completion.has_value() &&
            result.value().error.has_value(),
        context + " should retain completion evidence and structured error");
  check(result.value().completion->publication_attempted &&
            result.value().completion->publication_latched ==
                expected_latched &&
            result.value().completion->incomplete_use_prohibited,
        context + " should expose typed publication safety evidence");
  check(platform.publication_attempted,
        context + " should record attempted publication");
  check(platform.publication_latched == expected_latched,
        context + " should report the expected latch state");
  check(!platform.abort_called && !platform.abort_after_publication,
        context + " must never call abort after publication starts");
  check(platform.offline && platform.incomplete,
        context + " should remain offline and use-prohibited");
}

const FileSystemRecreateExecutionReport& require_completed(
    const ytec::clonecore::Result<FileSystemRecreateTransactionOutcome>& result,
    const std::string& context) {
  check(result.has_value(), context + " should return an outcome");
  check(result.value().disposition ==
            FileSystemRecreateTransactionDisposition::completed,
        context + " requires an explicit completed disposition");
  check(result.value().completed_report.has_value() &&
            result.value().completion.has_value() &&
            !result.value().abort.has_value() &&
            !result.value().error.has_value(),
        context + " should carry only completed evidence/report");
  return result.value().completed_report.value();
}

void test_success_uses_guarded_target_only_commit_last() {
  FakeSourceSession source;
  const auto execution = make_execution_plan(source);
  check(execution.has_value(), "Synthetic execution plan should bind");
  FakeTargetPlatform target;
  const auto result = execute_file_system_recreation_on_injected_target(
      execution.value(),
      source,
      target,
      FileSystemRecreateExecutionOptions{
          .maximum_transfer_bytes = 2U,
      });
  const auto& report = require_completed(
      result, "Guarded FAT32 recreate");
  check(
      report.directory_count == 1U && report.regular_file_count == 1U &&
          report.copied_file_bytes == payload().size(),
      "Success report should count the exact tree and bytes");
  check(
      report.verification.exact_tree_and_content_equivalence &&
          report.every_mutation_guard_revalidated &&
          report.every_file_flushed && report.full_namespace_read_back &&
          report.commit_was_last_mutation && report.target_left_offline &&
          report.publication_latched && !report.cleanup_pending &&
          !report.incomplete_use_prohibited,
      "Success requires exact readback, flush, guard, commit-last and offline evidence");
  check(
      !report.production_target_adapter_connected,
      "The execution report must keep production target mutation disconnected");
  check(
      !kWindowsFileSystemRecreateProductionTargetAdapterConnected,
      "This slice must explicitly keep production target mutation disconnected");
  check(target.committed && target.publication_attempted &&
            target.publication_latched && !target.incomplete &&
            !target.abort_called && !target.abort_after_publication,
        "Success should commit without abort");
  check(target.written == payload(),
        "Injected target should receive the exact source stream");
  check(!target.events.empty() && target.events.back() == "commit",
        "Commit must be the final target platform call");
  check(
      report.target_reidentification_count >= 10U &&
          source.revalidation_calls + 1U ==
              report.target_reidentification_count,
      "Every mutation/readback boundary should reidentify both sides; post-format root inspection adds one target-only readback");
}

void test_exfat_uses_the_same_target_only_transaction_contract() {
  FakeSourceSession source(MigrationFileSystem::exfat);
  const auto execution = make_execution_plan(source);
  check(execution.has_value(), "Synthetic exFAT execution plan should bind");
  FakeTargetPlatform target(MigrationFileSystem::exfat);
  const auto result = execute_file_system_recreation_on_injected_target(
      execution.value(), source, target);
  const auto& report = require_completed(
      result, "Guarded exFAT recreate");
  check(report.verification.exact_tree_and_content_equivalence &&
            report.copied_file_bytes == payload().size() &&
            target.written == payload(),
        "exFAT should use the exact same guarded copy/readback contract");
}

void test_4kn_source_to_512_target_uses_file_level_recreation() {
  FakeSourceSession source(MigrationFileSystem::fat32, 4096U);
  const auto execution = make_execution_plan(source);
  check(execution.has_value(),
        "A 4Kn source should bind to the separate 512-byte target geometry");
  check(execution.value().source_disk().logical_sector_size == 4096U &&
            execution.value().target().expected_target_disk
                    .logical_sector_size == 512U,
        "The execution plan must preserve distinct source and target logical sectors");
  FakeTargetPlatform target;
  const auto result = execute_file_system_recreation_on_injected_target(
      execution.value(), source, target);
  const auto& report = require_completed(
      result, "4Kn source to 512-byte file-level recreation");
  check(report.verification.exact_tree_and_content_equivalence &&
            target.written == payload(),
        "Cross-sector migration must copy and fully verify files, never raw sectors");
}

void test_execution_binding_rejects_same_disk_and_active_rescue_review() {
  FakeSourceSession source;
  const auto core = make_core_plan(source.canonical_tree());
  check(core.has_value(), "Synthetic core plan should succeed");

  auto target = selection();
  target.expected_target_disk = source.source_disk();
  target.expected_target_disk.is_system_disk = false;
  const auto same = bind_windows_file_system_recreate_execution_plan(
      core.value(), source, target);
  check(!same.has_value(), "Source and target on the same disk must fail");

  target = selection();
  target.reviewed_as_active_rescue_media = true;
  const auto rescue = bind_windows_file_system_recreate_execution_plan(
      core.value(), source, target);
  check(!rescue.has_value(), "Reviewed active rescue media must fail binding");
}

void test_fresh_target_active_rescue_stops_before_mutation() {
  FakeSourceSession source;
  const auto execution = make_execution_plan(source);
  check(execution.has_value(), "Synthetic execution plan should bind");
  FakeTargetPlatform target;
  target.active_rescue_media = true;
  const auto result = execute_file_system_recreation_on_injected_target(
      execution.value(), source, target);
  check(!result.has_value(), "Fresh active rescue observation must fail");
  check(!target.started && !target.abort_called,
        "Active rescue media should be rejected before the first mutation");
}

void test_source_or_target_drift_aborts_incomplete() {
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    source.drift_on_revalidation = 2U;
    FakeTargetPlatform target;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_aborted_failure(result, target, "Source epoch drift");
  }
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.drift_on_reidentification = 2U;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_aborted_failure(result, target, "Target stable identity drift");
  }
}

void test_short_io_cancel_and_guard_mismatch_abort() {
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.short_write = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_aborted_failure(result, target, "Short target write");
  }
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(),
        source,
        target,
        FileSystemRecreateExecutionOptions{
            .callbacks = ytec::clonecore::DiskOperationCallbacks{
                .cancellation_requested = [&target]() {
                  return target.started;
                },
            },
        });
    require_aborted_failure(result, target, "Cancellation after begin");
    check(result.value().error->code ==
              ytec::clonecore::ErrorCode::cancelled,
          "Cancellation should retain the cancelled classification");
  }
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.corrupt_receipt_guard = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_aborted_failure(result, target, "Adapter guard receipt mismatch");
  }
}

void test_format_root_readback_and_commit_evidence_fail_closed() {
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.actual_geometry_mismatch = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_aborted_failure(result, target, "Actual format geometry mismatch");
  }
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.root_reparse = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_aborted_failure(result, target, "Formatted root reparse");
  }
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.readback_mismatch = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_aborted_failure(result, target, "Full namespace readback mismatch");
  }
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.invalid_commit_evidence = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_post_publication_failure_without_abort(
        result, target, true, "Invalid post-latch commit evidence");
  }
}

void test_commit_publication_boundary_never_rolls_back_post_latch() {
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.partial_publication = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_post_publication_failure_without_abort(
        result, target, false, "Partial completion publication");
  }
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.latched_readback_failure = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_post_publication_failure_without_abort(
        result, target, true, "Latched publication readback failure");
  }
  {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.cleanup_pending = true;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    const auto& report = require_completed(
        result, "Non-mutating cleanup pending after a verified latch");
    check(report.publication_latched && report.cleanup_pending &&
              !report.incomplete_use_prohibited,
          "Cleanup-pending success should remain latched, offline and usable");
    check(!target.abort_called && !target.abort_after_publication,
          "Cleanup-pending success must not call abort");
  }
}

void test_partial_failure_injection_matrix_aborts_offline_incomplete() {
  const std::vector<FailurePoint> points{
      FailurePoint::begin,
      FailurePoint::format,
      FailurePoint::create_directory,
      FailurePoint::create_file,
      FailurePoint::write,
      FailurePoint::finalize_file,
      FailurePoint::directory_metadata,
      FailurePoint::namespace_flush,
      FailurePoint::readback,
      FailurePoint::commit,
  };
  for (const auto point : points) {
    FakeSourceSession source;
    const auto execution = make_execution_plan(source);
    check(execution.has_value(), "Synthetic execution plan should bind");
    FakeTargetPlatform target;
    target.failure = point;
    const auto result = execute_file_system_recreation_on_injected_target(
        execution.value(), source, target);
    require_aborted_failure(
        result,
        target,
        "Injected partial mutation/readback failure");
  }
}

void test_production_state_machine_commits_only_after_offline_retirement() {
  FakeSourceSession source;
  const auto execution = make_production_execution_plan(source);
  check(execution.has_value(), "Synthetic execution plan should bind");
  auto state = std::make_shared<ProductionIoState>();
  const auto request = production_request();
  auto target = make_windows_file_system_recreate_target_platform_with_io(
      execution.value(),
      request,
      std::make_unique<FakeProductionTargetIo>(state));
  check(target.has_value(),
        "Reviewed FAT32 production adapter should be constructible");
  check(state->observe_calls == 0U && state->events.empty(),
        "Factory construction must perform no target I/O");
  auto* lifecycle = target.value().get();
  const auto result = execute_file_system_recreation_on_injected_target(
      execution.value(), source, *target.value());
  const auto& report = require_completed(
      result, "Production typed lifecycle FAT32 recreate");
  check(lifecycle->lifecycle_state() ==
            WindowsFileSystemRecreateTargetLifecycleState::completed_offline,
        "Successful production lifecycle should finish completed/offline");
  check(report.full_namespace_read_back && report.commit_was_last_mutation &&
            report.target_left_offline && state->offline &&
            state->file_bytes == payload(),
        "Production lifecycle must copy, fully read back and finish offline");
  const auto close = std::find(
      state->events.begin(),
      state->events.end(),
      "io:close-dismount-offline");
  check(close != state->events.end(),
        "Opened namespace handles must be released before final publication");
  const auto first_notify = std::find(
      state->events.begin(), state->events.end(), "io:notify-layout");
  check(first_notify != state->events.end(),
        "Construction layout notification must be recorded");
  const auto second_notify = first_notify == state->events.end()
      ? state->events.end()
      : std::find(
            std::next(first_notify), state->events.end(), "io:notify-layout");
  const auto final_write = std::find(
      second_notify, state->events.end(), "disk:write");
  check(second_notify != state->events.end() &&
            final_write != state->events.end() && close < second_notify &&
            second_notify < final_write,
        "Final metadata write must follow dismount, offline proof and construction retirement");
}

void test_production_partial_publication_never_rolls_back() {
  FakeSourceSession source;
  const auto execution = make_production_execution_plan(source);
  check(execution.has_value(), "Synthetic execution plan should bind");
  auto state = std::make_shared<ProductionIoState>();
  state->inject_final_publication_failure = true;
  const auto request = production_request();
  auto target = make_windows_file_system_recreate_target_platform_with_io(
      execution.value(),
      request,
      std::make_unique<FakeProductionTargetIo>(state));
  check(target.has_value(), "Synthetic production adapter should construct");
  auto* lifecycle = target.value().get();
  const auto result = execute_file_system_recreation_on_injected_target(
      execution.value(), source, *target.value());
  check(result.has_value() &&
            result.value().disposition ==
                FileSystemRecreateTransactionDisposition::
                    partial_publication_use_prohibited &&
            result.value().completion.has_value() &&
            result.value().completion->publication_attempted &&
            result.value().completion->target_offline &&
            result.value().completion->incomplete_use_prohibited,
        "Final publication failure must be typed partial/use-prohibited");
  check(lifecycle->lifecycle_state() ==
            WindowsFileSystemRecreateTargetLifecycleState::
                partial_publication_use_prohibited &&
            state->offline,
        "Partial publication must remain offline and use-prohibited");
  const auto failure = std::find(
      state->events.begin(),
      state->events.end(),
      "disk:injected-final-failure");
  check(failure != state->events.end() &&
            std::find(
                std::next(failure), state->events.end(), "disk:write") ==
                state->events.end(),
        "No target write may occur after publication failure");
}

void test_production_exfat_uses_same_typed_lifecycle() {
  FakeSourceSession source(MigrationFileSystem::exfat);
  const auto execution = make_production_execution_plan(source);
  check(execution.has_value(), "Synthetic exFAT execution plan should bind");
  auto state = std::make_shared<ProductionIoState>();
  const auto request = production_request(MigrationFileSystem::exfat);
  auto target = make_windows_file_system_recreate_target_platform_with_io(
      execution.value(),
      request,
      std::make_unique<FakeProductionTargetIo>(
          state, MigrationFileSystem::exfat));
  check(target.has_value(),
        "Reviewed exFAT production adapter should be constructible");
  const auto result = execute_file_system_recreation_on_injected_target(
      execution.value(), source, *target.value());
  const auto& report = require_completed(
      result, "Production typed lifecycle exFAT recreate");
  check(report.full_namespace_read_back && report.commit_was_last_mutation &&
            report.target_left_offline && state->offline &&
            state->file_bytes == payload(),
        "exFAT production lifecycle must fully read back and commit offline");
}

void test_partial_publication_does_not_borrow_stale_offline_proof() {
  FakeSourceSession source;
  const auto execution = make_production_execution_plan(source);
  check(execution.has_value(), "Synthetic execution plan should bind");
  auto state = std::make_shared<ProductionIoState>();
  state->inject_final_publication_failure = true;
  state->fail_observe_after_publication_failure = true;
  auto target = make_windows_file_system_recreate_target_platform_with_io(
      execution.value(),
      production_request(),
      std::make_unique<FakeProductionTargetIo>(state));
  check(target.has_value(), "Synthetic production adapter should construct");
  const auto result = execute_file_system_recreation_on_injected_target(
      execution.value(), source, *target.value());
  check(result.has_value() && result.value().completion.has_value() &&
            result.value().disposition ==
                FileSystemRecreateTransactionDisposition::
                    partial_publication_use_prohibited &&
            !result.value().completion->target_offline &&
            result.value().completion->incomplete_use_prohibited,
        "A failed fresh post-publication observation must not reuse the earlier offline proof");
}

void test_production_factory_rejects_unreviewed_layout_before_io() {
  FakeSourceSession source;
  const auto execution = make_production_execution_plan(source);
  check(execution.has_value(), "Synthetic execution plan should bind");
  auto state = std::make_shared<ProductionIoState>();
  auto request = production_request();
  request.reviewed_layout.metadata.logical_sector_size = 4096U;
  const auto target = make_windows_file_system_recreate_target_platform_with_io(
      execution.value(),
      request,
      std::make_unique<FakeProductionTargetIo>(state));
  check(!target.has_value() && state->observe_calls == 0U &&
            state->events.empty(),
        "Mismatched/unsupported reviewed layout must stop before target I/O");
}

void test_source_factory_rejects_nonstandard_logical_sector_before_io() {
  auto source = make_source_disk_identity();
  source.logical_sector_size = 2048U;
  const auto epoch = digest(0x73U);
  const WindowsFileSystemRecreateSourceRequest request{
      .expected_source_disk = source,
      .source_table_index = 4U,
      .source_partition_offset_bytes = 1ULL * kMiB,
      .source_partition_length_bytes = 4ULL * kGiB,
      .expected_file_system = MigrationFileSystem::fat32,
      .source_root_path =
          L"\\\\?\\Volume{12345678-1234-1234-1234-123456789abc}\\",
      .expected_source_epoch_token_sha256 = epoch,
      .observe_source_epoch_token = [epoch]() {
        return ytec::clonecore::Result<FileSystemRecreateSha256>::success(
            epoch);
      },
  };

  const auto opened =
      open_windows_file_system_recreate_source_session_read_only(request);
  check(!opened.has_value() &&
            opened.error().code ==
                ytec::clonecore::ErrorCode::invalid_argument &&
            opened.error().operation == L"再作成source request",
        "Only 512-byte and 4096-byte logical source sectors may reach source I/O");
}

void test_win32_production_factory_constructs_without_target_io() {
  FakeSourceSession source;
  const auto execution = make_production_execution_plan(source);
  check(execution.has_value(), "Synthetic execution plan should bind");
  const auto target = make_windows_file_system_recreate_target_platform(
      execution.value(), production_request());
  check(target.has_value() &&
            target.value()->lifecycle_state() ==
                WindowsFileSystemRecreateTargetLifecycleState::ready,
        "Win32 production factory must allocate only and remain ready before the explicit begin call");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"success_uses_guarded_target_only_commit_last",
       test_success_uses_guarded_target_only_commit_last},
      {"exfat_uses_the_same_target_only_transaction_contract",
       test_exfat_uses_the_same_target_only_transaction_contract},
      {"4kn_source_to_512_target_uses_file_level_recreation",
       test_4kn_source_to_512_target_uses_file_level_recreation},
      {"execution_binding_rejects_same_disk_and_active_rescue_review",
       test_execution_binding_rejects_same_disk_and_active_rescue_review},
      {"fresh_target_active_rescue_stops_before_mutation",
       test_fresh_target_active_rescue_stops_before_mutation},
      {"source_or_target_drift_aborts_incomplete",
       test_source_or_target_drift_aborts_incomplete},
      {"short_io_cancel_and_guard_mismatch_abort",
       test_short_io_cancel_and_guard_mismatch_abort},
      {"format_root_readback_and_commit_evidence_fail_closed",
       test_format_root_readback_and_commit_evidence_fail_closed},
      {"commit_publication_boundary_never_rolls_back_post_latch",
       test_commit_publication_boundary_never_rolls_back_post_latch},
      {"partial_failure_injection_matrix_aborts_offline_incomplete",
       test_partial_failure_injection_matrix_aborts_offline_incomplete},
      {"production_state_machine_commits_only_after_offline_retirement",
       test_production_state_machine_commits_only_after_offline_retirement},
      {"production_partial_publication_never_rolls_back",
       test_production_partial_publication_never_rolls_back},
      {"production_exfat_uses_same_typed_lifecycle",
       test_production_exfat_uses_same_typed_lifecycle},
      {"partial_publication_does_not_borrow_stale_offline_proof",
       test_partial_publication_does_not_borrow_stale_offline_proof},
      {"production_factory_rejects_unreviewed_layout_before_io",
       test_production_factory_rejects_unreviewed_layout_before_io},
      {"source_factory_rejects_nonstandard_logical_sector_before_io",
       test_source_factory_rejects_nonstandard_logical_sector_before_io},
      {"win32_production_factory_constructs_without_target_io",
       test_win32_production_factory_constructs_without_target_io},
  };

  int failures{};
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

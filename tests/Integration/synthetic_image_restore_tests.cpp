#include "ytec/imageformat/backup_manifest.h"
#include "ytec/imageformat/dcimg.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/restore.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::clonecore::Error io_error(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

template <typename T>
T read_little(const std::vector<std::byte>& bytes, const std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

class MockTarget final : public ytec::clonecore::ITargetDiskWriter {
 public:
  struct Write final {
    std::uint64_t offset{};
    std::vector<std::byte> data;
  };

  explicit MockTarget(const std::uint64_t size)
      : storage_(static_cast<std::size_t>(size), std::byte{0x5A}) {}

  std::uint64_t size_bytes() const noexcept override {
    return storage_.size();
  }

  std::uint32_t logical_sector_size() const noexcept override {
    return 512;
  }

  ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (offset > storage_.size() || bytes.size() > storage_.size() - offset) {
      return ytec::clonecore::Status::failure(io_error(
          L"モック復元先書込み", L"モック復元先境界外です"));
    }
    std::copy(
        bytes.begin(),
        bytes.end(),
        storage_.begin() + static_cast<std::ptrdiff_t>(offset));
    writes.push_back(Write{
        .offset = offset,
        .data = std::vector<std::byte>(bytes.begin(), bytes.end()),
    });
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (fail_read_back) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          io_error(L"モック復元先読戻し", L"注入した読戻し失敗です"));
    }
    if (offset > storage_.size() || length > storage_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          io_error(L"モック復元先読戻し", L"モック復元先境界外です"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            storage_.begin() + static_cast<std::ptrdiff_t>(offset),
            storage_.begin() + static_cast<std::ptrdiff_t>(offset + length)));
  }

  ytec::clonecore::Status flush_target() override {
    ++flush_count;
    return ytec::clonecore::success_status();
  }

  const std::vector<std::byte>& storage() const noexcept { return storage_; }

  bool fail_read_back{};
  std::uint32_t flush_count{};
  std::vector<Write> writes;

 private:
  std::vector<std::byte> storage_;
};

constexpr std::uint64_t kSourceSize = 4ULL * 1024U * 1024U;
constexpr std::uint64_t kTargetSize = 6ULL * 1024U * 1024U;

ytec::clonecore::StableDiskIdentity target_identity() {
  return ytec::clonecore::StableDiskIdentity{
      .disk_number = 7,
      .model = L"VBOX HARDDISK",
      .size_bytes = kTargetSize,
      .logical_sector_size = 512,
      .serial_suffix = "RESTORE7",
      .device_instance_id = L"VBOX\\RESTORE\\7",
      .is_system_disk = false,
  };
}

ytec::imageformat::PartitionSnapshot mbr_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot;
  snapshot.style = ytec::imageformat::PartitionTableStyle::mbr;
  snapshot.source_disk_size = kSourceSize;
  snapshot.logical_sector_size = 512;
  ytec::imageformat::PartitionTableRegion mbr;
  mbr.disk_offset = 0;
  mbr.data.assign(512, std::byte{0x3C});
  mbr.data[510] = std::byte{0x55};
  mbr.data[511] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(mbr));
  return snapshot;
}

std::vector<std::byte> backup_manifest(
    const ytec::imageformat::DcimgCompression compression =
        ytec::imageformat::DcimgCompression::none) {
  const auto built = ytec::imageformat::build_backup_manifest_v1(
      ytec::imageformat::BackupImageManifest{
          .source =
              ytec::clonecore::StableDiskIdentity{
                  .disk_number = 1,
                  .model = L"SYNTHETIC SOURCE",
                  .size_bytes = kSourceSize,
                  .logical_sector_size = 512,
                  .serial_suffix = "SOURCE01",
                  .device_instance_id = L"VIRTUAL\\SOURCE\\1",
                  .is_system_disk = true,
              },
          .physical_sector_size = 4096,
          .partition_style =
              ytec::imageformat::BackupPartitionStyle::mbr,
          .boot_mode =
              ytec::imageformat::BackupBootMode::legacy_bios,
          .windows_major = 10,
          .windows_minor = 0,
          .windows_build = 19045,
          .windows_architecture = "AMD64",
          .bitlocker_fully_decrypted = true,
          .compression = compression,
          .compression_version =
              compression == ytec::imageformat::DcimgCompression::zstandard
                  ? 1U
                  : 0U,
          .chunk_size =
              ytec::imageformat::kDcimgChunkSize16MiB,
          .created_utc = "2026-07-31T12:00:00Z",
          .app_version = "0.1.0",
          .partitions = {
              ytec::imageformat::BackupManifestPartition{
                  .table_index = 0,
                  .offset_bytes = 1024U * 1024U,
                  .length_bytes = 3U * 1024U * 1024U,
                  .role =
                      ytec::imageformat::BackupPartitionRole::
                          windows_ntfs,
                  .file_system =
                      ytec::imageformat::BackupFileSystem::ntfs,
                  .cluster_size = 4096,
                  .name = L"Windows",
              },
          },
      });
  check(built.has_value(), "Synthetic backup manifest should build");
  return built.value();
}

std::vector<std::byte> sample_image(
    const bool overlap_partition_table = false,
    const ytec::imageformat::DcimgCompression compression =
        ytec::imageformat::DcimgCompression::none) {
  const auto partition =
      ytec::imageformat::build_partition_snapshot_v1(mbr_snapshot());
  check(partition.has_value(), "A valid MBR snapshot should build");

  ytec::imageformat::DcimgBuildRequest request;
  request.source_disk_size = kSourceSize;
  request.logical_sector_size = 512;
  request.physical_sector_size = 4096;
  request.compression = compression;
  request.manifest = backup_manifest(compression);
  request.partition_table_snapshot = partition.value();

  ytec::imageformat::DcimgBuildChunk data;
  data.logical_offset = overlap_partition_table ? 0 : 1024U * 1024U;
  data.logical_length = 4096;
  data.data.assign(4096, std::byte{0xA5});
  request.chunks.push_back(std::move(data));

  ytec::imageformat::DcimgBuildChunk zero;
  zero.logical_offset = 2U * 1024U * 1024U;
  zero.logical_length = 4096;
  zero.zero_filled = true;
  request.chunks.push_back(std::move(zero));

  const auto image = ytec::imageformat::build_dcimg_v1(request);
  check(image.has_value(), "A valid restore image should build");
  return image.value();
}

std::vector<std::byte> large_sample_image() {
  const auto partition =
      ytec::imageformat::build_partition_snapshot_v1(mbr_snapshot());
  check(partition.has_value(), "A valid MBR snapshot should build");

  ytec::imageformat::DcimgBuildRequest request;
  request.source_disk_size = kSourceSize;
  request.logical_sector_size = 512;
  request.physical_sector_size = 4096;
  request.manifest = backup_manifest();
  request.partition_table_snapshot = partition.value();

  ytec::imageformat::DcimgBuildChunk data;
  data.logical_offset = 1024U * 1024U;
  data.logical_length = 3U * 1024U * 1024U;
  data.data.assign(
      static_cast<std::size_t>(data.logical_length),
      std::byte{0x6D});
  request.chunks.push_back(std::move(data));

  const auto image =
      ytec::imageformat::build_uncompressed_dcimg_v1(request);
  check(image.has_value(), "A large restore image should build");
  return image.value();
}

ytec::imageformat::DcimgRestoreRequest restore_request() {
  const auto identity = target_identity();
  return ytec::imageformat::DcimgRestoreRequest{
      .expected_target = identity,
      .observed_target = identity,
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token =
              ytec::clonecore::make_target_confirmation_token(identity),
      },
      .maximum_chunk_bytes = 4096,
  };
}

void test_partition_snapshot_round_trip_and_corruption_rejection() {
  const auto encoded =
      ytec::imageformat::build_partition_snapshot_v1(mbr_snapshot());
  check(encoded.has_value(), "MBR snapshot build should succeed");
  const auto inspected =
      ytec::imageformat::inspect_partition_snapshot_v1(encoded.value());
  check(inspected.has_value(), "MBR snapshot inspection should succeed");
  check(
      inspected.value().regions.size() == 1 &&
          inspected.value().regions[0].data[510] == std::byte{0x55},
      "MBR snapshot bytes should survive round trip");

  auto corrupted = encoded.value();
  corrupted.back() ^= std::byte{0x01};
  check(
      !ytec::imageformat::inspect_partition_snapshot_v1(corrupted).has_value(),
      "A corrupted partition snapshot must be rejected");
}

void test_restore_verifies_then_commits_partition_table_last() {
  const auto image = sample_image();
  MockTarget target(kTargetSize);
  std::vector<ytec::clonecore::DiskOperationProgress> progress_events;
  auto request = restore_request();
  request.callbacks.progress =
      [&](const ytec::clonecore::DiskOperationProgress& progress) {
        progress_events.push_back(progress);
      };
  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      image, request, target);
  check(result.has_value(), "A valid synthetic restore should succeed");
  check(
      result.value().complete_image_verified_before_write &&
          result.value().backup_manifest_verified_before_write &&
          result.value().read_back_verified &&
          result.value().partition_table_committed,
      "All restore safety gates should be reported");
  check(result.value().restored_chunk_count == 2,
        "Both data and zero chunks should restore");
  check(target.flush_count == 3,
        "Invalidation, data, and final table stages should flush");
  check(!target.writes.empty() && target.writes.front().offset == 0 &&
            std::all_of(
                target.writes.front().data.begin(),
                target.writes.front().data.end(),
                [](const std::byte value) { return value == std::byte{0}; }),
        "The first write must invalidate the target partition table");
  check(target.writes.back().offset == 0 &&
            target.writes.back().data[510] == std::byte{0x55} &&
            target.writes.back().data[511] == std::byte{0xAA},
        "The final write must commit the verified MBR snapshot");
  check(target.storage()[1024U * 1024U] == std::byte{0xA5},
        "The data chunk should reach its logical offset");
  check(target.storage()[2U * 1024U * 1024U] == std::byte{0},
        "The zero chunk should be materialized and verified");
  check(!progress_events.empty(), "Restore progress should be observable");
  std::uint64_t completely_verified_image_bytes = 0;
  for (const auto& progress : progress_events) {
    if (progress.stage ==
        ytec::clonecore::DiskOperationStage::verifying_source) {
      completely_verified_image_bytes =
          (std::max)(
              completely_verified_image_bytes,
              progress.verified_bytes);
    }
  }
  check(
      completely_verified_image_bytes > image.size(),
      "Complete verification work should include metadata and chunk checks");
  for (std::size_t index = 1; index < progress_events.size(); ++index) {
    check(
        progress_events[index].read_bytes >=
                progress_events[index - 1].read_bytes &&
            progress_events[index].written_bytes >=
                progress_events[index - 1].written_bytes &&
            progress_events[index].verified_bytes >=
                progress_events[index - 1].verified_bytes,
        "Restore progress counters must be monotonic");
  }
  const auto& final_progress = progress_events.back();
  check(
      final_progress.stage ==
              ytec::clonecore::DiskOperationStage::completed &&
          final_progress.read_bytes ==
              completely_verified_image_bytes + 4096 &&
          final_progress.total_read_bytes ==
              completely_verified_image_bytes + 4096 &&
          final_progress.written_bytes == 8192 &&
          final_progress.total_write_bytes == 8192 &&
          final_progress.verified_bytes ==
              completely_verified_image_bytes + 8192 &&
          final_progress.total_verify_bytes ==
              completely_verified_image_bytes + 8192 &&
          !final_progress.cancellation_allowed,
      "Restore progress should distinguish source reads from zero writes");
}

void test_zstandard_restore_decompresses_before_target_write() {
  const auto image = sample_image(
      false, ytec::imageformat::DcimgCompression::zstandard);
  const auto inspection = ytec::imageformat::inspect_dcimg_v1(image);
  check(
      inspection.has_value() &&
          inspection.value().chunks.front().compression ==
              ytec::imageformat::DcimgCompression::zstandard,
      "Compressed restore fixture should contain a Zstandard frame");
  MockTarget target(kTargetSize);

  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      image, restore_request(), target);

  check(result.has_value(), "Verified compressed restore should succeed");
  check(target.storage()[1024U * 1024U] == std::byte{0xA5},
        "Restore must write uncompressed bytes at the logical offset");
  check(result.value().restored_data_bytes == 8192U,
        "Restore report must count logical bytes, not compressed bytes");
}

void test_sixteen_mib_restore_blocks_preserve_bytes_with_fewer_io_calls() {
  const auto image = large_sample_image();
  auto one_mib_request = restore_request();
  one_mib_request.maximum_chunk_bytes = 1024U * 1024U;
  auto sixteen_mib_request = restore_request();
  sixteen_mib_request.maximum_chunk_bytes = 16U * 1024U * 1024U;
  MockTarget one_mib_target(kTargetSize);
  MockTarget sixteen_mib_target(kTargetSize);

  const auto one_mib = ytec::imageformat::restore_verified_dcimg_v1(
      image, one_mib_request, one_mib_target);
  const auto sixteen_mib = ytec::imageformat::restore_verified_dcimg_v1(
      image, sixteen_mib_request, sixteen_mib_target);
  check(
      one_mib.has_value() && sixteen_mib.has_value(),
      "Both bounded restore block sizes should succeed");
  check(
      one_mib_target.storage() == sixteen_mib_target.storage(),
      "Larger restore blocks must preserve every target byte");
  const auto data_write_count = [](const MockTarget& target) {
    return static_cast<std::size_t>(std::count_if(
        target.writes.begin(),
        target.writes.end(),
        [](const MockTarget::Write& write) {
          return write.offset >= 1024U * 1024U;
        }));
  };
  check(
      data_write_count(one_mib_target) == 3U &&
          data_write_count(sixteen_mib_target) == 1U,
      "Sixteen MiB blocks should cut write/read-back calls by two thirds for a three MiB range");
  check(
      sixteen_mib.value().read_back_verified &&
          sixteen_mib.value().partition_table_committed,
      "The optimized restore must retain read-back and commit gates");
}

void test_recorded_image_fingerprint_mismatch_writes_nothing() {
  const auto image = sample_image();
  MockTarget target(kTargetSize);
  auto request = restore_request();
  ytec::imageformat::Sha256Digest wrong_hash{};
  wrong_hash.fill(std::byte{0xA7});
  request.expected_image_length = image.size();
  request.expected_global_hash = wrong_hash;

  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      image, request, target);

  check(!result.has_value(),
        "A job fingerprint mismatch must reject the restore");
  check(
      result.error().code ==
          ytec::clonecore::ErrorCode::identity_mismatch,
      "A job fingerprint mismatch should have a stable identity error");
  check(
      target.writes.empty() && target.flush_count == 0,
      "A job fingerprint mismatch must be detected before every target write");
}

void test_reader_restore_is_bounded_and_commits_last() {
  const auto image = sample_image();
  std::size_t largest_read = 0;
  std::size_t read_count = 0;
  const ytec::imageformat::Sha256ReadCallback reader =
      [&image, &largest_read, &read_count](
          const std::uint64_t offset,
          const std::size_t length)
      -> ytec::clonecore::Result<std::vector<std::byte>> {
    ++read_count;
    largest_read = (std::max)(largest_read, length);
    if (offset > image.size() || length > image.size() - offset) {
      return ytec::clonecore::Result<
          std::vector<std::byte>>::failure(
          io_error(L"モックdcimg読取り", L"境界外です"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            image.begin() + static_cast<std::ptrdiff_t>(offset),
            image.begin() +
                static_cast<std::ptrdiff_t>(offset + length)));
  };

  MockTarget target(kTargetSize);
  const auto result =
      ytec::imageformat::restore_verified_dcimg_v1_from_reader(
          image.size(), reader, restore_request(), target);
  check(result.has_value(), "A bounded reader restore should succeed");
  check(read_count > 1, "Reader restore should stream multiple ranges");
  check(
      largest_read <= 4096,
      "Synthetic reader requests should stay within the configured block");
  check(
      target.storage()[1024U * 1024U] == std::byte{0xA5},
      "Reader-restored data should reach the target");
  check(
      !target.writes.empty() &&
          target.writes.back().offset == 0 &&
          target.writes.back().data[510] == std::byte{0x55} &&
          target.writes.back().data[511] == std::byte{0xAA},
      "Reader restore must commit the MBR only as the final write");
}

void test_prepared_source_is_verified_once_and_single_use() {
  const auto image = sample_image();
  const auto inspection = ytec::imageformat::inspect_dcimg_v1(image);
  check(inspection.has_value(), "Synthetic image inspection should pass");

  std::size_t read_count = 0;
  const ytec::imageformat::Sha256ReadCallback reader =
      [&image, &read_count](
          const std::uint64_t offset,
          const std::size_t length)
      -> ytec::clonecore::Result<std::vector<std::byte>> {
    ++read_count;
    if (offset > image.size() || length > image.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          io_error(L"準備済みdcimg読取り", L"境界外です"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            image.begin() + static_cast<std::ptrdiff_t>(offset),
            image.begin() + static_cast<std::ptrdiff_t>(offset + length)));
  };

  auto request = restore_request();
  request.expected_image_length = image.size();
  request.expected_global_hash = inspection.value().global_hash;
  auto prepared = ytec::imageformat::prepare_verified_dcimg_v1_from_reader(
      image.size(), reader, request);
  check(prepared.has_value(), "A valid source should be prepared");
  const std::size_t reads_after_complete_verification = read_count;
  check(
      reads_after_complete_verification > 1,
      "Preparation should completely stream-verify the image");

  MockTarget target(kTargetSize);
  auto prepared_source = prepared.take_value();
  const auto restored = ytec::imageformat::restore_prepared_dcimg_v1(
      std::move(prepared_source), request, target);
  check(restored.has_value(), "A prepared source restore should succeed");
  check(
      read_count == reads_after_complete_verification + 1,
      "Restore should only re-read the one non-zero chunk, not rescan the image");

  MockTarget second_target(kTargetSize);
  const auto reused = ytec::imageformat::restore_prepared_dcimg_v1(
      std::move(prepared_source), request, second_target);
  check(!reused.has_value(), "A prepared source must be single-use");
  check(
      reused.error().code == ytec::clonecore::ErrorCode::invalid_argument,
      "Reusing a prepared source should have a stable invalid argument error");
  check(
      second_target.writes.empty() && second_target.flush_count == 0,
      "Reusing a consumed source must not write another target");
}

void test_reader_change_before_copy_is_rejected_without_commit() {
  const auto image = sample_image();
  const auto inspection = ytec::imageformat::inspect_dcimg_v1(image);
  check(inspection.has_value(), "Synthetic image inspection should pass");
  const std::uint64_t stored_offset =
      inspection.value().chunks.front().stored_offset;
  std::size_t exact_chunk_reads = 0;
  const ytec::imageformat::Sha256ReadCallback reader =
      [&image, stored_offset, &exact_chunk_reads](
          const std::uint64_t offset,
          const std::size_t length)
      -> ytec::clonecore::Result<std::vector<std::byte>> {
    if (offset > image.size() || length > image.size() - offset) {
      return ytec::clonecore::Result<
          std::vector<std::byte>>::failure(
          io_error(L"変更モックdcimg読取り", L"境界外です"));
    }
    std::vector<std::byte> bytes(
        image.begin() + static_cast<std::ptrdiff_t>(offset),
        image.begin() +
            static_cast<std::ptrdiff_t>(offset + length));
    if (offset == stored_offset &&
        ++exact_chunk_reads >= 2 &&
        !bytes.empty()) {
      bytes.front() ^= std::byte{0x01};
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(bytes));
  };

  MockTarget target(kTargetSize);
  const auto result =
      ytec::imageformat::restore_verified_dcimg_v1_from_reader(
          image.size(), reader, restore_request(), target);
  check(
      !result.has_value(),
      "A chunk changed after pre-verification must stop restore");
  check(
      result.error().code ==
          ytec::clonecore::ErrorCode::verification_failed,
      "Changed chunk should fail with verification_failed");
  check(
      target.storage()[510] == std::byte{0} &&
          target.storage()[511] == std::byte{0},
      "Changed source must leave the target partition table invalid");
  check(
      target.storage()[1024U * 1024U] == std::byte{0x5A},
      "Changed source data must not be written to its target range");
}

void test_reader_callback_exception_fails_before_writes() {
  const auto image = sample_image();
  const ytec::imageformat::Sha256ReadCallback reader =
      [](std::uint64_t, std::size_t)
      -> ytec::clonecore::Result<std::vector<std::byte>> {
    throw std::runtime_error("injected reader failure");
  };
  MockTarget target(kTargetSize);
  const auto result =
      ytec::imageformat::restore_verified_dcimg_v1_from_reader(
          image.size(), reader, restore_request(), target);
  check(!result.has_value(), "A throwing reader must fail closed");
  check(
      result.error().code == ytec::clonecore::ErrorCode::io_failed,
      "A throwing reader should be converted to an I/O failure");
  check(
      target.writes.empty(),
      "A reader exception during verification must not write the target");
}

void test_reader_cancellation_during_preverification_writes_nothing() {
  const auto image = sample_image();
  std::size_t completed_reads = 0;
  const ytec::imageformat::Sha256ReadCallback reader =
      [&image, &completed_reads](
          const std::uint64_t offset,
          const std::size_t length)
      -> ytec::clonecore::Result<std::vector<std::byte>> {
    if (offset > image.size() || length > image.size() - offset) {
      return ytec::clonecore::Result<
          std::vector<std::byte>>::failure(
          io_error(L"キャンセルモックdcimg読取り", L"境界外です"));
    }
    ++completed_reads;
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            image.begin() + static_cast<std::ptrdiff_t>(offset),
            image.begin() +
                static_cast<std::ptrdiff_t>(offset + length)));
  };
  auto request = restore_request();
  request.callbacks.cancellation_requested =
      [&completed_reads]() { return completed_reads >= 2; };
  MockTarget target(kTargetSize);
  const auto result =
      ytec::imageformat::restore_verified_dcimg_v1_from_reader(
          image.size(), reader, request, target);
  check(!result.has_value(), "Pre-verification cancellation must stop");
  check(
      result.error().code == ytec::clonecore::ErrorCode::cancelled,
      "Pre-verification cancellation should retain its error type");
  check(
      target.writes.empty(),
      "Pre-verification cancellation must not write the target");
}

void test_cancellation_leaves_partition_table_invalid() {
  const auto image = sample_image();
  MockTarget target(kTargetSize);
  bool cancellation_requested = false;
  bool commit_started = false;
  auto request = restore_request();
  request.callbacks.progress =
      [&](const ytec::clonecore::DiskOperationProgress& progress) {
        if (progress.stage ==
                ytec::clonecore::DiskOperationStage::copying_data &&
            progress.verified_bytes != 0) {
          cancellation_requested = true;
        }
        if (progress.stage ==
            ytec::clonecore::DiskOperationStage::
                committing_partition_table) {
          commit_started = true;
        }
      };
  request.callbacks.cancellation_requested =
      [&]() { return cancellation_requested; };

  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      image, request, target);
  check(!result.has_value(), "A requested restore cancellation must stop");
  check(
      result.error().code == ytec::clonecore::ErrorCode::cancelled,
      "Restore cancellation should use the dedicated cancelled error");
  check(!commit_started, "Cancellation must precede partition-table commit");
  check(
      target.storage()[510] == std::byte{0} &&
          target.storage()[511] == std::byte{0},
      "Cancelled restore must leave the partition table invalid");
}

void test_corrupt_image_rejected_before_writes() {
  auto image = sample_image();
  const std::uint64_t data_offset = read_little<std::uint64_t>(image, 104);
  image[static_cast<std::size_t>(data_offset)] ^= std::byte{0x01};
  MockTarget target(kTargetSize);
  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      image, restore_request(), target);
  check(!result.has_value(), "A corrupt image must not restore");
  check(target.writes.empty(), "Corruption must be detected before any write");
}

void test_wrong_confirmation_rejected_before_writes() {
  auto request = restore_request();
  request.confirmation.typed_token = L"WRONG";
  MockTarget target(kTargetSize);
  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      sample_image(), request, target);
  check(!result.has_value(), "Wrong confirmation must reject restore");
  check(target.writes.empty(), "Wrong confirmation must not write the target");
}

void test_small_target_rejected_before_writes() {
  auto request = restore_request();
  request.expected_target.size_bytes = 3U * 1024U * 1024U;
  request.observed_target.size_bytes = request.expected_target.size_bytes;
  request.confirmation.typed_token =
      ytec::clonecore::make_target_confirmation_token(
          request.observed_target);
  MockTarget target(request.expected_target.size_bytes);
  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      sample_image(), request, target);
  check(!result.has_value(), "A target smaller than the image disk must fail");
  check(target.writes.empty(), "Target size must be checked before writes");
}

void test_chunk_partition_overlap_rejected_before_writes() {
  MockTarget target(kTargetSize);
  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      sample_image(true), restore_request(), target);
  check(!result.has_value(), "Chunk/table overlap must reject restore");
  check(target.writes.empty(), "Overlap must be rejected before writes");
}

void test_readback_failure_stops_before_commit() {
  MockTarget target(kTargetSize);
  target.fail_read_back = true;
  const auto result = ytec::imageformat::restore_verified_dcimg_v1(
      sample_image(), restore_request(), target);
  check(!result.has_value(), "Injected readback failure must stop restore");
  check(target.writes.size() == 1,
        "Readback failure should stop immediately after invalidation write");
  check(target.writes.back().offset == 0 &&
            target.writes.back().data[510] == std::byte{0},
        "A failed restore must not commit the partition table");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"partition_snapshot_round_trip_and_corruption_rejection",
       test_partition_snapshot_round_trip_and_corruption_rejection},
      {"restore_verifies_then_commits_partition_table_last",
       test_restore_verifies_then_commits_partition_table_last},
      {"zstandard_restore_decompresses_before_target_write",
       test_zstandard_restore_decompresses_before_target_write},
      {"sixteen_mib_restore_blocks_preserve_bytes_with_fewer_io_calls",
       test_sixteen_mib_restore_blocks_preserve_bytes_with_fewer_io_calls},
      {"reader_restore_is_bounded_and_commits_last",
       test_reader_restore_is_bounded_and_commits_last},
      {"prepared_source_is_verified_once_and_single_use",
       test_prepared_source_is_verified_once_and_single_use},
      {"recorded_image_fingerprint_mismatch_writes_nothing",
       test_recorded_image_fingerprint_mismatch_writes_nothing},
      {"reader_change_before_copy_is_rejected_without_commit",
       test_reader_change_before_copy_is_rejected_without_commit},
      {"reader_callback_exception_fails_before_writes",
       test_reader_callback_exception_fails_before_writes},
      {"reader_cancellation_during_preverification_writes_nothing",
       test_reader_cancellation_during_preverification_writes_nothing},
      {"cancellation_leaves_partition_table_invalid",
       test_cancellation_leaves_partition_table_invalid},
      {"corrupt_image_rejected_before_writes",
       test_corrupt_image_rejected_before_writes},
      {"wrong_confirmation_rejected_before_writes",
       test_wrong_confirmation_rejected_before_writes},
      {"small_target_rejected_before_writes",
       test_small_target_rejected_before_writes},
      {"chunk_partition_overlap_rejected_before_writes",
       test_chunk_partition_overlap_rejected_before_writes},
      {"readback_failure_stops_before_commit",
       test_readback_failure_stops_before_commit},
  };

  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": unexpected exception: "
                << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

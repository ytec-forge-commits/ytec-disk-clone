#include "ytec/clonecore/offline_mbr_clone.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::clonecore::ByteRange;
using ytec::clonecore::Error;
using ytec::clonecore::ErrorCode;
using ytec::clonecore::NtfsGeometry;
using ytec::clonecore::Result;
using ytec::clonecore::Status;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

template <typename T>
void write_little(
    const std::span<std::byte> bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T>
T read_little(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

Error io_error(const std::wstring& operation) {
  return Error{
      .code = ErrorCode::io_failed,
      .native_code = ERROR_IO_DEVICE,
      .operation = operation,
      .message = L"合成I/O失敗",
  };
}

class MemorySource final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit MemorySource(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  std::uint64_t size_bytes() const noexcept override { return bytes_.size(); }
  std::uint32_t logical_sector_size() const noexcept override { return 512; }

  Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return Result<std::vector<std::byte>>::failure(io_error(L"合成コピー元読取り"));
    }
    return Result<std::vector<std::byte>>::success(std::vector<std::byte>(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length)));
  }

  const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
};

class MemoryTarget final : public ytec::clonecore::ITargetDiskWriter {
 public:
  explicit MemoryTarget(const std::size_t size) : bytes_(size, std::byte{0}) {}

  std::uint64_t size_bytes() const noexcept override { return bytes_.size(); }
  std::uint32_t logical_sector_size() const noexcept override { return 512; }

  Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (offset > bytes_.size() || bytes.size() > bytes_.size() - offset) {
      return Status::failure(io_error(L"合成コピー先書込み"));
    }
    write_offsets.push_back(offset);
    std::copy(
        bytes.begin(),
        bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return Result<std::vector<std::byte>>::failure(io_error(L"合成コピー先読戻し"));
    }
    auto result = std::vector<std::byte>(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length));
    if (corrupt_read_back && !result.empty()) {
      result[0] ^= std::byte{0x01};
    }
    return Result<std::vector<std::byte>>::success(std::move(result));
  }

  Status flush_target() override {
    ++flush_count;
    return ytec::clonecore::success_status();
  }

  const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

  bool corrupt_read_back{};
  int flush_count{};
  std::vector<std::uint64_t> write_offsets;

 private:
  std::vector<std::byte> bytes_;
};

class FixedRanges final : public ytec::clonecore::INtfsUsedRangeProvider {
 public:
  Result<std::vector<ByteRange>> query_used_ranges(
      const std::uint32_t partition_index,
      const NtfsGeometry&) override {
    const auto found = ranges.find(partition_index);
    if (found == ranges.end()) {
      return Result<std::vector<ByteRange>>::failure(Error{
          .code = ErrorCode::invalid_data,
          .native_code = ERROR_NOT_FOUND,
          .operation = L"合成使用クラスタ",
          .message = L"対応付けがありません",
      });
    }
    return Result<std::vector<ByteRange>>::success(found->second);
  }

  std::map<std::uint32_t, std::vector<ByteRange>> ranges;
};

class SequenceSignature final
    : public ytec::clonecore::IMbrSignatureGenerator {
 public:
  explicit SequenceSignature(std::vector<std::uint32_t> values)
      : values_(std::move(values)) {}

  Result<std::uint32_t> next_signature() override {
    if (index_ >= values_.size()) {
      return Result<std::uint32_t>::success(0);
    }
    return Result<std::uint32_t>::success(values_[index_++]);
  }

 private:
  std::vector<std::uint32_t> values_;
  std::size_t index_{};
};

constexpr std::uint32_t kNtfsStart = 64;
constexpr std::uint32_t kNtfsSectors = 4096;
constexpr std::uint32_t kRecoveryStart = kNtfsStart + kNtfsSectors;
constexpr std::uint32_t kRecoverySectors = 1024;
constexpr std::uint32_t kSourceSectors = 8192;

void write_partition(
    const std::span<std::byte> sector,
    const std::size_t index,
    const bool active,
    const std::uint8_t type,
    const std::uint32_t first_lba,
    const std::uint32_t sector_count) {
  const std::size_t offset = 446 + index * 16;
  sector[offset] = active ? std::byte{0x80} : std::byte{0};
  sector[offset + 4] = std::byte{type};
  write_little(sector, offset + 8, first_lba);
  write_little(sector, offset + 12, sector_count);
}

void write_ntfs_boot_sector(
    const std::span<std::byte> disk,
    const std::uint32_t first_lba,
    const std::uint32_t sector_count) {
  auto sector = disk.subspan(static_cast<std::size_t>(first_lba) * 512, 512);
  sector[0] = std::byte{0xEB};
  sector[1] = std::byte{0x52};
  sector[2] = std::byte{0x90};
  constexpr char signature[] = "NTFS    ";
  std::memcpy(sector.data() + 3, signature, 8);
  write_little<std::uint16_t>(sector, 11, 512);
  sector[13] = std::byte{8};
  write_little<std::uint64_t>(sector, 40, sector_count);
  sector[510] = std::byte{0x55};
  sector[511] = std::byte{0xAA};
}

void write_exfat_boot_sector(
    const std::span<std::byte> disk,
    const std::uint32_t first_lba,
    const std::uint32_t sector_count) {
  auto sector = disk.subspan(static_cast<std::size_t>(first_lba) * 512, 512);
  std::fill(sector.begin(), sector.end(), std::byte{0});
  const char signature[] = "EXFAT   ";
  std::memcpy(sector.data() + 3, signature, 8);
  constexpr std::uint32_t fat_offset = 24U;
  constexpr std::uint32_t fat_length = 8U;
  constexpr std::uint32_t heap_offset = fat_offset + fat_length;
  constexpr std::uint32_t sectors_per_cluster = 8U;
  const std::uint32_t cluster_count =
      (sector_count - heap_offset) / sectors_per_cluster;
  write_little<std::uint64_t>(sector, 64, first_lba);
  write_little<std::uint64_t>(sector, 72, sector_count);
  write_little<std::uint32_t>(sector, 80, fat_offset);
  write_little<std::uint32_t>(sector, 84, fat_length);
  write_little<std::uint32_t>(sector, 88, heap_offset);
  write_little<std::uint32_t>(sector, 92, cluster_count);
  write_little<std::uint32_t>(sector, 96, 2U);
  write_little<std::uint16_t>(sector, 104, 0x0100U);
  sector[108] = std::byte{9};
  sector[109] = std::byte{3};
  sector[110] = std::byte{1};
  sector[112] = std::byte{0};
  sector[510] = std::byte{0x55};
  sector[511] = std::byte{0xAA};
}

std::vector<std::byte> source_disk() {
  std::vector<std::byte> disk(
      static_cast<std::size_t>(kSourceSectors) * 512, std::byte{0});
  std::span<std::byte> bytes(disk);
  for (std::size_t index = 0; index < 440; ++index) {
    bytes[index] = std::byte{0x90};
  }
  write_little<std::uint32_t>(bytes, 440, 0x11223344U);
  write_partition(bytes.first(512), 0, true, 0x07, kNtfsStart, kNtfsSectors);
  write_partition(
      bytes.first(512), 1, false, 0x27, kRecoveryStart, kRecoverySectors);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};

  write_ntfs_boot_sector(bytes, kNtfsStart, kNtfsSectors);
  write_ntfs_boot_sector(bytes, kRecoveryStart, kRecoverySectors);
  const std::size_t ntfs_offset = static_cast<std::size_t>(kNtfsStart) * 512;
  std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(ntfs_offset + 8192),
              4096, std::byte{0x5A});
  const std::size_t recovery_offset =
      static_cast<std::size_t>(kRecoveryStart) * 512;
  std::fill(
      bytes.begin() + static_cast<std::ptrdiff_t>(recovery_offset + 512),
      bytes.begin() + static_cast<std::ptrdiff_t>(
          recovery_offset + static_cast<std::size_t>(kRecoverySectors) * 512),
      std::byte{0xA5});
  return disk;
}

std::vector<std::byte> source_disk_with_mbr_0x07_exfat() {
  std::vector<std::byte> disk(
      static_cast<std::size_t>(kSourceSectors) * 512, std::byte{0});
  std::span<std::byte> bytes(disk);
  write_little<std::uint32_t>(bytes, 440, 0x11223344U);
  write_partition(
      bytes.first(512), 0, false, 0x07, kNtfsStart, kNtfsSectors);
  bytes[510] = std::byte{0x55};
  bytes[511] = std::byte{0xAA};
  const auto partition_offset =
      static_cast<std::size_t>(kNtfsStart) * 512U;
  std::fill_n(
      bytes.begin() + static_cast<std::ptrdiff_t>(partition_offset),
      static_cast<std::size_t>(kNtfsSectors) * 512U,
      std::byte{0x6E});
  write_exfat_boot_sector(bytes, kNtfsStart, kNtfsSectors);
  return disk;
}

FixedRanges valid_ranges() {
  FixedRanges ranges;
  ranges.ranges[0] = {
      {.offset = 0, .length = 4096},
      {.offset = 8192, .length = 4096},
  };
  return ranges;
}

ytec::clonecore::StableDiskIdentity identity(
    const std::wstring& id,
    const std::uint64_t size,
    const bool source) {
  return ytec::clonecore::StableDiskIdentity{
      .model = L"Synthetic MBR",
      .size_bytes = size,
      .logical_sector_size = 512,
      .serial_suffix = source ? "SOURCE01" : "TARGET01",
      .device_instance_id = id,
  };
}

ytec::clonecore::OfflineMbrCloneRequest valid_request(
    const MemorySource& source,
    const MemoryTarget& target) {
  const auto source_identity = identity(L"SYNTHETIC\\MBR_SOURCE", source.size_bytes(), true);
  const auto target_identity = identity(L"SYNTHETIC\\MBR_TARGET", target.size_bytes(), false);
  return ytec::clonecore::OfflineMbrCloneRequest{
      .expected_source = source_identity,
      .observed_source = source_identity,
      .expected_target = target_identity,
      .observed_target = target_identity,
      .confirmation = ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = ytec::clonecore::make_target_confirmation_token(
              target_identity),
      },
      .maximum_chunk_bytes = 64U * 1024U,
      .connected_mbr_signatures = {0x55667788U},
  };
}

bool digest_is_zero(const std::array<std::byte, 32>& digest) {
  return std::all_of(
      digest.begin(),
      digest.end(),
      [](const std::byte value) { return value == std::byte{0}; });
}

void test_plan_uses_known_partition_modes() {
  MemorySource source(source_disk());
  MemoryTarget target(static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  auto ranges = valid_ranges();
  SequenceSignature signatures({0x55667788U, 0xAABBCCDDU});
  const std::array<std::uint32_t, 1> connected{0x55667788U};
  const auto plan = ytec::clonecore::build_offline_mbr_clone_plan(
      source, target, ranges, signatures, connected);
  check(plan.has_value(), "A known primary Windows MBR layout should plan");
  check(plan.value().partition_copies.size() == 2, "Both partitions should plan");
  check(
      plan.value().partition_copies[0].mode ==
          ytec::clonecore::MbrPartitionCopyMode::ntfs_used_clusters,
      "The Windows NTFS partition should use sparse ranges");
  check(
      plan.value().partition_copies[1].mode ==
          ytec::clonecore::MbrPartitionCopyMode::recovery_ntfs_raw,
      "The recovery partition should use raw copy");
  check(
      plan.value().target_mbr.target_disk.disk_signature == 0xAABBCCDDU,
      "A connected signature collision must be skipped");
}

void test_mbr_0x07_exfat_uses_whole_partition_exact_copy() {
  MemorySource source(source_disk_with_mbr_0x07_exfat());
  MemoryTarget target(static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  FixedRanges no_ntfs_ranges;
  SequenceSignature plan_signatures({0xAABBCCDDU});
  const std::array<std::uint32_t, 1> connected{0x55667788U};
  const auto plan = ytec::clonecore::build_offline_mbr_clone_plan(
      source, target, no_ntfs_ranges, plan_signatures, connected);
  check(
      plan.has_value(),
      "MBR 0x07 exFAT should plan without NTFS bitmap; code=" +
          std::to_string(
              plan.has_value()
                  ? 0
                  : static_cast<int>(plan.error().code)) +
          ", native=" +
          std::to_string(plan.has_value() ? 0U : plan.error().native_code));
  const ByteRange partition{
      .offset = static_cast<std::uint64_t>(kNtfsStart) * 512U,
      .length = static_cast<std::uint64_t>(kNtfsSectors) * 512U,
  };
  check(plan.value().partition_copies.size() == 1U &&
            plan.value().partition_copies.front().mode ==
                ytec::clonecore::MbrPartitionCopyMode::exfat_raw &&
            plan.value().partition_copies.front().source_ranges.size() == 1U &&
            plan.value().partition_copies.front().source_ranges.front().offset ==
                partition.offset &&
            plan.value().partition_copies.front().source_ranges.front().length ==
                partition.length,
        "MBR 0x07 exFAT must cover the complete partition");

  FixedRanges execute_ranges;
  SequenceSignature execute_signatures({0xAABBCCDDU});
  const auto report = ytec::clonecore::execute_offline_mbr_clone(
      valid_request(source, target),
      source,
      target,
      execute_ranges,
      execute_signatures);
  check(report.has_value(), "MBR 0x07 exFAT exact clone should execute");
  check(report.value().copied_data_bytes == partition.length,
        "MBR 0x07 exFAT must report the full partition length");
  check(std::equal(
            source.bytes().begin() +
                static_cast<std::ptrdiff_t>(partition.offset),
            source.bytes().begin() + static_cast<std::ptrdiff_t>(
                partition.offset + partition.length),
            target.bytes().begin() +
                static_cast<std::ptrdiff_t>(partition.offset)),
        "MBR 0x07 exFAT must preserve every partition byte");
}

void test_execute_commits_mbr_last_and_verifies_data() {
  MemorySource source(source_disk());
  MemoryTarget target(static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  auto ranges = valid_ranges();
  SequenceSignature signatures({0xAABBCCDDU});
  std::vector<ytec::clonecore::DiskOperationProgress> progress_events;
  std::vector<ytec::clonecore::DiskOperationSafeBoundary> safe_boundaries;
  bool cancellation_requested_after_commit_started = false;
  auto request = valid_request(source, target);
  request.callbacks.progress =
      [&](const ytec::clonecore::DiskOperationProgress& progress) {
        progress_events.push_back(progress);
        if (progress.stage ==
            ytec::clonecore::DiskOperationStage::
                committing_partition_table) {
          cancellation_requested_after_commit_started = true;
        }
      };
  request.callbacks.cancellation_requested =
      [&]() { return cancellation_requested_after_commit_started; };
  request.callbacks.safe_boundary =
      [&](const ytec::clonecore::DiskOperationSafeBoundary& boundary) {
        safe_boundaries.push_back(boundary);
        return ytec::clonecore::DiskOperationControlDecision::
            continue_operation;
      };
  const auto report = ytec::clonecore::execute_offline_mbr_clone(
      request, source, target, ranges, signatures);
  check(report.has_value(), "The synthetic MBR clone should execute");
  check(
      report.value().target_mbr_committed && report.value().read_back_verified,
      "The report should record readback and final MBR commit");
  check(
      !digest_is_zero(report.value().verified_write_digest),
      "Successful MBR clone must expose a non-zero verified-write digest");
  check(
      report.value().copied_data_bytes ==
          8192ULL + static_cast<std::uint64_t>(kRecoverySectors) * 512ULL,
      "Only NTFS used ranges and the raw recovery partition should copy");
  check(
      target.write_offsets.size() >= 3 &&
          target.write_offsets.front() == 0 &&
          target.write_offsets[1] == target.size_bytes() - 1024U * 1024U &&
          target.write_offsets.back() == 0,
      "Both target metadata ends should be cleared before the MBR is committed last");
  check(!progress_events.empty(), "MBR clone progress should be observable");
  check(
      !safe_boundaries.empty() &&
          safe_boundaries.back().kind ==
              ytec::clonecore::DiskOperationSafeBoundaryKind::
                  verified_chunk &&
          safe_boundaries.back().completed_bytes ==
              report.value().copied_data_bytes,
      "MBR clone must expose every read-back-verified chunk boundary");
  for (std::size_t index = 1; index < progress_events.size(); ++index) {
    check(
        progress_events[index].read_bytes >=
                progress_events[index - 1].read_bytes &&
            progress_events[index].written_bytes >=
                progress_events[index - 1].written_bytes &&
            progress_events[index].verified_bytes >=
                progress_events[index - 1].verified_bytes,
        "MBR progress counters must be monotonic");
  }
  check(
      std::all_of(
          progress_events.begin(),
          progress_events.end(),
          [](const ytec::clonecore::DiskOperationProgress& progress) {
            return !progress.pause_allowed ||
                progress.stage ==
                ytec::clonecore::DiskOperationStage::copying_data;
          }),
      "MBR metadata invalidation and commit must never advertise pause");
  const auto& final_progress = progress_events.back();
  const std::string final_progress_diagnostic =
      "Completed MBR progress should expose exact verified totals; "
      "read=" +
      std::to_string(final_progress.read_bytes) +
      ", written=" + std::to_string(final_progress.written_bytes) +
      ", verified=" + std::to_string(final_progress.verified_bytes) +
      ", totalWrite=" + std::to_string(final_progress.total_write_bytes) +
      ", expected=" + std::to_string(report.value().copied_data_bytes) +
      ", stage=" +
      std::to_string(static_cast<int>(final_progress.stage)) +
      ", cancellationAllowed=" +
      (final_progress.cancellation_allowed ? "true" : "false");
  check(
      final_progress.stage ==
              ytec::clonecore::DiskOperationStage::completed &&
          final_progress.read_bytes == report.value().copied_data_bytes &&
          final_progress.written_bytes ==
              report.value().copied_data_bytes &&
          final_progress.verified_bytes ==
              report.value().copied_data_bytes &&
          final_progress.total_write_bytes ==
              report.value().copied_data_bytes &&
          !final_progress.cancellation_allowed,
      final_progress_diagnostic);
  check(
      cancellation_requested_after_commit_started,
      "The MBR test should request cancellation after commit becomes non-cancellable");
  const std::span<const std::byte> target_bytes(target.bytes());
  check(
      read_little<std::uint32_t>(target_bytes, 440) == 0xAABBCCDDU,
      "The committed MBR should contain the new signature");

  const std::size_t ntfs_offset = static_cast<std::size_t>(kNtfsStart) * 512;
  check(
      std::equal(
          source.bytes().begin() + static_cast<std::ptrdiff_t>(ntfs_offset),
          source.bytes().begin() + static_cast<std::ptrdiff_t>(ntfs_offset + 4096),
          target.bytes().begin() + static_cast<std::ptrdiff_t>(ntfs_offset)),
      "The NTFS boot cluster should be copied");
  check(
      target.bytes()[ntfs_offset + 4096] == std::byte{0},
      "An unused NTFS cluster should remain zero on the target");

  const std::size_t recovery_offset =
      static_cast<std::size_t>(kRecoveryStart) * 512;
  const std::size_t recovery_length =
      static_cast<std::size_t>(kRecoverySectors) * 512;
  check(
      std::equal(
          source.bytes().begin() + static_cast<std::ptrdiff_t>(recovery_offset),
          source.bytes().begin() +
              static_cast<std::ptrdiff_t>(recovery_offset + recovery_length),
          target.bytes().begin() + static_cast<std::ptrdiff_t>(recovery_offset)),
      "The recovery partition should be copied in full");
}

void test_verified_write_digest_changes_with_copied_data() {
  auto original_bytes = source_disk();
  auto changed_bytes = original_bytes;
  const std::size_t changed_offset =
      static_cast<std::size_t>(kNtfsStart) * 512U + 8192U;
  changed_bytes[changed_offset] ^= std::byte{0x01};
  MemorySource original(std::move(original_bytes));
  MemorySource changed(std::move(changed_bytes));
  MemoryTarget original_target(
      static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  MemoryTarget changed_target(
      static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  auto original_ranges = valid_ranges();
  auto changed_ranges = valid_ranges();
  SequenceSignature original_signature({0xAABBCCDDU});
  SequenceSignature changed_signature({0xAABBCCDDU});

  const auto original_result = ytec::clonecore::execute_offline_mbr_clone(
      valid_request(original, original_target),
      original,
      original_target,
      original_ranges,
      original_signature);
  const auto changed_result = ytec::clonecore::execute_offline_mbr_clone(
      valid_request(changed, changed_target),
      changed,
      changed_target,
      changed_ranges,
      changed_signature);

  check(
      original_result.has_value() && changed_result.has_value(),
      "Both synthetic MBR clones must complete before comparing evidence");
  check(
      original_result.value().verified_write_digest !=
          changed_result.value().verified_write_digest,
      "Changing copied MBR payload bytes must change the verified-write digest");
}

void test_cancellation_leaves_target_mbr_invalid() {
  MemorySource source(source_disk());
  MemoryTarget target(static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  auto ranges = valid_ranges();
  SequenceSignature signatures({0xAABBCCDDU});
  bool cancellation_requested = false;
  bool commit_started = false;
  std::uint64_t safe_boundary_count = 0U;
  auto request = valid_request(source, target);
  request.callbacks.progress =
      [&](const ytec::clonecore::DiskOperationProgress& progress) {
        if (progress.stage ==
            ytec::clonecore::DiskOperationStage::
                committing_partition_table) {
          commit_started = true;
        }
      };
  request.callbacks.cancellation_requested =
      [&]() { return cancellation_requested; };
  request.callbacks.safe_boundary =
      [&](const ytec::clonecore::DiskOperationSafeBoundary&) {
        ++safe_boundary_count;
        cancellation_requested = true;
        return ytec::clonecore::DiskOperationControlDecision::
            cancel_operation;
      };

  const auto result = ytec::clonecore::execute_offline_mbr_clone(
      request, source, target, ranges, signatures);
  check(!result.has_value(), "A requested MBR clone cancellation must stop");
  check(
      result.error().code == ytec::clonecore::ErrorCode::cancelled,
      "MBR cancellation should use the dedicated cancelled error");
  check(!commit_started, "Cancellation must precede the MBR commit boundary");
  check(
      safe_boundary_count == 1U,
      "MBR cancellation must be honoured at the first verified chunk boundary");
  check(
      target.bytes()[510] == std::byte{0} &&
          target.bytes()[511] == std::byte{0},
      "Cancelled clone must leave the target MBR invalid");
}

void test_wrong_confirmation_stops_before_write() {
  MemorySource source(source_disk());
  MemoryTarget target(static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  auto ranges = valid_ranges();
  SequenceSignature signatures({0xAABBCCDDU});
  auto request = valid_request(source, target);
  request.confirmation.typed_token = L"WRONG";
  const auto result = ytec::clonecore::execute_offline_mbr_clone(
      request, source, target, ranges, signatures);
  check(!result.has_value(), "A wrong target token must stop the clone");
  check(target.write_offsets.empty(), "No target write may occur before confirmation");
}

void test_invalid_used_ranges_stop_before_write() {
  MemorySource source(source_disk());
  MemoryTarget target(static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  FixedRanges ranges;
  ranges.ranges[0] = {{.offset = 4096, .length = 4096}};
  SequenceSignature signatures({0xAABBCCDDU});
  const auto request = valid_request(source, target);
  const auto result = ytec::clonecore::execute_offline_mbr_clone(
      request, source, target, ranges, signatures);
  check(!result.has_value(), "Missing the NTFS boot cluster must stop the plan");
  check(target.write_offsets.empty(), "Plan failure must happen before invalidating MBR");
}

void test_readback_failure_prevents_success() {
  MemorySource source(source_disk());
  MemoryTarget target(static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  target.corrupt_read_back = true;
  auto ranges = valid_ranges();
  SequenceSignature signatures({0xAABBCCDDU});
  const auto request = valid_request(source, target);
  const auto result = ytec::clonecore::execute_offline_mbr_clone(
      request, source, target, ranges, signatures);
  check(!result.has_value(), "A readback mismatch must fail the clone");
  check(
      result.error().code == ErrorCode::verification_failed,
      "A readback mismatch must remain a verification failure");
  check(
      target.write_offsets.size() == 1 && target.write_offsets.front() == 0,
      "The first invalidation must not be followed by data writes after mismatch");
}

void test_bitlocker_signature_is_rejected_before_write() {
  auto disk = source_disk();
  const std::size_t offset = static_cast<std::size_t>(kNtfsStart) * 512 + 3;
  constexpr char signature[] = "-FVE-FS-";
  std::memcpy(disk.data() + offset, signature, 8);
  MemorySource source(std::move(disk));
  MemoryTarget target(static_cast<std::size_t>(kSourceSectors + 1024) * 512);
  auto ranges = valid_ranges();
  SequenceSignature signatures({0xAABBCCDDU});
  const auto request = valid_request(source, target);
  const auto result = ytec::clonecore::execute_offline_mbr_clone(
      request, source, target, ranges, signatures);
  check(!result.has_value(), "BitLocker-formatted NTFS must be rejected");
  check(target.write_offsets.empty(), "BitLocker detection must precede target writes");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"plan_uses_known_partition_modes", test_plan_uses_known_partition_modes},
      {"mbr_0x07_exfat_uses_whole_partition_exact_copy",
       test_mbr_0x07_exfat_uses_whole_partition_exact_copy},
      {"execute_commits_mbr_last_and_verifies_data",
       test_execute_commits_mbr_last_and_verifies_data},
      {"verified_write_digest_changes_with_copied_data",
       test_verified_write_digest_changes_with_copied_data},
      {"cancellation_leaves_target_mbr_invalid",
       test_cancellation_leaves_target_mbr_invalid},
      {"wrong_confirmation_stops_before_write",
       test_wrong_confirmation_stops_before_write},
      {"invalid_used_ranges_stop_before_write",
       test_invalid_used_ranges_stop_before_write},
      {"readback_failure_prevents_success",
       test_readback_failure_prevents_success},
      {"bitlocker_signature_is_rejected_before_write",
       test_bitlocker_signature_is_rejected_before_write},
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

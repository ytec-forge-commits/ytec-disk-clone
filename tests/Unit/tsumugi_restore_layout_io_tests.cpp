#include "ytec/imageformat/tsumugi_restore_layout_io.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kTargetSize = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kSectorSize = 512U;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error injected_error(
    std::wstring operation,
    std::wstring message) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = std::move(operation),
      .message = std::move(message),
  };
}

class MemoryTarget final : public ytec::clonecore::ITargetDiskWriter {
 public:
  MemoryTarget()
      : bytes_(static_cast<std::size_t>(kTargetSize), std::byte{0xCC}) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return reported_size.value_or(bytes_.size());
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return reported_sector;
  }

  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    events.push_back("write:" + std::to_string(offset));
    write_offsets.push_back(offset);
    if (offset > bytes_.size() || bytes.size() > bytes_.size() - offset) {
      return ytec::clonecore::Status::failure(injected_error(
          L"合成layout書込み", L"対象範囲外です"));
    }
    std::copy(
        bytes.begin(),
        bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    if (fail_write_once_at.has_value() &&
        *fail_write_once_at == offset) {
      fail_write_once_at.reset();
      return ytec::clonecore::Status::failure(injected_error(
          L"合成layout注入書込み", L"書込み後に失敗しました"));
    }
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    events.push_back("read:" + std::to_string(offset));
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"合成layout読戻し", L"対象範囲外です"));
    }
    std::vector<std::byte> result(
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length));
    if (corrupt_readback_at.has_value() &&
        *corrupt_readback_at == offset && !result.empty()) {
      result[0] ^= std::byte{1};
    }
    if (corrupt_readback_once_at.has_value() &&
        *corrupt_readback_once_at == offset && !result.empty()) {
      result[0] ^= std::byte{1};
      corrupt_readback_once_at.reset();
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::move(result));
  }

  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    events.push_back("flush");
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept {
    return bytes_;
  }

  void seed(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) {
    check(
        offset <= bytes_.size() && bytes.size() <= bytes_.size() - offset,
        "seed range must fit target");
    std::copy(
        bytes.begin(), bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
  }

  void clear_history() {
    write_offsets.clear();
    events.clear();
  }

  std::optional<std::uint64_t> reported_size;
  std::uint32_t reported_sector{kSectorSize};
  std::optional<std::uint64_t> corrupt_readback_at;
  mutable std::optional<std::uint64_t> corrupt_readback_once_at;
  std::optional<std::uint64_t> fail_write_once_at;
  std::vector<std::uint64_t> write_offsets;
  mutable std::vector<std::string> events;

 private:
  std::vector<std::byte> bytes_;
};

std::vector<std::byte> sector(const std::byte value) {
  return std::vector<std::byte>(kSectorSize, value);
}

ytec::imageformat::TsumugiWholeDiskRestoreLayoutPlan gpt_plan() {
  using namespace ytec::imageformat;
  TsumugiWholeDiskRestoreLayoutPlan plan{
      .style = PartitionTableStyle::gpt,
      .target_size_bytes = kTargetSize,
      .logical_sector_size = kSectorSize,
      .invalidation_ranges = {
          {.offset = 0U, .length = 1024U * 1024U},
          {.offset = kTargetSize - 1024U * 1024U,
           .length = 1024U * 1024U},
      },
      .staged_writes = {
          {TsumugiRestoreLayoutWriteKind::gpt_primary_entries,
           2U * kSectorSize,
           sector(std::byte{0x11})},
          {TsumugiRestoreLayoutWriteKind::gpt_backup_entries,
           kTargetSize - 2U * kSectorSize,
           sector(std::byte{0x22})},
      },
      .commit_writes = {
          {TsumugiRestoreLayoutWriteKind::gpt_backup_header,
           kTargetSize - kSectorSize,
           sector(std::byte{0x33})},
          {TsumugiRestoreLayoutWriteKind::gpt_protective_mbr,
           0U,
           sector(std::byte{0x44})},
          {TsumugiRestoreLayoutWriteKind::gpt_primary_header,
           kSectorSize,
           sector(std::byte{0x55})},
      },
      .target_layout = ytec::clonecore::GptDisk{
          .logical_sector_size = kSectorSize,
          .sector_count = kTargetSize / kSectorSize,
          .partition_entry_count = 4U,
          .partition_entry_size = 128U,
      },
  };
  return plan;
}

ytec::imageformat::TsumugiWholeDiskRestoreLayoutPlan mbr_plan() {
  using namespace ytec::imageformat;
  return TsumugiWholeDiskRestoreLayoutPlan{
      .style = PartitionTableStyle::mbr,
      .target_size_bytes = kTargetSize,
      .logical_sector_size = kSectorSize,
      .invalidation_ranges = {
          {.offset = 0U, .length = 1024U * 1024U},
          {.offset = kTargetSize - 1024U * 1024U,
           .length = 1024U * 1024U},
      },
      .commit_writes = {
          {TsumugiRestoreLayoutWriteKind::mbr_sector,
           0U,
           sector(std::byte{0x66})},
      },
      .target_layout = ytec::clonecore::MbrDisk{
          .logical_sector_size = kSectorSize,
          .sector_count = kTargetSize / kSectorSize,
      },
  };
}

ytec::clonecore::GptGuid guid(const std::byte seed) {
  ytec::clonecore::GptGuid result;
  result.bytes.fill(seed);
  return result;
}

class SequenceGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  explicit SequenceGuidGenerator(
      std::vector<ytec::clonecore::GptGuid> values)
      : values_(std::move(values)) {}

  [[nodiscard]] ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    if (next_ >= values_.size()) {
      return ytec::clonecore::Result<ytec::clonecore::GptGuid>::failure(
          injected_error(L"合成GUID", L"GUIDが不足しています"));
    }
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(
        values_[next_++]);
  }

 private:
  std::vector<ytec::clonecore::GptGuid> values_;
  std::size_t next_{};
};

ytec::imageformat::TsumugiRestoreLayoutWriteKind test_gpt_kind(
    const ytec::clonecore::GptMetadataKind kind) {
  using ytec::clonecore::GptMetadataKind;
  using ytec::imageformat::TsumugiRestoreLayoutWriteKind;
  switch (kind) {
    case GptMetadataKind::primary_entries:
      return TsumugiRestoreLayoutWriteKind::gpt_primary_entries;
    case GptMetadataKind::backup_entries:
      return TsumugiRestoreLayoutWriteKind::gpt_backup_entries;
    case GptMetadataKind::backup_header:
      return TsumugiRestoreLayoutWriteKind::gpt_backup_header;
    case GptMetadataKind::primary_header_commit:
      return TsumugiRestoreLayoutWriteKind::gpt_primary_header;
    case GptMetadataKind::protective_mbr:
      return TsumugiRestoreLayoutWriteKind::gpt_protective_mbr;
  }
  return TsumugiRestoreLayoutWriteKind::gpt_protective_mbr;
}

ytec::imageformat::TsumugiPreservingPartitionLayoutPlanV1
preserving_gpt_plan(MemoryTarget& target) {
  using namespace ytec;
  clonecore::GptDisk source{
      .logical_sector_size = kSectorSize,
      .sector_count = kTargetSize / kSectorSize,
      .disk_guid = guid(std::byte{0x10}),
      .first_usable_lba = 34U,
      .last_usable_lba = kTargetSize / kSectorSize - 34U,
      .partition_entry_count = 128U,
      .partition_entry_size = 128U,
      .partitions = {
          clonecore::GptPartition{
              .entry_index = 0U,
              .type_guid = clonecore::gpt_type_basic_data(),
              .unique_guid = guid(std::byte{0x11}),
              .first_lba = 2048U,
              .last_lba = 3071U,
              .name = u"existing",
          },
      },
  };
  SequenceGuidGenerator initial_ids({
      guid(std::byte{0x20}),
      guid(std::byte{0x21}),
  });
  auto initial = clonecore::make_gpt_write_plan(
      source, kTargetSize, kSectorSize, initial_ids);
  check(initial.has_value(), "initial preserving GPT must build");
  for (const auto& write : initial.value().writes) {
    target.seed(write.offset, write.bytes);
  }
  SequenceGuidGenerator added_ids({guid(std::byte{0x31})});
  auto added = clonecore::make_gpt_add_partition_plan(
      initial.value().target_disk,
      clonecore::GptAddPartitionRequest{
          .first_lba = 4096U,
          .sector_count = 1024U,
          .type_guid = clonecore::gpt_type_basic_data(),
          .name = u"added",
      },
      added_ids);
  check(added.has_value(), "preserving GPT addition must build");
  imageformat::TsumugiPreservingPartitionLayoutPlanV1 result{
      .style = imageformat::PartitionTableStyle::gpt,
      .target_size_bytes = kTargetSize,
      .logical_sector_size = kSectorSize,
      .new_partition_offset = 4096ULL * kSectorSize,
      .new_partition_size = 1024ULL * kSectorSize,
      .original_layout = initial.value().target_disk,
      .target_layout = added.value().target_disk,
  };
  const auto appended = std::find_if(
      added.value().target_disk.partitions.begin(),
      added.value().target_disk.partitions.end(),
      [](const clonecore::GptPartition& partition) {
        return partition.first_lba == 4096U;
      });
  check(appended != added.value().target_disk.partitions.end(),
        "new GPT entry must be identifiable");
  result.new_partition_number = appended->entry_index + 1U;
  const auto append = [&](const clonecore::GptMetadataKind kind,
                          std::vector<imageformat::TsumugiRestoreLayoutWrite>&
                              output,
                          const bool original_bytes) {
    const auto found = std::find_if(
        added.value().writes.begin(),
        added.value().writes.end(),
        [&](const clonecore::GptMetadataWrite& write) {
          return write.kind == kind;
        });
    check(found != added.value().writes.end(),
          "required preserving GPT write must exist");
    std::vector<std::byte> bytes = found->bytes;
    if (original_bytes) {
      bytes.assign(
          target.bytes().begin() +
              static_cast<std::ptrdiff_t>(found->offset),
          target.bytes().begin() +
              static_cast<std::ptrdiff_t>(found->offset + bytes.size()));
    }
    output.push_back({
        .kind = test_gpt_kind(kind),
        .offset = found->offset,
        .bytes = std::move(bytes),
    });
  };
  for (const auto kind : {
           clonecore::GptMetadataKind::backup_entries,
           clonecore::GptMetadataKind::backup_header,
           clonecore::GptMetadataKind::primary_entries,
           clonecore::GptMetadataKind::primary_header_commit,
       }) {
    append(kind, result.published_writes, false);
  }
  for (const auto kind : {
           clonecore::GptMetadataKind::primary_entries,
           clonecore::GptMetadataKind::primary_header_commit,
           clonecore::GptMetadataKind::backup_entries,
           clonecore::GptMetadataKind::backup_header,
       }) {
    append(kind, result.rollback_writes, true);
  }
  target.clear_history();
  return result;
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
shrink_final_plan() {
  using namespace ytec;
  auto metadata = mbr_plan();
  std::get<clonecore::MbrDisk>(metadata.target_layout).partitions = {
      clonecore::MbrPartition{
          .table_index = 0U,
          .type = 0x07U,
          .first_lba = static_cast<std::uint32_t>(
              (1ULL * 1024ULL * 1024ULL) / kSectorSize),
          .sector_count = static_cast<std::uint32_t>(
              (512ULL * 1024ULL) / kSectorSize),
      },
      clonecore::MbrPartition{
          .table_index = 1U,
          .type = 0x07U,
          .first_lba = static_cast<std::uint32_t>(
              (2ULL * 1024ULL * 1024ULL) / kSectorSize),
          .sector_count = static_cast<std::uint32_t>(
              (512ULL * 1024ULL) / kSectorSize),
      },
  };
  return imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1{
      .migration = {
          .target_style = migrationcore::MigrationPartitionStyle::mbr,
          .alignment_bytes = 1024ULL * 1024ULL,
          .minimum_target_size_bytes = 3ULL * 1024ULL * 1024ULL,
          .target_size_bytes = kTargetSize,
          .unallocated_tail_bytes = 1024ULL * 1024ULL,
          .source_remains_unchanged = true,
          .target_partitions = {
              {
                  .target_number = 1U,
                  .source_table_index = 1U,
                  .role = migrationcore::MigrationPartitionRole::windows,
                  .file_system = migrationcore::MigrationFileSystem::ntfs,
                  .action = migrationcore::MigrationPartitionAction::
                      apply_file_image,
                  .offset_bytes = 1ULL * 1024ULL * 1024ULL,
                  .size_bytes = 512ULL * 1024ULL,
              },
              {
                  .target_number = 2U,
                  .source_table_index = 3U,
                  .role = migrationcore::MigrationPartitionRole::data,
                  .file_system = migrationcore::MigrationFileSystem::exfat,
                  .action = migrationcore::MigrationPartitionAction::
                      apply_file_image,
                  .offset_bytes = 2ULL * 1024ULL * 1024ULL,
                  .size_bytes = 512ULL * 1024ULL,
              },
          },
      },
      .metadata = std::move(metadata),
  };
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
shrink_final_gpt_plan() {
  using namespace ytec;
  auto result = shrink_final_plan();
  result.migration.target_style =
      migrationcore::MigrationPartitionStyle::gpt;
  result.metadata = gpt_plan();
  auto& gpt = std::get<clonecore::GptDisk>(result.metadata.target_layout);
  gpt.disk_guid = guid(std::byte{0x70});
  gpt.partitions = {
      clonecore::GptPartition{
          .entry_index = 0U,
          .type_guid = clonecore::gpt_type_basic_data(),
          .unique_guid = guid(std::byte{0x71}),
          .first_lba =
              (1ULL * 1024ULL * 1024ULL) / kSectorSize,
          .last_lba =
              ((1ULL * 1024ULL * 1024ULL) + (512ULL * 1024ULL)) /
                      kSectorSize -
                  1U,
      },
      clonecore::GptPartition{
          .entry_index = 1U,
          .type_guid = clonecore::gpt_type_basic_data(),
          .unique_guid = guid(std::byte{0x72}),
          .first_lba =
              (2ULL * 1024ULL * 1024ULL) / kSectorSize,
          .last_lba =
              ((2ULL * 1024ULL * 1024ULL) + (512ULL * 1024ULL)) /
                      kSectorSize -
                  1U,
      },
  };
  return result;
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
shrink_generated_gpt_plan() {
  using namespace ytec;
  auto result = shrink_final_gpt_plan();
  constexpr std::uint64_t alignment = 256ULL * 1024ULL;
  result.migration.alignment_bytes = alignment;
  result.migration.target_partitions = {
      {
          .target_number = 1U,
          .role = migrationcore::MigrationPartitionRole::efi_system,
          .file_system = migrationcore::MigrationFileSystem::fat32,
          .action = migrationcore::MigrationPartitionAction::create_fat32,
          .offset_bytes = 1ULL * 1024ULL * 1024ULL,
          .size_bytes = alignment,
      },
      {
          .target_number = 2U,
          .role = migrationcore::MigrationPartitionRole::microsoft_reserved,
          .file_system = migrationcore::MigrationFileSystem::none,
          .action = migrationcore::MigrationPartitionAction::create_reserved,
          .offset_bytes = 1536ULL * 1024ULL,
          .size_bytes = alignment,
      },
      {
          .target_number = 3U,
          .source_table_index = 7U,
          .role = migrationcore::MigrationPartitionRole::data,
          .file_system = migrationcore::MigrationFileSystem::ntfs,
          .action =
              migrationcore::MigrationPartitionAction::create_empty_ntfs,
          .offset_bytes = 2ULL * 1024ULL * 1024ULL,
          .size_bytes = alignment,
      },
      {
          .target_number = 4U,
          .source_table_index = 8U,
          .role = migrationcore::MigrationPartitionRole::data,
          .file_system = migrationcore::MigrationFileSystem::unsupported,
          .action = migrationcore::MigrationPartitionAction::copy_exact_raw,
          .offset_bytes = 2560ULL * 1024ULL,
          .size_bytes = alignment,
      },
  };
  auto& gpt = std::get<clonecore::GptDisk>(result.metadata.target_layout);
  const auto partition = [](const std::uint32_t entry_index,
                            const clonecore::GptGuid& type,
                            const std::byte id,
                            const std::uint64_t offset,
                            const std::uint64_t size) {
    return clonecore::GptPartition{
        .entry_index = entry_index,
        .type_guid = type,
        .unique_guid = guid(id),
        .first_lba = offset / kSectorSize,
        .last_lba = (offset + size) / kSectorSize - 1U,
    };
  };
  gpt.partitions = {
      partition(
          0U,
          clonecore::gpt_type_efi_system(),
          std::byte{0x73},
          1ULL * 1024ULL * 1024ULL,
          alignment),
      partition(
          1U,
          clonecore::gpt_type_microsoft_reserved(),
          std::byte{0x74},
          1536ULL * 1024ULL,
          alignment),
      partition(
          2U,
          clonecore::gpt_type_basic_data(),
          std::byte{0x75},
          2ULL * 1024ULL * 1024ULL,
          alignment),
      partition(
          3U,
          clonecore::gpt_type_basic_data(),
          std::byte{0x76},
          2560ULL * 1024ULL,
          alignment),
  };
  return result;
}

std::vector<ytec::imageformat::TsumugiShrinkConstructionLayoutPlanV1>
shrink_construction_plans(
    const ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1&
        final_plan) {
  SequenceGuidGenerator ids({
      guid(std::byte{0x11}),
      guid(std::byte{0x12}),
      guid(std::byte{0x21}),
      guid(std::byte{0x22}),
  });
  auto result = ytec::imageformat::
      make_tsumugi_shrink_construction_layout_plans_v1(final_plan, ids);
  if (!result) {
    throw std::runtime_error("construction fixture generation failed");
  }
  return result.take_value();
}

void test_gpt_prepare_then_non_cancellable_commit_order() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  TsumugiWholeDiskRestoreLayoutTransaction transaction(gpt_plan(), target);
  const auto prepared = transaction.prepare();
  check(
      prepared.has_value() && prepared.value().invalidated_bytes ==
          2U * 1024U * 1024U,
      "both old table locations must be invalidated and verified");
  check(
      target.write_offsets.size() == 2U && target.write_offsets[0] == 0U &&
          target.write_offsets[1] == kTargetSize - 1024U * 1024U,
      "invalidation must cover the leading and trailing metadata ends first");
  const auto committed = transaction.commit();
  check(
      committed.has_value() &&
          committed.value().primary_partition_table_committed &&
          !committed.value().target_left_incomplete,
      "verified GPT metadata must finish with a committed primary table");
  const std::vector<std::uint64_t> expected{
      0U,
      kTargetSize - 1024U * 1024U,
      2U * kSectorSize,
      kTargetSize - 2U * kSectorSize,
      kTargetSize - kSectorSize,
      0U,
      kSectorSize,
  };
  check(
      target.write_offsets == expected,
      "GPT must stage entries, then backup header, protective MBR, and primary header");
  check(
      target.bytes()[kSectorSize] == std::byte{0x55},
      "primary GPT header must be the last exposed metadata write");
}

void test_cancel_before_prepare_writes_nothing() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  TsumugiWholeDiskRestoreLayoutTransaction transaction(gpt_plan(), target);
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .cancellation_requested = [] { return true; },
  };
  const auto prepared = transaction.prepare(callbacks);
  check(
      !prepared.has_value() && target.write_offsets.empty(),
      "pre-start cancellation must stop before the first destructive write");
}

void test_cancel_before_staging_leaves_target_unrecognizable() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  TsumugiWholeDiskRestoreLayoutTransaction transaction(gpt_plan(), target);
  check(transaction.prepare().has_value(), "fixture prepare must succeed");
  bool cancel = false;
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress = [&](const ytec::clonecore::DiskOperationProgress& progress) {
        if (progress.stage ==
                ytec::clonecore::DiskOperationStage::staging_partition_table &&
            progress.cancellation_allowed) {
          cancel = true;
        }
      },
      .cancellation_requested = [&] { return cancel; },
  };
  const auto committed = transaction.commit(callbacks);
  check(
      !committed.has_value() && target.write_offsets.size() == 2U &&
          target.bytes()[kSectorSize] == std::byte{0},
      "cancellation before staging must leave both valid GPT headers absent");
  transaction.abort();
  check(
      transaction.report().target_left_incomplete,
      "aborted whole-disk restore must remain explicitly incomplete");
}

void test_readback_failure_never_reaches_commit() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  target.corrupt_readback_at = 0U;
  TsumugiWholeDiskRestoreLayoutTransaction transaction(gpt_plan(), target);
  const auto prepared = transaction.prepare();
  check(
      !prepared.has_value() && target.write_offsets.size() == 1U &&
          !transaction.report().primary_partition_table_committed,
      "invalidation readback mismatch must stop before any new layout commit");
}

void test_dimension_or_overlapping_plan_fails_before_write() {
  using namespace ytec::imageformat;
  MemoryTarget changed_target;
  changed_target.reported_size = kTargetSize + kSectorSize;
  TsumugiWholeDiskRestoreLayoutTransaction changed(
      gpt_plan(), changed_target);
  check(
      !changed.prepare().has_value() && changed_target.write_offsets.empty(),
      "target dimension drift must stop before invalidation");

  auto overlap = gpt_plan();
  overlap.commit_writes[2].offset = 2U * kSectorSize;
  MemoryTarget overlap_target;
  TsumugiWholeDiskRestoreLayoutTransaction malformed(
      std::move(overlap), overlap_target);
  check(
      !malformed.prepare().has_value() && overlap_target.write_offsets.empty(),
      "overlapping stage/commit metadata must be rejected before writes");
}

void test_mbr_has_no_staging_and_one_final_sector() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  TsumugiWholeDiskRestoreLayoutTransaction transaction(mbr_plan(), target);
  check(transaction.prepare().has_value(), "MBR fixture prepare must succeed");
  const auto committed = transaction.commit();
  check(
      committed.has_value() && target.write_offsets.size() == 3U &&
          target.write_offsets.back() == 0U &&
          target.bytes()[0] == std::byte{0x66},
      "MBR must be exposed by exactly one final sector after invalidation");
}

void check_gpt_emergency_reinvalidation(
    const std::uint64_t failed_commit_offset,
    const std::string& message) {
  using namespace ytec::imageformat;
  MemoryTarget target;
  TsumugiWholeDiskRestoreLayoutTransaction transaction(gpt_plan(), target);
  check(transaction.prepare().has_value(), "GPT fixture prepare must succeed");
  target.corrupt_readback_once_at = failed_commit_offset;
  const auto committed = transaction.commit();
  check(!committed.has_value(), message + ": injected commit must fail");
  check(
      transaction.report().emergency_reinvalidation_verified &&
          transaction.report().target_left_incomplete &&
          !transaction.report().primary_partition_table_committed,
      message + ": emergency reinvalidation must be verified");
  const std::array<std::uint64_t, 5U> metadata_offsets{
      0U,
      kSectorSize,
      2U * kSectorSize,
      kTargetSize - 2U * kSectorSize,
      kTargetSize - kSectorSize,
  };
  for (const auto offset : metadata_offsets) {
    check(
        target.bytes()[static_cast<std::size_t>(offset)] == std::byte{0},
        message + ": every GPT metadata location must be zero after failure");
  }
}

void test_each_gpt_commit_boundary_reinvalidates_on_failure() {
  check_gpt_emergency_reinvalidation(
      kTargetSize - kSectorSize, "backup header failure");
  check_gpt_emergency_reinvalidation(0U, "protective MBR failure");
  check_gpt_emergency_reinvalidation(
      kSectorSize, "primary header failure");
}

void test_mbr_commit_failure_reinvalidates_sector_zero() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  TsumugiWholeDiskRestoreLayoutTransaction transaction(mbr_plan(), target);
  check(transaction.prepare().has_value(), "MBR fixture prepare must succeed");
  target.corrupt_readback_once_at = 0U;
  const auto committed = transaction.commit();
  check(
      !committed.has_value() &&
          transaction.report().emergency_reinvalidation_verified &&
          transaction.report().target_left_incomplete &&
          target.bytes()[0] == std::byte{0},
      "failed MBR publication must be readback-zeroed again");
}

void test_abort_retries_emergency_reinvalidation_idempotently() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  TsumugiWholeDiskRestoreLayoutTransaction transaction(gpt_plan(), target);
  check(transaction.prepare().has_value(), "retry fixture prepare must succeed");
  target.corrupt_readback_at = kTargetSize - kSectorSize;
  check(!transaction.commit().has_value(),
        "persistent corruption must also defeat first emergency readback");
  check(
      !transaction.report().target_left_incomplete &&
          !transaction.report().emergency_reinvalidation_verified,
      "failed emergency cleanup must not claim safe incomplete state");
  target.corrupt_readback_at.reset();
  transaction.abort();
  transaction.abort();
  check(
      transaction.report().target_left_incomplete &&
          transaction.report().emergency_reinvalidation_verified &&
          target.bytes()[kTargetSize - kSectorSize] == std::byte{0},
      "abort must safely retry emergency cleanup and remain idempotent");
}

void test_shrink_transaction_serializes_temporary_gpt_then_final_mbr() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  auto final_plan = shrink_final_plan();
  auto constructions = shrink_construction_plans(final_plan);
  TsumugiShrinkRestoreLayoutTransactionV1 transaction(
      std::move(final_plan), std::move(constructions), target);
  const auto prepared = transaction.prepare();
  check(
      prepared.has_value() && prepared.value().metadata_safely_withheld &&
          prepared.value().construction_layout_count == 2U,
      "shrink prepare must invalidate old metadata before construction");

  bool publication_started = false;
  const ytec::clonecore::DiskOperationCallbacks callbacks{
      .progress = [&](const auto& progress) {
        if (progress.stage == ytec::clonecore::DiskOperationStage::
                                  committing_partition_table &&
            !progress.cancellation_allowed) {
          publication_started = true;
        }
      },
      .cancellation_requested = [&] { return publication_started; },
  };
  const auto first = transaction.publish_construction(1U, callbacks);
  check(
      first.has_value() && first.value().temporary_layout_active &&
          first.value().active_final_target_number == 1U &&
          first.value().active_source_table_index == 1U &&
          transaction.active_construction_plan() != nullptr,
      "temporary GPT publication must ignore cancellation after its boundary");
  const auto writes_after_first_publication = target.write_offsets.size();
  check(
      !transaction.publish_construction(2U).has_value() &&
          target.write_offsets.size() == writes_after_first_publication,
      "a second temporary GPT must not be published while one is active");
  check(
      !transaction.commit_final().has_value() &&
          target.write_offsets.size() == writes_after_first_publication,
      "final MBR must remain withheld while temporary GPT is active");

  const ytec::clonecore::DiskOperationCallbacks cancelled_retirement{
      .cancellation_requested = [] { return true; },
  };
  const auto first_retired =
      transaction.retire_construction(1U, cancelled_retirement);
  check(
      first_retired.has_value() &&
          first_retired.value().metadata_safely_withheld &&
          first_retired.value().retired_construction_layouts == 1U,
      "exact retirement must be non-cancellable and restore withheld metadata");
  check(transaction.publish_construction(2U).has_value(),
        "the next temporary GPT may start only after exact retirement");
  check(transaction.retire_construction(2U).has_value(),
        "the second temporary GPT must retire successfully");
  const auto committed = transaction.commit_final();
  check(
      committed.has_value() &&
          committed.value().final_partition_table_committed &&
          !committed.value().target_left_incomplete &&
          committed.value().published_construction_layouts == 2U &&
          committed.value().retired_construction_layouts == 2U &&
          target.bytes()[0] == std::byte{0x66},
      "final MBR may be exposed only after every temporary GPT retired");
}

void test_shrink_generated_esp_and_empty_filesystem_complete_by_final_key() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  auto final_plan = shrink_generated_gpt_plan();
  auto constructions = shrink_construction_plans(final_plan);
  check(
      constructions.size() == 2U &&
          constructions[0].final_target_number == 1U &&
          constructions[0].purpose ==
              TsumugiShrinkConstructionPurposeV1::prepare_efi_system &&
          !constructions[0].source_table_index.has_value() &&
          constructions[1].final_target_number == 3U &&
          constructions[1].purpose == TsumugiShrinkConstructionPurposeV1::
              recreate_empty_file_system &&
          constructions[1].source_table_index == 7U,
      "only generated ESP and empty filesystem must receive purpose-bound construction plans");
  TsumugiShrinkRestoreLayoutTransactionV1 transaction(
      std::move(final_plan), std::move(constructions), target);
  check(transaction.prepare().has_value(),
        "generated construction fixture must prepare");
  const auto esp = transaction.publish_construction(1U);
  check(
      esp.has_value() && esp.value().active_final_target_number == 1U &&
          !esp.value().active_source_table_index.has_value(),
      "source-free ESP construction must be addressable by final target number");
  check(transaction.retire_construction(1U).has_value(),
        "generated ESP construction must retire exactly");
  check(!transaction.commit_final().has_value(),
        "final GPT must remain withheld while empty filesystem construction is pending");
  const auto empty = transaction.publish_construction(3U);
  check(
      empty.has_value() && empty.value().active_final_target_number == 3U &&
          empty.value().active_source_table_index == 7U,
      "empty filesystem construction must use final key while retaining source audit identity");
  check(transaction.retire_construction(3U).has_value(),
        "empty filesystem construction must retire exactly");
  const auto committed = transaction.commit_final();
  check(
      committed.has_value() &&
          committed.value().final_partition_table_committed &&
          committed.value().construction_layout_count == 2U,
      "final GPT may commit only after generated ESP and empty filesystem construction retire");
}

void test_shrink_missing_duplicate_or_tampered_construction_fails_prewrite() {
  using namespace ytec::imageformat;

  {
    MemoryTarget target;
    auto final_plan = shrink_generated_gpt_plan();
    auto constructions = shrink_construction_plans(final_plan);
    constructions.pop_back();
    TsumugiShrinkRestoreLayoutTransactionV1 transaction(
        std::move(final_plan), std::move(constructions), target);
    check(!transaction.prepare().has_value() && target.write_offsets.empty(),
          "missing empty-filesystem construction must fail before invalidation");
  }
  {
    MemoryTarget target;
    auto final_plan = shrink_generated_gpt_plan();
    auto constructions = shrink_construction_plans(final_plan);
    constructions[1].final_target_number = 1U;
    TsumugiShrinkRestoreLayoutTransactionV1 transaction(
        std::move(final_plan), std::move(constructions), target);
    check(!transaction.prepare().has_value() && target.write_offsets.empty(),
          "duplicate final construction key must fail before invalidation");
  }
  {
    MemoryTarget target;
    auto final_plan = shrink_generated_gpt_plan();
    auto constructions = shrink_construction_plans(final_plan);
    constructions[1].purpose =
        TsumugiShrinkConstructionPurposeV1::apply_file_image;
    TsumugiShrinkRestoreLayoutTransactionV1 transaction(
        std::move(final_plan), std::move(constructions), target);
    check(!transaction.prepare().has_value() && target.write_offsets.empty(),
          "tampered construction purpose must fail before invalidation");
  }
}

void test_shrink_retirement_failure_requires_abort_retry() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  auto final_plan = shrink_final_plan();
  auto constructions = shrink_construction_plans(final_plan);
  TsumugiShrinkRestoreLayoutTransactionV1 transaction(
      std::move(final_plan), std::move(constructions), target);
  check(transaction.prepare().has_value(), "shrink fixture prepare must pass");
  check(transaction.publish_construction(1U).has_value(),
        "shrink fixture publication must pass");
  target.corrupt_readback_once_at = 0U;
  check(!transaction.retire_construction(1U).has_value(),
        "retirement readback corruption must fail closed");
  check(
      !transaction.report().metadata_safely_withheld &&
          !transaction.report().final_partition_table_committed &&
          transaction.active_construction_plan() != nullptr,
      "unverified retirement must never claim a safely withheld or final layout");
  const auto writes_before_abort = target.write_offsets.size();
  transaction.abort();
  check(
      transaction.report().metadata_safely_withheld &&
          transaction.active_construction_plan() == nullptr &&
          target.write_offsets.size() > writes_before_abort,
      "abort must retry and verify exact temporary GPT retirement");
  const auto writes_after_abort = target.write_offsets.size();
  transaction.abort();
  check(
      target.write_offsets.size() == writes_after_abort,
      "repeated abort after verified retirement must not rewrite metadata");
}

void test_shrink_publication_failure_cleans_exact_metadata() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  auto final_plan = shrink_final_plan();
  auto constructions = shrink_construction_plans(final_plan);
  const auto failed_header =
      constructions.front().temporary_metadata.commit_writes.front().offset;
  TsumugiShrinkRestoreLayoutTransactionV1 transaction(
      std::move(final_plan), std::move(constructions), target);
  check(transaction.prepare().has_value(), "shrink fixture prepare must pass");
  target.corrupt_readback_once_at = failed_header;
  check(!transaction.publish_construction(1U).has_value(),
        "temporary GPT publication readback failure must fail closed");
  check(
      transaction.report().metadata_safely_withheld &&
          !transaction.report().temporary_layout_active &&
          !transaction.report().final_partition_table_committed &&
          transaction.active_construction_plan() == nullptr,
      "failed publication must exact-zero temporary metadata before returning");
  check(
      target.bytes()[0] == std::byte{0} &&
          target.bytes()[kSectorSize] == std::byte{0} &&
          target.bytes()[kTargetSize - kSectorSize] == std::byte{0},
      "failed temporary publication cleanup must remove both GPT headers and protective MBR");
}

void test_shrink_tampered_retirement_fails_before_any_write() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  auto final_plan = shrink_final_plan();
  auto constructions = shrink_construction_plans(final_plan);
  constructions.front().retirement_ranges.front().length += kSectorSize;
  TsumugiShrinkRestoreLayoutTransactionV1 transaction(
      std::move(final_plan), std::move(constructions), target);
  check(
      !transaction.prepare().has_value() && target.write_offsets.empty(),
      "tampered exact retirement ranges must fail before target invalidation");
}

void test_shrink_final_commit_failure_preserves_emergency_reinvalidation() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  auto final_plan = shrink_final_plan();
  auto constructions = shrink_construction_plans(final_plan);
  TsumugiShrinkRestoreLayoutTransactionV1 transaction(
      std::move(final_plan), std::move(constructions), target);
  check(transaction.prepare().has_value(), "shrink fixture prepare must pass");
  check(transaction.publish_construction(1U).has_value() &&
            transaction.retire_construction(1U).has_value() &&
            transaction.publish_construction(2U).has_value() &&
            transaction.retire_construction(2U).has_value(),
        "all temporary GPTs must publish and retire before final failure test");
  target.corrupt_readback_once_at = 0U;
  check(!transaction.commit_final().has_value(),
        "injected final MBR readback failure must fail closed");
  check(
      transaction.report().metadata_safely_withheld &&
          transaction.report().final_layout.emergency_reinvalidation_verified &&
          !transaction.report().final_partition_table_committed &&
          transaction.report().target_left_incomplete &&
          target.bytes()[0] == std::byte{0},
      "wrapper must preserve final transaction emergency reinvalidation semantics");
}

void test_shrink_migration_is_strictly_bound_to_final_metadata() {
  using namespace ytec::imageformat;
  MemoryTarget changed_size_target;
  auto changed_size = shrink_final_plan();
  auto changed_size_constructions = shrink_construction_plans(changed_size);
  changed_size.migration.target_partitions.front().size_bytes =
      256ULL * 1024ULL;
  TsumugiShrinkRestoreLayoutTransactionV1 changed_size_transaction(
      std::move(changed_size),
      std::move(changed_size_constructions),
      changed_size_target);
  check(
      !changed_size_transaction.prepare().has_value() &&
          changed_size_target.write_offsets.empty(),
      "migration size drift from final MBR metadata must fail before writes");

  MemoryTarget unaligned_target;
  auto unaligned = shrink_final_plan();
  auto unaligned_constructions = shrink_construction_plans(unaligned);
  unaligned.migration.target_partitions.front().offset_bytes += kSectorSize;
  TsumugiShrinkRestoreLayoutTransactionV1 unaligned_transaction(
      std::move(unaligned),
      std::move(unaligned_constructions),
      unaligned_target);
  check(
      !unaligned_transaction.prepare().has_value() &&
          unaligned_target.write_offsets.empty(),
      "migration offsets must honor reviewed alignment before target invalidation");
}

void test_shrink_gpt_role_and_identifiers_are_bound_before_write() {
  using namespace ytec::imageformat;
  MemoryTarget valid_target;
  auto valid = shrink_final_gpt_plan();
  auto valid_constructions = shrink_construction_plans(valid);
  TsumugiShrinkRestoreLayoutTransactionV1 valid_transaction(
      std::move(valid), std::move(valid_constructions), valid_target);
  check(valid_transaction.prepare().has_value(),
        "strictly matching final GPT and migration must prepare");
  valid_transaction.abort();

  MemoryTarget changed_type_target;
  auto changed_type = shrink_final_gpt_plan();
  std::get<ytec::clonecore::GptDisk>(changed_type.metadata.target_layout)
      .partitions.front()
      .type_guid = ytec::clonecore::gpt_type_efi_system();
  auto changed_type_constructions = shrink_construction_plans(changed_type);
  TsumugiShrinkRestoreLayoutTransactionV1 changed_type_transaction(
      std::move(changed_type),
      std::move(changed_type_constructions),
      changed_type_target);
  check(
      !changed_type_transaction.prepare().has_value() &&
          changed_type_target.write_offsets.empty(),
      "Windows role changed to an EFI GPT type must fail before writes");

  MemoryTarget duplicate_guid_target;
  auto duplicate_guid = shrink_final_gpt_plan();
  auto& duplicate_gpt =
      std::get<ytec::clonecore::GptDisk>(duplicate_guid.metadata.target_layout);
  duplicate_gpt.partitions.back().unique_guid =
      duplicate_gpt.partitions.front().unique_guid;
  auto duplicate_guid_constructions = shrink_construction_plans(duplicate_guid);
  TsumugiShrinkRestoreLayoutTransactionV1 duplicate_guid_transaction(
      std::move(duplicate_guid),
      std::move(duplicate_guid_constructions),
      duplicate_guid_target);
  check(
      !duplicate_guid_transaction.prepare().has_value() &&
          duplicate_guid_target.write_offsets.empty(),
      "duplicate final GPT identifiers must fail before writes");
}

void test_preserving_gpt_publishes_backup_first_without_touching_mbr() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  auto plan = preserving_gpt_plan(target);
  const std::vector<std::byte> original_mbr(
      target.bytes().begin(), target.bytes().begin() + kSectorSize);
  TsumugiPreservingPartitionLayoutTransactionV1 transaction(
      std::move(plan), target);
  const auto prepared = transaction.prepare();
  check(prepared.has_value() && target.write_offsets.empty(),
        "preserving prepare must only verify captured current metadata");
  const auto committed = transaction.commit();
  check(committed.has_value() &&
            committed.value().partition_table_committed &&
            committed.value().every_write_read_back_verified &&
            !committed.value().target_left_incomplete,
        "preserving GPT transaction must commit with complete evidence");
  check(target.write_offsets.size() == 4U &&
            target.write_offsets.front() > kTargetSize / 2U &&
            target.write_offsets[2] < kTargetSize / 2U &&
            std::equal(
                original_mbr.begin(), original_mbr.end(),
                target.bytes().begin()),
        "backup GPT must publish before primary and protective MBR must stay exact");
}

void test_preserving_gpt_failure_restores_exact_original_metadata() {
  using namespace ytec::imageformat;
  MemoryTarget target;
  auto plan = preserving_gpt_plan(target);
  const auto original = target.bytes();
  target.fail_write_once_at = plan.published_writes[2].offset;
  TsumugiPreservingPartitionLayoutTransactionV1 transaction(
      std::move(plan), target);
  check(transaction.prepare().has_value(),
        "preserving rollback fixture must prepare");
  const auto committed = transaction.commit();
  check(!committed.has_value() &&
            transaction.report().rollback_attempted &&
            transaction.report().rollback_read_back_verified &&
            transaction.report().original_layout_preserved_after_failure &&
            !transaction.report().target_left_incomplete,
        "failure after backup publication must verify an exact rollback");
  check(target.bytes() == original,
        "every primary and backup GPT metadata byte must match the capture");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"gpt_prepare_then_non_cancellable_commit_order",
       test_gpt_prepare_then_non_cancellable_commit_order},
      {"cancel_before_prepare_writes_nothing",
       test_cancel_before_prepare_writes_nothing},
      {"cancel_before_staging_leaves_target_unrecognizable",
       test_cancel_before_staging_leaves_target_unrecognizable},
      {"readback_failure_never_reaches_commit",
       test_readback_failure_never_reaches_commit},
      {"dimension_or_overlapping_plan_fails_before_write",
       test_dimension_or_overlapping_plan_fails_before_write},
      {"mbr_has_no_staging_and_one_final_sector",
       test_mbr_has_no_staging_and_one_final_sector},
      {"each_gpt_commit_boundary_reinvalidates_on_failure",
       test_each_gpt_commit_boundary_reinvalidates_on_failure},
      {"mbr_commit_failure_reinvalidates_sector_zero",
       test_mbr_commit_failure_reinvalidates_sector_zero},
      {"abort_retries_emergency_reinvalidation_idempotently",
       test_abort_retries_emergency_reinvalidation_idempotently},
      {"shrink_transaction_serializes_temporary_gpt_then_final_mbr",
       test_shrink_transaction_serializes_temporary_gpt_then_final_mbr},
      {"shrink_generated_esp_and_empty_filesystem_complete_by_final_key",
       test_shrink_generated_esp_and_empty_filesystem_complete_by_final_key},
      {"shrink_missing_duplicate_or_tampered_construction_fails_prewrite",
       test_shrink_missing_duplicate_or_tampered_construction_fails_prewrite},
      {"shrink_retirement_failure_requires_abort_retry",
       test_shrink_retirement_failure_requires_abort_retry},
      {"shrink_publication_failure_cleans_exact_metadata",
       test_shrink_publication_failure_cleans_exact_metadata},
      {"shrink_tampered_retirement_fails_before_any_write",
       test_shrink_tampered_retirement_fails_before_any_write},
      {"shrink_final_commit_failure_preserves_emergency_reinvalidation",
       test_shrink_final_commit_failure_preserves_emergency_reinvalidation},
      {"shrink_migration_is_strictly_bound_to_final_metadata",
       test_shrink_migration_is_strictly_bound_to_final_metadata},
      {"shrink_gpt_role_and_identifiers_are_bound_before_write",
       test_shrink_gpt_role_and_identifiers_are_bound_before_write},
      {"preserving_gpt_publishes_backup_first_without_touching_mbr",
       test_preserving_gpt_publishes_backup_first_without_touching_mbr},
      {"preserving_gpt_failure_restores_exact_original_metadata",
       test_preserving_gpt_failure_restores_exact_original_metadata},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

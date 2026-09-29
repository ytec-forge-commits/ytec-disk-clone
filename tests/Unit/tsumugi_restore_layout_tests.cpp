#include "ytec/clonecore/gpt.h"
#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi_restore_layout.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kSectorSize = 512U;
constexpr std::uint64_t kSourceSize = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kLargerTargetSize = 24ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;
constexpr std::uint64_t kShrinkTargetSize = 16ULL * kGiB;

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
    std::vector<std::byte>& bytes,
    const std::size_t offset,
    const T value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

class MemoryReader final : public ytec::clonecore::ISourceDiskReader {
 public:
  explicit MemoryReader(std::vector<std::byte> bytes)
      : bytes_(std::move(bytes)) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_READ_FAULT,
          .operation = L"合成ディスク読取り",
          .message = L"範囲外です",
      });
    }
    const auto first =
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            first, first + static_cast<std::ptrdiff_t>(length)));
  }

 private:
  std::vector<std::byte> bytes_;
};

class SequenceGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  explicit SequenceGuidGenerator(const std::uint8_t first) : next_(first) {}

  [[nodiscard]] ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    ytec::clonecore::GptGuid result;
    result.bytes[0] = static_cast<std::byte>(next_++);
    result.bytes[15] = std::byte{0xA5};
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(result);
  }

 private:
  std::uint8_t next_{};
};

class SequenceSignatureGenerator final
    : public ytec::clonecore::IMbrSignatureGenerator {
 public:
  explicit SequenceSignatureGenerator(std::vector<std::uint32_t> values)
      : values_(std::move(values)) {}

  [[nodiscard]] ytec::clonecore::Result<std::uint32_t>
  next_signature() override {
    if (next_ >= values_.size()) {
      return ytec::clonecore::Result<std::uint32_t>::failure({
          .code = ytec::clonecore::ErrorCode::io_failed,
          .native_code = ERROR_NO_MORE_ITEMS,
          .operation = L"合成署名生成",
          .message = L"値がありません",
      });
    }
    return ytec::clonecore::Result<std::uint32_t>::success(values_[next_++]);
  }

 private:
  std::vector<std::uint32_t> values_;
  std::size_t next_{};
};

ytec::clonecore::GptGuid guid(const std::uint8_t value) {
  ytec::clonecore::GptGuid result;
  result.bytes[0] = static_cast<std::byte>(value);
  result.bytes[15] = std::byte{0x5A};
  return result;
}

void apply_writes(
    std::vector<std::byte>& disk,
    const std::vector<ytec::clonecore::GptMetadataWrite>& writes) {
  for (const auto& write : writes) {
    check(
        write.offset <= disk.size() &&
            write.bytes.size() <= disk.size() - write.offset,
        "synthetic GPT metadata must fit");
    std::copy(
        write.bytes.begin(),
        write.bytes.end(),
        disk.begin() + static_cast<std::ptrdiff_t>(write.offset));
  }
}

struct GptFixture final {
  ytec::imageformat::TsumugiManifest manifest;
  ytec::clonecore::GptDisk source_layout;
};

GptFixture gpt_fixture(const bool select_second) {
  ytec::clonecore::GptDisk seed{
      .logical_sector_size = kSectorSize,
      .sector_count = kSourceSize / kSectorSize,
      .disk_guid = guid(0x10),
      .first_usable_lba = 34U,
      .last_usable_lba = kSourceSize / kSectorSize - 34U,
      .partition_entry_count = 128U,
      .partition_entry_size = 128U,
      .partitions = {
          ytec::clonecore::GptPartition{
              .entry_index = 0U,
              .type_guid = ytec::clonecore::gpt_type_basic_data(),
              .unique_guid = guid(0x20),
              .first_lba = 2048U,
              .last_lba = 8191U,
              .name = u"Data A",
          },
          ytec::clonecore::GptPartition{
              .entry_index = 3U,
              .type_guid = ytec::clonecore::gpt_type_basic_data(),
              .unique_guid = guid(0x21),
              .first_lba = 10240U,
              .last_lba = 14335U,
              .name = u"Data B",
          },
      },
  };
  SequenceGuidGenerator source_ids(1U);
  const auto source_plan = ytec::clonecore::make_gpt_write_plan(
      seed, kSourceSize, kSectorSize, source_ids);
  check(source_plan.has_value(), "source GPT plan must build");
  std::vector<std::byte> disk(
      static_cast<std::size_t>(kSourceSize), std::byte{0});
  apply_writes(disk, source_plan.value().writes);
  MemoryReader reader(std::move(disk));
  const auto snapshot = ytec::imageformat::capture_partition_snapshot_v1(
      reader, ytec::imageformat::PartitionTableStyle::gpt);
  check(snapshot.has_value(), "source GPT snapshot must build");

  ytec::imageformat::TsumugiManifest manifest{
      .mode = ytec::imageformat::TsumugiManifestMode::exact,
      .partition_style =
          ytec::imageformat::TsumugiManifestPartitionStyle::gpt,
      .flags = select_second
          ? ytec::imageformat::TsumugiManifestFlags::none
          : ytec::imageformat::TsumugiManifestFlags::partition_selection,
      .source_disk_size = kSourceSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-08T00:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = snapshot.value(),
  };
  manifest.source_model_hash.fill(std::byte{0x11});
  manifest.source_serial_hash.fill(std::byte{0x22});
  manifest.source_state_hash.fill(std::byte{0x33});
  for (std::size_t index = 0U;
       index < source_plan.value().target_disk.partitions.size(); ++index) {
    const auto& partition = source_plan.value().target_disk.partitions[index];
    const bool selected = index == 0U || select_second;
    const std::uint64_t offset = partition.first_lba * kSectorSize;
    const std::uint64_t length =
        (partition.last_lba - partition.first_lba + 1U) * kSectorSize;
    manifest.partitions.push_back({
        .source_table_index = partition.entry_index + 1U,
        .source_partition_number = static_cast<std::uint32_t>(index + 1U),
        .role = ytec::imageformat::TsumugiManifestPartitionRole::data,
        .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
        .flags = selected
            ? ytec::imageformat::TsumugiManifestPartitionFlags::selected
            : ytec::imageformat::TsumugiManifestPartitionFlags::none,
        .source_offset = offset,
        .source_size = length,
        .used_bytes = selected ? length : 0U,
        .minimum_target_bytes = selected ? length : 0U,
        .planned_target_bytes = selected ? length : 0U,
        .payload_logical_offset = selected ? offset : 0U,
        .payload_logical_length = selected ? length : 0U,
        .type_id = partition.type_guid.bytes,
        .unique_id = partition.unique_guid.bytes,
        .name_utf8 = index == 0U ? "Data A" : "Data B",
    });
  }
  check(
      ytec::imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
      "synthetic GPT manifest must be canonical");
  return GptFixture{
      .manifest = std::move(manifest),
      .source_layout = source_plan.value().target_disk,
  };
}

struct MbrFixture final {
  ytec::imageformat::TsumugiManifest manifest;
  std::uint32_t source_signature{};
};

MbrFixture mbr_fixture(const bool first_active = false) {
  std::vector<std::byte> disk(
      static_cast<std::size_t>(kSourceSize), std::byte{0});
  disk[0U] = std::byte{0xFA};
  constexpr std::uint32_t source_signature = 0x1234ABCDU;
  write_little(disk, 440U, source_signature);
  const auto write_partition = [&](const std::size_t table_index,
                                   const std::uint32_t first_lba,
                                   const std::uint32_t sectors) {
    const std::size_t offset = 446U + table_index * 16U;
    disk[offset + 4U] = std::byte{0x07};
    write_little(disk, offset + 8U, first_lba);
    write_little(disk, offset + 12U, sectors);
  };
  write_partition(0U, 2048U, 4096U);
  write_partition(2U, 8192U, 4096U);
  if (first_active) {
    disk[446U] = std::byte{0x80};
  }
  disk[510U] = std::byte{0x55};
  disk[511U] = std::byte{0xAA};
  MemoryReader reader(std::move(disk));
  const auto snapshot = ytec::imageformat::capture_partition_snapshot_v1(
      reader, ytec::imageformat::PartitionTableStyle::mbr);
  check(snapshot.has_value(), "source MBR snapshot must build");

  ytec::imageformat::TsumugiManifest manifest{
      .mode = ytec::imageformat::TsumugiManifestMode::exact,
      .partition_style =
          ytec::imageformat::TsumugiManifestPartitionStyle::mbr,
      .flags =
          ytec::imageformat::TsumugiManifestFlags::partition_selection,
      .source_disk_size = kSourceSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-08T00:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = snapshot.value(),
  };
  manifest.source_model_hash.fill(std::byte{0x41});
  manifest.source_serial_hash.fill(std::byte{0x42});
  manifest.source_state_hash.fill(std::byte{0x43});
  const auto add = [&](const std::uint32_t table_index,
                       const std::uint32_t partition_number,
                       const std::uint64_t offset,
                       const std::uint64_t length,
                       const bool selected) {
    ytec::imageformat::TsumugiManifestPartition partition{
        .source_table_index = table_index,
        .source_partition_number = partition_number,
        .role = ytec::imageformat::TsumugiManifestPartitionRole::data,
        .file_system = ytec::imageformat::TsumugiManifestFileSystem::ntfs,
        .flags = selected
            ? ytec::imageformat::TsumugiManifestPartitionFlags::selected
            : ytec::imageformat::TsumugiManifestPartitionFlags::none,
        .source_offset = offset,
        .source_size = length,
        .used_bytes = selected ? length : 0U,
        .minimum_target_bytes = selected ? length : 0U,
        .planned_target_bytes = selected ? length : 0U,
        .payload_logical_offset = selected ? offset : 0U,
        .payload_logical_length = selected ? length : 0U,
    };
    partition.type_id[0] = std::byte{0x07};
    manifest.partitions.push_back(std::move(partition));
  };
  add(1U, 1U, 2048ULL * kSectorSize, 4096ULL * kSectorSize, true);
  add(3U, 2U, 8192ULL * kSectorSize, 4096ULL * kSectorSize, false);
  if (first_active) {
    manifest.partitions[0].flags =
        manifest.partitions[0].flags |
        ytec::imageformat::TsumugiManifestPartitionFlags::active;
  }
  check(
      ytec::imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
      "synthetic MBR manifest must be canonical");
  return MbrFixture{
      .manifest = std::move(manifest),
      .source_signature = source_signature,
  };
}

bool partition_selected(
    const ytec::imageformat::TsumugiManifestPartition& partition) {
  return (static_cast<std::uint32_t>(partition.flags) &
          static_cast<std::uint32_t>(
              ytec::imageformat::TsumugiManifestPartitionFlags::selected)) !=
      0U;
}

void make_wim_payload(
    ytec::imageformat::TsumugiManifestPartition& partition,
    const std::uint64_t logical_offset,
    const std::uint64_t minimum_target_bytes) {
  partition.used_bytes = (std::min)(partition.source_size, 1ULL * kMiB);
  partition.minimum_target_bytes = minimum_target_bytes;
  partition.planned_target_bytes = minimum_target_bytes;
  partition.payload_logical_offset = logical_offset;
  partition.payload_logical_length = 4096U;
  partition.payload_encoding = ytec::imageformat::
      TsumugiManifestPayloadEncoding::microsoft_wim_single_image;
  partition.payload_format_version =
      ytec::imageformat::kTsumugiWimPayloadFormatVersion;
  partition.cluster_size = 4096U;
  if (partition.label_utf8.empty()) {
    partition.label_utf8 = partition.name_utf8;
  }
}

void make_data_shrink_manifest(
    ytec::imageformat::TsumugiManifest& manifest,
    const std::uint64_t first_minimum = 2ULL * kGiB) {
  using namespace ytec::imageformat;
  manifest.mode = TsumugiManifestMode::shrink;
  const bool has_unselected = std::any_of(
      manifest.partitions.begin(), manifest.partitions.end(),
      [](const TsumugiManifestPartition& partition) {
        return !partition_selected(partition);
      });
  manifest.flags = TsumugiManifestFlags::automatic_surplus_allocation;
  if (has_unselected) {
    manifest.flags = manifest.flags |
        TsumugiManifestFlags::partition_selection;
  }
  std::uint64_t logical_offset{};
  std::size_t selected_index{};
  for (auto& partition : manifest.partitions) {
    if (!partition_selected(partition)) {
      continue;
    }
    make_wim_payload(
        partition,
        logical_offset,
        selected_index == 0U ? first_minimum : 1ULL * kGiB);
    logical_offset += 4096U;
    ++selected_index;
  }
  check(
      build_tsumugi_manifest_v1(manifest).has_value(),
      "synthetic data shrink manifest must be canonical");
}

MbrFixture mbr_windows_shrink_fixture() {
  using namespace ytec::imageformat;
  auto fixture = mbr_fixture(true);
  auto& manifest = fixture.manifest;
  manifest.mode = TsumugiManifestMode::shrink;
  manifest.flags = TsumugiManifestFlags::source_contains_windows |
      TsumugiManifestFlags::automatic_surplus_allocation;
  auto& system = manifest.partitions[0];
  system.role = TsumugiManifestPartitionRole::bios_system;
  system.file_system = TsumugiManifestFileSystem::ntfs;
  system.flags = TsumugiManifestPartitionFlags::selected |
      TsumugiManifestPartitionFlags::required |
      TsumugiManifestPartitionFlags::active;
  system.name_utf8 = "System Reserved";
  system.label_utf8 = "System Reserved";
  make_wim_payload(system, 0U, 1ULL * kGiB);

  auto& windows = manifest.partitions[1];
  windows.role = TsumugiManifestPartitionRole::windows;
  windows.file_system = TsumugiManifestFileSystem::ntfs;
  windows.flags = TsumugiManifestPartitionFlags::selected |
      TsumugiManifestPartitionFlags::required |
      TsumugiManifestPartitionFlags::contains_windows;
  windows.name_utf8 = "Windows";
  windows.label_utf8 = "Windows";
  make_wim_payload(windows, 4096U, 11ULL * kGiB);
  check(
      build_tsumugi_manifest_v1(manifest).has_value(),
      "synthetic MBR Windows shrink manifest must be canonical");
  return fixture;
}

bool ranges_overlap(
    const ytec::imageformat::TsumugiRestoreLayoutWrite& left,
    const ytec::imageformat::TsumugiRestoreLayoutWrite& right) {
  return left.offset < right.offset + right.bytes.size() &&
      right.offset < left.offset + left.bytes.size();
}

void test_gpt_relocates_backup_and_commits_primary_last() {
  auto fixture = gpt_fixture(false);
  SequenceGuidGenerator target_ids(0x70U);
  SequenceSignatureGenerator unused_signatures({0x87654321U});
  const auto plan =
      ytec::imageformat::make_tsumugi_whole_disk_restore_layout_plan_v1(
          fixture.manifest,
          kLargerTargetSize,
          kSectorSize,
          target_ids,
          unused_signatures);
  check(plan.has_value(), "larger-target GPT restore layout must build");
  check(
      plan.value().staged_writes.size() == 2U &&
          plan.value().commit_writes.size() == 3U,
      "GPT must stage two entry arrays and reserve three commit writes");
  check(
      plan.value().commit_writes[0].kind ==
              ytec::imageformat::TsumugiRestoreLayoutWriteKind::
                  gpt_backup_header &&
          plan.value().commit_writes[1].kind ==
              ytec::imageformat::TsumugiRestoreLayoutWriteKind::
                  gpt_protective_mbr &&
          plan.value().commit_writes[2].kind ==
              ytec::imageformat::TsumugiRestoreLayoutWriteKind::
                  gpt_primary_header,
      "valid GPT headers must only appear in the non-cancellable commit order");
  check(
      plan.value().commit_writes[0].offset ==
          kLargerTargetSize - kSectorSize,
      "backup GPT header must move to the actual target end");
  const auto& target =
      std::get<ytec::clonecore::GptDisk>(plan.value().target_layout);
  check(
      target.disk_guid != fixture.source_layout.disk_guid &&
          target.partitions.size() == 1U &&
          target.partitions[0].unique_guid !=
              fixture.source_layout.partitions[0].unique_guid,
      "target GPT IDs must be fresh and unselected partitions must be absent");
  check(
      plan.value().invalidation_ranges.size() == 2U &&
          plan.value().invalidation_ranges.front().offset == 0U &&
          plan.value().invalidation_ranges.back().offset ==
              kLargerTargetSize - 1024U * 1024U,
      "both metadata ends must be invalidated before payload writes");

  std::vector<ytec::imageformat::TsumugiRestoreLayoutWrite> all =
      plan.value().staged_writes;
  all.insert(
      all.end(),
      plan.value().commit_writes.begin(),
      plan.value().commit_writes.end());
  for (std::size_t left = 0U; left < all.size(); ++left) {
    check(
        all[left].offset <= kLargerTargetSize &&
            all[left].bytes.size() <= kLargerTargetSize - all[left].offset,
        "every GPT metadata write must remain in target bounds");
    for (std::size_t right = left + 1U; right < all.size(); ++right) {
      check(
          !ranges_overlap(all[left], all[right]),
          "GPT metadata writes must not overlap");
    }
  }
}

void test_mbr_data_disk_filters_unselected_and_regenerates_signature() {
  auto fixture = mbr_fixture();
  SequenceGuidGenerator unused_ids(1U);
  constexpr std::uint32_t connected_signature = 0xAABBCCDDU;
  constexpr std::uint32_t target_signature = 0x87654321U;
  SequenceSignatureGenerator signatures(
      {fixture.source_signature, connected_signature, target_signature});
  const std::array<std::uint32_t, 1U> connected{connected_signature};
  const auto plan =
      ytec::imageformat::make_tsumugi_whole_disk_restore_layout_plan_v1(
          fixture.manifest,
          kLargerTargetSize,
          kSectorSize,
          unused_ids,
          signatures,
          connected);
  check(plan.has_value(), "MBR data-disk restore layout must build");
  const auto& target =
      std::get<ytec::clonecore::MbrDisk>(plan.value().target_layout);
  check(
      target.partitions.size() == 1U &&
          target.disk_signature == target_signature &&
          !target.partitions[0].active,
      "MBR data restore must omit unselected entries and use a unique signature");
  check(
      plan.value().staged_writes.empty() &&
          plan.value().commit_writes.size() == 1U &&
          plan.value().commit_writes[0].kind ==
              ytec::imageformat::TsumugiRestoreLayoutWriteKind::mbr_sector,
      "MBR must remain absent until its one-sector commit");
}

void test_manifest_snapshot_drift_is_rejected() {
  auto fixture = gpt_fixture(true);
  fixture.manifest.partitions[0].unique_id[7] ^= std::byte{0x40};
  SequenceGuidGenerator ids(0x70U);
  SequenceSignatureGenerator signatures({0x87654321U});
  const auto plan =
      ytec::imageformat::make_tsumugi_whole_disk_restore_layout_plan_v1(
          fixture.manifest,
          kLargerTargetSize,
          kSectorSize,
          ids,
          signatures);
  check(
      !plan.has_value() &&
          plan.error().code == ytec::clonecore::ErrorCode::identity_mismatch,
      "authenticated manifest/snapshot identifier drift must fail closed");
}

void test_smaller_or_sector_changed_target_is_rejected() {
  const auto fixture = gpt_fixture(true);
  SequenceGuidGenerator ids(0x70U);
  SequenceSignatureGenerator signatures({0x87654321U});
  check(
      !ytec::imageformat::make_tsumugi_whole_disk_restore_layout_plan_v1(
           fixture.manifest,
           kSourceSize - kSectorSize,
           kSectorSize,
           ids,
           signatures)
           .has_value(),
      "exact restore to a smaller target must be rejected");
  SequenceGuidGenerator ids2(0x70U);
  SequenceSignatureGenerator signatures2({0x87654321U});
  check(
      !ytec::imageformat::make_tsumugi_whole_disk_restore_layout_plan_v1(
           fixture.manifest,
           kLargerTargetSize,
           4096U,
           ids2,
           signatures2)
           .has_value(),
      "logical-sector changes must be rejected from the exact block path");
}

void test_shrink_manifest_requires_shrink_layout_engine() {
  auto fixture = gpt_fixture(true);
  fixture.manifest.mode = ytec::imageformat::TsumugiManifestMode::shrink;
  SequenceGuidGenerator ids(0x70U);
  SequenceSignatureGenerator signatures({0x87654321U});
  const auto plan =
      ytec::imageformat::make_tsumugi_whole_disk_restore_layout_plan_v1(
          fixture.manifest,
          kLargerTargetSize,
          kSectorSize,
          ids,
          signatures);
  check(
      !plan.has_value() &&
          plan.error().code == ytec::clonecore::ErrorCode::unsupported_layout,
      "shrink images must not enter the exact block-layout planner");
}

void test_shrink_gpt_data_layout_honors_manifest_floor_and_4kn_target() {
  using namespace ytec;
  auto fixture = gpt_fixture(true);
  make_data_shrink_manifest(fixture.manifest);
  SequenceGuidGenerator ids(0x70U);
  SequenceSignatureGenerator unused_signatures({0x87654321U});
  const auto plan = imageformat::
      make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
          fixture.manifest,
          kShrinkTargetSize,
          4096U,
          imageformat::TsumugiManifestPartitionStyle::gpt,
          false,
          ids,
          unused_signatures);
  check(
      plan.has_value(),
      "file-archive-only GPT shrink should support a 512-to-4Kn target");
  check(
      plan.value().migration.source_remains_unchanged &&
          !plan.value().migration.boot_finalization_required &&
          plan.value().migration.target_partitions.size() == 2U &&
          plan.value().migration.target_partitions[0].size_bytes >=
              2ULL * kGiB,
      "authenticated minimum bytes must raise the generic target floor");
  check(
      plan.value().metadata.style ==
              imageformat::PartitionTableStyle::gpt &&
          plan.value().metadata.logical_sector_size == 4096U &&
          plan.value().metadata.staged_writes.size() == 2U &&
          plan.value().metadata.commit_writes.size() == 3U,
      "4Kn GPT must keep entry staging and three-step final commit");
  const auto& target = std::get<clonecore::GptDisk>(
      plan.value().metadata.target_layout);
  check(
      target.partitions.size() == 2U &&
          target.partitions[0].type_guid ==
              fixture.source_layout.partitions[0].type_guid &&
          target.partitions[1].type_guid ==
              fixture.source_layout.partitions[1].type_guid,
      "same-style GPT shrink must preserve authenticated partition types");
}

void test_shrink_mbr_data_layout_preserves_bootstrap_and_commits_last() {
  using namespace ytec;
  auto fixture = mbr_fixture();
  make_data_shrink_manifest(fixture.manifest);
  SequenceGuidGenerator unused_ids(1U);
  constexpr std::uint32_t connected_signature = 0xAABBCCDDU;
  constexpr std::uint32_t target_signature = 0x87654321U;
  SequenceSignatureGenerator signatures(
      {fixture.source_signature, connected_signature, target_signature});
  const std::array<std::uint32_t, 1U> connected{connected_signature};
  const auto plan = imageformat::
      make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
          fixture.manifest,
          kShrinkTargetSize,
          kSectorSize,
          imageformat::TsumugiManifestPartitionStyle::mbr,
          false,
          unused_ids,
          signatures,
          connected);
  check(plan.has_value(), "MBR data shrink layout should build");
  const auto& target = std::get<clonecore::MbrDisk>(
      plan.value().metadata.target_layout);
  check(
      target.partitions.size() == 1U,
      "MBR shrink must omit unselected data partitions");
  check(
      target.bootstrap[0U] == std::byte{0xFA},
      "MBR shrink must retain the authenticated bootstrap");
  check(
      target.disk_signature == target_signature,
      "MBR shrink must use a fresh non-conflicting disk signature");
  check(
      target.partitions[0].type == 0x07U &&
          !target.partitions[0].active,
      "rebuilt NTFS data must use type 07 and stay non-active");
  check(
      plan.value().metadata.staged_writes.empty() &&
          plan.value().metadata.commit_writes.size() == 1U &&
          plan.value().metadata.commit_writes[0].kind ==
              imageformat::TsumugiRestoreLayoutWriteKind::mbr_sector,
      "the valid MBR sector must remain the sole final commit write");
}

void test_shrink_mbr_windows_to_gpt_generates_esp_and_msr() {
  using namespace ytec;
  auto fixture = mbr_windows_shrink_fixture();
  SequenceGuidGenerator ids(0x80U);
  SequenceSignatureGenerator unused_signatures({0x87654321U});
  const auto plan = imageformat::
      make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
          fixture.manifest,
          kShrinkTargetSize,
          kSectorSize,
          imageformat::TsumugiManifestPartitionStyle::gpt,
          true,
          ids,
          unused_signatures);
  check(plan.has_value(), "AMD64 MBR Windows shrink should plan MBR-to-GPT");
  const auto& migration = plan.value().migration;
  check(
      migration.boot_finalization_required &&
          migration.target_partitions.size() == 3U &&
          migration.target_partitions[0].role ==
              migrationcore::MigrationPartitionRole::efi_system &&
          migration.target_partitions[1].role ==
              migrationcore::MigrationPartitionRole::microsoft_reserved &&
          migration.target_partitions[2].role ==
              migrationcore::MigrationPartitionRole::windows &&
          migration.target_partitions[2].source_table_index == 3U,
      "MBR BIOS content must be replaced by generated ESP/MSR plus Windows");
  const auto& target = std::get<clonecore::GptDisk>(
      plan.value().metadata.target_layout);
  check(
      target.partitions.size() == 3U &&
          target.partitions[0].type_guid ==
              clonecore::gpt_type_efi_system() &&
          target.partitions[1].type_guid ==
              clonecore::gpt_type_microsoft_reserved() &&
          target.partitions[2].type_guid ==
              clonecore::gpt_type_basic_data(),
      "MBR-to-GPT metadata must use canonical ESP/MSR/basic-data types");
}

void test_shrink_unsafe_conversion_and_bitlocker_fail_closed() {
  using namespace ytec;
  auto gpt = gpt_fixture(true);
  make_data_shrink_manifest(gpt.manifest);
  SequenceGuidGenerator ids(0x70U);
  SequenceSignatureGenerator signatures({0x87654321U});
  check(
      !imageformat::make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
           gpt.manifest,
           kShrinkTargetSize,
           kSectorSize,
           imageformat::TsumugiManifestPartitionStyle::mbr,
           false,
           ids,
           signatures)
           .has_value(),
      "GPT-to-MBR must fail before any target I/O");

  auto raw = mbr_fixture();
  auto& manifest = raw.manifest;
  manifest.mode = imageformat::TsumugiManifestMode::shrink;
  manifest.flags = imageformat::TsumugiManifestFlags::partition_selection;
  auto& partition = manifest.partitions[0];
  partition.file_system =
      imageformat::TsumugiManifestFileSystem::unknown;
  partition.used_bytes = partition.source_size;
  partition.minimum_target_bytes = partition.source_size;
  partition.planned_target_bytes = partition.source_size;
  partition.payload_logical_offset = 0U;
  partition.payload_logical_length = partition.source_size;
  partition.payload_encoding =
      imageformat::TsumugiManifestPayloadEncoding::exact_raw;
  partition.payload_format_version = 0U;
  partition.cluster_size = 0U;
  check(
      imageformat::build_tsumugi_manifest_v1(manifest).has_value(),
      "synthetic exact RAW shrink manifest must be canonical");
  SequenceGuidGenerator raw_ids(0x90U);
  SequenceSignatureGenerator raw_signatures({0x87654321U});
  check(
      !imageformat::make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
           manifest,
           kShrinkTargetSize,
           kSectorSize,
           imageformat::TsumugiManifestPartitionStyle::gpt,
           false,
           raw_ids,
           raw_signatures)
           .has_value(),
      "exact RAW must not be reinterpreted through MBR-to-GPT conversion");

  gpt.manifest.flags = gpt.manifest.flags |
      imageformat::TsumugiManifestFlags::bitlocker_source_was_unlocked;
  gpt.manifest.partitions[0].flags =
      gpt.manifest.partitions[0].flags |
      imageformat::TsumugiManifestPartitionFlags::bitlocker_was_unlocked;
  check(
      imageformat::build_tsumugi_manifest_v1(gpt.manifest).has_value(),
      "BitLocker-marked fixture must remain structurally canonical");
  SequenceGuidGenerator bitlocker_ids(0xA0U);
  SequenceSignatureGenerator bitlocker_signatures({0x87654321U});
  check(
      !imageformat::make_tsumugi_shrink_whole_disk_restore_layout_plan_v1(
           gpt.manifest,
           kShrinkTargetSize,
           4096U,
           imageformat::TsumugiManifestPartitionStyle::gpt,
           false,
           bitlocker_ids,
           bitlocker_signatures)
           .has_value(),
      "the current unimplemented BitLocker shrink slice must fail closed");
}

void test_gpt_add_partition_plan_preserves_existing_identity() {
  auto fixture = gpt_fixture(true);
  const auto original_disk_guid = fixture.source_layout.disk_guid;
  const auto original_first_guid =
      fixture.source_layout.partitions.front().unique_guid;
  SequenceGuidGenerator ids(0xD0U);
  const auto plan = ytec::clonecore::make_gpt_add_partition_plan(
      fixture.source_layout,
      {
          .first_lba = 16'384U,
          .sector_count = 4'096U,
          .type_guid = ytec::clonecore::gpt_type_basic_data(),
          .name = u"Restored data",
      },
      ids);
  check(plan.has_value(), "A non-overlapping GPT gap should accept one entry");
  check(
      plan.value().target_disk.disk_guid == original_disk_guid &&
          plan.value().target_disk.partitions.size() == 3U &&
          plan.value().target_disk.partitions.front().unique_guid ==
              original_first_guid &&
          plan.value().target_disk.partitions.back().entry_index == 1U &&
          plan.value().writes.size() == 4U &&
          std::none_of(
              plan.value().writes.begin(),
              plan.value().writes.end(),
              [](const ytec::clonecore::GptMetadataWrite& write) {
                return write.kind ==
                    ytec::clonecore::GptMetadataKind::protective_mbr;
              }),
      "Preserving GPT addition must keep all old IDs and omit the protective MBR");
  SequenceGuidGenerator overlap_ids(0xE0U);
  check(
      !ytec::clonecore::make_gpt_add_partition_plan(
           fixture.source_layout,
           {
               .first_lba = 4'096U,
               .sector_count = 4'096U,
               .type_guid = ytec::clonecore::gpt_type_basic_data(),
           },
           overlap_ids)
           .has_value(),
      "An overlapping preserving GPT addition must fail closed");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"gpt_relocates_backup_and_commits_primary_last",
       test_gpt_relocates_backup_and_commits_primary_last},
      {"mbr_data_disk_filters_unselected_and_regenerates_signature",
       test_mbr_data_disk_filters_unselected_and_regenerates_signature},
      {"manifest_snapshot_drift_is_rejected",
       test_manifest_snapshot_drift_is_rejected},
      {"smaller_or_sector_changed_target_is_rejected",
       test_smaller_or_sector_changed_target_is_rejected},
      {"shrink_manifest_requires_shrink_layout_engine",
       test_shrink_manifest_requires_shrink_layout_engine},
      {"shrink_gpt_data_layout_honors_manifest_floor_and_4kn_target",
       test_shrink_gpt_data_layout_honors_manifest_floor_and_4kn_target},
      {"shrink_mbr_data_layout_preserves_bootstrap_and_commits_last",
       test_shrink_mbr_data_layout_preserves_bootstrap_and_commits_last},
      {"shrink_mbr_windows_to_gpt_generates_esp_and_msr",
       test_shrink_mbr_windows_to_gpt_generates_esp_and_msr},
      {"shrink_unsafe_conversion_and_bitlocker_fail_closed",
       test_shrink_unsafe_conversion_and_bitlocker_fail_closed},
      {"gpt_add_partition_plan_preserves_existing_identity",
       test_gpt_add_partition_plan_preserves_existing_identity},
  };
  int failures = 0;
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

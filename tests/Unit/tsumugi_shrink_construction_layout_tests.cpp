#include "ytec/imageformat/tsumugi_restore_layout.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kTargetSize = 128ULL * kMiB;
constexpr std::uint32_t kSectorSize = 512U;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::GptGuid guid(const std::byte seed) {
  ytec::clonecore::GptGuid value;
  value.bytes.fill(seed);
  return value;
}

class SequenceGuidGenerator final : public ytec::clonecore::IGuidGenerator {
 public:
  explicit SequenceGuidGenerator(std::vector<ytec::clonecore::GptGuid> values)
      : values_(std::move(values)) {}

  [[nodiscard]] ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    if (next_ >= values_.size()) {
      return ytec::clonecore::Result<ytec::clonecore::GptGuid>::failure(
          ytec::clonecore::Error{
              .code = ytec::clonecore::ErrorCode::internal_error,
              .operation = L"合成GUID",
              .message = L"GUIDが不足しています",
          });
    }
    return ytec::clonecore::Result<ytec::clonecore::GptGuid>::success(
        values_[next_++]);
  }

 private:
  std::vector<ytec::clonecore::GptGuid> values_;
  std::size_t next_{};
};

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1 final_plan(
    const ytec::migrationcore::MigrationPartitionStyle style =
        ytec::migrationcore::MigrationPartitionStyle::mbr) {
  using namespace ytec;
  imageformat::TsumugiWholeDiskRestoreLayoutPlan metadata{
      .style = style == migrationcore::MigrationPartitionStyle::gpt
          ? imageformat::PartitionTableStyle::gpt
          : imageformat::PartitionTableStyle::mbr,
      .target_size_bytes = kTargetSize,
      .logical_sector_size = kSectorSize,
      .invalidation_ranges = {
          {.offset = 0U, .length = kMiB},
          {.offset = kTargetSize - kMiB, .length = kMiB},
      },
  };
  if (style == migrationcore::MigrationPartitionStyle::gpt) {
    metadata.target_layout = clonecore::GptDisk{
        .logical_sector_size = kSectorSize,
        .sector_count = kTargetSize / kSectorSize,
        .partition_entry_count = 128U,
        .partition_entry_size = 128U,
    };
  } else {
    metadata.target_layout = clonecore::MbrDisk{
        .logical_sector_size = kSectorSize,
        .sector_count = kTargetSize / kSectorSize,
    };
  }
  return {
      .migration = {
          .target_style = style,
          .alignment_bytes = kMiB,
          .minimum_target_size_bytes = 64ULL * kMiB,
          .target_size_bytes = kTargetSize,
          .unallocated_tail_bytes = 64ULL * kMiB,
          .source_remains_unchanged = true,
          .target_partitions = {
              {
                  .target_number = 1U,
                  .source_table_index = 1U,
                  .role = migrationcore::MigrationPartitionRole::windows,
                  .file_system = migrationcore::MigrationFileSystem::ntfs,
                  .action = migrationcore::MigrationPartitionAction::
                      apply_file_image,
                  .offset_bytes = 1ULL * kMiB,
                  .size_bytes = 16ULL * kMiB,
              },
              {
                  .target_number = 2U,
                  .source_table_index = 2U,
                  .role = migrationcore::MigrationPartitionRole::data,
                  .file_system =
                      migrationcore::MigrationFileSystem::unsupported,
                  .action = migrationcore::MigrationPartitionAction::
                      copy_exact_raw,
                  .offset_bytes = 24ULL * kMiB,
                  .size_bytes = 8ULL * kMiB,
              },
              {
                  .target_number = 3U,
                  .source_table_index = 3U,
                  .role = migrationcore::MigrationPartitionRole::data,
                  .file_system = migrationcore::MigrationFileSystem::exfat,
                  .action = migrationcore::MigrationPartitionAction::
                      apply_file_image,
                  .offset_bytes = 40ULL * kMiB,
                  .size_bytes = 16ULL * kMiB,
              },
          },
      },
      .metadata = std::move(metadata),
  };
}

SequenceGuidGenerator generator() {
  return SequenceGuidGenerator({
      guid(std::byte{0x11}), guid(std::byte{0x12}),
      guid(std::byte{0x21}), guid(std::byte{0x22}),
  });
}

SequenceGuidGenerator extended_generator() {
  return SequenceGuidGenerator({
      guid(std::byte{0x11}), guid(std::byte{0x12}),
      guid(std::byte{0x21}), guid(std::byte{0x22}),
      guid(std::byte{0x31}), guid(std::byte{0x32}),
  });
}

ytec::imageformat::TsumugiShrinkWholeDiskRestoreLayoutPlanV1
generated_and_empty_plan() {
  using namespace ytec;
  auto result = final_plan(migrationcore::MigrationPartitionStyle::gpt);
  result.migration.target_partitions = {
      {
          .target_number = 1U,
          .role = migrationcore::MigrationPartitionRole::efi_system,
          .file_system = migrationcore::MigrationFileSystem::fat32,
          .action = migrationcore::MigrationPartitionAction::create_fat32,
          .offset_bytes = 1ULL * kMiB,
          .size_bytes = 4ULL * kMiB,
      },
      {
          .target_number = 2U,
          .role = migrationcore::MigrationPartitionRole::microsoft_reserved,
          .file_system = migrationcore::MigrationFileSystem::none,
          .action = migrationcore::MigrationPartitionAction::create_reserved,
          .offset_bytes = 8ULL * kMiB,
          .size_bytes = 4ULL * kMiB,
      },
      {
          .target_number = 3U,
          .source_table_index = 7U,
          .role = migrationcore::MigrationPartitionRole::data,
          .file_system = migrationcore::MigrationFileSystem::ntfs,
          .action =
              migrationcore::MigrationPartitionAction::create_empty_ntfs,
          .offset_bytes = 16ULL * kMiB,
          .size_bytes = 16ULL * kMiB,
      },
      {
          .target_number = 4U,
          .source_table_index = 8U,
          .role = migrationcore::MigrationPartitionRole::data,
          .file_system = migrationcore::MigrationFileSystem::exfat,
          .action = migrationcore::MigrationPartitionAction::apply_file_image,
          .offset_bytes = 40ULL * kMiB,
          .size_bytes = 16ULL * kMiB,
      },
      {
          .target_number = 5U,
          .source_table_index = 9U,
          .role = migrationcore::MigrationPartitionRole::data,
          .file_system = migrationcore::MigrationFileSystem::unsupported,
          .action = migrationcore::MigrationPartitionAction::copy_exact_raw,
          .offset_bytes = 64ULL * kMiB,
          .size_bytes = 8ULL * kMiB,
      },
  };
  return result;
}

void test_mbr_final_uses_one_nonbootable_temporary_gpt_per_archive() {
  using namespace ytec;
  auto ids = generator();
  const auto plans = imageformat::
      make_tsumugi_shrink_construction_layout_plans_v1(
          final_plan(), ids);
  check(plans.has_value() && plans.value().size() == 2U,
        "only file-archive partitions need temporary layouts");
  const std::array<std::uint32_t, 2U> expected_sources{1U, 3U};
  const std::array<std::uint64_t, 2U> expected_offsets{
      1ULL * kMiB, 40ULL * kMiB};
  std::set<std::array<std::byte, 16U>> observed_guids;
  for (std::size_t index = 0U; index < plans.value().size(); ++index) {
    const auto& plan = plans.value()[index];
    check(plan.source_table_index == expected_sources[index] &&
              plan.final_target_number == (index == 0U ? 1U : 3U) &&
              plan.purpose == imageformat::
                  TsumugiShrinkConstructionPurposeV1::apply_file_image &&
              plan.target_offset == expected_offsets[index] &&
              plan.temporary_metadata.style ==
                  imageformat::PartitionTableStyle::gpt,
          "temporary GPT must bind the reviewed archive placement");
    const auto* gpt = std::get_if<clonecore::GptDisk>(
        &plan.temporary_metadata.target_layout);
    if (gpt == nullptr) {
      throw std::runtime_error("temporary layout must be GPT");
    }
    check(gpt->partitions.size() == 1U,
          "temporary layout must expose exactly one GPT partition");
    const auto& partition = gpt->partitions.front();
    check(partition.entry_index == 0U &&
              partition.type_guid == clonecore::gpt_type_basic_data() &&
              partition.attributes == imageformat::
                  kTsumugiConstructionNoDefaultDriveLetterAttribute &&
              partition.name.starts_with(u"YTEC-Tsumugi-INCOMPLETE-") &&
              partition.first_lba * kSectorSize == plan.target_offset &&
              (partition.last_lba + 1U) * kSectorSize ==
                  plan.target_offset + plan.target_size,
          "temporary partition must be Basic Data, non-default-mounted, marked incomplete, and exact-range");
    check(observed_guids.insert(gpt->disk_guid.bytes).second &&
              observed_guids.insert(partition.unique_guid.bytes).second,
          "all construction-only disk and partition GUIDs must be unique");
    check(plan.retirement_ranges.size() == 2U &&
              plan.retirement_ranges.front().offset == 0U &&
              plan.retirement_ranges.front().length <= plan.target_offset &&
              plan.target_offset + plan.target_size <=
                  plan.retirement_ranges.back().offset &&
              plan.retirement_ranges.back().offset +
                      plan.retirement_ranges.back().length ==
                  kTargetSize,
          "retirement must erase only GPT metadata outside restored payload");
    const auto protective = std::find_if(
        plan.temporary_metadata.commit_writes.begin(),
        plan.temporary_metadata.commit_writes.end(),
        [](const auto& write) {
          return write.kind == imageformat::
              TsumugiRestoreLayoutWriteKind::gpt_protective_mbr;
        });
    check(protective != plan.temporary_metadata.commit_writes.end() &&
              protective->bytes.size() == kSectorSize &&
              protective->bytes[446U] == std::byte{0x00} &&
              protective->bytes[450U] == std::byte{0xEE},
          "temporary protective MBR must be non-active and expose no BIOS boot partition");
  }
}

void test_generated_esp_and_empty_filesystem_get_construction_plans() {
  using namespace ytec;
  auto ids = extended_generator();
  const auto plans = imageformat::
      make_tsumugi_shrink_construction_layout_plans_v1(
          generated_and_empty_plan(), ids);
  check(plans.has_value() && plans.value().size() == 3U,
        "generated ESP, empty filesystem, and file image need construction; MSR and RAW do not");
  const auto& esp = plans.value()[0];
  const auto& empty = plans.value()[1];
  const auto& archive = plans.value()[2];
  check(esp.final_target_number == 1U &&
            !esp.source_table_index.has_value() &&
            esp.purpose == imageformat::
                TsumugiShrinkConstructionPurposeV1::prepare_efi_system,
        "generated ESP must use a source-free, final-target-keyed preparation plan");
  check(empty.final_target_number == 3U &&
            empty.source_table_index == 7U &&
            empty.purpose == imageformat::
                TsumugiShrinkConstructionPurposeV1::
                    recreate_empty_file_system,
        "empty NTFS must use a filesystem-recreation construction plan");
  check(archive.final_target_number == 4U &&
            archive.source_table_index == 8U &&
            archive.purpose == imageformat::
                TsumugiShrinkConstructionPurposeV1::apply_file_image,
        "file archive must retain its explicit apply purpose");
  check(std::none_of(
            plans.value().begin(), plans.value().end(), [](const auto& plan) {
              return plan.final_target_number == 2U ||
                  plan.final_target_number == 5U;
        }),
        "generated MSR and exact RAW must never receive temporary volume layouts");

  const std::array<std::pair<
      migrationcore::MigrationPartitionAction,
      migrationcore::MigrationFileSystem>,
      2U>
      other_empty_file_systems{
          std::pair{
              migrationcore::MigrationPartitionAction::create_empty_exfat,
              migrationcore::MigrationFileSystem::exfat},
          std::pair{
              migrationcore::MigrationPartitionAction::create_empty_fat32,
              migrationcore::MigrationFileSystem::fat32},
      };
  for (const auto& [action, file_system] : other_empty_file_systems) {
    auto reviewed = generated_and_empty_plan();
    reviewed.migration.target_partitions[2].action = action;
    reviewed.migration.target_partitions[2].file_system = file_system;
    auto other_ids = extended_generator();
    const auto other = imageformat::
        make_tsumugi_shrink_construction_layout_plans_v1(
            reviewed, other_ids);
    check(other.has_value() && other.value().size() == 3U &&
              other.value()[1].purpose == imageformat::
                  TsumugiShrinkConstructionPurposeV1::
                      recreate_empty_file_system,
          "NTFS, exFAT, and FAT32 empty filesystems must share the verified recreation construction purpose");
  }
}

void test_missing_or_mismatched_construction_identity_fails_closed() {
  auto missing_source = generated_and_empty_plan();
  missing_source.migration.target_partitions[2].source_table_index.reset();
  auto missing_ids = extended_generator();
  check(!ytec::imageformat::
             make_tsumugi_shrink_construction_layout_plans_v1(
                 missing_source, missing_ids)
             .has_value(),
        "empty filesystem construction must remain bound to its source payload");

  auto mismatched_action = generated_and_empty_plan();
  mismatched_action.migration.target_partitions[2].action =
      ytec::migrationcore::MigrationPartitionAction::create_empty_exfat;
  auto mismatch_ids = extended_generator();
  check(!ytec::imageformat::
             make_tsumugi_shrink_construction_layout_plans_v1(
                 mismatched_action, mismatch_ids)
             .has_value(),
        "empty filesystem action must match the reviewed filesystem");

  auto duplicate_target = generated_and_empty_plan();
  duplicate_target.migration.target_partitions[3].target_number = 3U;
  auto duplicate_ids = extended_generator();
  check(!ytec::imageformat::
             make_tsumugi_shrink_construction_layout_plans_v1(
                 duplicate_target, duplicate_ids)
             .has_value(),
        "duplicate final target construction keys must fail closed");
}

void test_gpt_final_also_uses_fresh_construction_only_identifiers() {
  auto ids = generator();
  const auto plans = ytec::imageformat::
      make_tsumugi_shrink_construction_layout_plans_v1(
          final_plan(ytec::migrationcore::MigrationPartitionStyle::gpt),
          ids);
  check(plans.has_value() && plans.value().size() == 2U,
        "GPT final plan should retain one-at-a-time construction isolation");
}

void test_overlap_or_unaligned_final_plan_fails_closed() {
  auto overlap = final_plan();
  overlap.migration.target_partitions[2].offset_bytes = 16ULL * kMiB;
  auto ids = generator();
  check(!ytec::imageformat::
             make_tsumugi_shrink_construction_layout_plans_v1(
                 overlap, ids)
             .has_value(),
        "overlapping reviewed partitions must be rejected");

  auto unaligned = final_plan();
  unaligned.migration.target_partitions[0].offset_bytes += 1U;
  auto other_ids = generator();
  check(!ytec::imageformat::
             make_tsumugi_shrink_construction_layout_plans_v1(
                 unaligned, other_ids)
             .has_value(),
        "unaligned reviewed partition must be rejected");
}

void test_duplicate_construction_guid_fails_closed() {
  SequenceGuidGenerator repeated({
      guid(std::byte{0x33}), guid(std::byte{0x34}),
      guid(std::byte{0x33}), guid(std::byte{0x44}),
  });
  check(!ytec::imageformat::
             make_tsumugi_shrink_construction_layout_plans_v1(
                 final_plan(), repeated)
             .has_value(),
        "construction GUID reuse across temporary layouts must be rejected");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"mbr_final_uses_one_nonbootable_temporary_gpt_per_archive",
       test_mbr_final_uses_one_nonbootable_temporary_gpt_per_archive},
      {"gpt_final_also_uses_fresh_construction_only_identifiers",
       test_gpt_final_also_uses_fresh_construction_only_identifiers},
      {"generated_esp_and_empty_filesystem_get_construction_plans",
       test_generated_esp_and_empty_filesystem_get_construction_plans},
      {"missing_or_mismatched_construction_identity_fails_closed",
       test_missing_or_mismatched_construction_identity_fails_closed},
      {"overlap_or_unaligned_final_plan_fails_closed",
       test_overlap_or_unaligned_final_plan_fails_closed},
      {"duplicate_construction_guid_fails_closed",
       test_duplicate_construction_guid_fails_closed},
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

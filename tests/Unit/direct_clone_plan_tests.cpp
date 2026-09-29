#include "ytec/migrationcore/direct_clone_plan.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

ytec::migrationcore::DirectCloneSourcePartition partition(
    const std::uint32_t index,
    const ytec::migrationcore::MigrationPartitionRole role,
    const ytec::migrationcore::MigrationFileSystem file_system,
    const std::uint64_t size,
    const std::uint64_t used,
    const bool selected = true,
    const bool active = false,
    const bool required_for_windows = false) {
  return ytec::migrationcore::DirectCloneSourcePartition{
      .partition = ytec::migrationcore::ShrinkSourcePartition{
          .source_table_index = index,
          .role = role,
          .file_system = file_system,
          .source_size_bytes = size,
          .used_bytes = used,
          .cluster_size = file_system ==
                  ytec::migrationcore::MigrationFileSystem::fat32
              ? 32ULL * 1024ULL
              : 4096U,
          .label = role ==
                  ytec::migrationcore::MigrationPartitionRole::windows
              ? L"Windows"
              : L"DATA",
          .active = active,
      },
      .selected = selected,
      .required_for_windows = required_for_windows,
  };
}

ytec::migrationcore::DirectClonePlanningRequest gpt_windows_request() {
  using namespace ytec::migrationcore;
  return DirectClonePlanningRequest{
      .mode_choice = DirectCloneModeChoice::automatic,
      .partition_style_choice =
          DirectClonePartitionStyleChoice::preserve,
      .source_style = MigrationPartitionStyle::gpt,
      .source_size_bytes = 600ULL * kGiB,
      .source_logical_sector_size = 512U,
      .target_size_bytes = 600ULL * kGiB,
      .target_logical_sector_size = 512U,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          partition(
              0U,
              MigrationPartitionRole::efi_system,
              MigrationFileSystem::fat32,
              200ULL * kMiB,
              30ULL * kMiB,
              false),
          partition(
              1U,
              MigrationPartitionRole::microsoft_reserved,
              MigrationFileSystem::none,
              16ULL * kMiB,
              0U,
              false),
          partition(
              2U,
              MigrationPartitionRole::windows,
              MigrationFileSystem::ntfs,
              400ULL * kGiB,
              80ULL * kGiB),
          partition(
              3U,
              MigrationPartitionRole::recovery,
              MigrationFileSystem::ntfs,
              1ULL * kGiB,
              600ULL * kMiB,
              false,
              false,
              true),
          partition(
              4U,
              MigrationPartitionRole::data,
              MigrationFileSystem::ntfs,
              100ULL * kGiB,
              10ULL * kGiB,
              false),
      },
  };
}

ytec::migrationcore::DirectClonePlanningRequest mbr_data_request(
    const std::uint64_t target_size,
    const ytec::migrationcore::DirectCloneModeChoice mode_choice) {
  using namespace ytec::migrationcore;
  return DirectClonePlanningRequest{
      .mode_choice = mode_choice,
      .partition_style_choice =
          DirectClonePartitionStyleChoice::preserve,
      .source_style = MigrationPartitionStyle::mbr,
      .source_size_bytes = 500ULL * kGiB,
      .source_logical_sector_size = 512U,
      .target_size_bytes = target_size,
      .target_logical_sector_size = 512U,
      .source_is_windows_system = false,
      .windows_is_amd64 = false,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation = ShrinkSurplusAllocation::leave_unallocated,
      .source_partitions = {
          partition(
              0U,
              MigrationPartitionRole::data,
              MigrationFileSystem::ntfs,
              400ULL * kGiB,
              50ULL * kGiB),
      },
  };
}

const ytec::migrationcore::DirectClonePartitionSelection& selection(
    const ytec::migrationcore::DirectClonePlan& plan,
    const std::uint32_t source_index) {
  const auto found = std::find_if(
      plan.partition_selection().begin(),
      plan.partition_selection().end(),
      [source_index](const auto& candidate) {
        return candidate.source_table_index == source_index;
      });
  if (found == plan.partition_selection().end()) {
    throw TestFailure{"Expected source selection was not found"};
  }
  return *found;
}

const ytec::migrationcore::DirectClonePlannedPartition& target_partition(
    const ytec::migrationcore::DirectClonePlan& plan,
    const std::uint32_t source_index) {
  const auto found = std::find_if(
      plan.target_partitions().begin(),
      plan.target_partitions().end(),
      [source_index](const auto& candidate) {
        return candidate.source_table_index == source_index;
      });
  if (found == plan.target_partitions().end()) {
    throw TestFailure{"Expected target partition was not found"};
  }
  return *found;
}

void test_automatic_exact_forces_required_windows_partitions() {
  using namespace ytec::migrationcore;
  const auto result = plan_direct_clone(gpt_windows_request());
  check(result.has_value(), "A supported GPT Windows layout should plan");
  const auto& plan = result.value();
  check(
      plan.recommended_mode() == DirectCloneMode::exact &&
          plan.mode() == DirectCloneMode::exact &&
          !plan.mode_was_overridden() &&
          plan.source_style() == MigrationPartitionStyle::gpt &&
          plan.target_style() == MigrationPartitionStyle::gpt &&
          plan.source_size_bytes() == 600ULL * kGiB &&
          plan.source_logical_sector_size() == 512U &&
          plan.target_logical_sector_size() == 512U &&
          plan.source_remains_unchanged() &&
          plan.boot_finalization_required(),
      "Automatic planning should choose exact and preserve GPT");
  check(
      !selection(plan, 0U).requested && selection(plan, 0U).selected &&
          selection(plan, 0U).required &&
          !selection(plan, 1U).requested &&
          selection(plan, 1U).selected && selection(plan, 1U).required &&
          selection(plan, 2U).selected && selection(plan, 2U).required &&
          !selection(plan, 3U).requested &&
          selection(plan, 3U).selected && selection(plan, 3U).required &&
          !selection(plan, 4U).selected,
      "ESP, MSR, Windows, and required recovery must be immutable selections");
  check(
      plan.target_partitions().size() == 4U &&
          target_partition(plan, 2U).transfer ==
              DirectClonePartitionTransfer::exact_content &&
          target_partition(plan, 2U).minimum_size_bytes == 400ULL * kGiB,
      "Only normalized selections should appear in the exact target layout");
}

void test_automatic_recommends_shrink_only_when_exact_does_not_fit() {
  using namespace ytec::migrationcore;
  const auto result = plan_direct_clone(mbr_data_request(
      100ULL * kGiB, DirectCloneModeChoice::automatic));
  check(result.has_value(), "Used data plus reserve should fit in shrink mode");
  check(
      result.value().recommended_mode() == DirectCloneMode::shrink &&
          result.value().mode() == DirectCloneMode::shrink &&
          result.value().minimum_target_size_bytes() < 100ULL * kGiB &&
          result.value().target_partitions()[0].transfer ==
              DirectClonePartitionTransfer::file_system_content,
      "Automatic mode should recommend shrink after exact capacity failure");
}

void test_explicit_override_and_unsafe_exact_are_distinct() {
  using namespace ytec::migrationcore;
  const auto shrink = plan_direct_clone(mbr_data_request(
      500ULL * kGiB, DirectCloneModeChoice::shrink));
  check(
      shrink.has_value() &&
          shrink.value().recommended_mode() == DirectCloneMode::exact &&
          shrink.value().mode() == DirectCloneMode::shrink &&
          shrink.value().mode_was_overridden(),
      "An explicit safe shrink choice should override the exact recommendation");

  const auto exact = plan_direct_clone(mbr_data_request(
      100ULL * kGiB, DirectCloneModeChoice::exact));
  check(!exact.has_value(), "Exact must not override a capacity failure");
  check(
      exact.error().native_code == ERROR_DISK_FULL,
      "Unsafe exact override should fail before I/O with disk full");
}

void test_surplus_is_proportional_or_left_unallocated() {
  using namespace ytec::migrationcore;
  auto request = DirectClonePlanningRequest{
      .mode_choice = DirectCloneModeChoice::exact,
      .partition_style_choice =
          DirectClonePartitionStyleChoice::preserve,
      .source_style = MigrationPartitionStyle::mbr,
      .source_size_bytes = 1200ULL * kGiB,
      .source_logical_sector_size = 512U,
      .target_size_bytes = 1100ULL * kGiB + 3ULL * kMiB,
      .target_logical_sector_size = 512U,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation =
          ShrinkSurplusAllocation::automatic_proportional,
      .source_partitions = {
          partition(
              0U,
              MigrationPartitionRole::data,
              MigrationFileSystem::ntfs,
              600ULL * kGiB,
              100ULL * kGiB),
          partition(
              1U,
              MigrationPartitionRole::data,
              MigrationFileSystem::exfat,
              400ULL * kGiB,
              100ULL * kGiB),
      },
  };
  const auto automatic = plan_direct_clone(request);
  check(automatic.has_value(), "Proportional exact layout should plan");
  check(
      target_partition(automatic.value(), 0U).size_bytes ==
              660ULL * kGiB &&
          target_partition(automatic.value(), 1U).size_bytes ==
              440ULL * kGiB &&
          automatic.value().unallocated_tail_bytes() == 0U,
      "A 600:400 source ratio should receive a 60:40 aligned surplus");

  request.surplus_allocation = ShrinkSurplusAllocation::leave_unallocated;
  const auto unallocated = plan_direct_clone(request);
  check(unallocated.has_value(), "Unallocated surplus policy should plan");
  check(
      target_partition(unallocated.value(), 0U).size_bytes ==
              600ULL * kGiB &&
          target_partition(unallocated.value(), 1U).size_bytes ==
              400ULL * kGiB &&
          unallocated.value().unallocated_tail_bytes() == 100ULL * kGiB,
      "Leave-unallocated must preserve both exact minima and the full surplus");
}

void test_surplus_can_bind_one_selected_data_partition() {
  using namespace ytec::migrationcore;
  auto request = DirectClonePlanningRequest{
      .mode_choice = DirectCloneModeChoice::exact,
      .partition_style_choice =
          DirectClonePartitionStyleChoice::preserve,
      .source_style = MigrationPartitionStyle::mbr,
      .source_size_bytes = 1200ULL * kGiB,
      .source_logical_sector_size = 512U,
      .target_size_bytes = 1100ULL * kGiB + 3ULL * kMiB,
      .target_logical_sector_size = 512U,
      .bitlocker_fully_decrypted = true,
      .surplus_allocation =
          ShrinkSurplusAllocation::selected_data_partition,
      .surplus_target_source_table_index = 1U,
      .source_partitions = {
          partition(
              0U,
              MigrationPartitionRole::data,
              MigrationFileSystem::ntfs,
              600ULL * kGiB,
              100ULL * kGiB),
          partition(
              1U,
              MigrationPartitionRole::data,
              MigrationFileSystem::exfat,
              400ULL * kGiB,
              100ULL * kGiB),
      },
  };
  const auto targeted = plan_direct_clone(request);
  check(targeted.has_value(), "Selected-data surplus should plan");
  check(
      targeted.value().surplus_target_source_table_index() == 1U &&
          target_partition(targeted.value(), 0U).size_bytes ==
              600ULL * kGiB &&
          target_partition(targeted.value(), 1U).size_bytes ==
              500ULL * kGiB &&
          targeted.value().unallocated_tail_bytes() == 0U,
      "All aligned surplus must belong only to the bound data table index");

  request.surplus_target_source_table_index = 9U;
  check(
      !plan_direct_clone(request),
      "A missing surplus target table index must fail");

  request.surplus_target_source_table_index = 1U;
  request.source_partitions[1].selected = false;
  check(
      !plan_direct_clone(request),
      "An unchecked surplus target must fail");

  request.source_partitions[1].selected = true;
  request.source_partitions[1].partition.role =
      MigrationPartitionRole::recovery;
  check(
      !plan_direct_clone(request),
      "A selected non-data surplus target must fail");

  request.source_partitions[1].partition.role = MigrationPartitionRole::data;
  request.source_partitions[1].partition.file_system =
      MigrationFileSystem::none;
  check(
      !plan_direct_clone(request),
      "A selected non-archive filesystem surplus target must fail");

  request.source_partitions[1].partition.file_system =
      MigrationFileSystem::ntfs;
  request.source_partitions[1].partition.source_table_index = 0U;
  request.surplus_target_source_table_index = 0U;
  check(
      !plan_direct_clone(request),
      "Duplicate source table indexes must fail before surplus allocation");

  request.source_partitions[1].partition.source_table_index = 1U;
  request.surplus_allocation = ShrinkSurplusAllocation::leave_unallocated;
  request.surplus_target_source_table_index = 1U;
  check(
      !plan_direct_clone(request),
      "A target index must be forbidden for a non-targeted surplus policy");

  request.surplus_allocation =
      ShrinkSurplusAllocation::selected_data_partition;
  request.surplus_target_source_table_index.reset();
  check(
      !plan_direct_clone(request),
      "Selected-data policy must require exactly one target index");
}

void test_partition_style_choice_is_explicit_and_eligible() {
  using namespace ytec::migrationcore;
  auto request = DirectClonePlanningRequest{
      .mode_choice = DirectCloneModeChoice::automatic,
      .partition_style_choice =
          DirectClonePartitionStyleChoice::preserve,
      .source_style = MigrationPartitionStyle::mbr,
      .source_size_bytes = 500ULL * kGiB,
      .source_logical_sector_size = 512U,
      .target_size_bytes = 600ULL * kGiB,
      .target_logical_sector_size = 512U,
      .source_is_windows_system = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
      .source_partitions = {
          partition(
              0U,
              MigrationPartitionRole::bios_system,
              MigrationFileSystem::ntfs,
              500ULL * kMiB,
              80ULL * kMiB,
              false,
              true),
          partition(
              1U,
              MigrationPartitionRole::windows,
              MigrationFileSystem::ntfs,
              450ULL * kGiB,
              100ULL * kGiB),
      },
  };
  const auto preserved = plan_direct_clone(request);
  check(
      preserved.has_value() &&
          preserved.value().target_style() == MigrationPartitionStyle::mbr &&
          selection(preserved.value(), 0U).required,
      "Preserve must retain MBR and force the BIOS system selection");

  request.partition_style_choice =
      DirectClonePartitionStyleChoice::mbr_to_gpt;
  check(
      !plan_direct_clone(request).has_value(),
      "MBR-to-GPT must fail without read-only eligibility evidence");
  request.mbr_to_gpt_eligible = true;
  const auto converted = plan_direct_clone(request);
  check(
      converted.has_value() &&
          converted.value().target_style() == MigrationPartitionStyle::gpt &&
          converted.value().partition_style_choice() ==
              DirectClonePartitionStyleChoice::mbr_to_gpt &&
          converted.value().target_partitions()[0].role ==
              MigrationPartitionRole::efi_system &&
          converted.value().target_partitions()[1].role ==
              MigrationPartitionRole::microsoft_reserved,
      "Eligible conversion should immutably request GPT with new boot metadata");

  auto invalid = gpt_windows_request();
  invalid.partition_style_choice =
      DirectClonePartitionStyleChoice::mbr_to_gpt;
  invalid.mbr_to_gpt_eligible = true;
  check(
      !plan_direct_clone(invalid).has_value(),
      "GPT-to-GPT must not be mislabeled as MBR-to-GPT");
}

void test_selected_unsupported_file_system_uses_exact_raw() {
  using namespace ytec::migrationcore;
  auto request = mbr_data_request(
      500ULL * kGiB, DirectCloneModeChoice::automatic);
  request.source_partitions[0].partition.file_system =
      MigrationFileSystem::unsupported;
  constexpr std::uint64_t kSectorAlignedNonMiBRawSize =
      400ULL * kGiB - 512U;
  request.source_partitions[0].partition.source_size_bytes =
      kSectorAlignedNonMiBRawSize;
  request.source_partitions[0].partition.used_bytes =
      kSectorAlignedNonMiBRawSize;
  const auto planned = plan_direct_clone(request);
  check(planned.has_value(), "Selected unsupported FS must be retained");
  check(
      planned.value().target_partitions().size() == 1U &&
          planned.value().target_partitions()[0].transfer ==
              DirectClonePartitionTransfer::exact_content &&
          planned.value().target_partitions()[0].size_bytes ==
              request.source_partitions[0].partition.source_size_bytes &&
          planned.value().target_partitions()[0].minimum_size_bytes ==
              request.source_partitions[0].partition.source_size_bytes &&
          planned.value().target_partitions()[0].size_bytes % kMiB ==
              kSectorAlignedNonMiBRawSize % kMiB,
      "Unsupported FS must retain its sector-aligned non-MiB exact RAW size");

  request.source_partitions[0].selected = false;
  request.source_partitions.push_back(partition(
      1U,
      MigrationPartitionRole::data,
      MigrationFileSystem::ntfs,
      20ULL * kGiB,
      5ULL * kGiB));
  const auto omitted = plan_direct_clone(request);
  check(
      omitted.has_value() && !selection(omitted.value(), 0U).selected &&
          omitted.value().target_partitions().size() == 1U,
      "An explicitly omitted unsupported partition must never enter the layout");
}

void test_cross_sector_exact_fails_but_automatic_can_shrink() {
  using namespace ytec::migrationcore;
  auto request = mbr_data_request(
      100ULL * kGiB, DirectCloneModeChoice::automatic);
  request.target_logical_sector_size = 4096U;
  const auto automatic = plan_direct_clone(request);
  check(
      automatic.has_value() &&
          automatic.value().recommended_mode() == DirectCloneMode::shrink,
      "Cross-sector automatic planning should select file-level shrink");

  request.mode_choice = DirectCloneModeChoice::exact;
  const auto exact = plan_direct_clone(request);
  check(
      !exact.has_value() && exact.error().native_code == ERROR_NOT_SUPPORTED,
      "Cross-sector exact must fail closed before I/O");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"automatic_exact_forces_required_windows_partitions",
       test_automatic_exact_forces_required_windows_partitions},
      {"automatic_recommends_shrink_only_when_exact_does_not_fit",
       test_automatic_recommends_shrink_only_when_exact_does_not_fit},
      {"explicit_override_and_unsafe_exact_are_distinct",
       test_explicit_override_and_unsafe_exact_are_distinct},
      {"surplus_is_proportional_or_left_unallocated",
       test_surplus_is_proportional_or_left_unallocated},
      {"surplus_can_bind_one_selected_data_partition",
       test_surplus_can_bind_one_selected_data_partition},
      {"partition_style_choice_is_explicit_and_eligible",
       test_partition_style_choice_is_explicit_and_eligible},
      {"selected_unsupported_file_system_uses_exact_raw",
       test_selected_unsupported_file_system_uses_exact_raw},
      {"cross_sector_exact_fails_but_automatic_can_shrink",
       test_cross_sector_exact_fails_but_automatic_can_shrink},
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

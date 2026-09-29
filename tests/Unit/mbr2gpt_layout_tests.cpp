#include "ytec/bootrepair/mbr2gpt_layout.h"

#include <Windows.h>

#include <functional>
#include <iostream>
#include <array>
#include <cstddef>
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

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

class DeterministicGuidGenerator final
    : public ytec::clonecore::IGuidGenerator {
 public:
  [[nodiscard]] ytec::clonecore::Result<ytec::clonecore::GptGuid>
  next_guid() override {
    ytec::clonecore::GptGuid guid;
    guid.bytes[0] = static_cast<std::byte>(next_++);
    guid.bytes[15] = std::byte{0xA5};
    return ytec::clonecore::Result<
        ytec::clonecore::GptGuid>::success(guid);
  }

 private:
  std::uint8_t next_{1U};
};

ytec::bootrepair::Mbr2GptRebuildRequest standard_request() {
  using ytec::bootrepair::Mbr2GptFileSystem;
  using ytec::bootrepair::Mbr2GptSourcePartition;
  using ytec::bootrepair::Mbr2GptSourceRole;
  return ytec::bootrepair::Mbr2GptRebuildRequest{
      .source_disk_size_bytes = 56ULL * kGiB,
      .target_disk_size_bytes = 64ULL * kGiB,
      .logical_sector_size = 512U,
      .source_partitions =
          {
              Mbr2GptSourcePartition{
                  .number = 1U,
                  .offset_bytes = 1ULL * kMiB,
                  .size_bytes = 500ULL * kMiB,
                  .role = Mbr2GptSourceRole::system_reserved,
                  .file_system = Mbr2GptFileSystem::ntfs,
                  .primary = true,
                  .active = true,
              },
              Mbr2GptSourcePartition{
                  .number = 2U,
                  .offset_bytes = 501ULL * kMiB,
                  .size_bytes = 50ULL * kGiB,
                  .role = Mbr2GptSourceRole::windows,
                  .file_system = Mbr2GptFileSystem::ntfs,
                  .primary = true,
              },
              Mbr2GptSourcePartition{
                  .number = 3U,
                  .offset_bytes =
                      501ULL * kMiB + 50ULL * kGiB,
                  .size_bytes = 1ULL * kGiB,
                  .role = Mbr2GptSourceRole::recovery,
                  .file_system = Mbr2GptFileSystem::ntfs,
                  .primary = true,
              },
          },
      .winre_state =
          ytec::bootrepair::WinReSourceState::registered_partition,
      .registered_winre_partition_number = 3U,
      .winre_image_size_bytes = 700ULL * kMiB,
      .firmware_supports_uefi = true,
      .windows_is_amd64 = true,
      .bitlocker_fully_decrypted = true,
  };
}

void check_default_roles(
    const ytec::bootrepair::Mbr2GptRebuildPlan& plan) {
  using ytec::bootrepair::PlannedGptPartitionRole;
  check(
      plan.target_partitions.size() >= 4U,
      "The recommended layout needs at least four partitions");
  check(
      plan.target_partitions[0].role ==
          PlannedGptPartitionRole::efi_system,
      "ESP must be first");
  check(
      plan.target_partitions[1].role ==
          PlannedGptPartitionRole::microsoft_reserved,
      "MSR must follow ESP");
  check(
      plan.target_partitions[2].role ==
          PlannedGptPartitionRole::windows,
      "Windows must follow MSR");
  check(
      plan.target_partitions[3].role ==
          PlannedGptPartitionRole::recovery,
      "Recovery tools must immediately follow Windows");
}

void test_recommended_existing_layout_is_preserved_semantically() {
  const auto result =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(
          standard_request());
  check(result.has_value(), "A supported layout should produce a plan");
  const auto& plan = result.value();
  check_default_roles(plan);
  check(
      plan.target_partitions[0].size_bytes == 200ULL * kMiB,
      "512/512e targets need the current 200 MiB ESP minimum");
  check(
      plan.target_partitions[1].size_bytes == 16ULL * kMiB,
      "MSR must be 16 MiB");
  check(
      !plan.recovery_partition_created,
      "An adequate registered recovery partition should be copied");
  check(
      !plan.recovery_order_changed,
      "Recovery already following Windows needs no order change");
  check(
      plan.microsoft_in_place_precheck_passed,
      "The simple three-primary layout should pass the preliminary check");
  check(
      plan.source_disk_remains_unchanged &&
          plan.official_mbr2gpt_validate_still_required,
      "The plan must remain write-free and not replace official validation");
}

void test_recovery_before_windows_is_moved_after_windows_in_plan() {
  auto request = standard_request();
  auto recovery = request.source_partitions[2];
  recovery.number = 2U;
  recovery.offset_bytes = 501ULL * kMiB;
  auto windows = request.source_partitions[1];
  windows.number = 3U;
  windows.offset_bytes = recovery.offset_bytes + recovery.size_bytes;
  request.source_partitions = {
      request.source_partitions[0], recovery, windows};
  request.registered_winre_partition_number = 2U;

  const auto result =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(result.has_value(), "A known recovery partition can be relocated");
  check_default_roles(result.value());
  check(
      result.value().recovery_order_changed,
      "Recovery before Windows must be flagged as reordered");
  check(
      result.value().target_partitions[3].source_partition_number == 2U,
      "The verified recovery source identity must be retained");
}

void test_missing_recovery_is_created_only_from_verified_winre_image() {
  auto request = standard_request();
  request.source_partitions.pop_back();
  request.winre_state =
      ytec::bootrepair::WinReSourceState::image_available_in_windows;
  request.registered_winre_partition_number = 0U;

  const auto result =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(result.has_value(), "A verified WinRE image can fill a missing partition");
  check_default_roles(result.value());
  check(
      result.value().recovery_partition_created,
      "Missing recovery must be explicitly created");
  check(
      result.value().target_partitions[3].action ==
          ytec::bootrepair::PlannedGptPartitionAction::
              create_and_stage_winre,
      "New recovery must stage the verified WinRE image");
  check(
      result.value().target_partitions[3].size_bytes >=
          990ULL * kMiB,
      "New recovery must use the recommended minimum");
}

void test_missing_or_unknown_winre_fails_closed() {
  auto request = standard_request();
  request.source_partitions.pop_back();
  request.winre_state = ytec::bootrepair::WinReSourceState::missing;
  request.winre_image_size_bytes = 0U;
  const auto missing =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(!missing.has_value(), "Missing WinRE must not be fabricated");
  check(
      missing.error().code ==
          ytec::clonecore::ErrorCode::verification_failed,
      "Missing WinRE should be a verification failure");

  request.winre_state = ytec::bootrepair::WinReSourceState::unknown;
  const auto unknown =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(!unknown.has_value(), "Unknown WinRE state must fail closed");
}

void test_small_recovery_is_rebuilt_with_image_and_free_space() {
  auto request = standard_request();
  request.source_partitions[2].size_bytes = 600ULL * kMiB;
  request.winre_image_size_bytes = 800ULL * kMiB;
  const auto result =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(result.has_value(), "A small known recovery partition can be replaced");
  check(
      result.value().recovery_partition_created,
      "An undersized recovery partition must not be copied as complete");
  check(
      result.value().target_partitions[3].size_bytes >=
          1050ULL * kMiB,
      "Recovery must hold WinRE plus at least 250 MiB free");
}

void test_optional_data_is_placed_after_recovery() {
  auto request = standard_request();
  auto& recovery = request.source_partitions[2];
  request.source_partitions.push_back(
      ytec::bootrepair::Mbr2GptSourcePartition{
          .number = 4U,
          .offset_bytes =
              recovery.offset_bytes + recovery.size_bytes,
          .size_bytes = 2ULL * kGiB,
          .role = ytec::bootrepair::Mbr2GptSourceRole::data,
          .file_system = ytec::bootrepair::Mbr2GptFileSystem::ntfs,
          .primary = true,
      });
  const auto result =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(result.has_value(), "A known NTFS data partition should be preserved");
  check(
      result.value().target_partitions.size() == 5U &&
          result.value().target_partitions[4].role ==
              ytec::bootrepair::PlannedGptPartitionRole::data,
      "Optional data must follow the recovery tools partition");
  check(
      !result.value().microsoft_in_place_precheck_passed,
      "Four primary partitions exceed the official precheck limit");
}

void test_4kn_uses_300_mib_esp() {
  auto request = standard_request();
  request.logical_sector_size = 4096U;
  const auto result =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(result.has_value(), "Aligned 4Kn layout should be supported");
  check(
      result.value().target_partitions[0].size_bytes ==
          300ULL * kMiB,
      "4Kn target must use the current 300 MiB ESP minimum");
}

void test_insufficient_target_and_unsupported_layout_fail() {
  auto request = standard_request();
  request.target_disk_size_bytes = 50ULL * kGiB;
  const auto too_small =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(!too_small.has_value(), "A target that cannot hold all roles must fail");
  check(
      too_small.error().code ==
          ytec::clonecore::ErrorCode::unsupported_layout,
      "Capacity failure should remain an unsupported layout");

  request = standard_request();
  request.source_partitions[1].primary = false;
  const auto logical =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(!logical.has_value(), "Logical/extended layouts must not be guessed");

  request = standard_request();
  request.bitlocker_fully_decrypted = false;
  const auto encrypted =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(request);
  check(!encrypted.has_value(), "BitLocker must remain fail-closed");
}

void test_materializes_complete_gpt_metadata_without_device_io() {
  const auto request = standard_request();
  const auto layout =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(
          request);
  check(layout.has_value(), "The semantic layout must be available");
  DeterministicGuidGenerator guid_generator;
  const auto metadata =
      ytec::bootrepair::make_mbr2gpt_gpt_metadata_plan(
          layout.value(),
          request.target_disk_size_bytes,
          request.logical_sector_size,
          guid_generator);
  check(metadata.has_value(), "A valid layout should materialize in memory");
  check(
      metadata.value().writes.size() == 5U,
      "The metadata plan needs both GPT copies, protective MBR, and commit");
  check(
      metadata.value().writes.back().kind ==
          ytec::clonecore::GptMetadataKind::primary_header_commit,
      "The primary GPT header must remain the final commit write");
  const auto& partitions = metadata.value().target_disk.partitions;
  check(partitions.size() == 4U, "All recommended roles must be emitted");
  check(
      partitions[0].type_guid ==
          ytec::clonecore::gpt_type_efi_system() &&
          partitions[1].type_guid ==
              ytec::clonecore::gpt_type_microsoft_reserved() &&
          partitions[2].type_guid ==
              ytec::clonecore::gpt_type_basic_data() &&
          partitions[3].type_guid ==
              ytec::clonecore::gpt_type_windows_recovery(),
      "Each role must use the Microsoft GPT partition type");
  check(
      partitions[3].attributes == 0x8000000000000001ULL,
      "Windows RE must be required and receive no drive letter");
  for (std::size_t index = 0; index < partitions.size(); ++index) {
    check(
        !partitions[index].unique_guid.is_zero(),
        "Every partition needs a generated unique GUID");
    check(
        partitions[index].first_lba ==
                layout.value().target_partitions[index].offset_bytes /
                    request.logical_sector_size &&
            partitions[index].last_lba + 1U ==
                (layout.value().target_partitions[index].offset_bytes +
                 layout.value().target_partitions[index].size_bytes) /
                    request.logical_sector_size,
        "GPT LBAs must exactly match the semantic byte plan");
  }
}

void test_metadata_materialization_rejects_tampered_layout() {
  const auto request = standard_request();
  const auto original =
      ytec::bootrepair::plan_mbr2gpt_rebuild_to_new_target(
          request);
  check(original.has_value(), "The baseline layout must be available");

  auto wrong_order = original.value();
  std::swap(
      wrong_order.target_partitions[0],
      wrong_order.target_partitions[1]);
  DeterministicGuidGenerator guid_generator;
  const auto order_result =
      ytec::bootrepair::make_mbr2gpt_gpt_metadata_plan(
          wrong_order,
          request.target_disk_size_bytes,
          request.logical_sector_size,
          guid_generator);
  check(!order_result.has_value(), "Tampered role order must fail closed");

  auto overlapping = original.value();
  overlapping.target_partitions[3].offset_bytes =
      overlapping.target_partitions[2].offset_bytes;
  DeterministicGuidGenerator second_generator;
  const auto overlap_result =
      ytec::bootrepair::make_mbr2gpt_gpt_metadata_plan(
          overlapping,
          request.target_disk_size_bytes,
          request.logical_sector_size,
          second_generator);
  check(!overlap_result.has_value(), "Overlapping GPT roles must fail closed");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"recommended_existing_layout_is_preserved_semantically",
       test_recommended_existing_layout_is_preserved_semantically},
      {"recovery_before_windows_is_moved_after_windows_in_plan",
       test_recovery_before_windows_is_moved_after_windows_in_plan},
      {"missing_recovery_is_created_only_from_verified_winre_image",
       test_missing_recovery_is_created_only_from_verified_winre_image},
      {"missing_or_unknown_winre_fails_closed",
       test_missing_or_unknown_winre_fails_closed},
      {"small_recovery_is_rebuilt_with_image_and_free_space",
       test_small_recovery_is_rebuilt_with_image_and_free_space},
      {"optional_data_is_placed_after_recovery",
       test_optional_data_is_placed_after_recovery},
      {"4kn_uses_300_mib_esp", test_4kn_uses_300_mib_esp},
      {"insufficient_target_and_unsupported_layout_fail",
       test_insufficient_target_and_unsupported_layout_fail},
      {"materializes_complete_gpt_metadata_without_device_io",
       test_materializes_complete_gpt_metadata_without_device_io},
      {"metadata_materialization_rejects_tampered_layout",
       test_metadata_materialization_rejects_tampered_layout},
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

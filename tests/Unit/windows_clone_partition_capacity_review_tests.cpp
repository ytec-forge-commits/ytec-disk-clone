#include "ytec/windowsapp/clone_partition_capacity_review.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef YTEC_WINDOWS_APP_MAIN_SOURCE_PATH
#error YTEC_WINDOWS_APP_MAIN_SOURCE_PATH must identify the product UI source.
#endif

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, std::string message) {
  if (!condition) {
    throw TestFailure{std::move(message)};
  }
}

ytec::imageformat::Sha256Digest digest(const std::uint8_t value) {
  ytec::imageformat::Sha256Digest result{};
  result.fill(static_cast<std::byte>(value));
  return result;
}

ytec::windowsapp::WindowsClonePartitionCapacityBinding binding() {
  return ytec::windowsapp::WindowsClonePartitionCapacityBinding{
      .source = ytec::clonecore::StableDiskIdentity{
          .disk_number = 3U,
          .model = L"Review source",
          .size_bytes = 600ULL * kGiB,
          .logical_sector_size = 512U,
          .serial_suffix = "SRC-REVIEW",
          .device_instance_id = L"SCSI\\DISK&VEN_REVIEW",
          .is_system_disk = true,
      },
      .source_partition_style =
          ytec::migrationcore::MigrationPartitionStyle::gpt,
      .source_layout_hash = digest(0x31U),
      .source_analysis_hash = digest(0x52U),
  };
}

ytec::windowsapp::WindowsClonePartitionCapacityCandidate candidate(
    const std::uint32_t index,
    const ytec::migrationcore::MigrationPartitionRole role,
    const ytec::migrationcore::MigrationFileSystem file_system,
    const bool required_for_windows = false,
    const bool active = false) {
  return ytec::windowsapp::WindowsClonePartitionCapacityCandidate{
      .partition = ytec::migrationcore::ShrinkSourcePartition{
          .source_table_index = index,
          .role = role,
          .file_system = file_system,
          .source_size_bytes = role ==
                  ytec::migrationcore::MigrationPartitionRole::
                      microsoft_reserved
              ? 16ULL * kMiB
              : 50ULL * kGiB,
          .used_bytes = role ==
                  ytec::migrationcore::MigrationPartitionRole::
                      microsoft_reserved
              ? 0U
              : 5ULL * kGiB,
          .cluster_size = file_system ==
                  ytec::migrationcore::MigrationFileSystem::none
              ? 0U
              : 4096U,
           .label = role ==
                   ytec::migrationcore::MigrationPartitionRole::data
               ? L"DATA"
               : L"SYSTEM",
           .active = active,
       },
      .required_for_windows = required_for_windows,
  };
}

std::vector<ytec::windowsapp::WindowsClonePartitionCapacityCandidate>
candidates() {
  using namespace ytec::migrationcore;
  return {
      candidate(1U, MigrationPartitionRole::efi_system,
                MigrationFileSystem::fat32),
      candidate(2U, MigrationPartitionRole::microsoft_reserved,
                MigrationFileSystem::none),
      candidate(3U, MigrationPartitionRole::windows,
                MigrationFileSystem::ntfs),
      candidate(4U, MigrationPartitionRole::recovery,
                MigrationFileSystem::ntfs, true),
      candidate(5U, MigrationPartitionRole::data,
                MigrationFileSystem::ntfs),
      candidate(6U, MigrationPartitionRole::data,
                MigrationFileSystem::exfat),
  };
}

const ytec::windowsapp::WindowsClonePartitionCapacityRow& row(
    const ytec::windowsapp::WindowsClonePartitionCapacityReview& review,
    const std::uint32_t index) {
  const auto found = std::find_if(
      review.rows().begin(),
      review.rows().end(),
      [index](const auto& candidate_row) {
        return candidate_row.partition.source_table_index == index;
      });
  if (found == review.rows().end()) {
    throw TestFailure{"Expected review row was not found"};
  }
  return *found;
}

std::uintptr_t policy_item(
    const ytec::migrationcore::ShrinkSurplusAllocation allocation) {
  const auto encoded =
      ytec::windowsapp::encode_windows_clone_surplus_policy_item_data(
          allocation);
  if (!encoded) {
    throw TestFailure{"Expected surplus policy was not encodable"};
  }
  return *encoded;
}

ytec::windowsapp::WindowsClonePartitionCapacitySubmission submission_for(
    const ytec::windowsapp::WindowsClonePartitionCapacityReview& review,
    const ytec::migrationcore::ShrinkSurplusAllocation allocation) {
  ytec::windowsapp::WindowsClonePartitionCapacitySubmission submission{
      .revalidated_binding = review.binding(),
      .surplus_policy_item_data = policy_item(allocation),
  };
  submission.selected_partition_item_data.reserve(review.rows().size());
  for (const auto& item : review.rows()) {
    const auto encoded =
        ytec::windowsapp::encode_windows_clone_partition_item_data(
            item.partition.source_table_index);
    if (!encoded) {
      throw TestFailure{"Expected partition index was not encodable"};
    }
    submission.selected_partition_item_data.push_back(*encoded);
  }
  return submission;
}

ytec::windowsapp::WindowsClonePartitionCapacityReview supported_review() {
  const auto source_binding = binding();
  const auto source_candidates = candidates();
  auto result =
      ytec::windowsapp::build_windows_clone_partition_capacity_review(
          source_binding, source_candidates);
  if (!result) {
    throw TestFailure{"Supported partition review did not build"};
  }
  return result.take_value();
}

void test_all_partitions_default_selected_and_required_roles_are_locked() {
  using namespace ytec::migrationcore;
  const auto review = supported_review();
  check(
      review.rows().size() == 6U &&
          std::all_of(
              review.rows().begin(),
              review.rows().end(),
              [](const auto& item) { return item.selected_by_default; }) &&
          review.default_surplus_allocation() ==
              ShrinkSurplusAllocation::automatic_proportional,
      "Every analyzed partition must be selected by default with automatic surplus");
  check(
      row(review, 1U).required && row(review, 2U).required &&
          row(review, 3U).required && row(review, 4U).required &&
          !row(review, 5U).required && !row(review, 6U).required,
      "GPT boot, Windows, and required Recovery rows must be immutable");
  check(
      row(review, 5U).eligible_surplus_target &&
          !row(review, 3U).eligible_surplus_target &&
          !row(review, 6U).eligible_surplus_target,
      "Only an NTFS data row may own selected surplus in the Windows path");
}

void test_required_row_cannot_be_unchecked() {
  using namespace ytec::migrationcore;
  const auto review = supported_review();
  auto submission = submission_for(
      review, ShrinkSurplusAllocation::automatic_proportional);
  submission.selected_partition_item_data.erase(
      submission.selected_partition_item_data.begin());
  const auto rejected =
      ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, submission);
  check(
      !rejected &&
          rejected.error().code ==
              ytec::clonecore::ErrorCode::confirmation_required &&
          rejected.error().native_code == ERROR_CANCELLED,
      "Removing a required GPT row must fail before a decision is issued");

  submission = submission_for(
      review, ShrinkSurplusAllocation::automatic_proportional);
  submission.selected_partition_item_data.pop_back();
  const auto optional_omitted =
      ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, submission);
  check(
      optional_omitted.has_value() &&
          optional_omitted.value().selected_source_table_indexes.size() == 5U,
      "An optional data row may be deliberately omitted");
}

void test_completion_revalidates_source_layout_and_analysis_binding() {
  using namespace ytec::migrationcore;
  const auto review = supported_review();
  auto submission = submission_for(
      review, ShrinkSurplusAllocation::automatic_proportional);
  submission.revalidated_binding.source.serial_suffix = "CHANGED";
  auto rejected =
      ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, submission);
  check(
      !rejected &&
          rejected.error().code ==
              ytec::clonecore::ErrorCode::identity_mismatch,
      "A different stable source identity must invalidate the review");

  submission = submission_for(
      review, ShrinkSurplusAllocation::automatic_proportional);
  submission.revalidated_binding.source_layout_hash[0] ^= std::byte{0x01};
  rejected =
      ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, submission);
  check(!rejected, "A changed source layout hash must invalidate the review");

  submission = submission_for(
      review, ShrinkSurplusAllocation::automatic_proportional);
  submission.revalidated_binding.source_analysis_hash[0] ^= std::byte{0x01};
  rejected =
      ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, submission);
  check(!rejected, "A changed source analysis hash must invalidate the review");
}

void test_three_surplus_policies_decode_and_validate_target_item_data() {
  using namespace ytec::migrationcore;
  const auto options =
      ytec::windowsapp::windows_clone_surplus_policy_options();
  check(
      options.size() == 3U &&
          options[0].allocation ==
              ShrinkSurplusAllocation::automatic_proportional &&
          options[1].allocation ==
              ShrinkSurplusAllocation::selected_data_partition &&
          options[2].allocation ==
              ShrinkSurplusAllocation::leave_unallocated,
      "The UI policy list must expose the three stable choices in review order");
  for (const auto& option : options) {
    check(
        !option.label.empty() &&
            ytec::windowsapp::decode_windows_clone_surplus_policy_item_data(
                option.item_data) == option.allocation,
        "Every policy item-data value must round-trip to one policy");
  }
  check(
      !ytec::windowsapp::decode_windows_clone_surplus_policy_item_data(0U) &&
          !ytec::windowsapp::decode_windows_clone_surplus_policy_item_data(4U) &&
          !ytec::windowsapp::decode_windows_clone_partition_item_data(0U) &&
          ytec::windowsapp::decode_windows_clone_partition_item_data(5U) ==
              5U,
      "Unknown policy and zero partition item data must be rejected");
  if constexpr (sizeof(std::uintptr_t) > sizeof(std::uint32_t)) {
    check(
        !ytec::windowsapp::decode_windows_clone_partition_item_data(
            static_cast<std::uintptr_t>(
                (std::numeric_limits<std::uint32_t>::max)()) +
            1U),
        "Partition item data wider than uint32 must be rejected");
  }

  const auto review = supported_review();
  auto automatic = submission_for(
      review, ShrinkSurplusAllocation::automatic_proportional);
  auto decision =
      ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, automatic);
  check(
      decision.has_value() &&
          decision.value().surplus_allocation ==
              ShrinkSurplusAllocation::automatic_proportional &&
          !decision.value().surplus_target_source_table_index,
      "Automatic policy must complete without a target index");

  auto unallocated = submission_for(
      review, ShrinkSurplusAllocation::leave_unallocated);
  decision =
      ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, unallocated);
  check(
      decision.has_value() &&
          decision.value().surplus_allocation ==
              ShrinkSurplusAllocation::leave_unallocated &&
          !decision.value().surplus_target_source_table_index,
      "Unallocated policy must complete without a target index");

  auto targeted = submission_for(
      review, ShrinkSurplusAllocation::selected_data_partition);
  targeted.surplus_target_partition_item_data = 5U;
  decision =
      ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, targeted);
  check(
      decision.has_value() &&
          decision.value().surplus_target_source_table_index == 5U,
      "Selected-data policy must bind the decoded NTFS data table index");

  targeted.surplus_target_partition_item_data = 6U;
  check(
      !ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, targeted),
      "An exFAT row must not become the Windows production surplus target");
  targeted = submission_for(
      review, ShrinkSurplusAllocation::selected_data_partition);
  targeted.surplus_target_partition_item_data = 5U;
  targeted.selected_partition_item_data.erase(
      std::find(
          targeted.selected_partition_item_data.begin(),
          targeted.selected_partition_item_data.end(),
          static_cast<std::uintptr_t>(5U)));
  check(
      !ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, targeted),
      "An unchecked data row must not remain the surplus target");
}

void test_duplicate_candidate_and_duplicate_item_data_fail_closed() {
  using namespace ytec::migrationcore;
  auto duplicated_candidates = candidates();
  duplicated_candidates.back().partition.source_table_index = 5U;
  const auto duplicate_review =
      ytec::windowsapp::build_windows_clone_partition_capacity_review(
          binding(), duplicated_candidates);
  check(
      !duplicate_review &&
          duplicate_review.error().code ==
              ytec::clonecore::ErrorCode::identity_mismatch,
      "Duplicate source table indexes must not build a review");

  const auto review = supported_review();
  auto submission = submission_for(
      review, ShrinkSurplusAllocation::automatic_proportional);
  submission.selected_partition_item_data.push_back(5U);
  check(
      !ytec::windowsapp::complete_windows_clone_partition_capacity_review(
          review, submission),
      "Duplicate list item data must not issue a decision");
}

void test_mbr_boot_and_analysis_required_recovery_are_locked() {
  using namespace ytec::migrationcore;
  auto source_binding = binding();
  source_binding.source_partition_style = MigrationPartitionStyle::mbr;
  const std::vector source_candidates{
      candidate(1U, MigrationPartitionRole::bios_system,
                MigrationFileSystem::ntfs, false, true),
      candidate(2U, MigrationPartitionRole::windows,
                MigrationFileSystem::ntfs),
      candidate(3U, MigrationPartitionRole::recovery,
                MigrationFileSystem::ntfs, true),
      candidate(4U, MigrationPartitionRole::recovery,
                MigrationFileSystem::ntfs, false),
      candidate(5U, MigrationPartitionRole::data,
                MigrationFileSystem::ntfs),
  };
  auto review =
      ytec::windowsapp::build_windows_clone_partition_capacity_review(
          source_binding, source_candidates);
  check(review.has_value(), "Supported MBR system review must build");
  check(
      row(review.value(), 1U).required &&
          row(review.value(), 2U).required &&
          row(review.value(), 3U).required &&
          !row(review.value(), 4U).required &&
          !row(review.value(), 5U).required,
      "BIOS boot, Windows and analysis-required Recovery must be locked");
}

void test_product_ui_wires_scroll_keyboard_focus_and_item_data() {
  std::ifstream input(
      YTEC_WINDOWS_APP_MAIN_SOURCE_PATH,
      std::ios::binary);
  check(input.good(), "Product main source must be readable for UI wiring evidence");
  const std::string source{
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>()};
  const auto require_pattern = [&](const std::string_view pattern,
                                   const char* message) {
    check(source.find(pattern) != std::string::npos, message);
  };
  require_pattern(
      "L\"パーティション・容量設定…\"",
      "Clone product must expose the dedicated settings action");
  require_pattern("WC_LISTVIEWW", "Partition review must use a native ListView");
  require_pattern("WS_VSCROLL", "Partition ListView must be scrollable");
  require_pattern("LVS_EX_CHECKBOXES", "Partition ListView must expose checkboxes");
  require_pattern("LVN_ITEMCHANGING", "Mouse uncheck must pass the required-row veto");
  require_pattern("LVN_KEYDOWN", "Keyboard changes must pass an explicit review gate");
  require_pattern("VK_SPACE", "Space-key required-row behavior must be explicit");
  require_pattern("LVIF_PARAM", "List identity must be recovered from item lParam");
  require_pattern(
      ".lParam = static_cast<LPARAM>(\n                row.partition.source_table_index)",
      "List lParam must store source_table_index rather than row position");
  require_pattern("CB_SETITEMDATA", "Surplus policy and target combos must use item-data");
  require_pattern(
      "DialogBoxIndirectParamW",
      "Modal dialog loop must provide Tab/Shift+Tab, Enter and Esc routing");
  require_pattern("WS_TABSTOP", "Every interactive review control must join tab order");
  require_pattern("SetFocus(state->partition_list)", "Initial focus must enter the review list");
  require_pattern("BS_DEFPUSHBUTTON", "Enter must activate the explicit default accept button");
  require_pattern("IDOK", "Enter must have a default accept action");
  require_pattern("IDCANCEL", "Esc must have a modal cancellation action");
  require_pattern(
      "plan_windows_direct_shrink_clone_after_partition_review_with_windows_apis",
      "Production must use the freshly reanalyzed reviewed planner");
  require_pattern(
      "partition_capacity_visible && state.elevated",
      "Settings action must be interlocked to the clone shrink preflight");
}

}  // namespace

int main() {
  try {
    test_all_partitions_default_selected_and_required_roles_are_locked();
    test_required_row_cannot_be_unchecked();
    test_completion_revalidates_source_layout_and_analysis_binding();
    test_three_surplus_policies_decode_and_validate_target_item_data();
    test_duplicate_candidate_and_duplicate_item_data_fail_closed();
    test_mbr_boot_and_analysis_required_recovery_are_locked();
    test_product_ui_wires_scroll_keyboard_focus_and_item_data();
    std::cout << "windows clone partition/capacity review tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "windows clone partition/capacity review tests: FAIL: "
              << failure.message << '\n';
    return 1;
  }
}

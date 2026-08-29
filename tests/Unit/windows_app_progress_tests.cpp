#include "ytec/windowsapp/progress.h"
#include "ytec/windowsapp/selection.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <string>

namespace {

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

void test_units_and_duration() {
  check(
      ytec::windowsapp::format_bytes(1536) == L"1.5 KiB",
      "Binary byte units should be readable");
  check(
      ytec::windowsapp::format_duration(std::chrono::seconds(3661)) ==
          L"1時間1分1秒",
      "Duration should include hours, minutes, and seconds");
}

void test_eta_is_hidden_until_stable() {
  const auto view = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::OperationStage::reading,
          .processed_bytes = 8ULL * 1024ULL * 1024ULL,
          .total_bytes = 64ULL * 1024ULL * 1024ULL,
          .elapsed = std::chrono::seconds(2),
          .cancellation_allowed = true});
  check(!view.remaining.has_value(), "Early ETA must remain hidden");
  check(view.remaining_label == L"計算中", "Active ETA should say calculating");
  check(view.cancellation_allowed, "Cancellation state should be preserved");
}

void test_eta_and_progress_are_bounded() {
  const auto view = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::OperationStage::writing,
          .processed_bytes = 32ULL * 1024ULL * 1024ULL,
          .total_bytes = 64ULL * 1024ULL * 1024ULL,
          .elapsed = std::chrono::seconds(4)});
  check(view.fraction == 0.5, "Progress should be one half");
  check(view.bytes_per_second == 8ULL * 1024ULL * 1024ULL,
        "Speed should be calculated from elapsed time");
  check(
      view.remaining == std::chrono::seconds(4),
      "ETA should be calculated after the stability threshold");

  const auto overflow_safe = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::OperationStage::verifying,
          .processed_bytes = 16ULL * 1024ULL * 1024ULL,
          .total_bytes = (std::numeric_limits<std::uint64_t>::max)(),
          .elapsed = std::chrono::seconds(3)});
  check(
      overflow_safe.remaining.has_value(),
      "Large totals should calculate without unsigned overflow");
}

void test_terminal_labels() {
  const auto waiting = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{});
  check(waiting.remaining_label == L"—", "Waiting ETA should be neutral");

  const auto completed = ytec::windowsapp::calculate_progress(
      ytec::windowsapp::ProgressInput{
          .stage = ytec::windowsapp::OperationStage::completed,
          .processed_bytes = 100,
          .total_bytes = 100,
          .elapsed = std::chrono::seconds(8)});
  check(completed.fraction == 1.0, "Completed progress should be full");
  check(completed.remaining_label == L"0秒", "Completed ETA should be zero");
}

void test_online_image_progress_keeps_work_streams_distinct() {
  const auto writing =
      ytec::windowsapp::build_online_image_progress_view(
          ytec::clonecore::DiskOperationProgress{
              .stage =
                  ytec::clonecore::DiskOperationStage::copying_data,
              .total_read_bytes = 32ULL * 1024ULL * 1024ULL,
              .total_write_bytes = 32ULL * 1024ULL * 1024ULL,
              .total_verify_bytes = 64ULL * 1024ULL * 1024ULL,
              .read_bytes = 32ULL * 1024ULL * 1024ULL,
              .written_bytes = 32ULL * 1024ULL * 1024ULL,
              .verified_bytes = 0,
              .cancellation_allowed = true,
              .pause_allowed = true,
          },
          std::chrono::seconds(4));
  check(
      writing.fraction == 0.5 &&
          writing.percentage_label == L"50%",
      "Online-image percentage should cover read, write, and verification");
  check(
      writing.read_label == L"32 / 32 MiB" &&
          writing.write_label == L"32 / 32 MiB" &&
          writing.verified_label == L"0 / 64 MiB",
      "Online-image counters should stay visually distinct");
  check(
      writing.remaining_label == L"4秒" &&
          writing.cancellation_allowed && writing.pause_allowed,
      "Stable aggregate work should expose ETA, cancellation, and pause");

  const auto finalizing =
      ytec::windowsapp::build_online_image_progress_view(
          ytec::clonecore::DiskOperationProgress{
              .stage =
                  ytec::clonecore::DiskOperationStage::flushing_data,
              .total_read_bytes = 32ULL * 1024ULL * 1024ULL,
              .total_write_bytes = 32ULL * 1024ULL * 1024ULL,
              .total_verify_bytes = 64ULL * 1024ULL * 1024ULL,
              .read_bytes = 32ULL * 1024ULL * 1024ULL,
              .written_bytes = 32ULL * 1024ULL * 1024ULL,
              .verified_bytes = 64ULL * 1024ULL * 1024ULL,
              .cancellation_allowed = false,
              .pause_allowed = false,
          },
          std::chrono::seconds(9));
  check(
      finalizing.remaining_label == L"仕上げ中" &&
          !finalizing.cancellation_allowed && !finalizing.pause_allowed,
      "Final file commit should avoid an exact ETA and disable controls");
}

void test_clone_selection_safety() {
  ytec::diskmodel::DiskInfo system;
  system.disk_number = 1;
  system.size_bytes = 512;
  system.is_system_disk = true;
  system.read_only = false;

  ytec::diskmodel::DiskInfo target;
  target.disk_number = 2;
  target.size_bytes = 1024;
  target.read_only = false;
  target.partition_style = ytec::diskmodel::PartitionStyle::raw;

  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(system);
  inventory.disks.push_back(target);

  const auto ready = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false);
  check(
      ready.ready && !ready.target_requires_initialization,
      "System-to-larger empty RAW selection should be ready");

  const auto same = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 0, false);
  check(
      same.issue == ytec::windowsapp::CloneSelectionIssue::same_disk,
      "The same disk must be rejected");

  const auto system_target =
      ytec::windowsapp::evaluate_clone_selection(
          &inventory, 1, 0, false);
  check(
      system_target.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_is_system,
      "The running Windows disk must never be selected as target");

  inventory.disks[1].size_bytes = 256;
  const auto too_small = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false);
  check(
      too_small.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_too_small,
      "A smaller target must fail closed");
  const auto shrink_ready = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false, false);
  check(
      shrink_ready.ready,
      "Shrink mode should defer exact fit calculation and allow a smaller target candidate");

  inventory.disks[1].partition_style =
      ytec::diskmodel::PartitionStyle::gpt;
  inventory.disks[1].partitions.push_back(
      ytec::diskmodel::PartitionInfo{
          .number = 1,
          .style = ytec::diskmodel::PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
      });
  const auto initialized = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false, false);
  check(
      initialized.ready && initialized.target_requires_initialization,
      "Both transfer modes should accept a known basic initialized target");

  inventory.disks[1].partitions.front().type =
      L"{AF9B60A0-1431-4F62-BC68-3311714A69AD}";
  const auto unsupported = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false, false);
  check(
      unsupported.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_layout_unsupported,
      "Unknown or dynamic target layouts must still fail closed");

  inventory.disks[1].size_bytes = 1024;
  inventory.disks[1].partition_style =
      ytec::diskmodel::PartitionStyle::raw;
  inventory.disks[1].partitions.clear();
  inventory.disks[1].read_only.reset();
  const auto unknown = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false);
  check(
      unknown.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_state_unknown,
      "Unknown target state must fail closed");

  inventory.disks[1].read_only = false;
  inventory.disks[1].removable = true;
  inventory.disks[1].bus_type = L"USB";
  const auto usb_memory = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false);
  check(
      usb_memory.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_is_usb_memory,
      "A removable USB memory device must not be offered as clone target");

  inventory.disks[1].removable = false;
  const auto usb_disk = ytec::windowsapp::evaluate_clone_selection(
      &inventory, 0, 1, false);
  check(usb_disk.ready,
        "A non-removable USB HDD/SSD should remain a clone target");

  inventory.disks[1].health.state =
      ytec::diskmodel::DiskHealthState::failing;
  const auto failing_target =
      ytec::windowsapp::evaluate_clone_selection(
          &inventory, 0, 1, false);
  check(
      failing_target.issue ==
          ytec::windowsapp::CloneSelectionIssue::target_health_abnormal,
      "A SMART/NVMe abnormal target must fail closed");
}

void test_transfer_mode_context_is_explicit_and_fail_closed() {
  using ytec::windowsapp::WindowsTransferModeChoice;
  using ytec::windowsapp::WindowsTransferModeContext;

  const auto clone = ytec::windowsapp::windows_transfer_mode_options(
      WindowsTransferModeContext::clone);
  const auto image = ytec::windowsapp::windows_transfer_mode_options(
      WindowsTransferModeContext::create_image);
  const auto contains = [](const auto options,
                           const WindowsTransferModeChoice choice) {
    return std::ranges::any_of(
        options,
        [choice](const ytec::windowsapp::WindowsTransferModeOption& option) {
          return option.choice == choice;
        });
  };

  check(clone.size() == 3U, "The clone page must retain three transfer modes");
  check(image.size() == 3U, "The image-create page must expose three modes");
  check(
      contains(clone, WindowsTransferModeChoice::exact) &&
          contains(clone, WindowsTransferModeChoice::shrink) &&
          contains(clone, WindowsTransferModeChoice::rescue),
      "Clone transfer modes must remain normal, shrink, and rescue");
  check(
      contains(image, WindowsTransferModeChoice::exact) &&
          contains(image, WindowsTransferModeChoice::shrink) &&
          contains(image, WindowsTransferModeChoice::rescue),
      "Image-create transfer modes must remain normal, shrink, and rescue");
  const auto unknown_context =
      static_cast<WindowsTransferModeContext>(0xFFU);
  check(
      ytec::windowsapp::windows_transfer_mode_options(unknown_context)
              .empty() &&
          !ytec::windowsapp::windows_transfer_mode_allowed(
              unknown_context,
              WindowsTransferModeChoice::exact),
      "An unknown page context must not inherit another page's combo model");

  for (const auto& option : clone) {
    const auto decoded =
        ytec::windowsapp::decode_windows_transfer_mode_item_data(
            ytec::windowsapp::windows_transfer_mode_item_data(option.choice));
    check(
        decoded.has_value() && decoded.value() == option.choice,
        "Every combo item must round-trip through explicit item data");
  }
  check(
      !ytec::windowsapp::decode_windows_transfer_mode_item_data(0U)
           .has_value() &&
          !ytec::windowsapp::decode_windows_transfer_mode_item_data(999U)
               .has_value(),
      "Missing or unknown combo item data must fail closed");

  check(
      ytec::windowsapp::windows_transfer_mode_requires_same_or_larger_target(
          WindowsTransferModeChoice::exact) &&
          ytec::windowsapp::
              windows_transfer_mode_requires_same_or_larger_target(
                  WindowsTransferModeChoice::rescue) &&
          !ytec::windowsapp::
              windows_transfer_mode_requires_same_or_larger_target(
                  WindowsTransferModeChoice::shrink),
      "Only shrink mode may admit a smaller target candidate");

  using ytec::windowsapp::WindowsPartitionStyleChoice;
  const auto styles = ytec::windowsapp::windows_partition_style_options();
  check(styles.size() == 2U, "Clone style selector must expose two choices");
  for (const auto& option : styles) {
    const auto decoded =
        ytec::windowsapp::decode_windows_partition_style_item_data(
            ytec::windowsapp::windows_partition_style_item_data(
                option.choice));
    check(
        decoded.has_value() && decoded.value() == option.choice,
        "Every partition-style item must round-trip through item data");
  }
  check(
      !ytec::windowsapp::decode_windows_partition_style_item_data(0U)
           .has_value() &&
          !ytec::windowsapp::decode_windows_partition_style_item_data(999U)
               .has_value(),
      "Unknown partition-style item data must fail closed");
  check(
      !ytec::windowsapp::windows_partition_style_choice_allowed(
          WindowsTransferModeChoice::exact,
          ytec::diskmodel::PartitionStyle::mbr,
          true,
          WindowsPartitionStyleChoice::mbr_to_gpt) &&
          ytec::windowsapp::windows_partition_style_choice_allowed(
              WindowsTransferModeChoice::shrink,
              ytec::diskmodel::PartitionStyle::mbr,
              true,
              WindowsPartitionStyleChoice::mbr_to_gpt),
      "This product slice must expose MBR-to-GPT only with shrink transfer");
  check(
      !ytec::windowsapp::windows_partition_style_choice_allowed(
          WindowsTransferModeChoice::rescue,
          ytec::diskmodel::PartitionStyle::mbr,
          true,
          WindowsPartitionStyleChoice::mbr_to_gpt) &&
          !ytec::windowsapp::windows_partition_style_choice_allowed(
              WindowsTransferModeChoice::shrink,
              ytec::diskmodel::PartitionStyle::gpt,
              true,
              WindowsPartitionStyleChoice::mbr_to_gpt) &&
          !ytec::windowsapp::windows_partition_style_choice_allowed(
              WindowsTransferModeChoice::shrink,
              ytec::diskmodel::PartitionStyle::mbr,
              false,
              WindowsPartitionStyleChoice::mbr_to_gpt),
      "Rescue, GPT sources, and non-system sources must force preserve");
  check(
      ytec::windowsapp::windows_partition_style_choice_allowed(
          WindowsTransferModeChoice::exact,
          ytec::diskmodel::PartitionStyle::mbr,
          true,
          WindowsPartitionStyleChoice::preserve) &&
          ytec::windowsapp::windows_partition_style_choice_allowed(
              WindowsTransferModeChoice::shrink,
              ytec::diskmodel::PartitionStyle::gpt,
              true,
              WindowsPartitionStyleChoice::preserve) &&
          ytec::windowsapp::windows_partition_style_choice_allowed(
              WindowsTransferModeChoice::rescue,
              ytec::diskmodel::PartitionStyle::mbr,
              false,
              WindowsPartitionStyleChoice::preserve),
      "Preserve must remain available for exact, shrink, and rescue modes");
  check(
      ytec::windowsapp::windows_partition_style_route_available(
          WindowsTransferModeChoice::shrink,
          ytec::diskmodel::PartitionStyle::mbr,
          true,
          WindowsPartitionStyleChoice::preserve) &&
          ytec::windowsapp::windows_partition_style_route_available(
              WindowsTransferModeChoice::shrink,
              ytec::diskmodel::PartitionStyle::mbr,
              true,
              WindowsPartitionStyleChoice::mbr_to_gpt) &&
          ytec::windowsapp::windows_partition_style_route_available(
              WindowsTransferModeChoice::shrink,
              ytec::diskmodel::PartitionStyle::gpt,
              true,
              WindowsPartitionStyleChoice::preserve) &&
          !ytec::windowsapp::windows_partition_style_route_available(
              static_cast<WindowsTransferModeChoice>(0xFFU),
              ytec::diskmodel::PartitionStyle::mbr,
              true,
              WindowsPartitionStyleChoice::preserve) &&
          !ytec::windowsapp::windows_partition_style_route_available(
          WindowsTransferModeChoice::shrink,
          ytec::diskmodel::PartitionStyle::unknown,
          true,
          WindowsPartitionStyleChoice::preserve),
      "MBR-preserving shrink and target-only conversion must be explicit routes while unknown styles fail closed");
}

}  // namespace

int main() {
  try {
    test_units_and_duration();
    test_eta_is_hidden_until_stable();
    test_eta_and_progress_are_bounded();
    test_terminal_labels();
    test_online_image_progress_keeps_work_streams_distinct();
    test_clone_selection_safety();
    test_transfer_mode_context_is_explicit_and_fail_closed();
    std::cout << "windows app progress tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "windows app progress tests: FAIL: "
              << failure.message << '\n';
    return 1;
  }
}

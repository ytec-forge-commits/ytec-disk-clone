#include "ytec/winpeapp/dashboard.h"

#include <chrono>
#include <iostream>
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

ytec::diskmodel::DiskInfo disk(
    const std::uint32_t number,
    const bool system_disk) {
  return ytec::diskmodel::DiskInfo{
      .disk_number = number,
      .device_path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number),
      .device_instance_id = L"MOCK-" + std::to_wstring(number),
      .model = number == 0 ? L"Tsumugi Source" : L"Tsumugi Target",
      .size_bytes = number == 0 ? 64ULL * 1024ULL * 1024ULL * 1024ULL
                                : 80ULL * 1024ULL * 1024ULL * 1024ULL,
      .sector_count = 0,
      .logical_sector_size = 512,
      .physical_sector_size = 4096,
      .bus_type = L"NVMe",
      .serial_suffix = number == 0 ? "SOURCE01" : "TARGET01",
      .partition_style = ytec::diskmodel::PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = system_disk,
      .partitions = {
          ytec::diskmodel::PartitionInfo{
              .number = 1,
              .offset_bytes = 1024ULL * 1024ULL,
              .size_bytes = 100ULL * 1024ULL * 1024ULL,
              .style = ytec::diskmodel::PartitionStyle::gpt,
              .type = L"EFI",
              .identifier = L"MOCK",
              .name = L"EFIシステム",
              .bootable = true}},
  };
}

void ready_inventory_is_clear_and_selectable() {
  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(disk(0, true));
  inventory.disks.push_back(disk(1, false));
  inventory.disks[1].health.state =
      ytec::diskmodel::DiskHealthState::healthy;
  inventory.disks[1].health.temperature_celsius = 38;

  const auto view = ytec::winpeapp::build_dashboard_view(inventory);
  check(
      view.readiness == ytec::winpeapp::DashboardReadiness::ready,
      "Complete inventory should be ready");
  check(view.direct_clone_available,
        "Direct clone selection should be available");
  check(view.boot_repair_review_available,
        "Boot repair review should be available");
  check(view.disks.size() == 2, "Both disks should be displayed");
  check(
      view.disks[1].list_label ==
          L"ディスク 1  ·  80.0 GiB  Tsumugi Target  ·  健康 警告なし / 38°C",
      "One-line disk label should include identity and health information");
  check(!view.disks[0].selectable_as_target,
        "Current system disk must not be a target candidate");
  check(view.disks[1].selectable_as_target,
        "Known fixed non-system disk should be a target candidate");
  check(view.disks[1].details.find(L"シリアル末尾: …TARGET01") !=
            std::wstring::npos,
        "Only the masked serial suffix should be displayed");
  check(view.disks[1].details.find(L"健康状態: 警告なし / 38°C") !=
            std::wstring::npos,
        "Health state and temperature should be displayed");
}

void unknown_target_attribute_fails_closed() {
  ytec::diskmodel::InventoryReport inventory;
  auto target = disk(2, false);
  target.read_only.reset();
  inventory.disks.push_back(std::move(target));

  const auto view = ytec::winpeapp::build_dashboard_view(inventory);
  check(!view.disks[0].selectable_as_target,
        "Unknown read-only state must not be considered safe");
  check(view.disks[0].details.find(L"読み取り専用: 不明") !=
            std::wstring::npos,
        "Unknown state should be explained");
}

void inventory_issue_disables_all_operation_reviews() {
  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(disk(1, false));
  inventory.issues.push_back(ytec::diskmodel::InventoryIssue{
      .device = L"PhysicalDrive2",
      .error = ytec::clonecore::Error{
          .code = ytec::clonecore::ErrorCode::query_failed,
          .native_code = 5,
          .operation = L"安全属性の確認",
          .message = L"アクセスできません",
      }});

  const auto view = ytec::winpeapp::build_dashboard_view(inventory);
  check(
      view.readiness == ytec::winpeapp::DashboardReadiness::warning,
      "Partial inventory should be a warning");
  check(!view.direct_clone_available,
        "Direct clone must stop on unresolved inventory issues");
  check(!view.boot_repair_review_available,
        "Boot repair review must stop on unresolved inventory issues");
  check(view.diagnostics.size() == 1,
        "The inventory issue should remain visible");
}

void abnormal_health_is_visible_and_not_a_target() {
  ytec::diskmodel::InventoryReport inventory;
  auto target = disk(3, false);
  target.health.state = ytec::diskmodel::DiskHealthState::caution;
  target.health.temperature_celsius = 71;
  target.health.temperature_warning = true;
  inventory.disks.push_back(std::move(target));

  const auto view = ytec::winpeapp::build_dashboard_view(inventory);
  check(
      view.readiness == ytec::winpeapp::DashboardReadiness::warning &&
          view.headline.find(L"健康状態") != std::wstring::npos,
      "Health evidence should make the dashboard warning explicit");
  check(
      view.direct_clone_available,
      "A source health warning should not disable every operation globally");
  check(!view.disks[0].selectable_as_target,
        "A SMART/NVMe caution disk must not be a target candidate");
  check(view.disks[0].list_label.find(L"注意 / 71°C（温度警告）") !=
            std::wstring::npos,
        "Operation selectors must retain the health and temperature warning");
  check(view.disks[0].details.find(
            L"健康状態: 注意 / 71°C（温度警告）") !=
            std::wstring::npos,
        "The health and temperature warning must remain visible");
}

void no_disks_is_blocked() {
  const auto view = ytec::winpeapp::build_dashboard_view({});
  check(
      view.readiness == ytec::winpeapp::DashboardReadiness::blocked,
      "No disk inventory should be blocked");
  check(!view.direct_clone_available,
        "No-disk state must not expose direct clone");
}

void operation_progress_is_clear_and_eta_is_conservative() {
  const auto verification = ytec::winpeapp::build_operation_progress_view(
      ytec::clonecore::DiskOperationProgress{
          .stage =
              ytec::clonecore::DiskOperationStage::verifying_source,
          .total_read_bytes = 64ULL * 1024ULL * 1024ULL,
          .total_verify_bytes = 64ULL * 1024ULL * 1024ULL,
          .read_bytes = 16ULL * 1024ULL * 1024ULL,
          .verified_bytes = 16ULL * 1024ULL * 1024ULL,
          .cancellation_allowed = true,
          .pause_allowed = true,
      },
      std::chrono::seconds(2));
  check(
      verification.stage_label == L"復元イメージを完全検証中" &&
          verification.remaining_label == L"計算中",
      "Source verification should be an explicit cancellable progress stage");

  const auto early = ytec::winpeapp::build_operation_progress_view(
      ytec::clonecore::DiskOperationProgress{
          .stage = ytec::clonecore::DiskOperationStage::copying_data,
          .partition_index = 2,
          .total_read_bytes = 64ULL * 1024ULL * 1024ULL,
          .total_write_bytes = 64ULL * 1024ULL * 1024ULL,
          .total_verify_bytes = 64ULL * 1024ULL * 1024ULL,
          .read_bytes = 8ULL * 1024ULL * 1024ULL,
          .written_bytes = 8ULL * 1024ULL * 1024ULL,
          .verified_bytes = 8ULL * 1024ULL * 1024ULL,
          .cancellation_allowed = true,
          .pause_allowed = true,
      },
      std::chrono::seconds(2));
  check(
      early.remaining_label == L"計算中",
      "ETA should remain hidden until enough stable progress exists");
  check(
      early.partition_label == L"パーティション #2" &&
          early.cancellation_allowed && early.pause_allowed,
      "The current partition, cancellation, and pause state should be visible");

  const auto stable = ytec::winpeapp::build_operation_progress_view(
      ytec::clonecore::DiskOperationProgress{
          .stage = ytec::clonecore::DiskOperationStage::copying_data,
          .partition_index = 2,
          .total_read_bytes = 32ULL * 1024ULL * 1024ULL,
          .total_write_bytes = 64ULL * 1024ULL * 1024ULL,
          .total_verify_bytes = 64ULL * 1024ULL * 1024ULL,
          .read_bytes = 32ULL * 1024ULL * 1024ULL,
          .written_bytes = 32ULL * 1024ULL * 1024ULL,
          .verified_bytes = 32ULL * 1024ULL * 1024ULL,
          .cancellation_allowed = true,
          .pause_allowed = true,
      },
      std::chrono::seconds(4));
  check(stable.fraction == 0.5 && stable.percentage_label == L"50%",
        "Verified bytes should drive the bounded percentage");
  check(stable.speed_label == L"8.0 MiB/秒",
        "Verified throughput should be displayed");
  check(stable.remaining_label == L"4秒",
        "Stable throughput should produce a conservative ETA");
  check(stable.read_label == L"32 / 32 MiB" &&
            stable.write_label == L"32 / 64 MiB" &&
            stable.verified_label == L"32 / 64 MiB",
        "Read, write, and verification counters must remain distinct");
}

void final_commit_is_presented_as_non_cancellable() {
  const auto view = ytec::winpeapp::build_operation_progress_view(
      ytec::clonecore::DiskOperationProgress{
          .stage =
              ytec::clonecore::DiskOperationStage::
                  committing_partition_table,
          .total_read_bytes = 64ULL * 1024ULL * 1024ULL,
          .total_write_bytes = 64ULL * 1024ULL * 1024ULL,
          .total_verify_bytes = 64ULL * 1024ULL * 1024ULL,
          .read_bytes = 64ULL * 1024ULL * 1024ULL,
          .written_bytes = 64ULL * 1024ULL * 1024ULL,
          .verified_bytes = 64ULL * 1024ULL * 1024ULL,
          .cancellation_allowed = false,
          .pause_allowed = false,
      },
      std::chrono::seconds(12));
  check(view.remaining_label == L"仕上げ中",
        "Final metadata commit should not claim an exact ETA");
  check(!view.cancellation_allowed && !view.pause_allowed,
        "Final partition-table commit must disable cancel and pause");
  check(view.stage_label.find(L"最終確定") != std::wstring::npos,
        "The non-cancellable reason should be apparent from the stage label");
}

}  // namespace

int main() {
  int failures = 0;
  const auto run = [&](const char* name, const auto& test) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const TestFailure& failure) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << failure.message << '\n';
    }
  };

  run("ready_inventory_is_clear_and_selectable",
      ready_inventory_is_clear_and_selectable);
  run("unknown_target_attribute_fails_closed",
      unknown_target_attribute_fails_closed);
  run("inventory_issue_disables_all_operation_reviews",
      inventory_issue_disables_all_operation_reviews);
  run("abnormal_health_is_visible_and_not_a_target",
      abnormal_health_is_visible_and_not_a_target);
  run("no_disks_is_blocked", no_disks_is_blocked);
  run("operation_progress_is_clear_and_eta_is_conservative",
      operation_progress_is_clear_and_eta_is_conservative);
  run("final_commit_is_presented_as_non_cancellable",
      final_commit_is_presented_as_non_cancellable);
  return failures == 0 ? 0 : 1;
}

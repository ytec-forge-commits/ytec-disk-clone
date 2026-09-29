#pragma once

#include "ytec/clonecore/operation_progress.h"
#include "ytec/diskmodel/disk_inventory.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ytec::winpeapp {

enum class DashboardReadiness : std::uint8_t {
  scanning,
  ready,
  warning,
  blocked,
};

struct DashboardDiskView final {
  std::uint32_t disk_number{};
  std::wstring title;
  std::wstring list_label;
  std::wstring summary;
  std::wstring details;
  bool system_disk{};
  bool selectable_as_target{};
};

struct DashboardView final {
  DashboardReadiness readiness{DashboardReadiness::scanning};
  std::wstring headline;
  std::wstring guidance;
  std::vector<DashboardDiskView> disks;
  std::vector<std::wstring> diagnostics;
  bool direct_clone_available{};
  bool boot_repair_review_available{};
};

struct OperationProgressView final {
  double fraction{};
  std::wstring percentage_label;
  std::wstring stage_label;
  std::wstring partition_label;
  std::wstring read_label;
  std::wstring write_label;
  std::wstring verified_label;
  std::wstring speed_label;
  std::wstring elapsed_label;
  std::wstring remaining_label;
  bool cancellation_allowed{};
  bool pause_allowed{};
};

// Builds the read-only WinPE dashboard from an already collected inventory.
// It never opens a disk and never treats an unknown safety attribute as safe.
[[nodiscard]] DashboardView build_dashboard_view(
    const diskmodel::InventoryReport& inventory);

[[nodiscard]] std::wstring format_dashboard_capacity(
    std::uint64_t bytes);

[[nodiscard]] std::wstring format_dashboard_health(
    const diskmodel::DiskInfo& disk);

[[nodiscard]] std::wstring format_dashboard_health(
    const diskmodel::DiskHealthInfo& health);

[[nodiscard]] OperationProgressView build_operation_progress_view(
    const clonecore::DiskOperationProgress& progress,
    std::chrono::milliseconds elapsed);

}  // namespace ytec::winpeapp

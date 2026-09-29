#include "ytec/winpeapp/active_rescue_media.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kMarker[] = "12345678-1234-4abc-8def-1234567890ab";
constexpr char kOtherMarker[] = "abcdef01-2345-4678-9abc-def012345678";

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error synthetic_error(std::wstring operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::query_failed,
      .native_code = ERROR_NOT_READY,
      .operation = std::move(operation),
      .message = L"合成照会失敗です",
  };
}

ytec::diskmodel::DiskInfo media_disk(const std::uint32_t number = 7U) {
  return ytec::diskmodel::DiskInfo{
      .disk_number = number,
      .device_path = L"\\\\.\\PhysicalDrive7",
      .device_instance_id = L"USBSTOR\\YTEC-RESCUE-MEDIA",
      .model = L"Y-TEC Rescue USB",
      .size_bytes = 32ULL * 1024ULL * 1024ULL * 1024ULL,
      .sector_count = 32ULL * 1024ULL * 1024ULL * 1024ULL / 512ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"USB",
      .serial_suffix = "B007",
      .partition_style = ytec::diskmodel::PartitionStyle::mbr,
      .offline = false,
      .read_only = false,
      .removable = true,
  };
}

ytec::diskmodel::DiskInfo other_disk() {
  auto disk = media_disk(8U);
  disk.device_path = L"\\\\.\\PhysicalDrive8";
  disk.device_instance_id = L"SCSI\\RESTORE-TARGET";
  disk.model = L"Restore Target SSD";
  disk.serial_suffix = "T008";
  disk.bus_type = L"SATA";
  disk.removable = false;
  return disk;
}

ytec::clonecore::StableDiskIdentity stable(
    const ytec::diskmodel::DiskInfo& disk) {
  auto identity = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(identity.has_value(), "synthetic stable identity must build");
  return identity.take_value();
}

std::wstring candidate_path(const std::wstring& root) {
  return root + std::wstring(
      ytec::winpeapp::kActiveRescueMediaMarkerRelativePath);
}

struct Fixture final {
  std::map<std::wstring, std::optional<std::string>> markers{
      {std::wstring(
           ytec::winpeapp::kActiveRescueMediaRuntimeMarkerPath),
       std::string(kMarker)},
      {candidate_path(L"E:\\"), std::string(kMarker)},
  };
  std::vector<std::wstring> roots{L"X:\\", L"C:\\", L"E:\\"};
  std::vector<ytec::diskmodel::DiskInfo> disks{media_disk(), other_disk()};
  std::optional<std::wstring> read_failure_path;
  bool roots_fail{};
  bool drive_type_query_fails{};
  bool marker_changes_after_drive_type_query{};
  bool disk_query_fails{};
  bool inventory_fails{};
  bool marker_changes_after_disk_query{};
  std::uint32_t resolved_disk_number{7U};
  std::uint32_t matching_drive_type{DRIVE_REMOVABLE};
  std::size_t marker_reads{};
  std::size_t drive_type_queries{};
  std::size_t disk_queries{};
  std::size_t inventory_queries{};

  ytec::winpeapp::ActiveRescueMediaDependencies dependencies() {
    return {
        .read_marker =
            [&](const std::wstring& path) {
              ++marker_reads;
              if (read_failure_path.has_value() &&
                  path == *read_failure_path) {
                return ytec::clonecore::Result<
                    std::optional<std::string>>::failure(
                    synthetic_error(L"合成マーカー読取り"));
              }
              const auto found = markers.find(path);
              return ytec::clonecore::Result<
                  std::optional<std::string>>::success(
                  found == markers.end() ? std::nullopt : found->second);
            },
        .enumerate_local_drive_roots =
            [&]() {
              if (roots_fail) {
                return ytec::clonecore::Result<
                    std::vector<std::wstring>>::failure(
                    synthetic_error(L"合成ドライブ列挙"));
              }
              return ytec::clonecore::Result<
                   std::vector<std::wstring>>::success(roots);
            },
        .query_local_drive_type =
            [&](const std::wstring& root) {
              ++drive_type_queries;
              check(root == L"E:\\" || root == L"F:\\",
                    "only the unique matching marker root may be classified");
              if (drive_type_query_fails) {
                return ytec::clonecore::Result<std::uint32_t>::failure(
                    synthetic_error(L"合成ドライブ種別照会"));
              }
              if (marker_changes_after_drive_type_query) {
                markers[candidate_path(root)] = std::string(kOtherMarker);
              }
              return ytec::clonecore::Result<std::uint32_t>::success(
                  matching_drive_type);
            },
        .query_single_disk_number_for_path =
            [&](const std::wstring& path) {
              ++disk_queries;
              check(path == candidate_path(L"E:\\") ||
                        path == candidate_path(L"F:\\"),
                    "only the unique matching marker path may be mapped");
              if (disk_query_fails) {
                return ytec::clonecore::Result<std::uint32_t>::failure(
                    synthetic_error(L"合成物理ディスク対応"));
              }
              if (marker_changes_after_disk_query) {
                markers[path] = std::string(kOtherMarker);
              }
              return ytec::clonecore::Result<std::uint32_t>::success(
                  resolved_disk_number);
            },
        .enumerate_disks =
            [&]() {
              ++inventory_queries;
              if (inventory_fails) {
                return ytec::clonecore::Result<
                    ytec::diskmodel::InventoryReport>::failure(
                    synthetic_error(L"合成ディスク一覧"));
              }
              return ytec::clonecore::Result<
                  ytec::diskmodel::InventoryReport>::success({
                  .disks = disks,
              });
            },
    };
  }
};

ytec::clonecore::TargetConfirmation arbitrary_confirmation() {
  return {
      .first_step_acknowledged = false,
      .typed_token = L"lowercase-is-not-reinterpreted",
  };
}

void exact_stable_identity_is_active_without_interpreting_confirmation() {
  Fixture fixture;
  auto selected = stable(media_disk(91U));
  const auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      selected, arbitrary_confirmation(), fixture.dependencies());
  check(result.has_value() && result.value(),
        "the unique matching medium must be recognized by stable identity");
  check(fixture.disk_queries == 1U && fixture.inventory_queries == 1U,
        "the marker candidate must be resolved and freshly inventoried once");
  check(fixture.drive_type_queries == 2U,
        "the unique matching medium class must be freshly rechecked");
}

void storage_observation_proves_marker_and_physical_identity() {
  Fixture fixture;
  const auto result = ytec::winpeapp::resolve_active_rescue_media_storage(
      fixture.dependencies());
  check(result.has_value(), "the unique rescue storage must resolve");
  check(
      result.value().marker_path == candidate_path(L"E:\\") &&
          result.value().drive_type == DRIVE_REMOVABLE &&
          result.value().marker_identity_from_open_handle &&
          result.value().physical_identity.has_value(),
      "removable storage must expose the opened marker and stable identity");
  check(
      result.value().physical_identity->disk_number == 7U,
      "the storage observation must bind the freshly inventoried disk");

  Fixture optical;
  optical.matching_drive_type = DRIVE_CDROM;
  const auto optical_result =
      ytec::winpeapp::resolve_active_rescue_media_storage(
          optical.dependencies());
  check(
      optical_result.has_value() &&
          optical_result.value().marker_identity_from_open_handle &&
          !optical_result.value().physical_identity.has_value(),
      "optical storage must retain marker proof without a physical target");
}

void different_stable_identity_is_not_active() {
  Fixture fixture;
  const auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(other_disk()), arbitrary_confirmation(), fixture.dependencies());
  check(result.has_value() && !result.value(),
        "a different stable identity must be positively reported as inactive");
}

void runtime_marker_is_mandatory_and_strict() {
  Fixture missing;
  missing.markers.erase(std::wstring(
      ytec::winpeapp::kActiveRescueMediaRuntimeMarkerPath));
  auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), missing.dependencies());
  check(!result && missing.disk_queries == 0U,
        "legacy media without the WIM marker must fail before disk mapping");

  Fixture malformed;
  malformed.markers[std::wstring(
      ytec::winpeapp::kActiveRescueMediaRuntimeMarkerPath)] =
      "not-a-guid";
  result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), malformed.dependencies());
  check(!result && malformed.disk_queries == 0U,
        "a malformed WIM marker must fail closed");

  Fixture all_zero;
  all_zero.markers[std::wstring(
      ytec::winpeapp::kActiveRescueMediaRuntimeMarkerPath)] =
      "00000000-0000-0000-0000-000000000000";
  result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), all_zero.dependencies());
  check(!result, "the nil GUID must not identify a rescue medium");
}

void zero_or_multiple_matching_media_roots_fail_closed() {
  Fixture zero;
  zero.markers[candidate_path(L"E:\\")] = std::string(kOtherMarker);
  auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), zero.dependencies());
  check(!result && zero.disk_queries == 0U,
        "zero matching media roots must fail before physical mapping");

  Fixture multiple;
  multiple.roots.push_back(L"F:\\");
  multiple.markers[candidate_path(L"F:\\")] = std::string(kMarker);
  result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), multiple.dependencies());
  check(!result && multiple.disk_queries == 0U,
        "duplicate media markers must fail before physical mapping");
}

void ramdisk_marker_is_not_counted_as_a_physical_candidate() {
  Fixture fixture;
  // The X: marker is always present inside boot.wim. It is the reference,
  // never a physical-media candidate.
  const auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), fixture.dependencies());
  check(result.has_value() && result.value(),
        "X: plus one physical marker must remain a unique physical match");
  check(fixture.marker_reads == 5U,
        "X: must not be a candidate and the mapped marker must be rechecked");
}

void verified_optical_media_is_not_a_physical_target() {
  Fixture fixture;
  fixture.matching_drive_type = DRIVE_CDROM;
  const auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(other_disk()), arbitrary_confirmation(), fixture.dependencies());
  check(result.has_value() && !result.value(),
        "a verified optical boot medium cannot be a physical-disk target");
  check(fixture.drive_type_queries == 2U && fixture.disk_queries == 0U &&
            fixture.inventory_queries == 0U,
        "optical media must be reclassified without physical-disk mapping");
  check(fixture.marker_reads == 4U,
        "the optical marker must remain identical before returning inactive");

  Fixture changed;
  changed.matching_drive_type = DRIVE_CDROM;
  changed.marker_changes_after_drive_type_query = true;
  const auto changed_result =
      ytec::winpeapp::resolve_active_rescue_media_target(
          stable(other_disk()),
          arbitrary_confirmation(),
          changed.dependencies());
  check(!changed_result && changed.disk_queries == 0U,
        "an optical marker change must fail before any disk mapping");
}

void malformed_candidate_or_read_uncertainty_fails_closed() {
  Fixture malformed;
  malformed.markers[candidate_path(L"C:\\")] = "bad";
  auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), malformed.dependencies());
  check(!result && malformed.disk_queries == 0U,
        "a present malformed marker must not be silently ignored");

  Fixture unreadable;
  unreadable.read_failure_path = candidate_path(L"C:\\");
  result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), unreadable.dependencies());
  check(!result && unreadable.disk_queries == 0U,
        "an unreadable local marker location must remain uncertain");
}

void physical_mapping_and_inventory_must_be_unambiguous() {
  Fixture mapping_failure;
  mapping_failure.disk_query_fails = true;
  auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()),
      arbitrary_confirmation(),
      mapping_failure.dependencies());
  check(!result && mapping_failure.inventory_queries == 0U,
        "an unknown marker-to-disk mapping must fail before inventory match");

  Fixture changed_marker;
  changed_marker.marker_changes_after_disk_query = true;
  result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()),
      arbitrary_confirmation(),
      changed_marker.dependencies());
  check(!result && changed_marker.inventory_queries == 0U,
        "the marker must remain identical across physical-disk mapping");

  Fixture absent;
  absent.resolved_disk_number = 99U;
  result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), absent.dependencies());
  check(!result, "a mapped disk absent from inventory must fail closed");

  Fixture duplicate;
  duplicate.disks.push_back(media_disk());
  result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), duplicate.dependencies());
  check(!result, "duplicate inventory disk numbers must fail closed");
}

void malformed_input_and_missing_dependencies_fail_before_io() {
  Fixture fixture;
  auto invalid = stable(media_disk());
  invalid.serial_suffix.clear();
  invalid.device_instance_id.clear();
  auto result = ytec::winpeapp::resolve_active_rescue_media_target(
      invalid, arbitrary_confirmation(), fixture.dependencies());
  check(!result && fixture.marker_reads == 0U,
        "an invalid target identity must fail before marker I/O");

  ytec::winpeapp::ActiveRescueMediaDependencies missing{};
  result = ytec::winpeapp::resolve_active_rescue_media_target(
      stable(media_disk()), arbitrary_confirmation(), missing);
  check(!result, "missing read-only dependencies must fail closed");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"exact_stable_identity_is_active_without_interpreting_confirmation",
       exact_stable_identity_is_active_without_interpreting_confirmation},
      {"different_stable_identity_is_not_active",
       different_stable_identity_is_not_active},
      {"storage_observation_proves_marker_and_physical_identity",
       storage_observation_proves_marker_and_physical_identity},
      {"runtime_marker_is_mandatory_and_strict",
       runtime_marker_is_mandatory_and_strict},
      {"zero_or_multiple_matching_media_roots_fail_closed",
       zero_or_multiple_matching_media_roots_fail_closed},
      {"ramdisk_marker_is_not_counted_as_a_physical_candidate",
       ramdisk_marker_is_not_counted_as_a_physical_candidate},
      {"verified_optical_media_is_not_a_physical_target",
       verified_optical_media_is_not_a_physical_target},
      {"malformed_candidate_or_read_uncertainty_fails_closed",
       malformed_candidate_or_read_uncertainty_fails_closed},
      {"physical_mapping_and_inventory_must_be_unambiguous",
       physical_mapping_and_inventory_must_be_unambiguous},
      {"malformed_input_and_missing_dependencies_fail_before_io",
       malformed_input_and_missing_dependencies_fail_before_io},
  };
  std::size_t passed = 0U;
  for (const auto& [name, test] : tests) {
    try {
      test();
      ++passed;
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << passed << '/' << tests.size() << " tests passed\n";
  return passed == tests.size() ? 0 : 1;
}

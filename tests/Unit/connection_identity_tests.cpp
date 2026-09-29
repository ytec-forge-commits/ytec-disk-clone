#include "ytec/diskmodel/connection_identity.h"

#include <Windows.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using ytec::diskmodel::DiskIdentityBindingKind;
using ytec::diskmodel::DiskInfo;
using ytec::diskmodel::DiskSelectionIdentity;
using ytec::diskmodel::InventoryIssue;
using ytec::diskmodel::InventoryReport;
using ytec::diskmodel::PartitionInfo;
using ytec::diskmodel::PartitionStyle;

constexpr std::uint64_t kMiB = 1024U * 1024U;

struct TestFailure final {
  std::string message;
};

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw TestFailure{message};
  }
}

PartitionInfo partition(
    const std::uint32_t number,
    const std::uint64_t offset,
    const std::uint64_t size,
    std::wstring name) {
  return PartitionInfo{
      .number = number,
      .offset_bytes = offset,
      .size_bytes = size,
      .style = PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
      .identifier = L"{11111111-2222-3333-4444-55555555555" +
          std::to_wstring(number) + L"}",
      .name = std::move(name),
      .bootable = false,
  };
}

DiskInfo fixed_usb_without_serial() {
  DiskInfo disk;
  disk.disk_number = 7U;
  disk.device_path = L"\\\\.\\PhysicalDrive7";
  disk.device_interface_path =
      L"\\\\?\\USBSTOR#Disk&Ven_Y-TEC&Prod_Test#PORT-A";
  disk.connection_location_path = L"PCIROOT(0)#PCI(1400)#USBROOT(0)#USB(3)";
  disk.device_instance_id = L"USBSTOR\\DISK&VEN_Y-TEC&PROD_TEST\\PORT-A";
  disk.model = L"Y-TEC Synthetic USB SSD";
  disk.size_bytes = 64U * kMiB;
  disk.sector_count = disk.size_bytes / 512U;
  disk.logical_sector_size = 512U;
  disk.physical_sector_size = 4096U;
  disk.bus_type = L"USB";
  disk.partition_style = PartitionStyle::gpt;
  disk.disk_identifier = L"{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}";
  disk.removable = false;
  // Deliberately reverse the inventory order. The captured identity must
  // canonicalize layout order before comparing it.
  disk.partitions.push_back(partition(2U, 32U * kMiB, 16U * kMiB, L"Data"));
  disk.partitions.push_back(partition(1U, 1U * kMiB, 16U * kMiB, L"Windows"));
  return disk;
}

InventoryReport report_with(DiskInfo disk) {
  InventoryReport report;
  report.disks.push_back(std::move(disk));
  return report;
}

DiskSelectionIdentity connection_selection() {
  auto created = DiskSelectionIdentity::create(fixed_usb_without_serial());
  check(created.has_value(), "a complete fixed USB selection must be accepted");
  check(
      created.value().kind() == DiskIdentityBindingKind::same_connection,
      "a serial-less fixed USB disk must use same-connection binding");
  return created.take_value();
}

void test_fixed_usb_identity_is_canonical_and_ignores_disk_number() {
  const DiskInfo selected = fixed_usb_without_serial();
  const auto captured =
      ytec::diskmodel::make_connection_bound_disk_identity(selected);
  check(captured.has_value(), "the composite identity must be created");
  check(
      captured.value().partitions.front().number == 1U,
      "partition layout must be normalized by offset");

  DiskInfo observed = selected;
  observed.disk_number = 19U;
  observed.device_path = L"\\\\.\\PhysicalDrive19";
  observed.device_instance_id = L"USBSTOR\\CHANGED-BUT-NOT-TRUSTED";
  observed.bus_type = L"usb";
  observed.device_interface_path =
      L"\\\\?\\usbstor#disk&ven_y-tec&prod_test#port-a";
  observed.connection_location_path =
      L"pciroot(0)#pci(1400)#usbroot(0)#usb(3)";
  observed.model = L"y-tec synthetic usb ssd";
  std::reverse(observed.partitions.begin(), observed.partitions.end());

  auto selection = connection_selection();
  const auto reidentified = selection.reidentify(report_with(observed));
  check(reidentified.has_value(), "the unchanged port-bound disk must match");
  check(
      reidentified.value().disk_number == 19U,
      "PhysicalDrive number must not participate in same-port identity");
  check(!selection.reselection_required(), "a clean match must retain approval");

  auto reordered = captured.value();
  std::reverse(reordered.partitions.begin(), reordered.partitions.end());
  check(
      ytec::diskmodel::validate_connection_bound_disk_identity(
          captured.value(), reordered)
          .has_value(),
      "layout comparison must not depend on enumeration order");
}

void test_composite_mismatch_permanently_consumes_approval() {
  using Mutator = std::function<void(DiskInfo&)>;
  const std::vector<std::pair<std::string, Mutator>> mismatches{
      {"interface path", [](DiskInfo& disk) {
         disk.device_interface_path += L"-CHANGED";
       }},
      {"port", [](DiskInfo& disk) {
         disk.connection_location_path += L"#USB(9)";
       }},
      {"model", [](DiskInfo& disk) { disk.model += L" changed"; }},
      {"capacity", [](DiskInfo& disk) { disk.size_bytes += 512U; }},
      {"logical sector", [](DiskInfo& disk) {
         disk.logical_sector_size = 1024U;
       }},
      {"physical sector", [](DiskInfo& disk) {
         disk.physical_sector_size = 8192U;
       }},
      {"disk style", [](DiskInfo& disk) {
         disk.partition_style = PartitionStyle::mbr;
         for (auto& item : disk.partitions) {
           item.style = PartitionStyle::mbr;
         }
       }},
      {"disk identifier", [](DiskInfo& disk) {
         disk.disk_identifier = L"{BBBBBBBB-BBBB-CCCC-DDDD-EEEEEEEEEEEE}";
       }},
      {"partition number", [](DiskInfo& disk) {
         disk.partitions.front().number = 3U;
       }},
      {"partition offset", [](DiskInfo& disk) {
         disk.partitions.front().offset_bytes += 512U;
       }},
      {"partition size", [](DiskInfo& disk) {
         disk.partitions.front().size_bytes -= 512U;
       }},
      {"partition type", [](DiskInfo& disk) {
         disk.partitions.front().type += L"-changed";
       }},
      {"partition identifier", [](DiskInfo& disk) {
         disk.partitions.front().identifier += L"-changed";
       }},
      {"partition name", [](DiskInfo& disk) {
         disk.partitions.front().name += L" changed";
       }},
      {"partition boot flag", [](DiskInfo& disk) {
         disk.partitions.front().bootable = true;
       }},
      {"partition count", [](DiskInfo& disk) {
         disk.partitions.pop_back();
       }},
  };

  for (const auto& [name, mutate] : mismatches) {
    auto selection = connection_selection();
    DiskInfo changed = fixed_usb_without_serial();
    mutate(changed);
    const auto mismatch = selection.reidentify(report_with(std::move(changed)));
    check(!mismatch.has_value(), name + " mismatch must fail closed");
    check(
        selection.reselection_required(),
        name + " mismatch must permanently require reselection");
    const auto revived =
        selection.reidentify(report_with(fixed_usb_without_serial()));
    check(
        !revived.has_value(),
        name + " mismatch must not revive after the original disk returns");
    check(
        revived.error().code ==
            ytec::clonecore::ErrorCode::confirmation_required,
        name + " mismatch must require a new OK authorization");
  }
}

void test_disconnect_duplicate_issue_and_notification_are_irreversible() {
  {
    auto selection = connection_selection();
    const auto disconnected = selection.reidentify({});
    check(!disconnected.has_value(), "a missing disk must be treated as disconnect");
    check(selection.reselection_required(), "disconnect must latch reselection");
    check(
        !selection.reidentify(report_with(fixed_usb_without_serial())).has_value(),
        "a reconnect at the same port must not revive the old approval");
  }

  {
    auto selection = connection_selection();
    InventoryReport ambiguous;
    ambiguous.disks.push_back(fixed_usb_without_serial());
    DiskInfo duplicate = fixed_usb_without_serial();
    duplicate.disk_number = 8U;
    duplicate.device_path = L"\\\\.\\PhysicalDrive8";
    ambiguous.disks.push_back(std::move(duplicate));
    check(
        !selection.reidentify(ambiguous).has_value(),
        "multiple exact matches must be ambiguous");
    check(selection.reselection_required(), "ambiguity must latch reselection");
  }

  {
    auto selection = connection_selection();
    InventoryReport incomplete = report_with(fixed_usb_without_serial());
    incomplete.issues.push_back(InventoryIssue{
        .device = L"\\\\?\\unreadable-disk",
        .error = ytec::clonecore::Error{
            .code = ytec::clonecore::ErrorCode::query_failed,
            .native_code = ERROR_INVALID_DATA,
            .operation = L"synthetic enumeration",
            .message = L"incomplete",
        },
    });
    check(
        !selection.reidentify(incomplete).has_value(),
        "an incomplete inventory cannot prove a unique connection");
    check(
        selection.reselection_required(),
        "an incomplete inventory must consume same-connection approval");
  }

  {
    auto selection = connection_selection();
    selection.invalidate_connection();
    check(selection.reselection_required(), "device notification must latch");
    check(
        !selection.reidentify(report_with(fixed_usb_without_serial())).has_value(),
        "explicit disconnect notification must prevent approval revival");
  }
}

void test_removable_and_incomplete_connection_evidence_are_rejected() {
  DiskInfo removable = fixed_usb_without_serial();
  removable.removable = true;
  check(
      !DiskSelectionIdentity::create(removable).has_value(),
      "USB removable media must not be a clone target selection");

  removable.serial_suffix = "SER12345";
  check(
      !DiskSelectionIdentity::create(removable).has_value(),
      "a serial must not make USB removable media eligible");

  DiskInfo unknown_removable = fixed_usb_without_serial();
  unknown_removable.removable.reset();
  check(
      !DiskSelectionIdentity::create(unknown_removable).has_value(),
      "unknown USB removable state must fail closed");

  DiskInfo missing_physical_sector = fixed_usb_without_serial();
  missing_physical_sector.physical_sector_size = 0U;
  check(
      !DiskSelectionIdentity::create(missing_physical_sector).has_value(),
      "both logical and physical sector evidence are required");

  DiskInfo no_port = fixed_usb_without_serial();
  no_port.connection_location_path.clear();
  check(
      !DiskSelectionIdentity::create(no_port).has_value(),
      "the physical connection location is mandatory");

  DiskInfo raw_with_partition = fixed_usb_without_serial();
  raw_with_partition.partition_style = PartitionStyle::raw;
  raw_with_partition.disk_identifier = L"RAW";
  for (auto& item : raw_with_partition.partitions) {
    item.style = PartitionStyle::raw;
  }
  check(
      !DiskSelectionIdentity::create(raw_with_partition).has_value(),
      "RAW disks cannot claim a populated partition layout");

  DiskInfo partitioned_without_layout = fixed_usb_without_serial();
  partitioned_without_layout.partitions.clear();
  check(
      !DiskSelectionIdentity::create(partitioned_without_layout).has_value(),
      "a GPT disk must carry its partition layout");

  DiskInfo blank_raw = fixed_usb_without_serial();
  blank_raw.partition_style = PartitionStyle::raw;
  blank_raw.disk_identifier = L"RAW";
  blank_raw.partitions.clear();
  auto blank_selection = DiskSelectionIdentity::create(blank_raw);
  check(
      blank_selection.has_value() &&
          blank_selection.value().kind() ==
              DiskIdentityBindingKind::same_connection,
      "an internally consistent blank fixed USB target must remain eligible");

  DiskInfo unknown_partition = fixed_usb_without_serial();
  unknown_partition.partitions.front().style = PartitionStyle::unknown;
  check(
      !DiskSelectionIdentity::create(unknown_partition).has_value(),
      "unknown partition style cannot form a connection identity");

  DiskInfo unaligned_partition = fixed_usb_without_serial();
  unaligned_partition.partitions.front().offset_bytes += 1U;
  check(
      !DiskSelectionIdentity::create(unaligned_partition).has_value(),
      "unaligned layout evidence must fail closed");

  DiskInfo overlap = fixed_usb_without_serial();
  overlap.partitions.front().offset_bytes = 8U * kMiB;
  check(
      !DiskSelectionIdentity::create(overlap).has_value(),
      "overlapping partitions must not form an identity");

  DiskInfo duplicate_number = fixed_usb_without_serial();
  duplicate_number.partitions.push_back(
      partition(1U, 52U * kMiB, 4U * kMiB, L"duplicate number"));
  check(
      !DiskSelectionIdentity::create(duplicate_number).has_value(),
      "duplicate partition numbers must be rejected even when non-adjacent");

  auto complete = ytec::diskmodel::make_connection_bound_disk_identity(
      fixed_usb_without_serial());
  check(complete.has_value(), "complete synthetic identity must build");
  auto malformed = complete.value();
  malformed.physical_sector_size = 0U;
  check(
      !ytec::diskmodel::validate_connection_bound_disk_identity(
           complete.value(), malformed)
           .has_value(),
      "the public validator must reject malformed identity values");
}

void test_serial_and_non_usb_disks_keep_persistent_identity_behavior() {
  DiskInfo serial_usb = fixed_usb_without_serial();
  serial_usb.serial_suffix = "SER12345";
  serial_usb.device_instance_id = L"USBSTOR\\SER12345";
  auto created = DiskSelectionIdentity::create(serial_usb);
  check(created.has_value(), "a fixed USB disk with serial must be accepted");
  check(
      created.value().kind() ==
          DiskIdentityBindingKind::persistent_identifier,
      "a USB serial must retain the persistent identity path");
  auto selection = created.take_value();
  selection.invalidate_connection();
  check(
      !selection.reselection_required(),
      "connection invalidation must not alter persistent identity behavior");

  const auto temporarily_missing = selection.reidentify({});
  check(
      !temporarily_missing.has_value() && !selection.reselection_required(),
      "persistent identity failure must not use the connection-only latch");
  serial_usb.disk_number = 42U;
  serial_usb.device_path = L"\\\\.\\PhysicalDrive42";
  const auto found_again = selection.reidentify(report_with(serial_usb));
  check(
      found_again.has_value() && found_again.value().disk_number == 42U,
      "existing stable identity must still reidentify after disk renumbering");

  DiskInfo non_usb = fixed_usb_without_serial();
  non_usb.bus_type = L"SATA";
  non_usb.device_instance_id = L"SCSI\\DISK&VEN_Y-TEC\\STABLE";
  auto non_usb_selection = DiskSelectionIdentity::create(non_usb);
  check(
      non_usb_selection.has_value() &&
          non_usb_selection.value().kind() ==
              DiskIdentityBindingKind::persistent_identifier,
      "non-USB disks must continue to use the existing stable identity path");

  DiskInfo no_serial_but_instance = fixed_usb_without_serial();
  check(!no_serial_but_instance.device_instance_id.empty(), "fixture sanity");
  auto same_port = DiskSelectionIdentity::create(no_serial_but_instance);
  check(
      same_port.has_value() &&
          same_port.value().kind() == DiskIdentityBindingKind::same_connection,
      "USB device instance IDs must not bypass the no-serial connection rule");
}

}  // namespace

int main() {
  try {
    test_fixed_usb_identity_is_canonical_and_ignores_disk_number();
    test_composite_mismatch_permanently_consumes_approval();
    test_disconnect_duplicate_issue_and_notification_are_irreversible();
    test_removable_and_incomplete_connection_evidence_are_rejected();
    test_serial_and_non_usb_disks_keep_persistent_identity_behavior();
    std::cout << "connection identity tests: PASS\n";
    return 0;
  } catch (const TestFailure& failure) {
    std::cerr << "connection identity tests: FAIL: " << failure.message << '\n';
    return 1;
  } catch (const std::exception& exception) {
    std::cerr << "connection identity tests: FAIL: unexpected exception: "
              << exception.what() << '\n';
    return 1;
  }
}

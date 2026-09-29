#include "ytec/bootrepair/clone_boot_finalization.h"

#include <Windows.h>

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error test_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = operation,
      .message = L"失敗注入",
  };
}

ytec::bootrepair::EfiBootOwnershipEvidence safe_efi_ownership() {
  return ytec::bootrepair::EfiBootOwnershipEvidence{
      .state = ytec::bootrepair::EfiBootOwnershipState::
          microsoft_only_or_empty,
      .efi_directory_present = true,
      .microsoft_namespace_present = true,
      .boot_namespace_present = true,
      .fallback_loader_present = true,
      .fallback_loader_microsoft_signed = true,
      .microsoft_signed_efi_loader_count = 2U,
  };
}

ytec::bootrepair::EfiBootOwnershipEvidence untrusted_efi_ownership() {
  return ytec::bootrepair::EfiBootOwnershipEvidence{
      .state = ytec::bootrepair::EfiBootOwnershipState::
          non_microsoft_or_untrusted_present,
      .efi_directory_present = true,
      .non_microsoft_or_untrusted_entry_count = 1U,
      .top_level_non_microsoft_namespace_count = 1U,
  };
}

ytec::bootrepair::EfiBootOwnershipEvidence ambiguous_efi_ownership() {
  return ytec::bootrepair::EfiBootOwnershipEvidence{
      .state = ytec::bootrepair::EfiBootOwnershipState::ambiguous,
      .efi_directory_present = true,
  };
}

ytec::diskmodel::DiskInfo make_target() {
  using ytec::diskmodel::PartitionInfo;
  using ytec::diskmodel::PartitionStyle;
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 3U,
      .device_path = L"\\\\.\\PhysicalDrive3",
      .device_instance_id = L"MOCK\\TARGET\\3",
      .model = L"Tsumugi Target",
      .size_bytes = 128ULL * 1024ULL * 1024ULL,
      .sector_count = 262144ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "A1B2C3D4",
      .partition_style = PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  disk.partitions = {
      PartitionInfo{
          .number = 1U,
          .offset_bytes = 1ULL * 1024ULL * 1024ULL,
          .size_bytes = 16ULL * 1024ULL * 1024ULL,
          .style = PartitionStyle::gpt,
          .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
          .identifier = L"{00000000-0000-0000-0000-000000000001}",
          .name = L"EFI",
      },
      PartitionInfo{
          .number = 2U,
          .offset_bytes = 17ULL * 1024ULL * 1024ULL,
          .size_bytes = 96ULL * 1024ULL * 1024ULL,
          .style = PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
          .identifier = L"{00000000-0000-0000-0000-000000000002}",
          .name = L"Windows",
      },
  };
  return disk;
}

ytec::diskmodel::DiskInfo make_bios_target() {
  using ytec::diskmodel::PartitionInfo;
  using ytec::diskmodel::PartitionStyle;
  auto disk = make_target();
  disk.partition_style = PartitionStyle::mbr;
  disk.partitions = {
      PartitionInfo{
          .number = 1U,
          .offset_bytes = 1ULL * 1024ULL * 1024ULL,
          .size_bytes = 120ULL * 1024ULL * 1024ULL,
          .style = PartitionStyle::mbr,
          .type = L"0x07",
          .identifier = L"0x00000001",
          .name = L"Windows",
          .bootable = true,
      },
  };
  return disk;
}

ytec::clonecore::StableDiskIdentity identity_for(
    const ytec::diskmodel::DiskInfo& disk) {
  const auto identity = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(identity.has_value(), "Fixture identity should build");
  return identity.value();
}

ytec::bootrepair::BootVolumeObservation volume_for(
    const ytec::diskmodel::DiskInfo& disk,
    const ytec::diskmodel::PartitionInfo& partition,
    std::wstring volume_name,
    std::wstring file_system) {
  return ytec::bootrepair::BootVolumeObservation{
      .volume_name = std::move(volume_name),
      .location = ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = disk.disk_number,
          .starting_offset = partition.offset_bytes,
          .extent_length = partition.size_bytes,
          .file_system = std::move(file_system),
      },
  };
}

class Inventory final : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++calls;
    const std::size_t index = reports.size() == 1U
        ? 0U
        : (std::min)(calls - 1U, reports.size() - 1U);
    return ytec::clonecore::Result<
        ytec::diskmodel::InventoryReport>::success(reports[index]);
  }

  std::vector<ytec::diskmodel::InventoryReport> reports;
  std::size_t calls{};
};

class Volumes final
    : public ytec::bootrepair::ICloneBootFinalizationVolumeProvider {
 public:
  ytec::clonecore::Result<std::vector<
      ytec::bootrepair::BootVolumeObservation>>
  observe_volumes_read_only() override {
    ++observe_calls;
    if (!observation_batches.empty()) {
      const std::size_t index = (std::min)(
          observe_calls - 1U, observation_batches.size() - 1U);
      return ytec::clonecore::Result<std::vector<
          ytec::bootrepair::BootVolumeObservation>>::success(
          observation_batches[index]);
    }
    return ytec::clonecore::Result<std::vector<
        ytec::bootrepair::BootVolumeObservation>>::success(observations);
  }

  ytec::clonecore::Status wait_before_volume_retry() override {
    ++wait_calls;
    if (fail_wait) {
      return ytec::clonecore::Status::failure(
          test_error(L"モックボリューム到着待機"));
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<std::wstring>
  unavailable_drive_letters() override {
    return ytec::clonecore::Result<std::wstring>::success(L"C");
  }

  ytec::clonecore::Result<bool> contains_supported_offline_windows(
      const std::wstring& volume_root) override {
    const auto found = supported.find(volume_root);
    return ytec::clonecore::Result<bool>::success(
        found != supported.end() && found->second);
  }

  std::vector<ytec::bootrepair::BootVolumeObservation> observations;
  std::vector<std::vector<ytec::bootrepair::BootVolumeObservation>>
      observation_batches;
  std::map<std::wstring, bool> supported;
  std::size_t observe_calls{};
  std::size_t wait_calls{};
  bool fail_wait{};
};

class EfiOwnership final
    : public ytec::bootrepair::IEfiBootOwnershipInspector {
 public:
  ytec::clonecore::Result<ytec::bootrepair::EfiBootOwnershipEvidence>
  inspect_existing_esp_read_only(const std::wstring& volume_root) override {
    ++calls;
    observed_roots.push_back(volume_root);
    if (fail_with_wrong_identity) {
      return ytec::clonecore::Result<
          ytec::bootrepair::EfiBootOwnershipEvidence>::failure({
          .code = ytec::clonecore::ErrorCode::identity_mismatch,
          .native_code = ERROR_DEVICE_NOT_CONNECTED,
          .operation = L"モックESP Volume GUID再識別",
          .message = L"期待したESP Volume GUIDではありません",
      });
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::EfiBootOwnershipEvidence>::success(observed);
  }

  ytec::bootrepair::EfiBootOwnershipEvidence observed{
      safe_efi_ownership()};
  std::vector<std::wstring> observed_roots;
  std::size_t calls{};
  bool fail_with_wrong_identity{};
};

class Mounts final : public ytec::bootrepair::ISystemVolumeMountApi {
 public:
  ytec::clonecore::Status attach(
      const std::wstring& root,
      const std::wstring& volume_name) override {
    attached.emplace(root, volume_name);
    attach_order.push_back(root);
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<ytec::bootrepair::BootVolumeObservation> inspect(
      const std::wstring& root,
      const std::wstring& volume_name,
      const ytec::bootrepair::BootRepairVolumeLocation& location) override {
    if (!attached.contains(root) || attached[root] != volume_name) {
      return ytec::clonecore::Result<
          ytec::bootrepair::BootVolumeObservation>::failure(
          test_error(L"モックマウント検査"));
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::BootVolumeObservation>::success({
        .volume_name = volume_name,
        .location = location,
        .mount_points = {root},
    });
  }

  ytec::clonecore::Status detach(
      const std::wstring& root,
      const std::wstring& volume_name) override {
    if (!attached.contains(root) || attached[root] != volume_name) {
      return ytec::clonecore::Status::failure(
          test_error(L"モックマウント解除"));
    }
    attached.erase(root);
    detach_order.push_back(root);
    return ytec::clonecore::success_status();
  }

  std::map<std::wstring, std::wstring> attached;
  std::vector<std::wstring> attach_order;
  std::vector<std::wstring> detach_order;
};

class BootRepair final
    : public ytec::bootrepair::IStandaloneBootRepairService {
 public:
  explicit BootRepair(ytec::bootrepair::BootRepairTargetSelection selection)
      : selection_(std::move(selection)) {}

  ytec::clonecore::Result<ytec::bootrepair::BootRepairTargetSelection>
  inspect(const ytec::bootrepair::BootRepairTargetRequest& request) override {
    ++inspect_calls;
    last_request = request;
    const auto observed = inspect_ownership.has_value()
        ? inspect_ownership.value()
        : request.expected_efi_ownership;
    const auto ownership =
        ytec::bootrepair::validate_boot_repair_efi_ownership(
            request, observed);
    if (!ownership) {
      return ytec::clonecore::Result<
          ytec::bootrepair::BootRepairTargetSelection>::failure(
          ownership.error());
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::BootRepairTargetSelection>::success(selection_);
  }

  ytec::clonecore::Result<ytec::bootrepair::StandaloneBootRepairReport>
  execute(
      const ytec::bootrepair::StandaloneBootRepairExecutionRequest& request)
      override {
    ++execute_calls;
    if (fail_execute) {
      return ytec::clonecore::Result<
          ytec::bootrepair::StandaloneBootRepairReport>::failure(
          test_error(L"モックBCDBoot"));
    }
    check(request.target.store_policy ==
              ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh,
          "Finalizer must require a fresh BCD transaction");
    check(request.confirmation.first_step_acknowledged &&
              !request.confirmation.typed_token.empty(),
          "Finalizer must bind the internal repair confirmation");
    const auto observed = execute_ownership.has_value()
        ? execute_ownership.value()
        : request.target.expected_efi_ownership;
    const auto ownership =
        ytec::bootrepair::validate_boot_repair_efi_ownership(
            request.target, observed);
    if (!ownership) {
      return ytec::clonecore::Result<
          ytec::bootrepair::StandaloneBootRepairReport>::failure(
          ownership.error());
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::StandaloneBootRepairReport>::success({
        .repaired = selection_,
        .bcdboot = ytec::bootrepair::BcdBootReport{
            .exit_code = 0U,
            .microsoft_signature_verified = report_signature_verified,
            .prior_store_replaced = true,
            .fresh_store_verified = report_fresh_store_verified,
        },
        .boot_store_verified = report_boot_store_verified,
        .system_partition_temporarily_mounted = false,
        .temporary_mount_released = false,
        .efi_ownership_revalidated = report_ownership_revalidated,
        .nvram_unchanged = report_nvram_unchanged,
    });
  }

  ytec::bootrepair::BootRepairTargetSelection selection_;
  ytec::bootrepair::BootRepairTargetRequest last_request;
  std::size_t inspect_calls{};
  std::size_t execute_calls{};
  bool fail_execute{};
  std::optional<ytec::bootrepair::EfiBootOwnershipEvidence>
      inspect_ownership;
  std::optional<ytec::bootrepair::EfiBootOwnershipEvidence>
      execute_ownership;
  bool report_ownership_revalidated{true};
  bool report_nvram_unchanged{true};
  bool report_signature_verified{true};
  bool report_fresh_store_verified{true};
  bool report_boot_store_verified{true};
};

struct Fixture final {
  Fixture() : target(make_target()), expected(identity_for(target)) {
    inventory.reports.push_back({.disks = {target}});
    volumes.observations = {
        volume_for(
            target,
            target.partitions[0],
            L"\\\\?\\Volume{00000000-0000-0000-0000-000000000001}\\",
            L"FAT32"),
        volume_for(
            target,
            target.partitions[1],
            L"\\\\?\\Volume{00000000-0000-0000-0000-000000000002}\\",
            L"NTFS"),
    };
    volumes.supported[volumes.observations[1].volume_name] = true;
  }

  ytec::bootrepair::BootRepairTargetSelection selection() const {
    return {
        .disk = target,
        .identity = expected,
        .windows_partition = target.partitions[1],
        .system_partition = target.partitions[0],
    };
  }

  ytec::bootrepair::CloneBootFinalizationRequest request() const {
    return {
        .expected_target = expected,
        .expected_style = ytec::diskmodel::PartitionStyle::gpt,
        .expected_windows_partition_offset = target.partitions[1].offset_bytes,
    };
  }

  ytec::diskmodel::DiskInfo target;
  ytec::clonecore::StableDiskIdentity expected;
  Inventory inventory;
  Volumes volumes;
  EfiOwnership efi_ownership;
  Mounts mounts;
};

void test_success_reidentifies_and_releases() {
  Fixture fixture;
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(result.has_value(), "GPT finalization should succeed");
  check(result.value().temporary_mounts_released &&
            result.value().final_target_reidentified &&
            result.value().partition_layout_unchanged,
        "Success must prove cleanup and final identity/layout");
  check(fixture.mounts.attach_order.size() == 2U &&
            fixture.mounts.detach_order.size() == 2U &&
            fixture.mounts.attached.empty(),
        "Both exact volumes must be temporarily mounted and released");
  check(boot.inspect_calls == 1U && boot.execute_calls == 1U,
        "Fresh repair must be inspected and executed once");
  check(fixture.efi_ownership.calls == 1U &&
            fixture.efi_ownership.observed_roots.size() == 1U &&
            fixture.efi_ownership.observed_roots.front() ==
                fixture.volumes.observations.front().volume_name,
        "UEFI preflight must inspect the exact ESP Volume GUID once");
  check(boot.last_request.firmware ==
                ytec::bootrepair::BcdBootFirmware::uefi &&
            boot.last_request.store_policy ==
                ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh &&
            !boot.last_request.auto_mount_system_partition &&
            boot.last_request.system_volume_identity_root ==
                fixture.volumes.observations.front().volume_name &&
            boot.last_request.require_efi_ownership_recheck &&
            ytec::bootrepair::equivalent_efi_boot_ownership(
                boot.last_request.expected_efi_ownership,
                fixture.efi_ownership.observed) &&
            boot.last_request.third_party_efi_policy ==
                ytec::bootrepair::BootRepairThirdPartyEfiPolicy::
                    not_applicable &&
            !boot.last_request.reviewed_multi_windows_batch &&
            !boot.last_request.update_current_pc_nvram,
        "UEFI repair request must bind ownership, fresh BCD, and no NVRAM");
  check(result.value().boot_repair.bcdboot.microsoft_signature_verified &&
            result.value().boot_repair.bcdboot.fresh_store_verified &&
            result.value().boot_repair.boot_store_verified &&
            result.value().boot_repair.efi_ownership_revalidated &&
            result.value().boot_repair.nvram_unchanged &&
            !result.value().boot_repair.system_partition_temporarily_mounted &&
            !result.value().boot_repair.temporary_mount_released,
        "Inner repair must prove trust and leave outer-owned mounts alone");
}

void test_ambiguous_efi_is_rejected_before_mounting() {
  Fixture fixture;
  fixture.efi_ownership.observed = ambiguous_efi_ownership();
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::unsupported_layout &&
            fixture.efi_ownership.calls == 1U &&
            fixture.mounts.attach_order.empty() && boot.inspect_calls == 0U,
        "Ambiguous EFI content must fail before mounting or BootRepair");
}

void test_untrusted_efi_is_rejected_before_mounting() {
  Fixture fixture;
  fixture.efi_ownership.observed = untrusted_efi_ownership();
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::unsupported_layout &&
            fixture.efi_ownership.calls == 1U &&
            fixture.mounts.attach_order.empty() && boot.inspect_calls == 0U,
        "Third-party or untrusted EFI content must fail before mutation");
}

void test_wrong_esp_identity_is_rejected_before_mounting() {
  Fixture fixture;
  fixture.efi_ownership.fail_with_wrong_identity = true;
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch &&
            fixture.efi_ownership.calls == 1U &&
            fixture.mounts.attach_order.empty() && boot.inspect_calls == 0U,
        "A wrong ESP Volume GUID identity must fail before mutation");
}

void test_efi_ownership_drift_is_rejected_and_mounts_are_released() {
  Fixture fixture;
  BootRepair boot(fixture.selection());
  auto drifted = safe_efi_ownership();
  ++drifted.microsoft_signed_efi_loader_count;
  boot.execute_ownership = drifted;
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch &&
            boot.inspect_calls == 1U && boot.execute_calls == 1U,
        "EFI ownership drift immediately before mutation must fail closed");
  check(fixture.mounts.detach_order.size() == 2U &&
            fixture.mounts.attached.empty(),
        "Ownership drift must release every outer-owned temporary mount");
}

void test_bios_carries_not_applicable_efi_policy() {
  const auto target = make_bios_target();
  const auto expected = identity_for(target);
  Inventory inventory;
  inventory.reports.push_back({.disks = {target}});
  Volumes volumes;
  volumes.observations = {
      volume_for(
          target,
          target.partitions.front(),
          L"\\\\?\\Volume{00000000-0000-0000-0000-000000000010}\\",
          L"NTFS"),
  };
  volumes.supported[volumes.observations.front().volume_name] = true;
  EfiOwnership efi_ownership;
  Mounts mounts;
  BootRepair boot({
      .disk = target,
      .identity = expected,
      .windows_partition = target.partitions.front(),
      .system_partition = target.partitions.front(),
  });
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      ytec::bootrepair::CloneBootFinalizationRequest{
          .expected_target = expected,
          .expected_style = ytec::diskmodel::PartitionStyle::mbr,
          .expected_windows_partition_offset =
              target.partitions.front().offset_bytes,
      },
      inventory,
      volumes,
      efi_ownership,
      mounts,
      boot);
  check(result.has_value(), "BIOS finalization should succeed");
  check(efi_ownership.calls == 0U &&
            boot.last_request.firmware ==
                ytec::bootrepair::BcdBootFirmware::bios &&
            boot.last_request.system_volume_identity_root.empty() &&
            !boot.last_request.require_efi_ownership_recheck &&
            boot.last_request.expected_efi_ownership.state ==
                ytec::bootrepair::EfiBootOwnershipState::not_applicable &&
            boot.last_request.third_party_efi_policy ==
                ytec::bootrepair::BootRepairThirdPartyEfiPolicy::
                    not_applicable &&
            !boot.last_request.update_current_pc_nvram,
        "BIOS repair must carry only not-applicable EFI policy");
  check(result.value().temporary_mounts_released &&
            mounts.attach_order.size() == 1U &&
            mounts.detach_order.size() == 1U && mounts.attached.empty(),
        "BIOS finalization must release its single outer-owned mount");
}

void test_unverified_boot_report_is_rejected() {
  for (std::uint32_t scenario = 0U; scenario < 4U; ++scenario) {
    Fixture fixture;
    BootRepair boot(fixture.selection());
    switch (scenario) {
      case 0U:
        boot.report_signature_verified = false;
        break;
      case 1U:
        boot.report_fresh_store_verified = false;
        break;
      case 2U:
        boot.report_ownership_revalidated = false;
        break;
      case 3U:
        boot.report_nvram_unchanged = false;
        break;
      default:
        throw std::runtime_error("Unexpected evidence scenario");
    }
    const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
        fixture.request(), fixture.inventory, fixture.volumes,
        fixture.efi_ownership, fixture.mounts, boot);
    check(!result.has_value() &&
              result.error().code ==
                  ytec::clonecore::ErrorCode::verification_failed &&
              fixture.mounts.detach_order.size() == 2U &&
              fixture.mounts.attached.empty(),
          "Missing boot trust evidence must fail and release outer mounts");
  }
}

void test_repair_failure_still_releases_every_mount() {
  Fixture fixture;
  BootRepair boot(fixture.selection());
  boot.fail_execute = true;
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value(), "Injected repair failure must propagate");
  check(fixture.mounts.detach_order.size() == 2U &&
            fixture.mounts.attached.empty(),
        "Repair failure must release both temporary roots");
}

void test_final_layout_change_is_rejected_after_cleanup() {
  Fixture fixture;
  auto changed = fixture.target;
  changed.partitions[1].name = L"Changed";
  fixture.inventory.reports.push_back({.disks = {changed}});
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch,
        "A final layout change must fail closed");
  check(fixture.mounts.detach_order.size() == 2U &&
            fixture.mounts.attached.empty(),
        "Final reidentification failure occurs only after mount cleanup");
}

void test_ambiguous_windows_is_rejected_without_mounting() {
  Fixture fixture;
  auto extra = fixture.target.partitions[1];
  extra.number = 3U;
  extra.offset_bytes = 114ULL * 1024ULL * 1024ULL;
  extra.size_bytes = 8ULL * 1024ULL * 1024ULL;
  extra.identifier = L"{00000000-0000-0000-0000-000000000003}";
  fixture.target.partitions.push_back(extra);
  fixture.inventory.reports[0].disks[0] = fixture.target;
  fixture.volumes.observations.push_back(volume_for(
      fixture.target,
      fixture.target.partitions[2],
      L"\\\\?\\Volume{00000000-0000-0000-0000-000000000003}\\",
      L"NTFS"));
  fixture.volumes.supported[
      fixture.volumes.observations.back().volume_name] = true;
  auto request = fixture.request();
  request.expected_windows_partition_offset.reset();
  BootRepair boot({
      .disk = fixture.target,
      .identity = fixture.expected,
      .windows_partition = fixture.target.partitions[1],
      .system_partition = fixture.target.partitions[0],
  });
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      request, fixture.inventory, fixture.volumes, fixture.efi_ownership,
      fixture.mounts, boot);
  check(!result.has_value() && fixture.mounts.attach_order.empty() &&
            fixture.volumes.wait_calls == 0U,
        "Multiple supported Windows installations require an explicit choice");
}

void test_delayed_volume_arrival_reidentifies_before_success() {
  Fixture fixture;
  fixture.volumes.observation_batches = {
      {},
      fixture.volumes.observations,
  };
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(result.has_value(), "A delayed newly-online volume should settle");
  check(fixture.volumes.observe_calls == 2U &&
            fixture.volumes.wait_calls == 1U && fixture.inventory.calls == 3U,
        "Every retry must wait and reidentify before observing again");
}

void test_volume_arrival_timeout_is_bounded_without_mounting() {
  Fixture fixture;
  fixture.volumes.observations.clear();
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value() && result.error().native_code == ERROR_TIMEOUT,
        "A missing volume must end with an explicit bounded timeout");
  check(fixture.volumes.wait_calls > 0U &&
            fixture.volumes.wait_calls < 256U &&
            fixture.volumes.observe_calls == fixture.volumes.wait_calls + 1U &&
            fixture.mounts.attach_order.empty() && boot.inspect_calls == 0U,
        "Timeout retries must stay bounded and mutation-free");
}

void test_layout_change_during_volume_wait_is_rejected() {
  Fixture fixture;
  fixture.volumes.observation_batches = {
      {},
      fixture.volumes.observations,
  };
  auto changed = fixture.target;
  changed.partitions[1].size_bytes -= 512U;
  fixture.inventory.reports.push_back({.disks = {changed}});
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value() &&
            result.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch &&
            fixture.volumes.wait_calls == 1U &&
            fixture.mounts.attach_order.empty(),
        "A layout change while settling must fail before mounting");
}

void test_volume_wait_failure_propagates_without_reenumeration() {
  Fixture fixture;
  fixture.volumes.observations.clear();
  fixture.volumes.fail_wait = true;
  BootRepair boot(fixture.selection());
  const auto result = ytec::bootrepair::finalize_cloned_windows_boot(
      fixture.request(), fixture.inventory, fixture.volumes,
      fixture.efi_ownership, fixture.mounts, boot);
  check(!result.has_value() &&
            result.error().operation == L"モックボリューム到着待機" &&
            fixture.inventory.calls == 1U &&
            fixture.mounts.attach_order.empty(),
        "A wait failure must propagate before a second inventory query");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests{
      {"success_reidentifies_and_releases",
       test_success_reidentifies_and_releases},
      {"ambiguous_efi_is_rejected_before_mounting",
       test_ambiguous_efi_is_rejected_before_mounting},
      {"untrusted_efi_is_rejected_before_mounting",
       test_untrusted_efi_is_rejected_before_mounting},
      {"wrong_esp_identity_is_rejected_before_mounting",
       test_wrong_esp_identity_is_rejected_before_mounting},
      {"efi_ownership_drift_is_rejected_and_mounts_are_released",
       test_efi_ownership_drift_is_rejected_and_mounts_are_released},
      {"bios_carries_not_applicable_efi_policy",
       test_bios_carries_not_applicable_efi_policy},
      {"unverified_boot_report_is_rejected",
       test_unverified_boot_report_is_rejected},
      {"repair_failure_still_releases_every_mount",
       test_repair_failure_still_releases_every_mount},
      {"final_layout_change_is_rejected_after_cleanup",
       test_final_layout_change_is_rejected_after_cleanup},
      {"ambiguous_windows_is_rejected_without_mounting",
       test_ambiguous_windows_is_rejected_without_mounting},
      {"delayed_volume_arrival_reidentifies_before_success",
       test_delayed_volume_arrival_reidentifies_before_success},
      {"volume_arrival_timeout_is_bounded_without_mounting",
       test_volume_arrival_timeout_is_bounded_without_mounting},
      {"layout_change_during_volume_wait_is_rejected",
       test_layout_change_during_volume_wait_is_rejected},
      {"volume_wait_failure_propagates_without_reenumeration",
       test_volume_wait_failure_propagates_without_reenumeration},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& exception) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << exception.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}

#include "ytec/bootrepair/automatic_repair_windows.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

void check(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error test_error(
    const ytec::clonecore::ErrorCode code,
    std::wstring operation) {
  return ytec::clonecore::Error{
      .code = code,
      .native_code = ERROR_GEN_FAILURE,
      .operation = std::move(operation),
      .message = L"合成エラー",
  };
}

std::wstring volume_root(const std::uint32_t suffix) {
  std::wstring tail = std::to_wstring(suffix);
  while (tail.size() < 12U) {
    tail.insert(tail.begin(), L'0');
  }
  return L"\\\\?\\Volume{22222222-2222-2222-2222-" + tail + L"}\\";
}

ytec::diskmodel::DiskInfo selected_disk() {
  using ytec::diskmodel::PartitionInfo;
  using ytec::diskmodel::PartitionStyle;
  ytec::diskmodel::DiskInfo disk{
      .disk_number = 7U,
      .device_path = L"\\\\.\\PhysicalDrive7",
      .device_instance_id = L"MOCK\\BOOT\\WINDOWS-ADAPTER",
      .model = L"Tsumugi Adapter Disk",
      .size_bytes = 4ULL * 1024ULL * kMiB,
      .sector_count = 8ULL * 1024ULL * 1024ULL,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"NVMe",
      .serial_suffix = "ADAPT001",
      .partition_style = PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
  disk.partitions = {
      PartitionInfo{
          .number = 1U,
          .offset_bytes = 1ULL * kMiB,
          .size_bytes = 64ULL * kMiB,
          .style = PartitionStyle::gpt,
          .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
          .identifier = L"{ESP}",
          .name = L"EFI",
      },
      PartitionInfo{
          .number = 2U,
          .offset_bytes = 65ULL * kMiB,
          .size_bytes = 1024ULL * kMiB,
          .style = PartitionStyle::gpt,
          .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
          .identifier = L"{WINDOWS}",
          .name = L"Windows",
      },
  };
  return disk;
}

ytec::bootrepair::BootVolumeObservation observation_for(
    const ytec::diskmodel::DiskInfo& disk,
    const std::size_t partition_index,
    std::wstring root,
    std::wstring file_system) {
  const auto& partition = disk.partitions.at(partition_index);
  return {
      .volume_name = std::move(root),
      .location = ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = disk.disk_number,
          .starting_offset = partition.offset_bytes,
          .extent_length = partition.size_bytes,
          .file_system = std::move(file_system),
      },
  };
}

class SyntheticEnumerator final
    : public ytec::bootrepair::IReadOnlyBootVolumeEnumerator {
 public:
  ytec::clonecore::Result<std::vector<
      ytec::bootrepair::BootVolumeObservation>>
  enumerate_read_only() override {
    ++calls;
    if (error.has_value()) {
      return ytec::clonecore::Result<std::vector<
          ytec::bootrepair::BootVolumeObservation>>::failure(*error);
    }
    return ytec::clonecore::Result<std::vector<
        ytec::bootrepair::BootVolumeObservation>>::success(observations);
  }

  std::vector<ytec::bootrepair::BootVolumeObservation> observations;
  std::optional<ytec::clonecore::Error> error;
  std::size_t calls{};
};

class SyntheticOfflineProbe final
    : public ytec::bootrepair::IOfflineWindowsReadOnlyProbe {
 public:
  ytec::clonecore::Result<
      ytec::bootrepair::OfflineWindowsRootObservation>
  inspect_root_read_only(const std::wstring& root) override {
    ++root_calls;
    last_root = root;
    if (root_error.has_value()) {
      return ytec::clonecore::Result<
          ytec::bootrepair::OfflineWindowsRootObservation>::failure(
          *root_error);
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::OfflineWindowsRootObservation>::success(
        root_observation);
  }

  ytec::clonecore::Result<ytec::bootrepair::OfflineWindowsVersion>
  read_version_read_only(const std::wstring& root) override {
    ++version_calls;
    last_root = root;
    if (version_error.has_value()) {
      return ytec::clonecore::Result<
          ytec::bootrepair::OfflineWindowsVersion>::failure(*version_error);
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::OfflineWindowsVersion>::success(version);
  }

  ytec::clonecore::Status verify_supported_read_only(
      const std::wstring& root) override {
    ++verify_calls;
    last_root = root;
    return verification_error.has_value()
        ? ytec::clonecore::Status::failure(*verification_error)
        : ytec::clonecore::success_status();
  }

  ytec::bootrepair::OfflineWindowsRootObservation root_observation{
      ytec::bootrepair::OfflineWindowsRootObservation::absent};
  ytec::bootrepair::OfflineWindowsVersion version{
      .major = 10U,
      .build = 26100U,
      .installation_type = L"Client",
  };
  std::optional<ytec::clonecore::Error> root_error;
  std::optional<ytec::clonecore::Error> version_error;
  std::optional<ytec::clonecore::Error> verification_error;
  std::wstring last_root;
  std::size_t root_calls{};
  std::size_t version_calls{};
  std::size_t verify_calls{};
};

class DummyInventory final : public ytec::diskmodel::IDiskInventoryProvider {
 public:
  ytec::clonecore::Result<ytec::diskmodel::InventoryReport> enumerate()
      override {
    ++calls;
    return ytec::clonecore::Result<
        ytec::diskmodel::InventoryReport>::success({});
  }
  std::size_t calls{};
};

class DummyVolumes final
    : public ytec::bootrepair::IBootRepairVolumeObservationProvider {
 public:
  ytec::clonecore::Result<std::vector<
      ytec::bootrepair::BootVolumeObservation>>
  observe_read_only(const ytec::diskmodel::DiskInfo&) override {
    ++calls;
    return ytec::clonecore::Result<std::vector<
        ytec::bootrepair::BootVolumeObservation>>::success({});
  }
  std::size_t calls{};
};

class DummyValidator final
    : public ytec::bootrepair::IOfflineWindowsCandidateValidator {
 public:
  ytec::clonecore::Result<
      ytec::bootrepair::OfflineWindowsCandidateValidation>
  inspect_volume_read_only(const std::wstring&) override {
    ++calls;
    return ytec::clonecore::Result<
        ytec::bootrepair::OfflineWindowsCandidateValidation>::success({});
  }
  std::size_t calls{};
};

class DummyWinRe final : public ytec::bootrepair::IWinReDiagnosticService {
 public:
  ytec::clonecore::Result<ytec::bootrepair::WinReDiagnosticReport> inspect(
      const std::wstring&,
      std::uint32_t) override {
    ++calls;
    return ytec::clonecore::Result<
        ytec::bootrepair::WinReDiagnosticReport>::failure(
        test_error(ytec::clonecore::ErrorCode::query_failed, L"合成WinRE"));
  }
  std::size_t calls{};
};

class DummyEfiOwnership final
    : public ytec::bootrepair::IEfiBootOwnershipInspector {
 public:
  ytec::clonecore::Result<ytec::bootrepair::EfiBootOwnershipEvidence>
  inspect_existing_esp_read_only(const std::wstring&) override {
    ++calls;
    return ytec::clonecore::Result<
        ytec::bootrepair::EfiBootOwnershipEvidence>::success({
        .state = ytec::bootrepair::EfiBootOwnershipState::
            microsoft_only_or_empty,
    });
  }
  std::size_t calls{};
};

void test_provider_discards_out_of_scope_volumes_before_validation() {
  const auto disk = selected_disk();
  auto enumerator = std::make_unique<SyntheticEnumerator>();
  auto* const observed_enumerator = enumerator.get();
  enumerator->observations.push_back({
      .volume_name = L"not-a-volume-root",
      .location = ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 99U,
          .starting_offset = 0U,
          .extent_length = 0U,
          .file_system = L"",
      },
  });
  enumerator->observations.push_back(observation_for(
      disk, 1U, volume_root(2U), L"NTFS"));
  auto provider =
      ytec::bootrepair::make_filtered_boot_repair_volume_observation_provider(
          std::move(enumerator));

  const auto result = provider->observe_read_only(disk);
  check(result.has_value(), "Foreign observations must be discarded first");
  check(
      result.value().size() == 1U &&
          result.value().front().location.disk_number == disk.disk_number,
      "Only exact selected-disk observations may escape the adapter");
  check(observed_enumerator->calls == 1U, "Enumerator should run once");
}

void test_selected_incomplete_observations_fail_closed() {
  const auto disk = selected_disk();
  auto zero_extent = observation_for(disk, 1U, volume_root(2U), L"NTFS");
  zero_extent.location.extent_length = 0U;
  auto result = ytec::bootrepair::filter_boot_repair_volumes_for_selected_disk(
      disk, {zero_extent});
  check(!result.has_value(), "Zero-length selected volume must fail");

  auto empty_file_system = observation_for(
      disk, 1U, volume_root(2U), L"");
  result = ytec::bootrepair::filter_boot_repair_volumes_for_selected_disk(
      disk, {empty_file_system});
  check(!result.has_value(), "Missing selected filesystem must fail");

  auto invalid_root = observation_for(disk, 1U, L"D:\\", L"NTFS");
  invalid_root.volume_name = L"relative";
  result = ytec::bootrepair::filter_boot_repair_volumes_for_selected_disk(
      disk, {invalid_root});
  check(!result.has_value(), "Malformed selected volume root must fail");
}

void test_selected_duplicate_name_and_extent_fail_closed() {
  const auto disk = selected_disk();
  auto first = observation_for(disk, 0U, volume_root(1U), L"FAT32");
  auto duplicate_name = observation_for(
      disk, 1U, volume_root(1U), L"NTFS");
  auto result = ytec::bootrepair::filter_boot_repair_volumes_for_selected_disk(
      disk, {first, duplicate_name});
  check(!result.has_value(), "Duplicate Volume GUID must fail");
  check(
      result.error().code == ytec::clonecore::ErrorCode::identity_mismatch,
      "Duplicate Volume GUID must be an identity error");

  auto duplicate_extent = first;
  duplicate_extent.volume_name = volume_root(22U);
  result = ytec::bootrepair::filter_boot_repair_volumes_for_selected_disk(
      disk, {first, duplicate_extent});
  check(!result.has_value(), "Duplicate partition extent must fail");
}

void test_selected_range_mismatch_fails_closed() {
  const auto disk = selected_disk();
  auto mismatch = observation_for(disk, 1U, volume_root(2U), L"NTFS");
  ++mismatch.location.starting_offset;
  const auto result =
      ytec::bootrepair::filter_boot_repair_volumes_for_selected_disk(
          disk, {mismatch});
  check(!result.has_value(), "Non-exact selected partition range must fail");
  check(
      result.error().code == ytec::clonecore::ErrorCode::identity_mismatch,
      "Range mismatch must be an identity error");
}

void test_windows_absent_stops_without_hive_or_architecture_probe() {
  SyntheticOfflineProbe probe;
  const auto result = ytec::bootrepair::classify_offline_windows_candidate(
      volume_root(2U), probe);
  check(result.has_value(), "Absent Windows directory is not an error");
  check(
      result.value().state ==
          ytec::bootrepair::OfflineWindowsCandidateState::absent,
      "Absent Windows directory must remain absent");
  check(
      probe.root_calls == 1U && probe.version_calls == 0U &&
          probe.verify_calls == 0U,
      "Absent candidate must not open a hive or kernel verifier");
}

void test_supported_windows_requires_matching_version_and_verifier() {
  SyntheticOfflineProbe probe;
  probe.root_observation =
      ytec::bootrepair::OfflineWindowsRootObservation::candidate;
  const auto result = ytec::bootrepair::classify_offline_windows_candidate(
      volume_root(2U), probe);
  check(result.has_value(), "Supported Windows should classify");
  check(
      result.value().state == ytec::bootrepair::OfflineWindowsCandidateState::
          present_supported,
      "Supported Windows must be explicit");
  check(
      probe.root_calls == 1U && probe.version_calls == 1U &&
          probe.verify_calls == 1U,
      "Supported candidate should use every read-only validation stage once");
}

void test_unsupported_windows_is_distinct_from_probe_failure() {
  SyntheticOfflineProbe probe;
  probe.root_observation =
      ytec::bootrepair::OfflineWindowsRootObservation::candidate;
  probe.version = {
      .major = 6U,
      .build = 7601U,
      .installation_type = L"Client",
  };
  probe.verification_error = ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::unsupported_layout,
      .native_code = ERROR_NOT_SUPPORTED,
      .operation = L"合成Windows対応判定",
      .message = L"未保証Windows",
  };
  const auto result = ytec::bootrepair::classify_offline_windows_candidate(
      volume_root(2U), probe);
  check(result.has_value(), "Known unsupported Windows should classify");
  check(
      result.value().state == ytec::bootrepair::OfflineWindowsCandidateState::
          present_unsupported,
      "Known unsupported Windows must not be reported as absent");
}

void test_windows_probe_error_is_propagated_not_downgraded_to_absent() {
  SyntheticOfflineProbe probe;
  probe.root_error = test_error(
      ytec::clonecore::ErrorCode::access_denied, L"合成Windowsルート検査");
  const auto result = ytec::bootrepair::classify_offline_windows_candidate(
      volume_root(2U), probe);
  check(!result.has_value(), "Probe failure must fail closed");
  check(
      result.error().code == ytec::clonecore::ErrorCode::access_denied,
      "Probe error classification must be preserved");
  check(
      probe.version_calls == 0U && probe.verify_calls == 0U,
      "A failed root probe must stop later reads");
}

void test_inconsistent_windows_support_evidence_fails_closed() {
  SyntheticOfflineProbe probe;
  probe.root_observation =
      ytec::bootrepair::OfflineWindowsRootObservation::candidate;
  probe.version = {
      .major = 6U,
      .build = 7601U,
      .installation_type = L"Client",
  };
  const auto result = ytec::bootrepair::classify_offline_windows_candidate(
      volume_root(2U), probe);
  check(!result.has_value(), "Conflicting version and verifier must fail");
  check(
      result.error().code == ytec::clonecore::ErrorCode::invalid_data,
      "Conflicting support evidence must be invalid data");
}

void test_efi_ownership_classification_is_fail_closed() {
  auto result = ytec::bootrepair::classify_efi_boot_ownership({});
  check(
      result.has_value() &&
          ytec::bootrepair::efi_boot_ownership_allows_microsoft_rebuild(
              result.value()),
      "An empty ESP should be safe for a Microsoft-only rebuild");

  result = ytec::bootrepair::classify_efi_boot_ownership({
      .efi_directory_present = true,
      .microsoft_namespace_present = true,
      .boot_namespace_present = true,
      .fallback_loader_present = true,
      .fallback_loader_microsoft_signed = true,
      .microsoft_signed_efi_loader_count = 6U,
  });
  check(
      result.has_value() &&
          result.value().state ==
              ytec::bootrepair::EfiBootOwnershipState::
                  microsoft_only_or_empty &&
          ytec::bootrepair::efi_boot_ownership_allows_microsoft_rebuild(
              result.value()),
      "Microsoft-signed EFI content should remain executable");

  result = ytec::bootrepair::classify_efi_boot_ownership({
      .efi_directory_present = true,
      .top_level_non_microsoft_namespace_count = 1U,
  });
  check(
      result.has_value() &&
          result.value().state ==
              ytec::bootrepair::EfiBootOwnershipState::
                  non_microsoft_or_untrusted_present &&
          !ytec::bootrepair::efi_boot_ownership_allows_microsoft_rebuild(
              result.value()),
      "A third-party EFI namespace must block automatic repair");
  check(
      ytec::bootrepair::efi_boot_ownership_allows_third_party_preserve(
          result.value()),
      "A verified independent top-level namespace should be preservable");

  result = ytec::bootrepair::classify_efi_boot_ownership({
      .efi_directory_present = true,
      .boot_namespace_present = true,
      .boot_namespace_nonstandard_entries = 1U,
  });
  check(
      result.has_value() &&
          !ytec::bootrepair::efi_boot_ownership_allows_third_party_preserve(
              result.value()),
      "A nonstandard Boot namespace entry is not an independent preserve target");

  result = ytec::bootrepair::classify_efi_boot_ownership({
      .efi_directory_present = true,
      .microsoft_namespace_present = true,
      .non_microsoft_or_untrusted_efi_loader_count = 1U,
  });
  check(
      result.has_value() &&
          !ytec::bootrepair::efi_boot_ownership_allows_third_party_preserve(
              result.value()),
      "An untrusted loader below a managed namespace must remain blocked");

  result = ytec::bootrepair::classify_efi_boot_ownership({
      .efi_directory_present = true,
      .boot_namespace_present = true,
      .fallback_loader_present = true,
      .fallback_loader_microsoft_signed = false,
  });
  check(
      result.has_value() &&
          result.value().state ==
              ytec::bootrepair::EfiBootOwnershipState::
                  non_microsoft_or_untrusted_present,
      "An untrusted fallback loader must block automatic repair");

  result = ytec::bootrepair::classify_efi_boot_ownership({
      .efi_directory_present = true,
      .ambiguous_object_detected = true,
  });
  check(
      result.has_value() &&
          result.value().state ==
              ytec::bootrepair::EfiBootOwnershipState::ambiguous,
      "Reparse or ambiguous EFI objects must stay distinguishable");
}

void test_null_dependencies_are_rejected_without_io() {
  check(
      ytec::bootrepair::
          make_filtered_boot_repair_volume_observation_provider(nullptr) ==
          nullptr,
      "Null volume enumerator must be rejected");
  check(
      ytec::bootrepair::make_offline_windows_candidate_validator(nullptr) ==
          nullptr,
      "Null offline-Windows probe must be rejected");
  check(
      ytec::bootrepair::make_windows_efi_boot_ownership_inspector(nullptr) ==
          nullptr,
      "Null EFI trust verifier must be rejected");

  check(
      ytec::bootrepair::make_automatic_boot_repair_plan_service(
          nullptr,
          std::make_unique<DummyVolumes>(),
          std::make_unique<DummyValidator>(),
          std::make_unique<DummyWinRe>(),
          std::make_unique<DummyEfiOwnership>()) == nullptr,
      "Null inventory must be rejected");
  check(
      ytec::bootrepair::make_automatic_boot_repair_plan_service(
          std::make_unique<DummyInventory>(),
          nullptr,
          std::make_unique<DummyValidator>(),
          std::make_unique<DummyWinRe>(),
          std::make_unique<DummyEfiOwnership>()) == nullptr,
      "Null volume provider must be rejected");
  check(
      ytec::bootrepair::make_automatic_boot_repair_plan_service(
          std::make_unique<DummyInventory>(),
          std::make_unique<DummyVolumes>(),
          nullptr,
          std::make_unique<DummyWinRe>(),
          std::make_unique<DummyEfiOwnership>()) == nullptr,
      "Null Windows validator must be rejected");
  check(
      ytec::bootrepair::make_automatic_boot_repair_plan_service(
          std::make_unique<DummyInventory>(),
          std::make_unique<DummyVolumes>(),
          std::make_unique<DummyValidator>(),
          nullptr,
          std::make_unique<DummyEfiOwnership>()) == nullptr,
      "Null WinRE inspector must be rejected");
  check(
      ytec::bootrepair::make_automatic_boot_repair_plan_service(
          std::make_unique<DummyInventory>(),
          std::make_unique<DummyVolumes>(),
          std::make_unique<DummyValidator>(),
          std::make_unique<DummyWinRe>(),
          nullptr) == nullptr,
      "Null EFI ownership inspector must be rejected");

  auto inventory = std::make_unique<DummyInventory>();
  auto volumes = std::make_unique<DummyVolumes>();
  auto validator = std::make_unique<DummyValidator>();
  auto winre = std::make_unique<DummyWinRe>();
  auto efi_ownership = std::make_unique<DummyEfiOwnership>();
  auto* const inventory_observer = inventory.get();
  auto* const volumes_observer = volumes.get();
  auto* const validator_observer = validator.get();
  auto* const winre_observer = winre.get();
  auto* const efi_observer = efi_ownership.get();
  const auto service =
      ytec::bootrepair::make_automatic_boot_repair_plan_service(
          std::move(inventory),
          std::move(volumes),
          std::move(validator),
          std::move(winre),
          std::move(efi_ownership));
  check(service != nullptr, "Complete dependency set should compose");
  check(
      inventory_observer->calls == 0U && volumes_observer->calls == 0U &&
          validator_observer->calls == 0U && winre_observer->calls == 0U &&
          efi_observer->calls == 0U,
      "Composition must perform no read or write I/O");
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests{
      {"provider_discards_out_of_scope_volumes_before_validation",
       test_provider_discards_out_of_scope_volumes_before_validation},
      {"selected_incomplete_observations_fail_closed",
       test_selected_incomplete_observations_fail_closed},
      {"selected_duplicate_name_and_extent_fail_closed",
       test_selected_duplicate_name_and_extent_fail_closed},
      {"selected_range_mismatch_fails_closed",
       test_selected_range_mismatch_fails_closed},
      {"windows_absent_stops_without_hive_or_architecture_probe",
       test_windows_absent_stops_without_hive_or_architecture_probe},
      {"supported_windows_requires_matching_version_and_verifier",
       test_supported_windows_requires_matching_version_and_verifier},
      {"unsupported_windows_is_distinct_from_probe_failure",
       test_unsupported_windows_is_distinct_from_probe_failure},
      {"windows_probe_error_is_propagated_not_downgraded_to_absent",
       test_windows_probe_error_is_propagated_not_downgraded_to_absent},
      {"inconsistent_windows_support_evidence_fails_closed",
       test_inconsistent_windows_support_evidence_fails_closed},
      {"efi_ownership_classification_is_fail_closed",
       test_efi_ownership_classification_is_fail_closed},
      {"null_dependencies_are_rejected_without_io",
       test_null_dependencies_are_rejected_without_io},
  };

  std::size_t failed = 0U;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  if (failed != 0U) {
    std::cerr << failed << " test(s) failed\n";
    return 1;
  }
  std::cout << tests.size() << " test(s) passed\n";
  return 0;
}

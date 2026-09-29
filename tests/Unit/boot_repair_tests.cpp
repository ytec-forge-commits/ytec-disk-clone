#include "ytec/bootrepair/bcdboot.h"
#include "ytec/bootrepair/offline_windows.h"
#include "ytec/bootrepair/registry_hive.h"
#include "ytec/bootrepair/standalone_repair.h"
#include "ytec/bootrepair/system_volume_mount.h"

#include <Windows.h>

#include <array>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
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

class TemporaryVersionHive final {
 public:
  TemporaryVersionHive(
      const DWORD major,
      const std::wstring& build,
      const std::wstring& installation_type) {
    std::array<wchar_t, MAX_PATH> temporary_directory{};
    const DWORD directory_length = GetTempPathW(
        static_cast<DWORD>(temporary_directory.size()),
        temporary_directory.data());
    if (directory_length == 0 ||
        directory_length >= temporary_directory.size()) {
      throw TestFailure{"Could not resolve the temporary directory"};
    }

    std::array<wchar_t, MAX_PATH> temporary_file{};
    if (GetTempFileNameW(
            temporary_directory.data(),
            L"YTH",
            0,
            temporary_file.data()) == 0) {
      throw TestFailure{"Could not reserve a temporary hive name"};
    }
    path_ = temporary_file.data();
    if (!DeleteFileW(path_.c_str())) {
      throw TestFailure{"Could not prepare an absent application hive path"};
    }

    HKEY hive{};
    LSTATUS status = RegLoadAppKeyW(
        path_.c_str(), &hive, KEY_ALL_ACCESS, REG_PROCESS_APPKEY, 0);
    if (status != ERROR_SUCCESS) {
      cleanup();
      throw TestFailure{"Could not create a synthetic application hive"};
    }

    HKEY version_key{};
    status = RegCreateKeyExW(
        hive,
        L"Microsoft\\Windows NT\\CurrentVersion",
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &version_key,
        nullptr);
    if (status == ERROR_SUCCESS) {
      status = RegSetValueExW(
          version_key,
          L"CurrentMajorVersionNumber",
          0,
          REG_DWORD,
          reinterpret_cast<const BYTE*>(&major),
          sizeof(major));
    }
    if (status == ERROR_SUCCESS) {
      status = set_text_value(
          version_key, L"CurrentBuildNumber", build);
    }
    if (status == ERROR_SUCCESS) {
      status = set_text_value(
          version_key, L"InstallationType", installation_type);
    }
    if (status == ERROR_SUCCESS) {
      status = RegFlushKey(hive);
    }
    if (version_key != nullptr) {
      RegCloseKey(version_key);
    }
    RegCloseKey(hive);
    if (status != ERROR_SUCCESS) {
      cleanup();
      throw TestFailure{"Could not populate the synthetic application hive"};
    }
  }

  ~TemporaryVersionHive() { cleanup(); }

  TemporaryVersionHive(const TemporaryVersionHive&) = delete;
  TemporaryVersionHive& operator=(const TemporaryVersionHive&) = delete;

  [[nodiscard]] const std::wstring& path() const noexcept { return path_; }

 private:
  static LSTATUS set_text_value(
      const HKEY key,
      const wchar_t* const name,
      const std::wstring& value) {
    const auto bytes = static_cast<DWORD>(
        (value.size() + 1U) * sizeof(wchar_t));
    return RegSetValueExW(
        key,
        name,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        bytes);
  }

  void cleanup() noexcept {
    if (path_.empty()) {
      return;
    }
    (void)DeleteFileW(path_.c_str());
    (void)DeleteFileW((path_ + L".LOG1").c_str());
    (void)DeleteFileW((path_ + L".LOG2").c_str());
  }

  std::wstring path_;
};

class TemporaryBcdHive final {
 public:
  explicit TemporaryBcdHive(const bool include_boot_manager) {
    std::array<wchar_t, MAX_PATH> temporary_directory{};
    const DWORD directory_length = GetTempPathW(
        static_cast<DWORD>(temporary_directory.size()),
        temporary_directory.data());
    if (directory_length == 0U ||
        directory_length >= temporary_directory.size()) {
      throw TestFailure{"Could not resolve the BCD temp directory"};
    }
    std::array<wchar_t, MAX_PATH> temporary_file{};
    if (GetTempFileNameW(
            temporary_directory.data(), L"YTB", 0U,
            temporary_file.data()) == 0U) {
      throw TestFailure{"Could not reserve a BCD hive path"};
    }
    path_ = temporary_file.data();
    if (!DeleteFileW(path_.c_str())) {
      throw TestFailure{"Could not prepare an absent BCD hive path"};
    }

    HKEY hive = nullptr;
    LSTATUS status = RegLoadAppKeyW(
        path_.c_str(),
        &hive,
        KEY_ALL_ACCESS,
        REG_PROCESS_APPKEY,
        0U);
    if (status != ERROR_SUCCESS || hive == nullptr) {
      cleanup();
      throw TestFailure{"Could not create a synthetic BCD hive"};
    }
    HKEY objects = nullptr;
    status = RegCreateKeyExW(
        hive,
        L"Objects",
        0U,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_ALL_ACCESS,
        nullptr,
        &objects,
        nullptr);
    HKEY boot_manager = nullptr;
    if (status == ERROR_SUCCESS && include_boot_manager) {
      status = RegCreateKeyExW(
          objects,
          L"{9dea862c-5cdd-4e70-acc1-f32b344d4795}",
          0U,
          nullptr,
          REG_OPTION_NON_VOLATILE,
          KEY_READ,
          nullptr,
          &boot_manager,
          nullptr);
    }
    if (status == ERROR_SUCCESS) {
      status = RegFlushKey(hive);
    }
    if (boot_manager != nullptr) {
      (void)RegCloseKey(boot_manager);
    }
    if (objects != nullptr) {
      (void)RegCloseKey(objects);
    }
    (void)RegCloseKey(hive);
    if (status != ERROR_SUCCESS) {
      cleanup();
      throw TestFailure{"Could not populate a synthetic BCD hive"};
    }
  }

  ~TemporaryBcdHive() { cleanup(); }

  TemporaryBcdHive(const TemporaryBcdHive&) = delete;
  TemporaryBcdHive& operator=(const TemporaryBcdHive&) = delete;

  [[nodiscard]] const std::wstring& path() const noexcept { return path_; }

 private:
  void cleanup() noexcept {
    if (path_.empty()) {
      return;
    }
    (void)DeleteFileW(path_.c_str());
    (void)DeleteFileW((path_ + L".LOG1").c_str());
    (void)DeleteFileW((path_ + L".LOG2").c_str());
  }

  std::wstring path_;
};

ytec::clonecore::Error mock_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::verification_failed,
      .native_code = ERROR_INVALID_DATA,
      .operation = operation,
      .message = L"モック検証失敗",
  };
}

class MockTrustVerifier final
    : public ytec::bootrepair::IExecutableTrustVerifier {
 public:
  ytec::clonecore::Status verify_microsoft_signed(
      const std::wstring& executable_path) override {
    ++call_count;
    received_path = executable_path;
    if (should_fail) {
      return ytec::clonecore::Status::failure(mock_error(L"モック署名検証"));
    }
    return ytec::clonecore::success_status();
  }

  std::wstring received_path;
  int call_count{};
  bool should_fail{};
};

class MockProcessRunner final : public ytec::bootrepair::IProcessRunner {
 public:
  ytec::clonecore::Result<ytec::bootrepair::ProcessResult> run(
      const std::wstring& executable_path,
      const std::vector<std::wstring>& arguments,
      const std::wstring& working_directory) override {
    ++call_count;
    received_path = executable_path;
    received_arguments = arguments;
    received_argument_sets.push_back(arguments);
    received_working_directory = working_directory;
    if (on_run_with_index) {
      on_run_with_index(call_count);
    } else if (on_run) {
      on_run();
    }
    return ytec::clonecore::Result<ytec::bootrepair::ProcessResult>::success(
        ytec::bootrepair::ProcessResult{
            .exit_code = exit_code,
            .standard_output = "mock stdout",
            .standard_error = "mock stderr",
        });
  }

  std::wstring received_path;
  std::wstring received_working_directory;
  std::vector<std::wstring> received_arguments;
  std::vector<std::vector<std::wstring>> received_argument_sets;
  std::uint32_t exit_code{};
  int call_count{};
  std::function<void()> on_run;
  std::function<void(int)> on_run_with_index;
};

class MockBcdStoreFileSystem final
    : public ytec::bootrepair::IBcdStoreFileSystem {
 public:
  ytec::clonecore::Result<bool> is_regular_non_reparse_file(
      const std::wstring& path) override {
    const auto identity = observe_regular_file_identity(path);
    if (!identity) {
      return ytec::clonecore::Result<bool>::failure(identity.error());
    }
    return ytec::clonecore::Result<bool>::success(
        identity.value().has_value());
  }

  ytec::clonecore::Result<
      std::optional<ytec::bootrepair::BcdStoreFileIdentity>>
  observe_regular_file_identity(const std::wstring& path) override {
    ++query_count;
    if (query_fails) {
      return ytec::clonecore::Result<
          std::optional<ytec::bootrepair::BcdStoreFileIdentity>>::failure(
          mock_error(L"モックBCD属性確認"));
    }
    if (!files.contains(path)) {
      return ytec::clonecore::Result<
          std::optional<ytec::bootrepair::BcdStoreFileIdentity>>::success(
          std::nullopt);
    }
    return ytec::clonecore::Result<
        std::optional<ytec::bootrepair::BcdStoreFileIdentity>>::success(
        identity_for(path));
  }

  ytec::clonecore::Status move_file_no_replace(
      const std::wstring& source,
      const std::wstring& destination,
      const ytec::bootrepair::BcdStoreFileIdentity&
          expected_source) override {
    moves.emplace_back(source, destination);
    if (move_fails || !files.contains(source) ||
        files.contains(destination)) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モックBCD移動"));
    }
    const auto source_identity = identity_for(source);
    if (!ytec::bootrepair::equivalent_bcd_store_file_identity(
            expected_source, source_identity)) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モックBCD移動元identity"));
    }
    files.erase(source);
    files.insert(destination);
    identities.erase(source);
    identities.insert_or_assign(destination, source_identity);
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status verify_bcd_store_read_only(
      const std::wstring& path) override {
    ++verification_count;
    if (verification_fails || !files.contains(path)) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モックBCD hive検証"));
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Status remove_file_if_identity_matches(
      const std::wstring& path,
      const ytec::bootrepair::BcdStoreFileIdentity& expected) override {
    removals.push_back(path);
    auto found = identities.find(path);
    if (replace_before_owned_remove && found != identities.end()) {
      ++found->second.change_time;
      replace_before_owned_remove = false;
    }
    if (remove_fails || !files.contains(path) ||
        found == identities.end() ||
        !ytec::bootrepair::equivalent_bcd_store_file_identity(
            expected, found->second)) {
      return ytec::clonecore::Status::failure(
          mock_error(L"モックBCD所有identity削除"));
    }
    files.erase(path);
    identities.erase(found);
    return ytec::clonecore::success_status();
  }

 private:
  ytec::bootrepair::BcdStoreFileIdentity identity_for(
      const std::wstring& path) {
    const auto found = identities.find(path);
    if (found != identities.end()) {
      return found->second;
    }
    ytec::bootrepair::BcdStoreFileIdentity identity{
        .volume_serial_number = 77U,
        .length = 4'096U,
        .last_write_time = next_identity,
        .change_time = next_identity,
    };
    identity.file_id[0] = static_cast<std::byte>(next_identity & 0xFFU);
    identity.file_id[1] =
        static_cast<std::byte>((next_identity >> 8U) & 0xFFU);
    ++next_identity;
    identities.insert_or_assign(path, identity);
    return identity;
  }

 public:

  std::set<std::wstring> files;
  std::vector<std::pair<std::wstring, std::wstring>> moves;
  std::vector<std::wstring> removals;
  int query_count{};
  int verification_count{};
  bool query_fails{};
  bool verification_fails{};
  bool move_fails{};
  bool remove_fails{};
  bool replace_before_owned_remove{};

 private:
  std::map<std::wstring, ytec::bootrepair::BcdStoreFileIdentity> identities;
  std::uint64_t next_identity{1U};
};

ytec::bootrepair::BcdBootRequest valid_request() {
  return ytec::bootrepair::BcdBootRequest{
      .target_windows_directory = L"W:\\Windows",
      .target_system_partition_root = L"S:\\",
  };
}

ytec::diskmodel::InventoryReport boot_repair_inventory(
    const ytec::diskmodel::PartitionStyle style) {
  ytec::diskmodel::InventoryReport report;
  ytec::diskmodel::DiskInfo disk;
  disk.disk_number = 7;
  disk.device_path = L"\\\\.\\PhysicalDrive7";
  disk.device_instance_id = L"MOCK\\DISK\\7";
  disk.model = L"MOCK SYSTEM DISK";
  disk.size_bytes = 128ULL * 1024U * 1024U * 1024U;
  disk.logical_sector_size = 512;
  disk.physical_sector_size = 4096;
  disk.serial_suffix = "REPAIR07";
  disk.partition_style = style;
  disk.offline = false;
  disk.read_only = false;
  disk.removable = false;
  if (style == ytec::diskmodel::PartitionStyle::gpt) {
    disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
        .number = 1,
        .offset_bytes = 1'048'576,
        .size_bytes = 272'629'760,
        .style = style,
        .type = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}",
    });
    disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
        .number = 3,
        .offset_bytes = 290'455'552,
        .size_bytes = 100ULL * 1024U * 1024U * 1024U,
        .style = style,
        .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
    });
  } else {
    disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
        .number = 1,
        .offset_bytes = 1'048'576,
        .size_bytes = 524'288'000,
        .style = style,
        .type = L"0x07",
        .bootable = true,
    });
    disk.partitions.push_back(ytec::diskmodel::PartitionInfo{
        .number = 2,
        .offset_bytes = 525'336'576,
        .size_bytes = 100ULL * 1024U * 1024U * 1024U,
        .style = style,
        .type = L"0x07",
    });
  }
  report.disks.push_back(std::move(disk));
  return report;
}

void test_uefi_standalone_target_is_bound_to_esp_and_windows() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto selection = ytec::bootrepair::evaluate_boot_repair_target(
      ytec::bootrepair::BootRepairTargetRequest{
          .disk_number = 7,
          .windows_root = L"W:\\",
          .system_root = L"S:\\",
          .firmware = ytec::bootrepair::BcdBootFirmware::uefi,
      },
      inventory,
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 290'455'552,
          .extent_length = 100ULL * 1024U * 1024U * 1024U,
          .file_system = L"NTFS",
      },
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 1'048'576,
          .extent_length = 272'629'760,
          .file_system = L"FAT32",
      });
  check(selection.has_value(), "Valid offline GPT/UEFI target should pass");
  check(selection.value().windows_partition.number == 3,
        "Windows volume must bind to the basic-data partition");
  check(selection.value().system_partition.number == 1,
        "System volume must bind to the EFI system partition");
}

void test_uefi_standalone_target_rejects_cross_disk_esp() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto selection = ytec::bootrepair::evaluate_boot_repair_target(
      ytec::bootrepair::BootRepairTargetRequest{
          .disk_number = 7,
          .windows_root = L"W:\\",
          .system_root = L"S:\\",
          .firmware = ytec::bootrepair::BcdBootFirmware::uefi,
      },
      inventory,
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 290'455'552,
          .extent_length = 100ULL * 1024U * 1024U * 1024U,
          .file_system = L"NTFS",
      },
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 8,
          .starting_offset = 1'048'576,
          .extent_length = 272'629'760,
          .file_system = L"FAT32",
      });
  check(!selection.has_value(),
        "An ESP on a different disk must fail closed");
  check(selection.error().code ==
            ytec::clonecore::ErrorCode::identity_mismatch,
        "Cross-disk system volume must be an identity failure");
}

void test_uefi_standalone_target_accepts_read_only_unassigned_esp_plan() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto selection = ytec::bootrepair::evaluate_boot_repair_target(
      ytec::bootrepair::BootRepairTargetRequest{
          .disk_number = 7,
          .windows_root = L"W:\\",
          .system_root = L"",
          .firmware = ytec::bootrepair::BcdBootFirmware::uefi,
          .auto_mount_system_partition = true,
      },
      inventory,
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 290'455'552,
          .extent_length = 100ULL * 1024U * 1024U * 1024U,
          .file_system = L"NTFS",
      },
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 1'048'576,
          .extent_length = 272'629'760,
          .file_system = L"FAT32",
      });
  check(selection.has_value(),
        "Read-only preflight should accept an exact unassigned ESP plan");

  const auto conflicting = ytec::bootrepair::evaluate_boot_repair_target(
      ytec::bootrepair::BootRepairTargetRequest{
          .disk_number = 7,
          .windows_root = L"W:\\",
          .system_root = L"S:\\",
          .firmware = ytec::bootrepair::BcdBootFirmware::uefi,
          .auto_mount_system_partition = true,
      },
      inventory,
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 290'455'552,
          .extent_length = 100ULL * 1024U * 1024U * 1024U,
          .file_system = L"NTFS",
      },
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 1'048'576,
          .extent_length = 272'629'760,
          .file_system = L"FAT32",
      });
  check(!conflicting.has_value(),
        "Auto-mount and a caller-selected system root must not be combined");
}

void test_bios_standalone_target_requires_one_active_partition() {
  auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::mbr);
  inventory.disks[0].partitions[1].bootable = true;
  const auto selection = ytec::bootrepair::evaluate_boot_repair_target(
      ytec::bootrepair::BootRepairTargetRequest{
          .disk_number = 7,
          .windows_root = L"W:\\",
          .system_root = L"S:\\",
          .firmware = ytec::bootrepair::BcdBootFirmware::bios,
      },
      inventory,
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 525'336'576,
          .extent_length = 100ULL * 1024U * 1024U * 1024U,
          .file_system = L"NTFS",
      },
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 1'048'576,
          .extent_length = 524'288'000,
          .file_system = L"NTFS",
      });
  check(!selection.has_value(),
        "Ambiguous BIOS Active partitions must fail closed");
}

void test_standalone_confirmation_is_target_specific() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto selection = ytec::bootrepair::evaluate_boot_repair_target(
      ytec::bootrepair::BootRepairTargetRequest{
          .disk_number = 7,
          .windows_root = L"W:\\",
          .system_root = L"S:\\",
          .firmware = ytec::bootrepair::BcdBootFirmware::uefi,
      },
      inventory,
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 290'455'552,
          .extent_length = 100ULL * 1024U * 1024U * 1024U,
          .file_system = L"NTFS",
      },
      ytec::bootrepair::BootRepairVolumeLocation{
          .disk_number = 7,
          .starting_offset = 1'048'576,
          .extent_length = 272'629'760,
          .file_system = L"FAT32",
      });
  check(selection.has_value(), "Fixture selection should pass");
  const std::wstring token =
      ytec::bootrepair::make_boot_repair_confirmation_token(
          selection.value().identity,
          ytec::bootrepair::BcdBootFirmware::uefi);
  check(token.find(L"REPAIR BOOT UEFI") == 0,
        "Confirmation must state the non-erasure operation and firmware");
  const auto valid = ytec::bootrepair::validate_boot_repair_selection(
      selection.value(),
      selection.value(),
      ytec::bootrepair::BcdBootFirmware::uefi,
      ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = token,
      });
  check(valid.has_value(), "Exact two-step confirmation should pass");
  auto changed_disk = selection.value();
  changed_disk.disk.partitions.front().name += L" changed";
  const auto changed_layout =
      ytec::bootrepair::validate_boot_repair_selection(
          selection.value(),
          changed_disk,
          ytec::bootrepair::BcdBootFirmware::uefi,
          ytec::clonecore::TargetConfirmation{
              .first_step_acknowledged = true,
              .typed_token = token,
          });
  check(!changed_layout.has_value(),
        "Any full-disk layout field change must invalidate execution");
  const auto invalid = ytec::bootrepair::validate_boot_repair_selection(
      selection.value(),
      selection.value(),
      ytec::bootrepair::BcdBootFirmware::uefi,
      ytec::clonecore::TargetConfirmation{
          .first_step_acknowledged = true,
          .typed_token = L"REPAIR BOOT WRONG",
      });
  check(!invalid.has_value(), "Wrong confirmation must fail");
}

void test_uefi_efi_ownership_is_rechecked_and_nvram_stays_disabled() {
  const ytec::bootrepair::EfiBootOwnershipEvidence safe{
      .state = ytec::bootrepair::EfiBootOwnershipState::
          microsoft_only_or_empty,
      .efi_directory_present = true,
      .microsoft_namespace_present = true,
      .microsoft_signed_efi_loader_count = 4U,
  };
  ytec::bootrepair::BootRepairTargetRequest request{
      .disk_number = 7U,
      .windows_root = L"W:\\",
      .system_root = L"S:\\",
      .firmware = ytec::bootrepair::BcdBootFirmware::uefi,
      .store_policy = ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh,
      .system_volume_identity_root =
          L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\",
      .require_efi_ownership_recheck = true,
      .expected_efi_ownership = safe,
  };
  check(ytec::bootrepair::validate_boot_repair_efi_ownership(request, safe)
            .has_value(),
        "Matching Microsoft-only EFI evidence should pass");

  auto changed = safe;
  ++changed.microsoft_signed_efi_loader_count;
  auto status = ytec::bootrepair::validate_boot_repair_efi_ownership(
      request, changed);
  check(!status.has_value() &&
            status.error().code ==
                ytec::clonecore::ErrorCode::identity_mismatch,
        "Any EFI observation change must invalidate execution");

  auto third_party = safe;
  third_party.state = ytec::bootrepair::EfiBootOwnershipState::
      non_microsoft_or_untrusted_present;
  third_party.non_microsoft_or_untrusted_entry_count = 1U;
  third_party.top_level_non_microsoft_namespace_count = 1U;
  request.expected_efi_ownership = third_party;
  status = ytec::bootrepair::validate_boot_repair_efi_ownership(
      request, third_party);
  check(!status.has_value() &&
            status.error().code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "Third-party EFI content must be preserved by refusing execution");

  request.third_party_efi_policy =
      ytec::bootrepair::BootRepairThirdPartyEfiPolicy::preserve;
  status = ytec::bootrepair::validate_boot_repair_efi_ownership(
      request, third_party);
  check(!status.has_value() &&
            status.error().code ==
                ytec::clonecore::ErrorCode::confirmation_required,
        "A standalone request must not reuse the reviewed-batch preserve policy");

  request.reviewed_multi_windows_batch = true;
  status = ytec::bootrepair::validate_boot_repair_efi_ownership(
      request, third_party);
  check(status.has_value(),
        "An exact reviewed-batch preserve policy should retain third-party EFI");

  request.third_party_efi_policy =
      ytec::bootrepair::BootRepairThirdPartyEfiPolicy::delete_non_microsoft;
  status = ytec::bootrepair::validate_boot_repair_efi_ownership(
      request, third_party);
  check(!status.has_value() &&
            status.error().code ==
                ytec::clonecore::ErrorCode::unsupported_layout,
        "The unimplemented third-party EFI delete transaction must stay disabled");

  request.expected_efi_ownership = safe;
  request.third_party_efi_policy =
      ytec::bootrepair::BootRepairThirdPartyEfiPolicy::not_applicable;
  request.reviewed_multi_windows_batch = false;
  request.update_current_pc_nvram = true;
  status = ytec::bootrepair::validate_boot_repair_efi_ownership(
      request, safe);
  check(!status.has_value() &&
            status.error().code ==
                ytec::clonecore::ErrorCode::unsupported_layout,
        "The direct existing-ESP path must not modify current-PC NVRAM");
}

void test_bcdboot_arguments_are_fixed_and_separate() {
  const auto arguments =
      ytec::bootrepair::build_bcdboot_arguments(valid_request());
  check(arguments.has_value(), "Valid target paths should be accepted");
  check(arguments.value().size() == 6, "BCDBoot must receive six fixed arguments");
  check(arguments.value()[0] == L"W:\\Windows", "Windows path is argv[1]");
  check(arguments.value()[1] == L"/s", "The /s switch must be fixed");
  check(arguments.value()[2] == L"S:\\", "ESP root is a separate argument");
  check(arguments.value()[4] == L"UEFI", "The Phase 1 request should use UEFI");
}

void test_bios_arguments_allow_active_windows_partition() {
  auto request = valid_request();
  request.target_system_partition_root = L"W:\\";
  request.firmware = ytec::bootrepair::BcdBootFirmware::bios;
  const auto arguments = ytec::bootrepair::build_bcdboot_arguments(request);
  check(arguments.has_value(), "BIOS BCDBoot should allow the active Windows volume");
  check(arguments.value()[2] == L"W:\\", "The active system root must remain separate argv");
  check(arguments.value()[4] == L"BIOS", "Only the fixed BIOS firmware value is allowed");
}

void test_fresh_store_policy_adds_fixed_c_switch() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const auto arguments =
      ytec::bootrepair::build_bcdboot_arguments(request);
  check(arguments.has_value(), "Fresh BCD policy should be accepted");
  check(arguments.value().size() == 7,
        "Fresh BCD policy adds exactly one fixed argument");
  check(arguments.value().back() == L"/c",
        "Fresh BCD policy must use the documented /c switch");
}

void test_invalid_store_policy_is_rejected() {
  auto request = valid_request();
  request.store_policy =
      static_cast<ytec::bootrepair::BcdBootStorePolicy>(0xff);
  const auto arguments =
      ytec::bootrepair::build_bcdboot_arguments(request);
  check(!arguments.has_value(), "Unknown BCD policy must fail closed");
}

void test_bcd_store_path_is_fixed_by_firmware() {
  auto request = valid_request();
  const auto uefi = ytec::bootrepair::bcd_store_path(request);
  check(uefi.has_value(), "UEFI BCD path should be resolved");
  check(
      uefi.value() == L"S:\\EFI\\Microsoft\\Boot\\BCD",
      "UEFI BCD path must remain below the explicit ESP root");

  request.target_system_partition_root = L"W:\\";
  request.firmware = ytec::bootrepair::BcdBootFirmware::bios;
  const auto bios = ytec::bootrepair::bcd_store_path(request);
  check(bios.has_value(), "BIOS BCD path should be resolved");
  check(
      bios.value() == L"W:\\Boot\\BCD",
      "BIOS BCD path must remain below the explicit active system root");
}

void test_windows_bcd_hive_verifier_rejects_missing_objects_and_hardlinks() {
  TemporaryBcdHive valid(true);
  const auto accepted =
      ytec::bootrepair::verify_bcd_store_file_with_windows_apis(
          valid.path());
  check(accepted.has_value(), "A bounded synthetic BCD hive should verify");

  TemporaryBcdHive missing_boot_manager(false);
  const auto rejected =
      ytec::bootrepair::verify_bcd_store_file_with_windows_apis(
          missing_boot_manager.path());
  check(!rejected.has_value(), "A BCD without Boot Manager must be rejected");

  const std::wstring hardlink = valid.path() + L".hardlink";
  (void)DeleteFileW(hardlink.c_str());
  check(
      CreateHardLinkW(hardlink.c_str(), valid.path().c_str(), nullptr) != 0,
      "The synthetic hard-link fixture should be created");
  const auto linked =
      ytec::bootrepair::verify_bcd_store_file_with_windows_apis(hardlink);
  check(!linked.has_value(), "A hard-linked BCD path must fail closed");
  check(DeleteFileW(hardlink.c_str()) != 0,
        "The exact synthetic hard-link fixture should be removed");
}

void test_fresh_store_transaction_replaces_prior_store() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  const std::wstring backup = store + L".ytec-rebuild-backup";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  file_system.files.insert(store);
  runner.on_run = [&file_system, &store]() {
    file_system.files.insert(store);
  };

  const auto result =
      ytec::bootrepair::execute_bcdboot_with_store_transaction(
          request,
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(result.has_value(), "Fresh BCD transaction should succeed");
  check(verifier.call_count == 2,
        "BCDBoot must be trusted before mutation and again before launch");
  check(runner.call_count == 1, "BCDBoot must execute exactly once");
  check(file_system.files.contains(store),
        "The newly created BCD store must remain");
  check(!file_system.files.contains(backup),
        "The prior BCD backup must be removed after verification");
  check(result.value().prior_store_replaced,
        "The report must record replacement of an existing store");
  check(result.value().fresh_store_verified,
        "The report must record fresh-store verification");
  check(file_system.verification_count == 1,
        "A regular file alone is insufficient; the BCD hive must be opened");
}

void test_fresh_store_invalid_hive_restores_prior_store() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  const std::wstring backup = store + L".ytec-rebuild-backup";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  file_system.files.insert(store);
  file_system.verification_fails = true;
  runner.on_run = [&file_system, &store]() {
    file_system.files.insert(store);
  };

  const auto result =
      ytec::bootrepair::execute_bcdboot_with_store_transaction(
          request,
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(!result.has_value(), "An invalid BCD hive must fail verification");
  check(
      file_system.verification_count == 1 &&
          file_system.files.contains(store) &&
          !file_system.files.contains(backup) &&
          file_system.moves.size() == 2U &&
          file_system.removals.size() == 1U,
      "Invalid-hive verification must roll the original BCD back");
}

void test_fresh_store_failure_restores_prior_store() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  const std::wstring backup = store + L".ytec-rebuild-backup";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  file_system.files.insert(store);
  runner.exit_code = 7;
  runner.on_run = [&file_system, &store]() {
    file_system.files.insert(store);
  };

  const auto result =
      ytec::bootrepair::execute_bcdboot_with_store_transaction(
          request,
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(!result.has_value(), "A failed BCDBoot transaction must fail");
  check(file_system.files.contains(store),
        "The prior BCD must be restored after process failure");
  check(!file_system.files.contains(backup),
        "Rollback must not leave a renamed prior BCD");
  check(file_system.moves.size() == 2U,
        "Rollback must move the prior BCD out and back exactly once");
  check(file_system.removals.size() == 1U,
        "Rollback must remove a partial replacement before restoration");
}

void test_fresh_store_missing_output_restores_prior_store() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  const std::wstring backup = store + L".ytec-rebuild-backup";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  file_system.files.insert(store);

  const auto result =
      ytec::bootrepair::execute_bcdboot_with_store_transaction(
          request,
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(!result.has_value(),
        "BCDBoot success without a fresh store must not be accepted");
  check(file_system.files.contains(store),
        "The prior BCD must be restored when fresh output is missing");
  check(!file_system.files.contains(backup),
        "Missing output rollback must restore the original path");
}

void test_fresh_store_stale_backup_stops_before_process() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  file_system.files.insert(store);
  file_system.files.insert(store + L".ytec-rebuild-backup");

  const auto result =
      ytec::bootrepair::execute_bcdboot_with_store_transaction(
          request,
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(!result.has_value(), "A stale BCD backup must fail closed");
  check(runner.call_count == 0,
        "A stale backup must stop before BCDBoot execution");
  check(file_system.moves.empty(),
        "A stale backup must stop before changing either BCD file");
}

void test_multi_windows_bcd_transaction_is_one_ordered_commit() {
  auto first = valid_request();
  first.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  auto second = first;
  second.target_windows_directory = L"D:\\Windows";
  second.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::preserve_existing;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  const std::wstring backup = store + L".ytec-rebuild-backup";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  file_system.files.insert(store);
  runner.on_run = [&file_system, &store]() {
    file_system.files.insert(store);
  };

  const auto result = ytec::bootrepair::
      execute_multi_windows_bcdboot_with_store_transaction(
          {first, second},
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(result.has_value(), "Two reviewed Windows should commit together");
  check(
      result.value().windows_registrations.size() == 2U &&
          result.value().prior_store_replaced &&
          result.value().fresh_store_verified,
      "The combined report must retain every registration and one commit");
  check(
      verifier.call_count == 3 && runner.call_count == 2,
      "Trust must be checked before mutation and before both launches");
  check(file_system.verification_count == 2,
        "The BCD hive must be verified after every Windows registration");
  check(
      runner.received_argument_sets.size() == 2U &&
          runner.received_argument_sets[0].back() == L"/c" &&
          std::find(
              runner.received_argument_sets[1].begin(),
              runner.received_argument_sets[1].end(),
              L"/c") == runner.received_argument_sets[1].end() &&
          runner.received_argument_sets[0].front() == L"W:\\Windows" &&
          runner.received_argument_sets[1].front() == L"D:\\Windows" &&
          runner.received_argument_sets[0][1] == L"/s" &&
          runner.received_argument_sets[1][1] == L"/s" &&
          std::find(
              runner.received_argument_sets[0].begin(),
              runner.received_argument_sets[0].end(),
              L"/p") == runner.received_argument_sets[0].end() &&
          std::find(
              runner.received_argument_sets[1].begin(),
              runner.received_argument_sets[1].end(),
              L"/p") == runner.received_argument_sets[1].end(),
      "Priority order must remain and every call must target /s without NVRAM mutation");
  check(
      file_system.files.contains(store) &&
          !file_system.files.contains(backup) &&
          file_system.moves.size() == 1U &&
          file_system.removals.size() == 1U,
      "Successful multi-registration must remove only the prior backup");
}

void test_multi_windows_failure_rolls_back_the_whole_store() {
  auto first = valid_request();
  first.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  auto second = first;
  second.target_windows_directory = L"D:\\Windows";
  second.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::preserve_existing;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  const std::wstring backup = store + L".ytec-rebuild-backup";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  file_system.files.insert(store);
  runner.on_run_with_index = [&](const int call_count) {
    file_system.files.insert(store);
    if (call_count == 2) {
      runner.exit_code = 9U;
    }
  };

  const auto result = ytec::bootrepair::
      execute_multi_windows_bcdboot_with_store_transaction(
          {first, second},
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(!result.has_value(), "A later Windows failure must fail the batch");
  check(
      file_system.files.contains(store) &&
          !file_system.files.contains(backup) &&
          file_system.moves.size() == 2U &&
          file_system.removals.size() == 1U,
      "A later failure must remove the partial store and restore the original");
}

void test_multi_windows_missing_original_creates_verified_store() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  runner.on_run = [&file_system, &store]() {
    file_system.files.insert(store);
  };
  const auto result = ytec::bootrepair::
      execute_multi_windows_bcdboot_with_store_transaction(
          {request},
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(result.has_value(), "A missing BCD should be created and verified");
  check(
      file_system.files.contains(store) &&
          file_system.moves.empty() &&
          file_system.removals.empty() && runner.call_count == 1 &&
          file_system.verification_count == 1,
      "A new BCD must be kept only after identity and hive verification");
}

void test_multi_windows_missing_original_failure_removes_owned_partial() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  runner.exit_code = 9U;
  runner.on_run = [&file_system, &store]() {
    file_system.files.insert(store);
  };

  const auto result = ytec::bootrepair::
      execute_multi_windows_bcdboot_with_store_transaction(
          {request},
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(!result.has_value(), "A failed new BCD build must fail");
  check(
      !file_system.files.contains(store) && file_system.moves.empty() &&
          file_system.removals.size() == 1U && runner.call_count == 1,
      "Rollback must delete only the opened-handle identity it observed");
}

void test_multi_windows_identity_swap_blocks_partial_cleanup() {
  auto request = valid_request();
  request.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  const std::wstring store = L"S:\\EFI\\Microsoft\\Boot\\BCD";
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;
  runner.exit_code = 9U;
  runner.on_run = [&file_system, &store]() {
    file_system.files.insert(store);
    file_system.replace_before_owned_remove = true;
  };

  const auto result = ytec::bootrepair::
      execute_multi_windows_bcdboot_with_store_transaction(
          {request},
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);

  check(!result.has_value(), "A swapped partial must fail cleanup");
  check(
      file_system.files.contains(store) &&
          result.error().code ==
              ytec::clonecore::ErrorCode::verification_failed,
      "Identity mismatch must retain the replacement instead of deleting it");
}

void test_multi_windows_invalid_plan_stops_before_mutation() {
  auto first = valid_request();
  first.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::rebuild_fresh;
  auto second = first;
  second.target_windows_directory = L"W:\\Windows";
  second.store_policy =
      ytec::bootrepair::BcdBootStorePolicy::preserve_existing;
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  MockBcdStoreFileSystem file_system;

  const auto duplicate = ytec::bootrepair::
      execute_multi_windows_bcdboot_with_store_transaction(
          {first, second},
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);
  check(
      !duplicate.has_value() && verifier.call_count == 0 &&
          runner.call_count == 0 && file_system.moves.empty(),
      "Duplicate Windows choices must stop before trust, process, or BCD I/O");

  second.target_windows_directory = L"D:\\Windows";
  second.target_system_partition_root = L"T:\\";
  const auto other_system = ytec::bootrepair::
      execute_multi_windows_bcdboot_with_store_transaction(
          {first, second},
          L"X:\\Windows\\System32",
          verifier,
          runner,
          file_system);
  check(
      !other_system.has_value() && verifier.call_count == 0 &&
          runner.call_count == 0 && file_system.moves.empty(),
      "A mixed system partition batch must stop before mutation");
}

void test_windows_argument_escaping() {
  check(
      ytec::bootrepair::quote_windows_argument(L"simple") == L"simple" &&
          ytec::bootrepair::quote_windows_argument(L"/FS:NTFS") ==
              L"/FS:NTFS",
      "Arguments without shell separators should remain unquoted");
  check(
      ytec::bootrepair::quote_windows_argument(L"C:\\path with space\\") ==
          L"\"C:\\path with space\\\\\"",
      "A trailing backslash must be doubled before the closing quote");
  check(
      ytec::bootrepair::quote_windows_argument(L"a\"b") == L"\"a\\\"b\"",
      "Embedded quotes must be escaped according to CreateProcess rules");
}

void test_execute_uses_system32_and_signature_gate() {
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  const auto result = ytec::bootrepair::execute_bcdboot(
      valid_request(), L"X:\\Windows\\System32", verifier, runner);
  check(result.has_value(), "Successful mocked BCDBoot should report success");
  check(verifier.call_count == 1, "Signature verification must run once");
  check(runner.call_count == 1, "Process must run only after verification");
  check(
      verifier.received_path == L"X:\\Windows\\System32\\bcdboot.exe",
      "PATH lookup must not be used");
  check(
      runner.received_path == verifier.received_path,
      "The verified executable must be the executed executable");
  check(result.value().microsoft_signature_verified, "Report records the trust gate");
}

void test_invalid_path_stops_before_signature() {
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  auto request = valid_request();
  request.target_windows_directory = L"W:\\Windows\\..\\Source\\Windows";
  const auto result = ytec::bootrepair::execute_bcdboot(
      request, L"X:\\Windows\\System32", verifier, runner);
  check(!result.has_value(), "Parent traversal must be rejected");
  check(verifier.call_count == 0, "Invalid arguments stop before trust checks");
  check(runner.call_count == 0, "Invalid arguments must never start a process");
}

void test_signature_failure_stops_process() {
  MockTrustVerifier verifier;
  verifier.should_fail = true;
  MockProcessRunner runner;
  const auto result = ytec::bootrepair::execute_bcdboot(
      valid_request(), L"X:\\Windows\\System32", verifier, runner);
  check(!result.has_value(), "Untrusted BCDBoot must fail");
  check(runner.call_count == 0, "Untrusted executable must never run");
}

void test_nonzero_exit_is_not_success() {
  MockTrustVerifier verifier;
  MockProcessRunner runner;
  runner.exit_code = 7;
  const auto result = ytec::bootrepair::execute_bcdboot(
      valid_request(), L"X:\\Windows\\System32", verifier, runner);
  check(!result.has_value(), "Nonzero BCDBoot exit must fail");
  check(
      result.error().code == ytec::clonecore::ErrorCode::verification_failed,
      "A failed boot repair cannot be reported as a successful clone");
}

void test_default_streamed_runner_delivers_captured_output() {
  MockProcessRunner runner;
  std::string streamed;
  const auto result = runner.run_streamed(
      L"X:\\Windows\\System32\\mock.exe",
      {L"--fixed"},
      L"X:\\Windows\\System32",
      [&streamed](const std::string_view chunk) {
        streamed.append(chunk);
      });
  check(result.has_value(), "Default streamed runner should preserve success");
  check(runner.call_count == 1, "Default streaming must execute only once");
  check(
      streamed == "mock stdout",
      "Default streaming must deliver the captured stdout");
}

void test_offline_windows_version_gate() {
  check(
      ytec::bootrepair::is_supported_offline_windows_version(
          ytec::bootrepair::OfflineWindowsVersion{
              .major = 10,
              .build = 19045,
              .installation_type = L"Client"}),
      "Windows 10 client should be supported");
  check(
      ytec::bootrepair::is_supported_offline_windows_version(
          ytec::bootrepair::OfflineWindowsVersion{
              .major = 10,
              .build = 26100,
              .installation_type = L"Client"}),
      "Windows 11 client should be supported");
  check(
      !ytec::bootrepair::is_supported_offline_windows_version(
          ytec::bootrepair::OfflineWindowsVersion{
              .major = 6,
              .build = 7601,
              .installation_type = L"Client"}),
      "Windows 7 x64 must not pass the Windows 10/11 gate");
  check(
      !ytec::bootrepair::is_supported_offline_windows_version(
          ytec::bootrepair::OfflineWindowsVersion{
              .major = 10,
              .build = 20348,
              .installation_type = L"Server"}),
      "Windows Server must not pass a client-only gate");
}

void test_offline_windows_volume_root_is_strictly_bounded() {
  const auto drive =
      ytec::bootrepair::normalize_offline_windows_volume_root(L"c:/");
  check(drive.has_value(), "Drive roots should be accepted");
  check(drive.value() == L"C:\\", "Drive roots should be normalized");

  constexpr std::wstring_view kVolumeRoot =
      L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\";
  const auto volume =
      ytec::bootrepair::normalize_offline_windows_volume_root(kVolumeRoot);
  check(volume.has_value(), "Exact Volume GUID roots should be accepted");
  check(volume.value() == kVolumeRoot, "Volume GUID roots should be stable");

  const std::array<std::wstring_view, 5> rejected{
      L"C:\\Windows",
      L"\\\\server\\share\\",
      L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\Windows\\",
      L"\\\\?\\Volume{11111111-2222-3333-4444-55555555555Z}\\",
      L"\\\\.\\PhysicalDrive1",
  };
  for (const auto candidate : rejected) {
    const auto result =
        ytec::bootrepair::normalize_offline_windows_volume_root(candidate);
    check(!result.has_value(), "Child, UNC, malformed, and device paths must fail");
    check(
        result.error().code == ytec::clonecore::ErrorCode::invalid_argument,
        "Rejected roots should report invalid_argument");
  }
}

void test_offline_registry_reader_reads_synthetic_hive() {
  TemporaryVersionHive hive(10, L"19045", L"Client");
  const auto raw_values =
      ytec::bootrepair::read_windows_current_version_from_hive(hive.path());
  check(raw_values.has_value(), "Bounded REGF reader should load the hive");
  check(raw_values.value().major == 10, "Raw major version should match");
  check(raw_values.value().build == L"19045", "Raw build should match");
  check(
      raw_values.value().installation_type == L"Client",
      "Raw installation type should match");

  const auto version =
      ytec::bootrepair::read_offline_windows_version_hive(hive.path());
  check(version.has_value(), "Synthetic offline SOFTWARE hive should load");
  check(version.value().major == 10, "Offline major version should match");
  check(version.value().build == 19045, "Offline build should match");
  check(
      version.value().installation_type == L"Client",
      "Offline installation type should match");
}

ytec::bootrepair::BootVolumeObservation unassigned_system_volume(
    const ytec::diskmodel::DiskInfo& disk,
    const std::size_t partition_index,
    const std::wstring& file_system) {
  const auto& partition = disk.partitions[partition_index];
  return ytec::bootrepair::BootVolumeObservation{
      .volume_name = L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\",
      .location =
          ytec::bootrepair::BootRepairVolumeLocation{
              .disk_number = disk.disk_number,
              .starting_offset = partition.offset_bytes,
              .extent_length = partition.size_bytes,
              .file_system = file_system,
          },
  };
}

ytec::clonecore::Error mount_test_error(const std::wstring& operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = operation,
      .message = L"注入した一時割当エラー",
  };
}

class MockSystemVolumeMountApi final
    : public ytec::bootrepair::ISystemVolumeMountApi {
 public:
  ytec::clonecore::Status attach(
      const std::wstring& temporary_root,
      const std::wstring& volume_name) override {
    ++attach_count;
    attached_root = temporary_root;
    attached_volume = volume_name;
    if (attach_fails) {
      return ytec::clonecore::Status::failure(
          mount_test_error(L"モック一時割当"));
    }
    return ytec::clonecore::success_status();
  }

  ytec::clonecore::Result<ytec::bootrepair::BootVolumeObservation> inspect(
      const std::wstring& temporary_root,
      const std::wstring& expected_volume_name,
      const ytec::bootrepair::BootRepairVolumeLocation&
          expected_location) override {
    ++inspect_count;
    inspected_root = temporary_root;
    inspected_volume = expected_volume_name;
    inspected_location = expected_location;
    if (inspect_fails) {
      return ytec::clonecore::Result<
          ytec::bootrepair::BootVolumeObservation>::failure(
          mount_test_error(L"モック割当後再識別"));
    }
    return ytec::clonecore::Result<
        ytec::bootrepair::BootVolumeObservation>::success(observation);
  }

  ytec::clonecore::Status detach(
      const std::wstring& temporary_root,
      const std::wstring& expected_volume_name) override {
    ++detach_count;
    detached_root = temporary_root;
    detached_volume = expected_volume_name;
    if (detach_fails) {
      return ytec::clonecore::Status::failure(
          mount_test_error(L"モック一時割当解除"));
    }
    return ytec::clonecore::success_status();
  }

  bool attach_fails{};
  bool inspect_fails{};
  bool detach_fails{};
  int attach_count{};
  int inspect_count{};
  int detach_count{};
  std::wstring attached_root;
  std::wstring attached_volume;
  std::wstring inspected_root;
  std::wstring inspected_volume;
  ytec::bootrepair::BootRepairVolumeLocation inspected_location;
  std::wstring detached_root;
  std::wstring detached_volume;
  ytec::bootrepair::BootVolumeObservation observation;
};

void test_unassigned_uefi_system_volume_plan_is_exact_and_read_only() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto& disk = inventory.disks.front();
  const auto volume = unassigned_system_volume(disk, 0U, L"FAT32");

  const auto plan =
      ytec::bootrepair::plan_temporary_system_volume_mount(
          disk,
          ytec::bootrepair::BcdBootFirmware::uefi,
          {volume},
          L"WXY");

  check(plan.has_value(),
        "A unique unassigned FAT32 ESP should produce a mount plan");
  check(
      plan.value().partition_number == 1U &&
          plan.value().volume_name == volume.volume_name &&
          plan.value().temporary_root == L"V:\\",
      "The plan must bind the exact ESP/Volume GUID and skip used W/X/Y letters");
}

void test_unassigned_system_volume_plan_rejects_existing_mount_or_ambiguity() {
  auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto& disk = inventory.disks.front();
  auto mounted = unassigned_system_volume(disk, 0U, L"FAT32");
  mounted.mount_points = {L"S:\\"};
  const auto mounted_result =
      ytec::bootrepair::plan_temporary_system_volume_mount(
          disk,
          ytec::bootrepair::BcdBootFirmware::uefi,
          {mounted},
          L"WXS");
  check(!mounted_result.has_value(),
        "An already mounted ESP must use its existing root instead of an alias");

  const auto unassigned = unassigned_system_volume(disk, 0U, L"FAT32");
  auto duplicate = unassigned;
  duplicate.volume_name =
      L"\\\\?\\Volume{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}\\";
  const auto duplicate_result =
      ytec::bootrepair::plan_temporary_system_volume_mount(
          disk,
          ytec::bootrepair::BcdBootFirmware::uefi,
          {unassigned, duplicate},
          L"WX");
  check(!duplicate_result.has_value(),
        "Two Volume GUIDs for one ESP must fail closed");
}

void test_unassigned_system_volume_plan_skips_all_observed_drive_letters() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto& disk = inventory.disks.front();
  const auto target = unassigned_system_volume(disk, 0U, L"FAT32");
  auto unrelated = unassigned_system_volume(disk, 1U, L"NTFS");
  unrelated.volume_name =
      L"\\\\?\\Volume{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}\\";
  unrelated.mount_points = {L"Y:\\"};

  const auto plan = ytec::bootrepair::plan_temporary_system_volume_mount(
      disk,
      ytec::bootrepair::BcdBootFirmware::uefi,
      {target, unrelated},
      L"WX");

  check(plan.has_value() && plan.value().temporary_root == L"V:\\",
        "Planner must skip drive letters mounted by unrelated volumes");
}

void test_unassigned_bios_system_volume_requires_one_active_ntfs_partition() {
  auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::mbr);
  auto& disk = inventory.disks.front();
  disk.partitions[1].bootable = true;
  const auto first = unassigned_system_volume(disk, 0U, L"NTFS");
  const auto second = unassigned_system_volume(disk, 1U, L"NTFS");

  const auto plan =
      ytec::bootrepair::plan_temporary_system_volume_mount(
          disk,
          ytec::bootrepair::BcdBootFirmware::bios,
          {first, second},
          L"WX");

  check(!plan.has_value(),
        "Multiple BIOS Active partitions must stop before temporary mounting");
}

void test_temporary_system_volume_mount_verifies_and_releases() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto& disk = inventory.disks.front();
  const auto volume = unassigned_system_volume(disk, 0U, L"FAT32");
  const auto plan =
      ytec::bootrepair::plan_temporary_system_volume_mount(
          disk,
          ytec::bootrepair::BcdBootFirmware::uefi,
          {volume},
          L"WX");
  check(plan.has_value(), "Fixture mount plan should succeed");
  MockSystemVolumeMountApi api;
  api.observation = volume;

  auto mounted = ytec::bootrepair::TemporarySystemVolumeMount::acquire(
      plan.value(), api);
  check(mounted.has_value() && mounted.value().mounted(),
        "A matching post-mount observation should acquire the root");
  check(
      api.attach_count == 1 && api.inspect_count == 1 &&
          api.detach_count == 0 &&
          api.inspected_root == plan.value().temporary_root &&
          api.inspected_volume == plan.value().volume_name &&
          api.inspected_location.disk_number ==
              plan.value().expected_location.disk_number &&
          api.inspected_location.starting_offset ==
              plan.value().expected_location.starting_offset,
      "Acquire must attach once, then reidentify before use");
  const auto released = mounted.value().release();
  check(released.has_value() && !mounted.value().mounted(),
        "Explicit release should make cleanup success observable");
  check(
      api.detach_count == 1 &&
          api.detached_root == plan.value().temporary_root &&
          api.detached_volume == plan.value().volume_name,
      "Release must detach only the exact planned root and Volume GUID");
}

void test_temporary_system_volume_mount_mismatch_cleans_up_and_fails() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto& disk = inventory.disks.front();
  const auto volume = unassigned_system_volume(disk, 0U, L"FAT32");
  const auto plan =
      ytec::bootrepair::plan_temporary_system_volume_mount(
          disk,
          ytec::bootrepair::BcdBootFirmware::uefi,
          {volume},
          L"WX");
  check(plan.has_value(), "Fixture mount plan should succeed");
  MockSystemVolumeMountApi api;
  api.observation = volume;
  api.observation.location.disk_number = 8U;

  const auto mounted =
      ytec::bootrepair::TemporarySystemVolumeMount::acquire(
          plan.value(), api);

  check(!mounted.has_value(),
        "A post-mount disk identity change must fail closed");
  check(api.detach_count == 1,
        "A mismatched post-mount observation must be cleaned up immediately");
}

void test_temporary_system_volume_release_failure_is_not_hidden() {
  const auto inventory =
      boot_repair_inventory(ytec::diskmodel::PartitionStyle::gpt);
  const auto& disk = inventory.disks.front();
  const auto volume = unassigned_system_volume(disk, 0U, L"FAT32");
  const auto plan =
      ytec::bootrepair::plan_temporary_system_volume_mount(
          disk,
          ytec::bootrepair::BcdBootFirmware::uefi,
          {volume},
          L"WX");
  check(plan.has_value(), "Fixture mount plan should succeed");
  MockSystemVolumeMountApi api;
  api.observation = volume;
  auto mounted = ytec::bootrepair::TemporarySystemVolumeMount::acquire(
      plan.value(), api);
  check(mounted.has_value(), "Fixture mount should acquire");
  api.detach_fails = true;

  const auto released = mounted.value().release();

  check(!released.has_value() && mounted.value().mounted(),
        "Cleanup failure must fail the operation and retain the mounted state for retry");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"bcdboot_arguments_are_fixed_and_separate",
       test_bcdboot_arguments_are_fixed_and_separate},
      {"bios_arguments_allow_active_windows_partition",
       test_bios_arguments_allow_active_windows_partition},
      {"fresh_store_policy_adds_fixed_c_switch",
       test_fresh_store_policy_adds_fixed_c_switch},
      {"invalid_store_policy_is_rejected",
       test_invalid_store_policy_is_rejected},
      {"bcd_store_path_is_fixed_by_firmware",
       test_bcd_store_path_is_fixed_by_firmware},
      {"windows_bcd_hive_verifier_rejects_missing_objects_and_hardlinks",
       test_windows_bcd_hive_verifier_rejects_missing_objects_and_hardlinks},
      {"fresh_store_transaction_replaces_prior_store",
       test_fresh_store_transaction_replaces_prior_store},
      {"fresh_store_invalid_hive_restores_prior_store",
       test_fresh_store_invalid_hive_restores_prior_store},
      {"fresh_store_failure_restores_prior_store",
       test_fresh_store_failure_restores_prior_store},
      {"fresh_store_missing_output_restores_prior_store",
       test_fresh_store_missing_output_restores_prior_store},
      {"fresh_store_stale_backup_stops_before_process",
       test_fresh_store_stale_backup_stops_before_process},
      {"multi_windows_bcd_transaction_is_one_ordered_commit",
       test_multi_windows_bcd_transaction_is_one_ordered_commit},
      {"multi_windows_failure_rolls_back_the_whole_store",
       test_multi_windows_failure_rolls_back_the_whole_store},
      {"multi_windows_missing_original_creates_verified_store",
       test_multi_windows_missing_original_creates_verified_store},
      {"multi_windows_missing_original_failure_removes_owned_partial",
       test_multi_windows_missing_original_failure_removes_owned_partial},
      {"multi_windows_identity_swap_blocks_partial_cleanup",
       test_multi_windows_identity_swap_blocks_partial_cleanup},
      {"multi_windows_invalid_plan_stops_before_mutation",
       test_multi_windows_invalid_plan_stops_before_mutation},
      {"windows_argument_escaping", test_windows_argument_escaping},
      {"execute_uses_system32_and_signature_gate",
       test_execute_uses_system32_and_signature_gate},
      {"invalid_path_stops_before_signature",
       test_invalid_path_stops_before_signature},
      {"signature_failure_stops_process", test_signature_failure_stops_process},
      {"nonzero_exit_is_not_success", test_nonzero_exit_is_not_success},
      {"default_streamed_runner_delivers_captured_output",
       test_default_streamed_runner_delivers_captured_output},
      {"offline_registry_reader_reads_synthetic_hive",
       test_offline_registry_reader_reads_synthetic_hive},
      {"uefi_standalone_target_is_bound_to_esp_and_windows",
       test_uefi_standalone_target_is_bound_to_esp_and_windows},
      {"uefi_standalone_target_rejects_cross_disk_esp",
       test_uefi_standalone_target_rejects_cross_disk_esp},
      {"uefi_standalone_target_accepts_read_only_unassigned_esp_plan",
       test_uefi_standalone_target_accepts_read_only_unassigned_esp_plan},
      {"bios_standalone_target_requires_one_active_partition",
       test_bios_standalone_target_requires_one_active_partition},
      {"standalone_confirmation_is_target_specific",
       test_standalone_confirmation_is_target_specific},
      {"uefi_efi_ownership_is_rechecked_and_nvram_stays_disabled",
       test_uefi_efi_ownership_is_rechecked_and_nvram_stays_disabled},
      {"unassigned_uefi_system_volume_plan_is_exact_and_read_only",
       test_unassigned_uefi_system_volume_plan_is_exact_and_read_only},
      {"unassigned_system_volume_plan_rejects_existing_mount_or_ambiguity",
       test_unassigned_system_volume_plan_rejects_existing_mount_or_ambiguity},
      {"unassigned_system_volume_plan_skips_all_observed_drive_letters",
       test_unassigned_system_volume_plan_skips_all_observed_drive_letters},
      {"unassigned_bios_system_volume_requires_one_active_ntfs_partition",
       test_unassigned_bios_system_volume_requires_one_active_ntfs_partition},
      {"temporary_system_volume_mount_verifies_and_releases",
       test_temporary_system_volume_mount_verifies_and_releases},
      {"temporary_system_volume_mount_mismatch_cleans_up_and_fails",
       test_temporary_system_volume_mount_mismatch_cleans_up_and_fails},
      {"temporary_system_volume_release_failure_is_not_hidden",
       test_temporary_system_volume_release_failure_is_not_hidden},
      {"offline_windows_version_gate", test_offline_windows_version_gate},
      {"offline_windows_volume_root_is_strictly_bounded",
       test_offline_windows_volume_root_is_strictly_bounded},
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

#include "ytec/imageformat/partition_snapshot.h"
#include "ytec/imageformat/tsumugi.h"
#include "ytec/imageformat/tsumugi_manifest.h"
#include "ytec/imageformat/tsumugi_stream.h"
#include "ytec/windowsapp/online_image_create.h"
#include "ytec/windowsapp/restore_preflight.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kDiskBytes = 4ULL * kMiB;
constexpr std::uint64_t kPartitionOffset = 1ULL * kMiB;
constexpr std::uint64_t kPartitionBytes = 3ULL * kMiB;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::vector<std::byte> mbr_snapshot() {
  ytec::imageformat::PartitionSnapshot snapshot{
      .style = ytec::imageformat::PartitionTableStyle::mbr,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
  };
  ytec::imageformat::PartitionTableRegion region;
  region.disk_offset = 0U;
  region.data.assign(512U, std::byte{0});
  region.data[446U + 4U] = std::byte{0x07};
  region.data[510U] = std::byte{0x55};
  region.data[511U] = std::byte{0xAA};
  snapshot.regions.push_back(std::move(region));
  auto encoded = ytec::imageformat::build_partition_snapshot_v1(snapshot);
  check(encoded.has_value(), "Synthetic MBR snapshot should build");
  return encoded.take_value();
}

ytec::imageformat::TsumugiManifest exact_manifest() {
  using namespace ytec::imageformat;
  TsumugiManifest manifest{
      .mode = TsumugiManifestMode::exact,
      .partition_style = TsumugiManifestPartitionStyle::mbr,
      .flags = TsumugiManifestFlags::automatic_surplus_allocation,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .created_utc = "2026-08-04T20:00:00Z",
      .app_version = "1.0.0",
      .partition_snapshot = mbr_snapshot(),
  };
  manifest.source_model_hash[0] = std::byte{0x11};
  manifest.source_serial_hash[0] = std::byte{0x22};
  manifest.source_state_hash[0] = std::byte{0x33};
  TsumugiManifestPartition partition{
      .source_table_index = 1U,
      .source_partition_number = 1U,
      .role = TsumugiManifestPartitionRole::data,
      .file_system = TsumugiManifestFileSystem::ntfs,
      .flags = TsumugiManifestPartitionFlags::selected |
          TsumugiManifestPartitionFlags::required,
      .source_offset = kPartitionOffset,
      .source_size = kPartitionBytes,
      .used_bytes = 64U * 1024U,
      .minimum_target_bytes = kPartitionBytes,
      .planned_target_bytes = kPartitionBytes,
      .payload_logical_offset = kPartitionOffset,
      .payload_logical_length = kPartitionBytes,
      .name_utf8 = "Data",
      .label_utf8 = "D",
  };
  partition.type_id[0] = std::byte{0x07};
  manifest.partitions.push_back(std::move(partition));
  return manifest;
}

std::vector<std::byte> exact_image(const bool encrypted = false) {
  auto manifest = ytec::imageformat::build_tsumugi_manifest_v1(
      exact_manifest());
  check(manifest.has_value(), "Synthetic Tsumugi manifest should build");
  ytec::imageformat::TsumugiBuildRequest request{
      .payload_kind = ytec::imageformat::TsumugiPayloadKind::exact_disk,
      .source_disk_size = kDiskBytes,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .chunk_size = ytec::imageformat::kImageChunkSize16MiB,
      .compression = ytec::imageformat::ImageCompression::none,
      .manifest = manifest.take_value(),
  };
  request.image_id[0] = std::byte{0x51};
  request.chunks.push_back(ytec::imageformat::TsumugiBuildChunk{
      .logical_offset = kPartitionOffset,
      .logical_length = kPartitionBytes,
      .data = std::vector<std::byte>(
          static_cast<std::size_t>(kPartitionBytes), std::byte{0x5A}),
  });
  if (encrypted) {
    ytec::imageformat::TsumugiEncryptionSettings settings{};
    settings.password = "Synthetic password 42!";
    for (std::size_t index = 0U;
         index < settings.argon2.salt.size();
         ++index) {
      settings.argon2.salt[index] =
          static_cast<std::byte>(0x20U + index);
    }
    for (std::size_t index = 0U;
         index < settings.base_nonce.size();
         ++index) {
      settings.base_nonce[index] =
          static_cast<std::byte>(0x50U + index);
    }
    request.encryption = settings;
  }
  auto built = ytec::imageformat::build_tsumugi_v1(request);
  check(built.has_value(), "Synthetic .tsumugi should build");
  return built.take_value();
}

class TemporaryTsumugiFile final {
 public:
  explicit TemporaryTsumugiFile(const std::span<const std::byte> bytes) {
    std::vector<wchar_t> directory(MAX_PATH + 1U, L'\0');
    const DWORD length = GetTempPathW(
        static_cast<DWORD>(directory.size()), directory.data());
    check(length != 0U && length < directory.size(),
          "Temporary directory should be available");
    path_.assign(directory.data(), length);
    path_ += L"Ytec-Tsumugi-preflight-";
    path_ += std::to_wstring(GetCurrentProcessId());
    path_ += L"-";
    path_ += std::to_wstring(GetTickCount64());
    path_ += L".tsumugi";
    const HANDLE file = CreateFileW(
        path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("Temporary image should open");
    }
    std::size_t offset{};
    while (offset < bytes.size()) {
      const DWORD request = static_cast<DWORD>((std::min)(
          bytes.size() - offset,
          static_cast<std::size_t>(1024U * 1024U)));
      DWORD written{};
      const BOOL ok = WriteFile(
          file, bytes.data() + offset, request, &written, nullptr);
      if (ok == FALSE || written != request) {
        CloseHandle(file);
        DeleteFileW(path_.c_str());
        throw std::runtime_error("Temporary image write failed");
      }
      offset += written;
    }
    if (CloseHandle(file) == FALSE) {
      DeleteFileW(path_.c_str());
      throw std::runtime_error("Temporary image should close");
    }
  }

  ~TemporaryTsumugiFile() {
    if (!path_.empty()) {
      DeleteFileW(path_.c_str());
    }
  }

  TemporaryTsumugiFile(const TemporaryTsumugiFile&) = delete;
  TemporaryTsumugiFile& operator=(const TemporaryTsumugiFile&) = delete;

  [[nodiscard]] const std::wstring& path() const noexcept { return path_; }

 private:
  std::wstring path_;
};

ytec::diskmodel::DiskInfo compatible_target() {
  return ytec::diskmodel::DiskInfo{
      .disk_number = 2U,
      .device_path = L"\\\\.\\PhysicalDrive2",
      .device_instance_id = L"VIRTUAL\\TSUMUGI\\TARGET2",
      .model = L"TSUMUGI TARGET",
      .size_bytes = 8ULL * kMiB,
      .sector_count = 16'384U,
      .logical_sector_size = 512U,
      .physical_sector_size = 4096U,
      .bus_type = L"SATA",
      .serial_suffix = "TARGET02",
      .partition_style = ytec::diskmodel::PartitionStyle::gpt,
      .offline = false,
      .read_only = false,
      .removable = false,
      .is_system_disk = false,
  };
}

void complete_file_preflight_accepts_only_tsumugi() {
  const TemporaryTsumugiFile file(exact_image());
  const auto report =
      ytec::windowsapp::inspect_tsumugi_restore_image_file(file.path());
  check(report.has_value(), "Valid .tsumugi should verify completely");
  check(report.value().complete_container_verified &&
            report.value().metadata_verified &&
            report.value().restore_layout_verified &&
            !report.value().restore_execution_enabled,
        "Preflight should remain read-only after every verification gate");
  check(report.value().manifest.partitions.size() == 1U &&
            report.value().header.chunk_count == 1U &&
            report.value().image_length != 0U,
        "Verified typed metadata should be returned");

  const auto wrong = ytec::windowsapp::inspect_tsumugi_restore_image_file(
      file.path() + L".dcimg");
  check(!wrong.has_value() &&
            wrong.error().operation == L"Tsumugi復元イメージパス",
        "Legacy/wrong extensions must stop before file open");
}

void cancellation_stops_before_file_access() {
  const auto result = ytec::windowsapp::inspect_tsumugi_restore_image_file(
      L"C:\\does-not-exist\\cancelled.tsumugi",
      ytec::windowsapp::TsumugiRestoreImagePreflightOptions{
          .cancellation_requested = [] { return true; },
      });
  check(!result.has_value() && result.error().native_code == ERROR_CANCELLED,
        "Cancellation should win before path or file access");
}

void bounded_header_probe_drives_password_prompt_only() {
  {
    const TemporaryTsumugiFile plain(exact_image());
    const auto probe =
        ytec::imageformat::probe_tsumugi_file_header_v1(plain.path());
    check(probe.has_value() && !probe.value().encrypted,
          "Plain image probe should not request a password");
  }
  {
    const TemporaryTsumugiFile encrypted(exact_image(true));
    const auto probe =
        ytec::imageformat::probe_tsumugi_file_header_v1(encrypted.path());
    check(probe.has_value() && probe.value().encrypted,
          "Encrypted image probe should request a password");
  }
  {
    const std::vector<std::byte> invalid(600U, std::byte{0});
    const TemporaryTsumugiFile broken(invalid);
    const auto probe =
        ytec::imageformat::probe_tsumugi_file_header_v1(broken.path());
    check(!probe.has_value(),
          "Invalid fixed header must fail before full verification");
  }
}

void password_prompt_flow_fails_closed_on_probe_or_cancel() {
  using Decision =
      ytec::windowsapp::TsumugiRestorePasswordPromptDecision;
  using State = ytec::windowsapp::TsumugiRestorePasswordPromptState;

  check(ytec::windowsapp::evaluate_tsumugi_restore_password_prompt(
            State{.header_probe_succeeded = false}) == Decision::stop,
        "Probe failure must stop before a password dialog or verification");
  check(ytec::windowsapp::evaluate_tsumugi_restore_password_prompt(
            State{.header_probe_succeeded = true, .encrypted = false}) ==
            Decision::no_password_required,
        "Plain images must proceed without a password dialog");
  check(ytec::windowsapp::evaluate_tsumugi_restore_password_prompt(
            State{.header_probe_succeeded = true, .encrypted = true}) ==
            Decision::prompt_required,
        "Encrypted images must request a password");
  check(ytec::windowsapp::evaluate_tsumugi_restore_password_prompt(State{
            .header_probe_succeeded = true,
            .encrypted = true,
            .prompt_accepted = false,
        }) == Decision::stop,
        "Password prompt cancellation must stop the flow");
  check(ytec::windowsapp::evaluate_tsumugi_restore_password_prompt(State{
            .header_probe_succeeded = true,
            .encrypted = true,
            .prompt_accepted = true,
            .password_available = false,
        }) == Decision::stop,
        "An accepted encrypted prompt without a password must stop");
  check(ytec::windowsapp::evaluate_tsumugi_restore_password_prompt(State{
            .header_probe_succeeded = true,
            .encrypted = true,
            .prompt_accepted = true,
            .password_available = true,
        }) == Decision::password_ready,
        "An accepted encrypted password may enter complete verification");
}

void image_storage_disk_is_rejected_before_confirmation() {
  const auto storage_disk = compatible_target();
  const auto storage_identity =
      ytec::diskmodel::make_stable_disk_identity(storage_disk, false);
  check(storage_identity.has_value(),
        "Synthetic image storage identity should be stable");

  const auto same_disk = ytec::windowsapp::
      validate_tsumugi_restore_storage_target_separation(
          storage_identity.value(), storage_identity.value());
  check(!same_disk.has_value() &&
            same_disk.error().native_code == ERROR_INVALID_DRIVE,
        "The disk backing the image must be rejected before confirmation");

  auto distinct_disk = compatible_target();
  distinct_disk.disk_number = 3U;
  distinct_disk.serial_suffix = "TARGET03";
  distinct_disk.device_instance_id = L"VIRTUAL\\TSUMUGI\\TARGET3";
  const auto distinct_identity =
      ytec::diskmodel::make_stable_disk_identity(distinct_disk, false);
  check(distinct_identity.has_value() &&
            ytec::windowsapp::
                validate_tsumugi_restore_storage_target_separation(
                    storage_identity.value(), distinct_identity.value())
                .has_value(),
        "A stably identified distinct target should remain eligible");

  auto unstable_identity = distinct_identity.value();
  unstable_identity.serial_suffix.clear();
  unstable_identity.device_instance_id.clear();
  check(!ytec::windowsapp::
             validate_tsumugi_restore_storage_target_separation(
                 storage_identity.value(), unstable_identity)
             .has_value(),
        "An unstable target identity must fail closed");
}

void target_selection_accepts_existing_basic_layout_read_only() {
  ytec::windowsapp::TsumugiRestoreImagePreflightReport image{
      .header = ytec::imageformat::TsumugiHeader{
          .source_disk_size = kDiskBytes,
          .logical_sector_size = 512U,
          .physical_sector_size = 4096U,
      },
      .manifest = exact_manifest(),
      .complete_container_verified = true,
      .metadata_verified = true,
      .restore_layout_verified = true,
  };
  ytec::diskmodel::InventoryReport inventory;
  inventory.disks.push_back(compatible_target());
  const auto ready =
      ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
          &image, &inventory, 0U, false);
  check(ready.ready_for_confirmation &&
            !ready.restore_execution_enabled &&
            ready.target_identity.has_value(),
        "Existing GPT/MBR targets should be reviewable before later erase");

  inventory.disks[0].is_system_disk = true;
  check(ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
            &image, &inventory, 0U, false).issue ==
            ytec::windowsapp::RestoreTargetSelectionIssue::target_is_system,
        "Running Windows target must fail closed");

  inventory.disks[0] = compatible_target();
  inventory.disks[0].size_bytes = 2ULL * kMiB;
  check(ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
            &image, &inventory, 0U, false).issue ==
            ytec::windowsapp::RestoreTargetSelectionIssue::target_too_small,
        "Exact restore must reject a smaller target");

  inventory.disks[0] = compatible_target();
  inventory.disks[0].logical_sector_size = 4096U;
  check(ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
            &image, &inventory, 0U, false).issue ==
            ytec::windowsapp::RestoreTargetSelectionIssue::
                logical_sector_mismatch,
        "Unsupported sector conversion must fail closed");

  inventory.disks[0] = compatible_target();
  inventory.disks[0].health.state =
      ytec::diskmodel::DiskHealthState::caution;
  check(ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
            &image, &inventory, 0U, false).issue ==
            ytec::windowsapp::RestoreTargetSelectionIssue::
                target_health_abnormal,
        "A SMART/NVMe caution target must fail closed");

  inventory.disks[0] = compatible_target();
  const auto model_hash = ytec::windowsapp::hash_tsumugi_source_model(
      inventory.disks[0].model);
  const auto serial_hash = ytec::windowsapp::hash_tsumugi_source_serial(
      inventory.disks[0].serial_suffix,
      inventory.disks[0].device_instance_id);
  check(model_hash.has_value() && serial_hash.has_value(),
        "Target privacy hashes should build");
  image.manifest.source_model_hash = model_hash.value();
  image.manifest.source_serial_hash = serial_hash.value();
  check(ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
            &image, &inventory, 0U, false).issue ==
            ytec::windowsapp::RestoreTargetSelectionIssue::
                target_is_original_source,
        "The original source identity hashes must be rejected");
}

void individual_target_accepts_smaller_disk_when_partition_is_exact() {
  ytec::windowsapp::TsumugiRestoreImagePreflightReport image{
      .header = ytec::imageformat::TsumugiHeader{
          .source_disk_size = kDiskBytes,
          .logical_sector_size = 512U,
          .physical_sector_size = 4096U,
      },
      .manifest = exact_manifest(),
      .complete_container_verified = true,
      .metadata_verified = true,
      .restore_layout_verified = true,
  };
  ytec::diskmodel::InventoryReport inventory;
  auto target = compatible_target();
  target.size_bytes = kPartitionOffset + kPartitionBytes;
  target.sector_count = target.size_bytes / target.logical_sector_size;
  target.partitions.push_back({
      .number = 4U,
      .offset_bytes = kPartitionOffset,
      .size_bytes = kPartitionBytes,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
  });
  inventory.disks.push_back(target);
  ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection selection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalExistingPartitionRestoreSelection{
                  .target_table_index = 1U,
                  .target_partition_number = 4U,
                  .target_offset = kPartitionOffset,
                  .target_size = kPartitionBytes,
              },
      };
  const auto ready =
      ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
          &image, &inventory, 0U, false, selection);
  check(ready.ready_for_confirmation &&
            ready.issue == ytec::windowsapp::
                RestoreTargetSelectionIssue::ready_for_confirmation,
        "individual restore must use selected-partition size instead of whole disk size");

  std::get<ytec::imageformat::
      TsumugiPhysicalExistingPartitionRestoreSelection>(selection.target)
      .target_offset += 512U;
  const auto drifted =
      ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
          &image, &inventory, 0U, false, selection);
  check(drifted.issue == ytec::windowsapp::
            RestoreTargetSelectionIssue::individual_selection_invalid,
        "individual geometry drift must remain disabled at read-only review");
}

void unallocated_target_is_ready_only_for_same_style_exact_gap() {
  ytec::windowsapp::TsumugiRestoreImagePreflightReport image{
      .header = ytec::imageformat::TsumugiHeader{
          .source_disk_size = kDiskBytes,
          .logical_sector_size = 512U,
          .physical_sector_size = 4096U,
      },
      .manifest = exact_manifest(),
      .complete_container_verified = true,
      .metadata_verified = true,
      .restore_layout_verified = true,
  };
  ytec::diskmodel::InventoryReport inventory;
  auto target = compatible_target();
  target.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  inventory.disks.push_back(target);
  ytec::imageformat::
      TsumugiPhysicalIndividualPartitionRestoreSelection selection{
          .source_table_index = 1U,
          .target = ytec::imageformat::
              TsumugiPhysicalUnallocatedRestoreSelection{
                  .target_offset = 4ULL * kMiB,
                  .target_size = kPartitionBytes,
              },
      };
  const auto ready =
      ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
          &image, &inventory, 0U, false, selection);
  check(ready.ready_for_confirmation &&
            ready.issue == ytec::windowsapp::
                RestoreTargetSelectionIssue::ready_for_confirmation,
        "same-style exact unallocated gap must reach final confirmation");

  inventory.disks[0].partition_style =
      ytec::diskmodel::PartitionStyle::gpt;
  const auto changed_style =
      ytec::windowsapp::evaluate_tsumugi_restore_target_selection(
          &image, &inventory, 0U, false, selection);
  check(changed_style.issue == ytec::windowsapp::
            RestoreTargetSelectionIssue::individual_selection_invalid,
        "partition-style conversion must remain blocked for unallocated restore");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"complete_file_preflight_accepts_only_tsumugi",
       complete_file_preflight_accepts_only_tsumugi},
      {"cancellation_stops_before_file_access",
       cancellation_stops_before_file_access},
      {"bounded_header_probe_drives_password_prompt_only",
       bounded_header_probe_drives_password_prompt_only},
      {"password_prompt_flow_fails_closed_on_probe_or_cancel",
       password_prompt_flow_fails_closed_on_probe_or_cancel},
      {"image_storage_disk_is_rejected_before_confirmation",
       image_storage_disk_is_rejected_before_confirmation},
      {"target_selection_accepts_existing_basic_layout_read_only",
       target_selection_accepts_existing_basic_layout_read_only},
      {"individual_target_accepts_smaller_disk_when_partition_is_exact",
       individual_target_accepts_smaller_disk_when_partition_is_exact},
      {"unallocated_target_is_ready_only_for_same_style_exact_gap",
       unallocated_target_is_ready_only_for_same_style_exact_gap},
  };
  int failures{};
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

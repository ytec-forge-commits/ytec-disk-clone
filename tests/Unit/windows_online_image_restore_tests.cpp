#include "ytec/windowsapp/online_image_create.h"
#include "ytec/windowsapp/online_image_restore.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kDiskSize = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kSectorSize = 512U;

void check(const bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

ytec::clonecore::Error injected_error(std::wstring operation) {
  return ytec::clonecore::Error{
      .code = ytec::clonecore::ErrorCode::io_failed,
      .native_code = ERROR_WRITE_FAULT,
      .operation = std::move(operation),
      .message = L"合成失敗です",
  };
}

class MemoryWriter final : public ytec::clonecore::ITargetDiskWriter {
 public:
  MemoryWriter()
      : bytes_(static_cast<std::size_t>(kDiskSize), std::byte{0}) {}

  [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
    return bytes_.size();
  }

  [[nodiscard]] std::uint32_t logical_sector_size() const noexcept override {
    return kSectorSize;
  }

  [[nodiscard]] ytec::clonecore::Status write_target(
      const std::uint64_t offset,
      const std::span<const std::byte> bytes) override {
    if (offset > bytes_.size() || bytes.size() > bytes_.size() - offset) {
      return ytec::clonecore::Status::failure(
          injected_error(L"合成復元先書込み"));
    }
    std::copy(
        bytes.begin(),
        bytes.end(),
        bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    return ytec::clonecore::success_status();
  }

  [[nodiscard]] ytec::clonecore::Result<std::vector<std::byte>> read_back(
      const std::uint64_t offset,
      const std::size_t length) const override {
    if (offset > bytes_.size() || length > bytes_.size() - offset) {
      return ytec::clonecore::Result<std::vector<std::byte>>::failure(
          injected_error(L"合成復元先読戻し"));
    }
    return ytec::clonecore::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
            bytes_.begin() + static_cast<std::ptrdiff_t>(offset + length)));
  }

  [[nodiscard]] ytec::clonecore::Status flush_target() override {
    return ytec::clonecore::success_status();
  }

 private:
  std::vector<std::byte> bytes_;
};

ytec::diskmodel::DiskInfo target_disk(
    std::wstring bus = L"SATA",
    const bool removable = false) {
  return ytec::diskmodel::DiskInfo{
      .disk_number = 4U,
      .device_path = L"\\\\.\\PhysicalDrive4",
      .device_instance_id = L"SYNTHETIC\\TARGET-4",
      .model = L"Synthetic Target SSD",
      .size_bytes = kDiskSize,
      .sector_count = kDiskSize / kSectorSize,
      .logical_sector_size = kSectorSize,
      .physical_sector_size = 4096U,
      .bus_type = std::move(bus),
      .serial_suffix = "T004",
      .partition_style = ytec::diskmodel::PartitionStyle::raw,
      .offline = false,
      .read_only = false,
      .removable = removable,
  };
}

ytec::clonecore::StableDiskIdentity stable(
    const ytec::diskmodel::DiskInfo& disk) {
  const auto identity = ytec::diskmodel::make_stable_disk_identity(
      disk, disk.is_system_disk);
  check(identity.has_value(), "synthetic target identity must build");
  return identity.value();
}

ytec::imageformat::TsumugiVerifiedImage verified_image(
    const ytec::imageformat::TsumugiManifestPartitionStyle style =
        ytec::imageformat::TsumugiManifestPartitionStyle::gpt) {
  ytec::imageformat::TsumugiVerifiedImage image;
  image.manifest.mode = ytec::imageformat::TsumugiManifestMode::exact;
  image.manifest.partition_style = style;
  image.manifest.source_disk_size = 8ULL * 1024ULL * 1024ULL;
  image.manifest.logical_sector_size = kSectorSize;
  image.manifest.physical_sector_size = 4096U;
  const auto model = ytec::windowsapp::hash_tsumugi_source_model(
      L"Synthetic Source SSD");
  const auto serial = ytec::windowsapp::hash_tsumugi_source_serial(
      "S001", L"SYNTHETIC\\SOURCE-1");
  check(model.has_value() && serial.has_value(), "source hashes must build");
  image.manifest.source_model_hash = model.value();
  image.manifest.source_serial_hash = serial.value();
  image.manifest.source_state_hash.fill(std::byte{0x31});
  image.container.global_hash.fill(std::byte{0x61});
  return image;
}

ytec::imageformat::TsumugiRestoreReport successful_engine_report() {
  return ytec::imageformat::TsumugiRestoreReport{
      .written_logical_bytes = 4096U,
      .written_chunk_count = 1U,
      .callbacks_started_after_complete_verification = true,
      .image_matched_prepared_plan = true,
      .target_reidentified_before_write = true,
      .all_writes_read_back_verified = true,
      .final_layout_committed = true,
  };
}

struct Fixture final {
  ytec::diskmodel::DiskInfo initial = target_disk();
  ytec::diskmodel::DiskInfo opened = initial;
  ytec::imageformat::TsumugiVerifiedImage image = verified_image();
  std::vector<std::string> events;
  std::vector<bool> offline_requests;
  bool engine_called{};
  bool connection_token_called{};
  bool signature_collector_called{};
  bool engine_fails{};
  bool omit_final_proof{};
  bool engine_saw_usb_token{};
  std::vector<std::uint32_t> engine_signatures;
  std::size_t reidentify_calls{};

  ytec::windowsapp::OnlineImageRestoreRequest request() const {
    const auto layout =
        ytec::windowsapp::hash_online_image_restore_target_layout(initial);
    check(layout.has_value(), "synthetic target layout hash must build");
    return ytec::windowsapp::OnlineImageRestoreRequest{
        .image = {
            .image_path = L"D:\\synthetic.tsumugi",
            .storage_file_system =
                ytec::imageformat::TsumugiImageStorageFileSystem::ntfs,
        },
        .expected_image_global_hash = image.container.global_hash,
        .expected_source_state_hash = image.manifest.source_state_hash,
        .expected_target = stable(initial),
        .expected_target_layout_hash = layout.value(),
        .confirmation = {
            .first_step_acknowledged = true,
            .typed_token = L"OK",
        },
        .administrator = true,
    };
  }

  ytec::windowsapp::OnlineImageRestoreDependencies dependencies() {
    return {
        .verify_image =
            [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
                const ytec::clonecore::DiskOperationCallbacks&) {
              events.push_back("verify");
              return ytec::clonecore::Result<
                  ytec::imageformat::TsumugiVerifiedImage>::success(image);
            },
        .reidentify_target =
            [&](const ytec::clonecore::StableDiskIdentity&,
                const ytec::clonecore::TargetConfirmation&) {
              events.push_back("reidentify");
              ++reidentify_calls;
              const auto& current = reidentify_calls == 1U ? initial : opened;
              return ytec::clonecore::Result<
                  ytec::diskmodel::ReidentifiedPhysicalTarget>::success({
                  .target = current,
                  .target_identity = stable(current),
              });
            },
        .set_target_offline =
            [&](const ytec::clonecore::StableDiskIdentity&,
                const ytec::clonecore::TargetConfirmation&,
                const bool offline) {
              events.push_back("offline");
              offline_requests.push_back(offline);
              return ytec::clonecore::success_status();
            },
        .open_offline_target =
            [&](const ytec::clonecore::StableDiskIdentity&,
                const ytec::clonecore::TargetConfirmation&) {
              events.push_back("open");
              opened.offline = true;
              std::unique_ptr<ytec::clonecore::ITargetDiskWriter> writer =
                  std::make_unique<MemoryWriter>();
              return ytec::clonecore::Result<
                  ytec::diskmodel::PhysicalTargetHandle>::success({
                  .observed = {
                      .target = opened,
                      .target_identity = stable(opened),
                  },
                  .target = std::move(writer),
              });
            },
        .collect_mbr_signatures =
            [&](const ytec::clonecore::StableDiskIdentity&) {
              events.push_back("signatures");
              signature_collector_called = true;
              return ytec::clonecore::Result<
                  std::vector<std::uint32_t>>::success(
                  {0x1234ABCDU, 0x87654321U});
            },
        .make_connection_token = [&] {
          events.push_back("connection-token");
          connection_token_called = true;
          ytec::imageformat::Sha256Digest token{};
          token.fill(std::byte{0x7A});
          return ytec::clonecore::Result<
              ytec::imageformat::Sha256Digest>::success(token);
        },
        .execute_engine =
            [&](const ytec::imageformat::TsumugiImageVerifyRequest&,
                const ytec::imageformat::TsumugiVerifiedImage&,
                const ytec::imageformat::TsumugiRestoreDiskIdentity& identity,
                ytec::diskmodel::PhysicalTargetHandle handle,
                const std::span<const std::uint32_t> signatures,
                const ytec::imageformat::
                    TsumugiPhysicalRestoreLockedTargetRevalidator&
                        revalidate_locked_target,
                const ytec::clonecore::DiskOperationCallbacks&) {
              events.push_back("engine");
              engine_called = true;
              engine_saw_usb_token = identity.is_usb_attached &&
                  std::any_of(
                      identity.connection_instance_hash.begin(),
                      identity.connection_instance_hash.end(),
                      [](const std::byte value) {
                        return value != std::byte{0};
                      });
              engine_signatures.assign(signatures.begin(), signatures.end());
              check(handle.target != nullptr, "engine must own exact target handle");
              auto revalidated = revalidate_locked_target();
              if (!revalidated) {
                return ytec::clonecore::Result<
                    ytec::imageformat::TsumugiRestoreReport>::failure(
                    revalidated.error());
              }
              if (engine_fails) {
                return ytec::clonecore::Result<
                    ytec::imageformat::TsumugiRestoreReport>::failure(
                    injected_error(L"合成復元Engine"));
              }
              auto report = successful_engine_report();
              if (omit_final_proof) {
                report.final_layout_committed = false;
              }
              return ytec::clonecore::Result<
                  ytec::imageformat::TsumugiRestoreReport>::success(report);
            },
    };
  }
};

void test_happy_path_verifies_before_offline_and_leaves_offline() {
  Fixture fixture;
  const auto result = ytec::windowsapp::execute_online_image_restore(
      fixture.request(), fixture.dependencies());
  check(result.has_value(), "safe exact restore controller path must succeed");
  check(
      fixture.events == std::vector<std::string>{
          "verify", "reidentify", "offline", "open", "engine",
          "reidentify", "offline"},
      "image verification must finish before target offline/open/engine");
  check(
      fixture.offline_requests == std::vector<bool>{true, true} &&
          result.value().target_left_offline &&
          result.value().target_handle_reidentified,
      "successful restore must verify and preserve offline state");
  check(
      !fixture.connection_token_called && !fixture.signature_collector_called,
      "SATA/GPT restore must not create USB epoch or inspect MBR signatures");
}

void test_original_source_is_rejected_before_offline() {
  Fixture fixture;
  const auto model = ytec::windowsapp::hash_tsumugi_source_model(
      fixture.initial.model);
  const auto serial = ytec::windowsapp::hash_tsumugi_source_serial(
      fixture.initial.serial_suffix, fixture.initial.device_instance_id);
  fixture.image.manifest.source_model_hash = model.value();
  fixture.image.manifest.source_serial_hash = serial.value();
  const auto result = ytec::windowsapp::execute_online_image_restore(
      fixture.request(), fixture.dependencies());
  check(
      !result.has_value() && fixture.offline_requests.empty() &&
          !fixture.engine_called,
      "the image's original source disk must never reach target mutation");
}

void test_image_replacement_after_review_is_rejected() {
  Fixture fixture;
  auto request = fixture.request();
  fixture.image.container.global_hash[0] ^= std::byte{0x01};
  const auto replaced = ytec::windowsapp::execute_online_image_restore(
      request, fixture.dependencies());
  check(
      !replaced.has_value() && fixture.offline_requests.empty() &&
          !fixture.engine_called,
      "an image replaced after final review must fail before target mutation");

  fixture = Fixture{};
  request = fixture.request();
  fixture.image.manifest.source_state_hash[0] ^= std::byte{0x01};
  const auto source_drift = ytec::windowsapp::execute_online_image_restore(
      request, fixture.dependencies());
  check(
      !source_drift.has_value() && fixture.offline_requests.empty() &&
          !fixture.engine_called,
      "source-state drift at the same path must fail before target mutation");
}

void test_unsupported_target_classes_fail_closed() {
  for (const std::wstring bus :
       {L"Storage Spaces", L"RAID", L"Virtual", L"Unknown(99)"}) {
    Fixture fixture;
    fixture.initial.bus_type = bus;
    fixture.opened = fixture.initial;
    const auto result = ytec::windowsapp::execute_online_image_restore(
        fixture.request(), fixture.dependencies());
    check(
        !result.has_value() && fixture.offline_requests.empty(),
        "unsupported target bus must fail before offline transition");
  }

  Fixture dynamic;
  dynamic.initial.partition_style = ytec::diskmodel::PartitionStyle::mbr;
  dynamic.initial.partitions.push_back({
      .number = 1U,
      .offset_bytes = 1024U * 1024U,
      .size_bytes = 4U * 1024U * 1024U,
      .style = ytec::diskmodel::PartitionStyle::mbr,
      .type = L"0x42",
  });
  dynamic.opened = dynamic.initial;
  const auto result = ytec::windowsapp::execute_online_image_restore(
      dynamic.request(), dynamic.dependencies());
  check(
      !result.has_value() && dynamic.offline_requests.empty(),
      "Dynamic Disk marker must fail before target mutation");
}

void test_usb_disk_gets_connection_epoch_but_usb_memory_is_rejected() {
  Fixture usb_disk;
  usb_disk.initial.bus_type = L"USB";
  usb_disk.opened = usb_disk.initial;
  const auto restored = ytec::windowsapp::execute_online_image_restore(
      usb_disk.request(), usb_disk.dependencies());
  check(
      restored.has_value() && usb_disk.connection_token_called &&
          usb_disk.engine_saw_usb_token,
      "USB disk must bind the restore plan to a nonzero connection epoch");

  Fixture usb_memory;
  usb_memory.initial.bus_type = L"USB";
  usb_memory.initial.removable = true;
  usb_memory.opened = usb_memory.initial;
  const auto rejected = ytec::windowsapp::execute_online_image_restore(
      usb_memory.request(), usb_memory.dependencies());
  check(
      !rejected.has_value() && !usb_memory.connection_token_called &&
          usb_memory.offline_requests.empty(),
      "USB memory must be rejected before generating an execution epoch");
}

void test_engine_failure_or_missing_proof_stays_offline() {
  Fixture failure;
  failure.engine_fails = true;
  const auto failed = ytec::windowsapp::execute_online_image_restore(
      failure.request(), failure.dependencies());
  check(
      !failed.has_value() &&
          failure.offline_requests == std::vector<bool>{true, true},
      "engine failure must reassert offline state");

  Fixture incomplete;
  incomplete.omit_final_proof = true;
  const auto rejected = ytec::windowsapp::execute_online_image_restore(
      incomplete.request(), incomplete.dependencies());
  check(
      !rejected.has_value() &&
          incomplete.offline_requests == std::vector<bool>{true, true},
      "missing final commit evidence must not be reported as success");
}

void test_opened_target_class_drift_stops_before_engine() {
  Fixture fixture;
  fixture.opened.bus_type = L"Storage Spaces";
  const auto result = ytec::windowsapp::execute_online_image_restore(
      fixture.request(), fixture.dependencies());
  check(
      !result.has_value() && !fixture.engine_called &&
          fixture.offline_requests == std::vector<bool>{true, true},
      "classification drift at the exact opened handle must stop the engine");
}

void test_target_layout_drift_stops_before_or_after_offline() {
  Fixture before;
  auto request = before.request();
  before.initial.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  before.initial.partitions.push_back({
      .number = 1U,
      .offset_bytes = 1024U * 1024U,
      .size_bytes = 4U * 1024U * 1024U,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
  });
  before.opened = before.initial;
  const auto rejected_before =
      ytec::windowsapp::execute_online_image_restore(
          request, before.dependencies());
  check(
      !rejected_before.has_value() && before.offline_requests.empty() &&
          !before.engine_called,
      "layout drift after review must stop before taking the target offline");

  Fixture after;
  request = after.request();
  after.opened.partition_style = ytec::diskmodel::PartitionStyle::gpt;
  after.opened.partitions.push_back({
      .number = 1U,
      .offset_bytes = 1024U * 1024U,
      .size_bytes = 4U * 1024U * 1024U,
      .style = ytec::diskmodel::PartitionStyle::gpt,
      .type = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}",
  });
  const auto rejected_after =
      ytec::windowsapp::execute_online_image_restore(
          request, after.dependencies());
  check(
      !rejected_after.has_value() && !after.engine_called &&
          after.offline_requests == std::vector<bool>{true, true},
      "layout drift on the exact opened handle must keep the target offline");
}

void test_mbr_collects_connected_signatures_before_offline() {
  Fixture fixture;
  fixture.image = verified_image(
      ytec::imageformat::TsumugiManifestPartitionStyle::mbr);
  const auto result = ytec::windowsapp::execute_online_image_restore(
      fixture.request(), fixture.dependencies());
  check(
      result.has_value() && fixture.signature_collector_called &&
          fixture.engine_signatures ==
              std::vector<std::uint32_t>{0x1234ABCDU, 0x87654321U} &&
          fixture.events[2] == "signatures" && fixture.events[3] == "offline",
      "MBR collision signatures must be collected before destructive state change");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"happy_path_verifies_before_offline_and_leaves_offline",
       test_happy_path_verifies_before_offline_and_leaves_offline},
      {"original_source_is_rejected_before_offline",
       test_original_source_is_rejected_before_offline},
      {"image_replacement_after_review_is_rejected",
       test_image_replacement_after_review_is_rejected},
      {"unsupported_target_classes_fail_closed",
       test_unsupported_target_classes_fail_closed},
      {"usb_disk_gets_connection_epoch_but_usb_memory_is_rejected",
       test_usb_disk_gets_connection_epoch_but_usb_memory_is_rejected},
      {"engine_failure_or_missing_proof_stays_offline",
       test_engine_failure_or_missing_proof_stays_offline},
      {"opened_target_class_drift_stops_before_engine",
       test_opened_target_class_drift_stops_before_engine},
      {"target_layout_drift_stops_before_or_after_offline",
       test_target_layout_drift_stops_before_or_after_offline},
      {"mbr_collects_connected_signatures_before_offline",
       test_mbr_collects_connected_signatures_before_offline},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
